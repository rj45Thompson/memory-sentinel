"""The static pass (UC-1): read a project's own C++ and report the UObject pointers the garbage collector cannot see.

Why it matters: C++ has no runtime reflection, so Unreal's garbage collector only follows the references Unreal Header
Tool listed for it: members marked UPROPERTY, and whatever a type reports from AddReferencedObjects. Any other pointer to a
UObject is invisible to it. When nothing visible holds the object, the collector deletes it and the hidden pointer dangles.

The pass reads source text, not compiled code, so each finding says how it knows what it claims (`how_known`): where the
owner type and the pointed-to type were declared, or that the pointed-to type was judged a UObject only by Unreal's naming
rule (U or A prefix) because its declaration is not in the scanned code.

Classes, each named for the step it serves (USE_CASES.md):
- TypeIndex       every class and struct, its reflection macro, bases and body (UC-1.1, 1.2, 2c)
- MemberReader    the member declarations of one type body, functions and nested types skipped (UC-1.2, 1.3)
- PointerType     what a declared type holds: raw pointers, TObjectPtr, containers of them, shared pointers (UC-1.2, 1.6)
- MemberRules     members against R1 and R2 (UC-1.2, 1.3)
- CaptureFinder   lambdas handed to delegates and timers (UC-1.4, and the self capture of UC-1.6)
- VoidHideFinder  UObject pointers cast or assigned into void* (UC-1.5)
- Scanner         the whole pass, writes one FindingsFile (UC-1.1, 2b, 1a)
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import cycles
from .cpp import SourceText, angle_body, match_close, split_template_args
from .findings import Finding, FindingsFile

IDENT = r"[A-Za-z_]\w*"
REFLECT = ("UCLASS", "USTRUCT", "UINTERFACE")
SKIP_DIRS = {"Intermediate", "Binaries", "Saved", "DerivedDataCache", "ThirdParty", "Content", ".git", ".vs"}
NAMING_RULE = re.compile(r"[UA][A-Z][A-Za-z0-9_]*[a-z][A-Za-z0-9_]*")
QUALIFIERS = {"const", "volatile", "class", "struct", "typename", "mutable", "static", "inline", "constexpr", "unsigned",
              "signed", "enum"}
SAFE_WRAPPERS = {"TWeakObjectPtr", "TSoftObjectPtr", "TSoftClassPtr", "TLazyObjectPtr", "TStrongObjectPtr",
                 "FWeakObjectPtr", "FSoftObjectPtr", "FLazyObjectPtr", "FSoftObjectPath", "TWeakInterfacePtr"}
OPAQUE = {"TFunction", "TFunctionRef", "TUniqueFunction", "TDelegate", "TMulticastDelegate", "TBaseDelegate",
          "TAttribute", "TFuture", "TPromise", "TSharedFuture"}
SHARED_STRONG = {"TSharedPtr", "TSharedRef"}
SHARED_WEAK = {"TWeakPtr"}
# Plain types that usually live across frames: a raw UObject pointer in one of these is an error, not a warning.
LONG_LIVED_BASES = {"FTickableGameObject", "FTickableEditorObject", "IModuleInterface", "FRunnable", "TSharedFromThis",
                    "SWidget", "SCompoundWidget", "SLeafWidget", "SPanel", "SUserWidget", "FTSTicker", "IConsoleObject",
                    "FSelfRegisteringExec", "FExec", "FGCObject"}

BLIND_SPOTS = [
    "Only the project's own Source/ folders and its plugins' Source/ folders are read; engine code and engine plugins are not.",
    "Generated code (.generated.h, .gen.cpp) is not read: what Unreal Header Tool adds is taken as given.",
    "A pointer held through a type alias (using, typedef) or written by a macro is not followed.",
    "Cycles that only form at runtime are not visible in the types; a type that holds its own type is listed as info.",
    "Whether a plain struct lives across a frame is not known: a short-lived one (an argument, a stack local) is safe, so "
    "those findings are warnings unless the type is one that usually lives on.",
    "A lambda is judged by the call it is handed to. A lambda stored in a variable first and bound later is not followed.",
    "A UObject passed through void* is seen only where the cast or assignment names it.",
]


# ---------------------------------------------------------------------------------------------------------- types

@dataclass
class TypeDecl:
    """One class or struct definition: its reflection macro (UCLASS, USTRUCT, UINTERFACE or '' for plain C++)."""
    name: str
    macro: str
    keyword: str
    bases: list[str]
    src: SourceText
    head_at: int
    body_start: int          # offset of the opening brace
    body_end: int            # offset just past the closing brace
    template: bool = False

    @property
    def line(self) -> int:
        return self.src.line_of(self.head_at)

    @property
    def where(self) -> str:
        return f"{self.src.rel}:{self.line}"

    @property
    def kind(self) -> str:
        return self.macro or ("plain " + self.keyword)


class TypeIndex:
    """Every class and struct definition in the scanned files, by name (UC-1.1). A forward declaration is not a
    definition and is not indexed; two definitions of one name (#if branches) are both kept."""

    HEAD = re.compile(r"\b(class|struct)\s+")

    def __init__(self, sources: list[SourceText]):
        self.sources = sources
        self.types: dict[str, list[TypeDecl]] = {}
        for src in sources:
            for t in self._read(src):
                self.types.setdefault(t.name, []).append(t)
        self.derived: dict[str, set[str]] = {}
        for name, decls in self.types.items():
            for t in decls:
                for b in t.bases:
                    self.derived.setdefault(b, set()).add(name)

    def _read(self, src: SourceText):
        code = src.code
        for m in self.HEAD.finditer(code):
            keyword = m.group(1)
            before = code[:m.start()].rstrip()
            if re.search(r"\b(enum|friend|typename)$", before) or before.endswith(("<", ",")):
                continue
            j = m.end()
            # API macro (MYGAME_API), alignas(...), attributes
            while True:
                mm = re.match(r"\s*(?:[A-Z][A-Z0-9_]*_API\b|alignas\s*\([^)]*\)|\[\[[^\]]*\]\])", code[j:])
                if not mm:
                    break
                j += mm.end()
            nm = re.match(rf"\s*({IDENT}(?:\s*::\s*{IDENT})*)\s*(final\b)?\s*", code[j:])
            if not nm:
                continue
            name = re.sub(r"\s+", "", nm.group(1)).split("::")[-1]
            k = j + nm.end()
            if k >= len(code) or code[k] not in ":{":
                continue  # a forward declaration, an elaborated type (class UFoo* X) or a template argument
            bases: list[str] = []
            if code[k] == ":":
                if code.startswith("::", k):
                    continue
                brace = code.find("{", k)
                semi = code.find(";", k)
                if brace < 0 or (0 <= semi < brace):
                    continue
                for part in split_template_args(code[k + 1:brace]):
                    part = re.sub(r"\b(public|protected|private|virtual)\b", " ", part).strip()
                    bm = re.match(rf"({IDENT}(?:\s*::\s*{IDENT})*)", part)
                    if bm:
                        bases.append(re.sub(r"\s+", "", bm.group(1)).split("::")[-1])
                k = brace
            macro = ""
            pre = before
            if pre.endswith(")"):
                open_at = _match_open_back(pre, len(pre) - 1)
                mac = re.search(rf"({IDENT})\s*$", pre[:open_at])
                if mac and mac.group(1) in REFLECT:
                    macro = mac.group(1)
            template = bool(re.search(r"template\s*<[^;{}]*>\s*$", before))
            yield TypeDecl(name=name, macro=macro, keyword=keyword, bases=bases, src=src, head_at=m.start(),
                           body_start=k, body_end=match_close(code, k), template=template)

    def get(self, name: str) -> TypeDecl | None:
        decls = self.types.get(name)
        return decls[0] if decls else None

    def all(self):
        for decls in self.types.values():
            yield from decls

    def ancestry(self, name: str, seen: set | None = None) -> list[str]:
        """Every base reachable from `name` through the scanned code (names not in the index stop the walk)."""
        seen = seen if seen is not None else set()
        out = []
        for t in self.types.get(name, []):
            for b in t.bases:
                if b in seen:
                    continue
                seen.add(b)
                out.append(b)
                out.extend(self.ancestry(b, seen))
        return out

    def descendants(self, name: str) -> set[str]:
        out, todo = set(), [name]
        while todo:
            for d in self.derived.get(todo.pop(), ()):
                if d not in out:
                    out.add(d)
                    todo.append(d)
        return out

    def uobject(self, name: str) -> tuple[bool, str]:
        """Is `name` a UObject class? Returns (answer, how it is known)."""
        t = self.get(name)
        if t is not None:
            if t.macro == "UCLASS":
                return True, f"{name} is a UCLASS ({t.where})"
            if t.macro == "UINTERFACE":
                return True, f"{name} is a UINTERFACE ({t.where})"
            return False, f"{name} is a {t.kind} ({t.where})"
        if name in ("UObject", "UClass", "AActor") or NAMING_RULE.fullmatch(name):
            return True, (f"{name} is not declared in the scanned code; it is taken as a UObject by Unreal's naming rule "
                          f"(U and A prefixes mark UObject and AActor classes)")
        return False, ""

    def interface(self, name: str) -> tuple[bool, str]:
        """The native I-class of a UINTERFACE declared in the scanned code (IFoo for UFoo)."""
        if name.startswith("I") and len(name) > 1:
            t = self.get("U" + name[1:])
            if t is not None and t.macro == "UINTERFACE":
                return True, f"{name} is the interface of the UINTERFACE U{name[1:]} ({t.where})"
        return False, ""


def _match_open_back(text: str, close_at: int) -> int:
    depth = 0
    for k in range(close_at, -1, -1):
        if text[k] == ")":
            depth += 1
        elif text[k] == "(":
            depth -= 1
            if depth == 0:
                return k
    return 0


# ---------------------------------------------------------------------------------------------------------- members

@dataclass
class Member:
    owner: TypeDecl
    name: str
    type_text: str
    at: int
    uproperty: bool
    static: bool

    @property
    def line(self) -> int:
        return self.owner.src.line_of(self.at)


class MemberReader:
    """The member declarations of one type body (UC-1.2). Function bodies, nested types, brace initializers and macro
    statements are stepped over; UPROPERTY(...) in front of a declaration is remembered."""

    STRIP_LEAD = re.compile(r"^\s*(?:(?:public|protected|private)\s*:(?!:)|GENERATED_\w*BODY\w*\s*\(\s*\)\s*;?)")
    SKIP_START = re.compile(r"^\s*(?:using|typedef|friend|static_assert|template|enum|union|operator|DECLARE_\w+|"
                            r"SLATE_\w+|UE_\w+_DEFINE|virtual\s+~|~)\b")

    def __init__(self, t: TypeDecl):
        self.t = t

    def members(self):
        code = self.t.src.code
        i = self.t.body_start + 1
        end = self.t.body_end - 1
        stmt = i
        while i < end:
            ch = code[i]
            if ch in "([":
                i = match_close(code, i)
                continue
            if ch == "{":
                head = code[stmt:i]
                close = match_close(code, i)
                kind = self._brace_kind(head)
                if kind == "initializer":
                    i = close
                    continue
                if kind == "type":
                    # a nested type; `struct {..} X;` declares X, read to the ';'
                    j = close
                    while j < end and code[j] in " \t\n":
                        j += 1
                    if j < end and code[j] == ";":
                        j += 1
                    i = stmt = j
                    continue
                i = stmt = close   # a function body
                continue
            if ch == ";":
                yield from self._declaration(stmt, i)
                stmt = i + 1
            i += 1

    def _clean(self, text: str) -> tuple[str, bool, int]:
        """Strip access specifiers, GENERATED_BODY, UPROPERTY(...) and other leading macro calls.
        Returns (rest, had UPROPERTY, chars removed)."""
        removed, uprop = 0, False
        while True:
            m = self.STRIP_LEAD.match(text)
            if m:
                text, removed = text[m.end():], removed + m.end()
                continue
            m = re.match(rf"^\s*({IDENT})\s*\(", text)
            if m and m.group(1).isupper() or (m and m.group(1) in ("UPROPERTY", "UFUNCTION", "UE_DEPRECATED")):
                close = match_close(text, m.end() - 1)
                rest = text[close:]
                if m.group(1) == "UPROPERTY":
                    uprop = True
                elif not rest.strip():
                    return "", uprop, removed      # a macro statement: MACRO(...);
                text, removed = rest, removed + close
                continue
            return text, uprop, removed

    def _brace_kind(self, head: str) -> str:
        rest, _, _ = self._clean(head)
        if re.match(r"^\s*(?:template\s*<.*>\s*)?(?:struct|class|union|enum|namespace)\b", rest, re.S):
            return "type"
        eq, par = _top_level(rest, "="), _top_level(rest, "(")
        if eq >= 0 and (par < 0 or eq < par):
            return "initializer"
        if par >= 0:
            return "function"
        return "initializer"

    def _declaration(self, a: int, b: int):
        code = self.t.src.code
        text, uprop, removed = self._clean(code[a:b])
        if not text.strip() or self.SKIP_START.match(text):
            return
        at = a + removed + (len(text) - len(text.lstrip()))
        text = text.strip()
        eq, par = _top_level(text, "="), _top_level(text, "(")
        if par >= 0 and (eq < 0 or par < eq):
            return  # a function declaration (or a function pointer member)
        decl = text[:eq] if eq >= 0 else text
        brace = _top_level(decl, "{")
        if brace >= 0:
            decl = decl[:brace]
        static = bool(re.match(r"^\s*(?:inline\s+)?static\b", decl))
        parts = split_template_args(decl)
        if not parts:
            return
        first = parts[0]
        m = re.match(rf"^(.*?)({IDENT})\s*(\[[^\]]*\])*\s*(:\s*\d+)?\s*$", first, re.S)
        if not m or not m.group(1).strip():
            return
        type_text, name = m.group(1).strip(), m.group(2)
        if name in QUALIFIERS or type_text.endswith("::"):
            return
        yield Member(self.t, name, type_text, at, uprop, static)
        base = re.sub(r"[\*&\s]+$", "", type_text)
        for extra in parts[1:]:
            em = re.match(rf"^\s*([\*&\s]*)({IDENT})", extra)
            if em:
                yield Member(self.t, em.group(2), base + " " + em.group(1).strip(), at, uprop, static)


def _top_level(text: str, ch: str) -> int:
    """Offset of `ch` outside (), [], {} and <>, or -1. '=' inside '==', '<=', '>=', '!=' is skipped."""
    depth = angle = 0
    for k, c in enumerate(text):
        if c in "([{":
            if c == ch and depth == 0 and angle == 0:
                return k
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "<":
            angle += 1
        elif c == ">" and angle > 0 and text[k - 1] != "-":
            angle -= 1
        elif c == ch and depth == 0 and angle == 0:
            if ch == "=" and (text[k + 1:k + 2] == "=" or text[k - 1:k] in "<>!="):
                continue
            return k
    return -1


# ---------------------------------------------------------------------------------------------------------- pointer types

@dataclass
class Hold:
    how: str       # raw, objectptr, scriptinterface, subclassof, safe, shared, sharedweak
    target: str
    via: list[str] = field(default_factory=list)

    def describe(self) -> str:
        inner = {"raw": f"{self.target}*", "objectptr": f"TObjectPtr<{self.target}>",
                 "scriptinterface": f"TScriptInterface<{self.target}>", "subclassof": f"TSubclassOf<{self.target}>",
                 "shared": f"shared pointer to {self.target}", "sharedweak": f"TWeakPtr<{self.target}>",
                 "safe": f"weak or soft pointer to {self.target}"}[self.how]
        return inner + (" inside " + " inside ".join(reversed(self.via)) if self.via else "")


class PointerType:
    """What a declared C++ type holds (UC-1.2, 1.6). `TArray<TObjectPtr<UFoo>>` holds UFoo through TObjectPtr inside TArray."""

    def __init__(self, text: str):
        self.text = text

    def holds(self) -> list[Hold]:
        node = _parse_type(self.text)
        return _holds(node, []) if node else []


@dataclass
class _Node:
    name: str
    args: list["_Node"]
    stars: int
    raw_args: str = ""


def _parse_type(text: str) -> _Node | None:
    s = text.strip()
    while True:
        m = re.match(r"^(?:const|volatile|class|struct|typename|mutable|static|inline|constexpr|unsigned|signed)\b\s*", s)
        if not m:
            break
        s = s[m.end():]
    m = re.match(rf"^(::)?({IDENT}(?:\s*::\s*{IDENT})*)", s)
    if not m:
        return None
    name = re.sub(r"\s+", "", m.group(2)).split("::")[-1]
    rest = s[m.end():].lstrip()
    args: list[_Node] = []
    raw_args = ""
    if rest.startswith("<"):
        raw_args, after = angle_body(rest, 0)
        if name not in OPAQUE:
            for a in split_template_args(raw_args):
                n = _parse_type(a)
                if n:
                    args.append(n)
        rest = rest[after:]
    rest = re.sub(r"\b(const|volatile)\b", "", rest)
    return _Node(name, args, rest.count("*"), raw_args)


def _holds(n: _Node, via: list[str]) -> list[Hold]:
    if n.name in OPAQUE:
        return []
    target = n.args[0].name if n.args else ""
    if n.name == "TObjectPtr" and target:
        return [Hold("objectptr", target, via)]
    if n.name in SAFE_WRAPPERS:
        return [Hold("safe", target, via)] if target else []
    if n.name == "TSubclassOf" and target:
        return [Hold("subclassof", target, via)]
    if n.name == "TScriptInterface" and target:
        return [Hold("scriptinterface", target, via)]
    if n.name in SHARED_STRONG and target:
        return [Hold("shared", target, via)]
    if n.name in SHARED_WEAK and target:
        return [Hold("sharedweak", target, via)]
    if n.stars and not n.args:
        return [Hold("raw", n.name, via)]
    out: list[Hold] = []
    for a in n.args:
        out.extend(_holds(a, via + [n.name]))
    return out


VALUE_CONTAINERS = {"TArray", "TSet", "TMap", "TMultiMap", "TSortedMap", "TStaticArray", "TOptional", "TInlineAllocator",
                    "TSparseArray", "TChunkedArray", "TIndirectArray", "TPair", "TTuple", "TVariant"}


def _by_value_names(node: "_Node | None") -> list[str]:
    """The type names a declaration holds by value: FFoo, and FFoo inside TArray<FFoo> (not FFoo* or TSharedPtr<FFoo>)."""
    if node is None or node.stars:
        return []
    if not node.args:
        return [node.name]
    if node.name in VALUE_CONTAINERS:
        return [n for a in node.args for n in _by_value_names(a)]
    return []


# ---------------------------------------------------------------------------------------------------------- reports

class Reported:
    """The member names a type reports to the garbage collector from its own AddReferencedObjects (UC-1, alternate 2a)."""

    def __init__(self, index: TypeIndex):
        self.index = index
        self._cache: dict[str, tuple[set[str], str]] = {}

    def names(self, type_name: str) -> tuple[set[str], str]:
        if type_name in self._cache:
            return self._cache[type_name]
        found: set[str] = set()
        where = ""
        for t in self.index.types.get(type_name, []):
            body = t.src.code[t.body_start:t.body_end]
            for m in re.finditer(r"\bAddReferencedObjects\s*\(", body):
                blk = _body_after(t.src.code, t.body_start + m.end() - 1)
                if blk:
                    found |= set(re.findall(IDENT, t.src.code[blk[0]:blk[1]]))
                    where = where or f"{t.src.rel}:{t.src.line_of(t.body_start + m.start())}"
        pat = re.compile(rf"\b{re.escape(type_name)}\s*::\s*AddReferencedObjects\s*\(")
        for src in self.index.sources:
            for m in pat.finditer(src.code):
                blk = _body_after(src.code, m.end() - 1)
                if blk:
                    found |= set(re.findall(IDENT, src.code[blk[0]:blk[1]]))
                    where = where or f"{src.rel}:{src.line_of(m.start())}"
        self._cache[type_name] = (found, where)
        return found, where


class Rooted:
    """Pointer members whose object the code puts in the root set itself (X->AddToRoot(), or NewObject with
    RF_MarkAsRootSet), read from the files that share the declaring file's name. A rooted object is never collected, so
    the pointer cannot dangle while it stays rooted."""

    def __init__(self, index: TypeIndex):
        self.index = index

    def where(self, member: Member) -> str:
        stem = Path(member.owner.src.rel).stem
        n = re.escape(member.name)
        rx = re.compile(r"\b" + n + r"\s*->\s*AddToRoot\s*\(|\b" + n + r"\s*=\s*NewObject\s*<[^;]*\bRF_MarkAsRootSet\b")
        for src in self.index.sources:
            if Path(src.rel).stem != stem:
                continue
            m = rx.search(src.code)
            if m:
                return f"{src.rel}:{src.line_of(m.start())}"
        return ""


def _body_after(code: str, paren_at: int) -> tuple[int, int] | None:
    """For a function whose parameter list opens at paren_at: the span of its body, or None for a declaration."""
    close = match_close(code, paren_at)
    m = re.match(r"\s*(?:const\b|override\b|final\b|noexcept\b|\s)*\{", code[close:])
    if not m:
        return None
    start = close + m.end() - 1
    return start, match_close(code, start)


class MemberRules:
    """Members against R1 (UCLASS/USTRUCT members the collector cannot see, static pointers) and R2 (plain types)."""

    def __init__(self, index: TypeIndex, reported: Reported):
        self.index = index
        self.reported = reported
        self.rooted = Rooted(index)
        self.kept_by_root: list[tuple[str, int, str]] = []   # not reported: the code roots the object itself

    def check(self, m: Member) -> list[Finding]:
        out = []
        holds = [h for h in PointerType(m.type_text).holds() if h.how in ("raw", "objectptr", "scriptinterface", "subclassof")]
        for h in holds:
            if h.how == "scriptinterface":
                is_obj, how_t = True, f"TScriptInterface holds the UObject that implements {h.target}"
            else:
                is_obj, how_t = self.index.uobject(h.target)
                if not is_obj and h.how == "raw":
                    is_obj, how_t = self.index.interface(h.target)
            if h.how == "subclassof":
                is_obj, how_t = True, f"TSubclassOf<{h.target}> holds a UClass"
            if not is_obj:
                continue
            f = self._judge(m, h, how_t)
            if f:
                out.append(f)
                break   # one finding per member: TMap<UA*, UB*> is one declaration to fix, not two
        return out

    def _judge(self, m: Member, h: Hold, how_t: str) -> Finding | None:
        t = m.owner
        src = t.src
        where = {"file": src.rel, "line": m.line, "code": src.line_text(m.line)}
        ev = {"owner": t.name, "owner_kind": t.kind, "member": m.name, "type": m.type_text, "holds": h.describe()}
        decl_at = f"the declaration at {src.rel}:{m.line}"
        if not h.via and h.how in ("raw", "objectptr"):
            rooted_at = self.rooted.where(m)
            if rooted_at:
                self.kept_by_root.append((src.rel, m.line, rooted_at))
                return None
        if m.static:
            return self._f("static-uobject-pointer", "warning" if h.how == "subclassof" else "error", f"Static {h.describe()} {t.name}::{m.name}", where,
                           "The garbage collector never looks at static variables, so this pointer does not keep the object "
                           "alive and is not cleared when the object is destroyed.",
                           "Use TWeakObjectPtr if it only needs to find the object, or TStrongObjectPtr (or AddToRoot) if it "
                           "must keep the object alive.",
                           f"READ: {how_t}; {decl_at} is static", ev, src, m.line)
        names, rep_where = self.reported.names(t.name)
        if m.name in names:
            return None
        if m.uproperty and t.macro in ("UCLASS", "USTRUCT", "UINTERFACE"):
            return None
        if t.macro != "UCLASS" and not h.via and h.how in ("raw", "objectptr"):
            inside = self._container_of(t.name, h.target)
            if inside:
                rule = "uobject-member-unseen" if t.macro else "uobject-in-plain-type"
                return self._f(rule, "info", f"{t.name}::{m.name} points back at the {h.target} that contains it", where,
                               f"The garbage collector cannot see this pointer, but {t.name} lives inside {inside}, so it points "
                               f"back at the object that holds it and cannot outlive it while it stays there. A copy of "
                               f"{t.name} kept anywhere else can.",
                               f"Nothing, while every {t.name} stays inside its {h.target}. If copies are kept elsewhere, make "
                               f"this a TWeakObjectPtr.",
                               f"READ: {how_t}; {t.name} is held by value in {inside}", ev, src, m.line)
        if t.macro in ("UCLASS", "USTRUCT", "UINTERFACE"):
            if h.how == "subclassof":
                return self._f("subclassof-unseen", "warning", f"{t.name}::{m.name} holds a class with no UPROPERTY",
                               where,
                               "TSubclassOf holds a UClass pointer. Native classes are never collected, but a Blueprint class "
                               "can be unloaded, and without UPROPERTY nothing tells the collector this member uses it.",
                               "Add UPROPERTY() to the declaration.",
                               f"READ: {t.name} is a {t.macro} ({t.where}); {how_t}; no UPROPERTY on {decl_at}", ev, src,
                               m.line)
            known = f"READ: {t.name} is a {t.macro} ({t.where}); {how_t}; no UPROPERTY on {decl_at}"
            if t.macro == "UCLASS":
                known += f"; not named in {t.name}::AddReferencedObjects" + (f" ({rep_where})" if rep_where else
                                                                             " (it has none)")
            return self._f("uobject-member-unseen", "error",
                           f"{t.name}::{m.name} holds a {h.describe()} the garbage collector cannot see", where,
                           "With no UPROPERTY, Unreal Header Tool leaves this member out of the reference list the garbage "
                           "collector walks. If nothing else holds the object, it is deleted and this pointer dangles.",
                           "Add UPROPERTY() to the declaration (and use TObjectPtr<> for a raw pointer). If it must not keep "
                           "the object alive, make it a TWeakObjectPtr instead.",
                           known, ev, src, m.line)
        # a plain C++ type
        chain = [t.name] + self.index.ancestry(t.name)
        if "FGCObject" in chain:
            return self._f("fgcobject-missing-member", "warning" if h.how == "subclassof" else "error",
                           f"{t.name}::{m.name} is not reported by its AddReferencedObjects", where,
                           f"{t.name} is an FGCObject, so the garbage collector asks it for its references, but its "
                           f"AddReferencedObjects does not report {m.name}. The object can be collected while this pointer "
                           f"still points at it.",
                           f"Call Collector.AddReferencedObject({m.name}) in {t.name}::AddReferencedObjects.",
                           f"READ: {t.name} derives from FGCObject ({t.where}); {how_t}; {m.name} is not named in its "
                           f"AddReferencedObjects" + (f" ({rep_where})" if rep_where else " (none found)"), ev, src, m.line)
        long_lived = next((b for b in chain if b in LONG_LIVED_BASES or re.fullmatch(r"S[A-Z]\w*Widget|S[A-Z]\w*", b)
                           and b != t.name), None)
        sev = "error" if long_lived and h.how != "subclassof" else "warning"
        why = ("A plain C++ type is invisible to the garbage collector, so this pointer neither keeps the object alive nor is "
               "cleared when the object is destroyed.")
        if h.how == "subclassof":
            why += " A native class is never collected; a Blueprint class can be."
        if long_lived:
            why += f" {t.name} derives from {long_lived}, which usually lives across frames, so it can outlive the object."
        else:
            why += " If an instance lives across a frame (a member of something long-lived, a cache), it can dangle."
        return self._f("uobject-in-plain-type", sev, f"{t.name}::{m.name} holds a {h.describe()} in a plain C++ type",
                       where, why,
                       f"Use TWeakObjectPtr, make {t.name} a USTRUCT with UPROPERTY members, or derive it from FGCObject "
                       f"and report the member in AddReferencedObjects.",
                       f"READ: {t.name} is a {t.kind} with no reflection macro ({t.where}); {how_t}"
                       + (f"; derives from {long_lived}" if long_lived else ""), ev, src, m.line)

    def _container_of(self, owner: str, target: str) -> str:
        """Where `owner` is held by value (directly or in a container) inside `target` or a type derived from it:
        'UMyComponent::Effects (file:line)', or ''."""
        if not hasattr(self, "_by_value"):
            self._by_value: dict[str, list[tuple[str, str]]] = {}
            for d in self.index.all():
                for mm in MemberReader(d).members():
                    node = _parse_type(mm.type_text)
                    for name in _by_value_names(node):
                        self._by_value.setdefault(name, []).append((d.name, f"{d.name}::{mm.name} ({d.src.rel}:{mm.line})"))
        holders = {target} | self.index.descendants(target)
        for holder, where in self._by_value.get(owner, []):
            if holder in holders:
                return where
        return ""

    @staticmethod
    def _f(rule, sev, title, where, why, fix, known, ev, src: SourceText, line: int) -> Finding:
        return Finding(rule=rule, severity=sev, title=title, where=where, why=why, fix=fix, how_known=known, evidence=ev,
                       suppressed=src.suppression(line))


# ---------------------------------------------------------------------------------------------------------- lambdas

DEFERRED = {"CreateLambda", "BindLambda", "AddLambda", "SetTimer", "SetTimerForNextTick", "AsyncTask", "Async",
            "AsyncThread", "AsyncPool", "ExecuteOnGameThread", "CreateAndDispatchWhenReady", "Launch", "AddTicker"}
GUARDED = {"CreateWeakLambda", "AddWeakLambda", "BindWeakLambda", "CreateUObject", "AddUObject", "BindUObject",
           "CreateSPLambda", "AddSPLambda", "BindSPLambda", "CreateSP", "AddSP", "BindSP"}
SELF_SHARED = re.compile(r"\b(?:SharedThis\s*\(\s*this\s*\)|AsShared\s*\(\s*\)|SharedFromThis\s*\()")
INHERITED_MEMBERS = {"GetWorld", "GetOwner", "GetName", "GetClass", "GetOuter", "GetActorLocation", "GetActorRotation",
                     "GetActorTransform", "SetActorLocation", "Destroy", "GetGameInstance", "GetComponentByClass",
                     "GetRootComponent", "GetPawn", "GetController", "GetOwningPlayer", "IsValidLowLevel",
                     "MarkPackageDirty", "GetTypedOuter", "GetWorldTimerManager"}
LAMBDA = re.compile(r"\[([^\[\]]*)\]\s*(?:\((?:[^()]|\([^()]*\))*\))?\s*(?:mutable\b\s*)?(?:->[^{;]*?)?\{")


@dataclass
class FunctionSpan:
    cls: str
    name: str
    start: int
    end: int


def function_spans(src: SourceText) -> list[FunctionSpan]:
    """Out-of-line member function definitions `Class::Method(...) ... { }` and their body spans."""
    code = src.code
    out = []
    for m in re.finditer(rf"\b({IDENT})\s*::\s*(~?{IDENT})\s*\(", code):
        close = match_close(code, m.end() - 1)
        j = close
        n = len(code)
        # walk past qualifiers and a constructor's initializer list to the body
        depth_ok = True
        while j < n:
            c = code[j]
            if c == "{":
                prev = code[:j].rstrip()
                if prev.endswith((")", "}")) or re.search(r"\b(const|override|final|noexcept)$", prev):
                    break
                j = match_close(code, j)
                continue
            if c == "(":
                j = match_close(code, j)
                continue
            if c in ";=":
                depth_ok = False
                break
            j += 1
        if not depth_ok or j >= n:
            continue
        out.append(FunctionSpan(m.group(1), m.group(2), j, match_close(code, j)))
    return out


class CaptureFinder:
    """Lambdas handed to a delegate or a timer that capture a UObject they do not guard (UC-1.4), and lambdas that keep
    their own shared owner alive from one of its member delegates (UC-1.6)."""

    def __init__(self, index: TypeIndex, uobject_vars: "UObjectVars"):
        self.index = index
        self.vars = uobject_vars
        self.released: list[tuple[str, int, str]] = []   # lambdas not reported because their handle is released
        self.on_stack: list[tuple[str, int, str]] = []   # lambdas bound to a delegate that lives on the stack

    def scan(self, src: SourceText) -> list[Finding]:
        code = src.code
        spans = function_spans(src)
        out = []
        for m in LAMBDA.finditer(code):
            prev = code[:m.start()].rstrip()
            if not prev or not (prev[-1] in "(,={:?" or prev.endswith("return")):
                continue
            call = self._enclosing_call(code, m.start())
            if not call:
                continue
            fn, receiver, paren_at, root = call
            caps = [c.strip() for c in split_template_args(m.group(1))]
            owner = self._owner(src, spans, m.start())
            body_end = match_close(code, m.end() - 1)
            body = code[m.end():body_end]
            line = src.line_of(m.start())
            where = {"file": src.rel, "line": line, "code": src.line_text(line)}
            local_in = self._is_local(code, spans, m.start(), root)
            if local_in:
                self.on_stack.append((src.rel, line, f"{root} is a stack variable of {local_in}"))
                continue
            if fn in DEFERRED:
                f = self._uobject_capture(src, fn, caps, owner, body, where, line, paren_at)
                if f:
                    out.append(f)
            if fn in DEFERRED | {"Bind", "Add"} and receiver and owner:
                selfcap = next((c for c in caps if SELF_SHARED.search(c)), None)
                if selfcap and receiver in self._member_names(owner):
                    out.append(Finding(
                        rule="shared-self-capture", severity="warning",
                        title=f"{owner}::{receiver} holds a lambda that holds {owner}", where=where,
                        why=f"The lambda captures a shared pointer to its own object ({selfcap}) and is stored in that "
                            f"object's member delegate {receiver}. The object holds the delegate, the delegate holds the "
                            f"object: neither reference count reaches zero.",
                        fix="Capture a TWeakPtr (or use CreateSP / CreateSPLambda, which hold the object weakly) and pin it "
                            "inside the lambda.",
                        how_known=f"READ: the capture list names {selfcap}; {receiver} is a member of {owner} "
                                  f"({self.index.get(owner).where if self.index.get(owner) else 'scanned code'})",
                        evidence={"owner": owner, "member": receiver, "capture": selfcap, "call": fn},
                        suppressed=src.suppression(line)))
        return out

    def _uobject_capture(self, src, fn, caps, owner, body, where, line, paren_at) -> Finding | None:
        owner_obj, owner_how = self.index.uobject(owner) if owner else (False, "")
        hit, how = None, ""
        for c in caps:
            if c == "this" or re.fullmatch(r"\w+\s*=\s*this", c):
                if owner_obj:
                    hit, how = c, f"the capture list names this; {owner_how}"
                    break
            elif c in ("=", "&"):
                if owner_obj:
                    used = self._uses_member(owner, body)
                    if used:
                        hit, how = c, (f"the default capture [{c}] captures this because the body uses {used}, a member of "
                                       f"{owner}; {owner_how}")
                        break
            else:
                name = re.sub(r"^&", "", c).split("=")[0].strip()
                init = c.split("=", 1)[1].strip() if "=" in c else name
                if re.search(r"\b(MakeWeakObjectPtr|TWeakObjectPtr|TWeakPtr|TSoftObjectPtr|TStrongObjectPtr)\b", init):
                    continue
                decl = self.vars.lookup(src, init)
                if decl:
                    hit, how = c, f"{init} is declared as {decl}"
                    break
        if not hit:
            return None
        handle = self._released_handle(src, fn, paren_at)
        if handle:
            self.released.append((src.rel, line, handle))
            return None
        verb = "a timer" if "Timer" in fn else ("a task" if fn.startswith(("Async", "Launch", "Execute", "CreateAndDispatch"))
                                                else "a delegate")
        return Finding(
            rule="lambda-captures-uobject", severity="warning",
            title=f"Lambda handed to {fn} captures a UObject it does not guard", where=where,
            why=f"The lambda is kept by {verb} and runs later. It captures {hit} as a raw pointer, which the garbage "
                f"collector does not see: if the object is destroyed first, the lambda runs on a dangling pointer.",
            fix="Use CreateWeakLambda(this, ...) / AddWeakLambda / AddUObject, or capture a TWeakObjectPtr and check it "
                "inside the lambda.",
            how_known=f"READ: the lambda is an argument of {fn}; {how}",
            evidence={"call": fn, "capture": hit, "owner": owner or ""}, suppressed=src.suppression(line))

    RELEASE = ("Remove", "RemoveTicker", "Unregister", "ClearTimer", "ClearAndInvalidateTimer", "RemoveAll", "Unbind")

    def _released_handle(self, src: SourceText, fn: str, paren_at: int) -> str:
        """The handle of this binding when the code releases it (Room->OnJoined.Remove(Handle), ClearTimer(Handle)):
        a released binding cannot fire after its owner is gone, so it is not reported. Returns '' when none is found."""
        code = src.code
        handle = ""
        if fn == "SetTimer":
            args = split_template_args(code[paren_at + 1:match_close(code, paren_at) - 1])
            handle = args[0] if args else ""
        else:
            start = max(code.rfind(";", 0, paren_at), code.rfind("{", 0, paren_at), code.rfind("}", 0, paren_at)) + 1
            m = re.match(rf"\s*(?:(?:const\s+)?{IDENT}\s+)?([\w\.\->\s]+?)\s*=(?!=)", code[start:paren_at])
            handle = m.group(1) if m else ""
        names = re.findall(IDENT, handle)
        if not names:
            return ""
        h = re.escape(names[-1])
        rx = re.compile(r"\b(?:" + "|".join(self.RELEASE) + r")\s*\((?:[^();]*,)?\s*&?\s*(?:\w+\s*(?:\.|->)\s*)*" + h
                        + r"\s*\)|\b" + h + r"\s*\.\s*(?:Reset|Invalidate)\s*\(")
        stem = Path(src.rel).stem
        for other in self.index.sources:
            if Path(other.rel).stem == stem and rx.search(other.code):
                return names[-1]
        return ""

    @staticmethod
    def _enclosing_call(code: str, at: int) -> tuple[str, str, int, str] | None:
        """The function a lambda at `at` is an argument of, the receiver it is called on (Member.AddLambda -> Member),
        the offset of the call's '(' and the first name of the receiver chain (Query.Delegate.BindLambda -> Query)."""
        depth = 0
        k = at - 1
        while k >= 0:
            c = code[k]
            if c == ")":
                depth += 1
            elif c == "(":
                if depth == 0:
                    head = code[max(0, k - 240):k]
                    m = re.search(rf"((?:{IDENT}\s*(?:\.|->)\s*)*)({IDENT})\s*(?:<[^<>;]*>)?\s*$", head)
                    if not m:
                        return None
                    chain = re.findall(IDENT, m.group(1))
                    receiver = chain[-1] if chain else ""
                    if not receiver:
                        am = re.search(rf"({IDENT})\s*=\s*[\w:]*\s*$", code[max(0, k - 240):k - len(m.group(2))])
                        receiver = am.group(1) if am else ""
                    return m.group(2), receiver, k, chain[0] if chain else ""
                depth -= 1
            elif c in ";{}" and depth == 0:
                return None
            k -= 1
        return None

    @staticmethod
    def _is_local(code: str, spans: list[FunctionSpan], at: int, name: str) -> str:
        """Where `name` is declared as a stack variable of the function around `at` (before `at`), or ''. A delegate on a
        stack variable dies with the function, so a lambda bound to it never runs after the function returns."""
        if not name:
            return ""
        span = next((s for s in spans if s.start < at < s.end), None)
        if span is None:
            return ""
        # a value declaration only: `FQuery Query;` lives on the stack, `FQuery& Query = ...` or `FQuery* Query` does not
        rx = re.compile(r"(?<![\.\w>:&*-])(?:const\s+)?[A-Z]\w*(?:\s*<[^;{}]*?>)?\s+" + re.escape(name) + r"\s*[;={(]")
        m = rx.search(code, span.start, at)
        return f"{span.cls}::{span.name}" if m else ""

    def _owner(self, src: SourceText, spans: list[FunctionSpan], at: int) -> str:
        best = None
        for t in self.index.all():
            if t.src is src and t.body_start < at < t.body_end:
                if best is None or t.body_start > best.body_start:
                    best = t
        if best:
            return best.name
        for s in spans:
            if s.start < at < s.end:
                return s.cls
        return ""

    def _member_names(self, owner: str) -> set[str]:
        """The data members (not methods, not names merely mentioned) of `owner` and its scanned bases."""
        names = set()
        for name in [owner] + self.index.ancestry(owner):
            for t in self.index.types.get(name, []):
                names |= {mem.name for mem in MemberReader(t).members()}
        return names

    def _uses_member(self, owner: str, body: str) -> str:
        used = set(re.findall(IDENT, body))
        own = self._fields_and_methods(owner)
        for name in sorted(used):
            if name in own or name in INHERITED_MEMBERS:
                return name
        return ""

    def _fields_and_methods(self, owner: str) -> set[str]:
        out = set()
        for t in [*self.index.types.get(owner, [])] + [d for b in self.index.ancestry(owner) for d in self.index.types.get(b, [])]:
            for mem in MemberReader(t).members():
                out.add(mem.name)
            for fm in re.finditer(rf"\b({IDENT})\s*\(", t.src.code[t.body_start:t.body_end]):
                out.add(fm.group(1))
        return out - {"if", "for", "while", "switch", "return", "sizeof", "UPROPERTY", "UFUNCTION", "GENERATED_BODY",
                      "check", "ensure", "TEXT"}


# ---------------------------------------------------------------------------------------------------------- void*

class UObjectVars:
    """Names declared as UObject pointers: members of scanned types, and locals and parameters of each file."""

    DECL = re.compile(rf"\b(?:const\s+)?(?:class\s+)?({IDENT})\s*\*\s*(?:const\s+)?({IDENT})\s*(?=[=;,)\[])")

    def __init__(self, index: TypeIndex):
        self.index = index
        self.members: dict[str, str] = {}
        for t in index.all():
            for m in MemberReader(t).members():
                for h in PointerType(m.type_text).holds():
                    if h.how in ("raw", "objectptr") and not h.via and index.uobject(h.target)[0]:
                        self.members.setdefault(m.name, f"{h.describe()} ({t.src.rel}:{m.line})")
        self._local: dict[str, dict[str, str]] = {}

    def lookup(self, src: SourceText, name: str) -> str:
        if not re.fullmatch(IDENT, name) or name in ("this", "nullptr"):
            return ""
        loc = self._local.get(src.rel)
        if loc is None:
            loc = {}
            for m in self.DECL.finditer(src.code):
                if self.index.uobject(m.group(1))[0]:
                    loc.setdefault(m.group(2), f"{m.group(1)}* ({src.rel}:{src.line_of(m.start())})")
            self._local[src.rel] = loc
        return loc.get(name) or self.members.get(name, "")


class VoidHideFinder:
    """A UObject pointer cast or assigned into a void* (UC-1.5): the collector cannot follow it, so whoever reads it back
    may get a deleted object."""

    CASTS = [
        re.compile(r"\bstatic_cast\s*<\s*(?:const\s+)?void\s*\*\s*>\s*\(\s*(this|\w+)\s*\)"),
        re.compile(r"\breinterpret_cast\s*<\s*(?:const\s+)?void\s*\*\s*>\s*\(\s*(this|\w+)\s*\)"),
        # the cast's whole operand must be the name: (void*)Obj->GetWorld() casts something else
        re.compile(r"\(\s*(?:const\s+)?void\s*\*\s*\)\s*(this|\w+)\b(?!\s*(?:->|\.|\(|\[|::))"),
    ]

    def __init__(self, index: TypeIndex, vars_: UObjectVars, captures: CaptureFinder):
        self.index = index
        self.vars = vars_
        self.captures = captures
        self.void_members: dict[str, str] = {}
        for t in index.all():
            for m in MemberReader(t).members():
                if re.fullmatch(r"(?:const\s+)?void\s*\*", m.type_text.strip()):
                    self.void_members[m.name] = f"{t.name}::{m.name} ({t.src.rel}:{m.line})"

    def scan(self, src: SourceText) -> list[Finding]:
        code = src.code
        spans = function_spans(src)
        out, seen = [], set()
        void_locals = {m.group(1): src.line_of(m.start()) for m in re.finditer(r"\bvoid\s*\*\s*(\w+)\s*[=;,)]", code)}
        hits = []
        for rx in self.CASTS:
            for m in rx.finditer(code):
                hits.append((m.start(), m.group(1), "a cast to void*"))
        for m in re.finditer(r"(?:\b|->|\.)(\w+)\s*=\s*(this|\w+)\s*;", code):
            lhs, rhs = m.group(1), m.group(2)
            if lhs in self.void_members or lhs in void_locals:
                hits.append((m.start(), rhs, f"an assignment to the void* {self.void_members.get(lhs, lhs)}"))
        for at, x, how in sorted(hits):
            line = src.line_of(at)
            if (line, x) in seen:
                continue
            if x == "this":
                owner = self.captures._owner(src, spans, at)
                ok, own_how = self.index.uobject(owner) if owner else (False, "")
                if not ok:
                    continue
                known = f"this inside {owner}; {own_how}"
            else:
                decl = self.vars.lookup(src, x)
                if not decl:
                    continue
                known = f"{x} is declared as {decl}"
            seen.add((line, x))
            out.append(Finding(
                rule="void-hides-uobject", severity="warning", title=f"{x} is stored as a void*",
                where={"file": src.rel, "line": line, "code": src.line_text(line)},
                why="A void* tells the garbage collector nothing. The object can be collected while this pointer still holds "
                    "its address, and whoever casts it back gets a deleted object.",
                fix="Keep a TWeakObjectPtr next to the void* and check it before casting back, or pass an index or "
                    "FObjectKey instead of the address.",
                how_known=f"READ: {how}; {known}", evidence={"value": x, "via": how}, suppressed=src.suppression(line)))
        return out


# ---------------------------------------------------------------------------------------------------------- the pass

class Scanner:
    """The static pass over one project or folder (UC-1). It never reports a clean project when it read nothing (1a)."""

    def __init__(self, root: Path):
        self.root = root.resolve()

    def roots(self) -> list[Path]:
        if any(self.root.glob("*.uproject")):
            out = [self.root / "Source"] if (self.root / "Source").is_dir() else []
            plugins = self.root / "Plugins"
            if plugins.is_dir():
                out += sorted(p for p in plugins.rglob("Source") if p.is_dir() and not (SKIP_DIRS & set(p.parts)))
            return out
        return [self.root]

    def files(self) -> list[Path]:
        seen, out = set(), []
        for r in self.roots():
            for p in sorted(r.rglob("*")):
                if p.suffix.lower() not in (".h", ".hpp", ".cpp", ".inl") or SKIP_DIRS & set(p.relative_to(r).parts):
                    continue
                if p.name.endswith((".generated.h", ".gen.cpp")):
                    continue
                rp = p.resolve()
                if rp not in seen:
                    seen.add(rp)
                    out.append(p)
        return out

    def run(self) -> FindingsFile:
        files = self.files()
        target = {"root": str(self.root), "roots": [str(r) for r in self.roots()]}
        if not files:
            ff = FindingsFile(kind="scan", target=target, checked={"files": 0}, blind_spots=BLIND_SPOTS, ok=False,
                              failure="nothing to scan: no C++ files under " + ", ".join(target["roots"] or [str(self.root)]))
            return ff
        sources = [SourceText.read(p, self.root) for p in files]
        index = TypeIndex(sources)
        reported = Reported(index)
        rules = MemberRules(index, reported)
        findings: list[Finding] = []
        n_members = 0
        for t in index.all():
            for m in MemberReader(t).members():
                n_members += 1
                findings.extend(rules.check(m))
        uvars = UObjectVars(index)
        caps = CaptureFinder(index, uvars)
        voids = VoidHideFinder(index, uvars, caps)
        for src in sources:
            findings.extend(caps.scan(src))
            findings.extend(voids.scan(src))
        findings.extend(cycles.SharedGraph(index).findings())
        findings.sort(key=lambda f: ({"error": 0, "warning": 1, "info": 2}[f.severity], f.where.get("file", ""),
                                     f.where.get("line", 0)))
        n_types = sum(len(v) for v in index.types.values())
        ff = FindingsFile(kind="scan", target=target, blind_spots=BLIND_SPOTS, findings=findings,
                          checked={"files": len(files), "types": n_types,
                                   "reflected_types": sum(1 for t in index.all() if t.macro),
                                   "members": n_members, "lines": sum(s.raw.count("\n") + 1 for s in sources),
                                   # read and judged safe, so not reported: counted, so a reader can ask about them
                                   "not_reported": {"rooted_by_the_code": len(rules.kept_by_root),
                                                    "lambda_handle_released": len(caps.released),
                                                    "lambda_on_a_stack_delegate": len(caps.on_stack)}})
        ff.number("SCAN")
        return ff


def scan_text(text: str, rel: str = "snippet.h") -> FindingsFile:
    """Scan one piece of C++ held in a string (tests and the manual's examples)."""
    src = SourceText.from_string(text, rel)
    index = TypeIndex([src])
    rules = MemberRules(index, Reported(index))
    findings = [f for t in index.all() for m in MemberReader(t).members() for f in rules.check(m)]
    uvars = UObjectVars(index)
    caps = CaptureFinder(index, uvars)
    findings += caps.scan(src) + VoidHideFinder(index, uvars, caps).scan(src)
    findings += cycles.SharedGraph(index).findings()
    ff = FindingsFile(kind="scan", target={"root": rel}, checked={"files": 1}, blind_spots=BLIND_SPOTS, findings=findings)
    ff.number("SCAN")
    return ff
