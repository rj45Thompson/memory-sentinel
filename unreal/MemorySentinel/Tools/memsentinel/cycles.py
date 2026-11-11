"""Shared pointer cycles between types (UC-1.6, R3).

A TSharedPtr (or TSharedRef) is a reference count. When A holds a strong shared pointer to B and B holds one back to A,
neither count can reach zero once both exist, so both are leaked. The fix is to make one side a TWeakPtr.

`SharedGraph` draws one edge Owner -> Target for every member that holds a strong shared pointer to Target, directly or
inside a container. A type inherits its bases' edges, and an edge to a base also reaches every type derived from it in
the scanned code (a TSharedPtr<FBase> can point at an FDerived). Strongly connected components of that graph are the
cycles; Tarjan's algorithm finds them in one pass.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from .findings import Finding

BACK_NAMES = re.compile(r"(?i)(parent|owner|outer|prev|previous|back|container|manager|host|listener|delegate|root|up)")


@dataclass
class Edge:
    owner: str        # the type that declares the member
    via: str          # the type the edge leaves from (owner, or a type that inherits owner's member)
    member: str
    target: str       # the declared target type
    reaches: str      # the type the edge arrives at (target, or a type derived from it)
    file: str
    line: int
    holds: str

    def to_json(self) -> dict:
        return {"from": self.via, "to": self.reaches, "member": f"{self.owner}::{self.member}", "file": self.file,
                "line": self.line, "holds": self.holds, "declared_target": self.target}


class SharedGraph:
    def __init__(self, index):
        from .scanner import MemberReader, PointerType
        self.index = index
        self.declared: dict[str, list[Edge]] = {}
        for t in index.all():
            for m in MemberReader(t).members():
                if m.static:
                    continue
                for h in PointerType(m.type_text).holds():
                    if h.how == "shared":
                        self.declared.setdefault(t.name, []).append(
                            Edge(t.name, t.name, m.name, h.target, h.target, t.src.rel, m.line, h.describe()))
        self.edges: dict[str, list[Edge]] = {}
        for name in set(index.types) | set(self.declared):
            own = list(self.declared.get(name, []))
            for b in index.ancestry(name):
                own += self.declared.get(b, [])
            for e in own:
                for reach in [e.target] + sorted(index.descendants(e.target)):
                    self.edges.setdefault(name, []).append(
                        Edge(e.owner, name, e.member, e.target, reach, e.file, e.line, e.holds))

    def components(self) -> list[list[str]]:
        """Tarjan's strongly connected components, iterative (no recursion limit on big projects)."""
        index_of, low, on_stack, stack, out = {}, {}, set(), [], []
        counter = 0
        nodes = sorted(set(self.edges) | {e.reaches for es in self.edges.values() for e in es})
        for start in nodes:
            if start in index_of:
                continue
            work = [(start, iter(sorted({e.reaches for e in self.edges.get(start, [])})))]
            index_of[start] = low[start] = counter
            counter += 1
            stack.append(start)
            on_stack.add(start)
            while work:
                node, it = work[-1]
                advanced = False
                for nxt in it:
                    if nxt not in index_of:
                        index_of[nxt] = low[nxt] = counter
                        counter += 1
                        stack.append(nxt)
                        on_stack.add(nxt)
                        work.append((nxt, iter(sorted({e.reaches for e in self.edges.get(nxt, [])}))))
                        advanced = True
                        break
                    if nxt in on_stack:
                        low[node] = min(low[node], index_of[nxt])
                if advanced:
                    continue
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[node])
                if low[node] == index_of[node]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on_stack.discard(w)
                        comp.append(w)
                        if w == node:
                            break
                    out.append(sorted(comp))
        return out

    def path(self, comp: list[str]) -> list[Edge]:
        """One concrete cycle through a component: breadth first from its first type back to itself."""
        inside = set(comp)
        start = comp[0]
        prev: dict[str, Edge] = {}
        frontier = [start]
        seen = {start}
        while frontier:
            nxt = []
            for node in frontier:
                for e in sorted(self.edges.get(node, []), key=lambda e: (e.reaches, e.member)):
                    if e.reaches not in inside:
                        continue
                    if e.reaches == start:
                        chain = [e]
                        cur = node
                        while cur != start:
                            chain.append(prev[cur])
                            cur = prev[cur].via
                        return list(reversed(chain))
                    if e.reaches not in seen:
                        seen.add(e.reaches)
                        prev[e.reaches] = e
                        nxt.append(e.reaches)
            frontier = nxt
        return []

    def findings(self) -> list[Finding]:
        out = []
        for comp in self.components():
            if len(comp) > 1:
                cyc = self.path(comp)
                if not cyc:
                    continue
                weaken = next((e for e in reversed(cyc) if BACK_NAMES.search(e.member)), cyc[-1])
                first = cyc[0]
                names = " -> ".join([cyc[0].via] + [e.reaches for e in cyc])
                src = next((t.src for t in self.index.types.get(first.owner, [])), None)
                out.append(Finding(
                    rule="shared-cycle", severity="error", title=f"Shared pointer cycle: {names}",
                    where={"file": first.file, "line": first.line, "code": src.line_text(first.line) if src else ""},
                    why="Each type holds a strong shared pointer to the next and the last holds the first. Once a ring of "
                        "these exists, no reference count reaches zero, so none of them is ever freed.",
                    fix=f"Make {weaken.owner}::{weaken.member} a TWeakPtr (pin it where it is used). "
                        f"It is the edge that points back.",
                    how_known="READ: " + "; ".join(f"{e.owner}::{e.member} holds a {e.holds} ({e.file}:{e.line})"
                                                   + (f", and {e.reaches} derives from {e.target}" if e.reaches != e.target
                                                      else "") for e in cyc),
                    evidence={"cycle": [cyc[0].via] + [e.reaches for e in cyc], "edges": [e.to_json() for e in cyc],
                              "weaken": f"{weaken.owner}::{weaken.member}", "types": comp},
                    suppressed=src.suppression(first.line) if src else None))
        for name, es in sorted(self.edges.items()):
            for e in es:
                if e.reaches == name and e.via == e.owner and e.target == name:
                    src = next((t.src for t in self.index.types.get(e.owner, [])), None)
                    out.append(Finding(
                        rule="shared-self-edge", severity="info", title=f"{name} holds a shared pointer to its own type",
                        where={"file": e.file, "line": e.line, "code": src.line_text(e.line) if src else ""},
                        why=f"A chain or tree of {name} is fine until the code links one back to an earlier one. Then it "
                            f"is a ring and is never freed. Nothing in the types prevents it, so only a run can tell.",
                        fix=f"If {e.member} can point back up or around, make it a TWeakPtr.",
                        how_known=f"READ: {e.owner}::{e.member} holds a {e.holds} ({e.file}:{e.line})",
                        evidence={"cycle": [name, name], "edges": [e.to_json()]},
                        suppressed=src.suppression(e.line) if src else None))
        return out
