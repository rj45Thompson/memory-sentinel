"""The one findings schema every detector writes, in Python (UC-1..UC-4, R10, R11).

A findings file is what one run of one detector found: the static pass, a level leak test or a heap sampler run. The C++
side writes the same shape (FFindingsJson), the panel and the visual report read it. Three rules hold for every file:

- every finding has `how_known`: where the fact was read (a declaration's file:line, the engine's reference search, a canary
  read back from memory), so no finding rests on a guess (R10);
- `checked` says how much the detector looked at, so "no findings" can be told apart from "did not run" (R10);
- `blind_spots` says what this detector cannot see (R11).
"""
from __future__ import annotations

import datetime as _dt
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

SCHEMA = 1
SEVERITIES = ("error", "warning", "info")
KINDS = ("scan", "leaktest", "heap", "run")

# Every rule a detector can report, with the problem it belongs to (the spec's numbered problems) and its plain title.
RULES = {
    "uobject-member-unseen": ("dangling", "UObject pointer the garbage collector cannot see"),
    "static-uobject-pointer": ("dangling", "Static UObject pointer"),
    "subclassof-unseen": ("dangling", "TSubclassOf the garbage collector cannot see"),
    "uobject-in-plain-type": ("dangling", "UObject pointer in a plain C++ type"),
    "fgcobject-missing-member": ("dangling", "FGCObject that does not report a member"),
    "lambda-captures-uobject": ("dangling", "Lambda captures a UObject for later"),
    "void-hides-uobject": ("dangling", "UObject pointer hidden in a void*"),
    "shared-cycle": ("cycle", "Shared pointer cycle"),
    "shared-self-edge": ("cycle", "Type holds a shared pointer to its own type"),
    "shared-self-capture": ("cycle", "Lambda keeps its owner alive"),
    "level-survivor": ("leak", "Survived the level unload"),
    "heap-overrun": ("stomp", "Write past the end of a block"),
    "write-after-free": ("stomp", "Write into a freed block"),
    "double-free": ("stomp", "A block freed twice"),
    "guard-fault": ("stomp", "Touched a guard page"),
    "run-crash": ("crash", "The run crashed"),
}
PROBLEMS = {
    "dangling": "Dangling UObject pointers",
    "cycle": "Reference cycles",
    "leak": "Objects that outlive their level",
    "stomp": "Memory stomps",
    "crash": "Crashes during a run",
}


@dataclass
class Finding:
    rule: str
    severity: str
    title: str
    where: dict
    why: str
    fix: str
    how_known: str
    evidence: dict = field(default_factory=dict)
    id: str = ""
    suppressed: str | None = None

    def __post_init__(self) -> None:
        if self.rule not in RULES:
            raise ValueError(f"unknown rule {self.rule!r}")
        if self.severity not in SEVERITIES:
            raise ValueError(f"unknown severity {self.severity!r}")
        if not self.how_known:
            raise ValueError(f"{self.rule}: a finding must say how it is known")

    @property
    def problem(self) -> str:
        return RULES[self.rule][0]

    def to_json(self) -> dict:
        d = asdict(self)
        d["problem"] = self.problem
        if d["suppressed"] is None:
            del d["suppressed"]
        return d


@dataclass
class FindingsFile:
    kind: str
    target: dict
    checked: dict
    blind_spots: list[str]
    findings: list[Finding] = field(default_factory=list)
    made: str = ""
    tool: str = "memsentinel"
    version: str = "1.0"
    ok: bool = True
    failure: str = ""

    def __post_init__(self) -> None:
        if self.kind not in KINDS:
            raise ValueError(f"unknown kind {self.kind!r}")
        if not self.made:
            self.made = _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()

    def number(self, prefix: str) -> None:
        for k, f in enumerate(self.findings, 1):
            f.id = f"{prefix}-{k:03d}"

    def counts(self) -> dict:
        live = [f for f in self.findings if not f.suppressed]
        return {
            "error": sum(f.severity == "error" for f in live),
            "warning": sum(f.severity == "warning" for f in live),
            "info": sum(f.severity == "info" for f in live),
            "suppressed": sum(1 for f in self.findings if f.suppressed),
        }

    def to_json(self) -> dict:
        return {
            "schema": SCHEMA, "tool": self.tool, "version": self.version, "kind": self.kind, "made": self.made,
            "ok": self.ok, "failure": self.failure, "target": self.target, "checked": self.checked,
            "counts": self.counts(), "blind_spots": self.blind_spots,
            "findings": [f.to_json() for f in self.findings],
        }

    def write(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_json(), indent=1), encoding="utf-8")
        return path


def load(path: Path) -> dict:
    """Read a findings file written by either side and check its shape. Raises ValueError naming what is wrong."""
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    problems = []
    if data.get("tool") != "memsentinel":
        problems.append("not a memsentinel findings file (tool)")
    if data.get("schema") != SCHEMA:
        problems.append(f"schema {data.get('schema')!r}, expected {SCHEMA}")
    if data.get("kind") not in KINDS:
        problems.append(f"kind {data.get('kind')!r}")
    for key in ("checked", "blind_spots", "findings"):
        if key not in data:
            problems.append(f"no {key!r}")
    for k, f in enumerate(data.get("findings", [])):
        if f.get("rule") not in RULES:
            problems.append(f"finding {k}: rule {f.get('rule')!r}")
        if f.get("severity") not in SEVERITIES:
            problems.append(f"finding {k}: severity {f.get('severity')!r}")
        if not f.get("how_known"):
            problems.append(f"finding {k}: no how_known")
        f.setdefault("problem", RULES.get(f.get("rule"), ("crash",))[0])
    if problems:
        raise ValueError(f"{path}: " + "; ".join(problems))
    return data
