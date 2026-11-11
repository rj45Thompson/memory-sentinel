"""C++ source text the scanner can search without being fooled by comments or strings (UC-1.1).

`SourceText` keeps every character position and every line break of the file, but turns the inside of comments, string
literals, character literals and preprocessor lines into spaces. A regex over `code` then never matches a pointer that
only appears in a comment or a log message, and `line_of(offset)` still names the line a human sees. The comment text is
kept per line, because a suppression (`// memsentinel: ignore <reason>`) lives there.
"""
from __future__ import annotations

import bisect
import re
from dataclasses import dataclass, field
from pathlib import Path

SUPPRESS = re.compile(r"memsentinel\s*:\s*ignore\b\s*(.*)", re.I)


@dataclass
class SourceText:
    path: Path
    rel: str
    raw: str
    code: str = ""
    comments: dict[int, str] = field(default_factory=dict)
    _starts: list[int] = field(default_factory=list)

    @classmethod
    def read(cls, path: Path, root: Path) -> "SourceText":
        raw = path.read_text(encoding="utf-8-sig", errors="replace").replace("\r\n", "\n").replace("\r", "\n")
        rel = path.relative_to(root).as_posix() if root in path.parents or path == root else path.as_posix()
        src = cls(path=path, rel=rel, raw=raw)
        src._blank()
        return src

    @classmethod
    def from_string(cls, text: str, rel: str = "snippet.h") -> "SourceText":
        src = cls(path=Path(rel), rel=rel, raw=text.replace("\r\n", "\n"))
        src._blank()
        return src

    # ------------------------------------------------------------------ blanking
    def _blank(self) -> None:
        s = self.raw
        out = list(s)
        n = len(s)
        i = 0
        line = 1
        at_line_start = True

        def blank(a: int, b: int) -> None:
            for k in range(a, b):
                if out[k] != "\n":
                    out[k] = " "

        while i < n:
            c = s[i]
            if c == "\n":
                line += 1
                at_line_start = True
                i += 1
                continue
            if c in " \t":
                i += 1
                continue
            if at_line_start and c == "#":
                # A preprocessor line, with its backslash continuations. Its comment part is still a comment.
                j = i
                while j < n:
                    if s[j] == "\n" and s[j - 1] != "\\":
                        break
                    if s.startswith("//", j):
                        k = s.find("\n", j)
                        k = n if k < 0 else k
                        self._note(line + s.count("\n", i, j), s[j + 2:k])
                        j = k
                        break
                    j += 1
                line += s.count("\n", i, j)
                blank(i, j)
                i = j
                at_line_start = False
                continue
            at_line_start = False
            if s.startswith("//", i):
                j = s.find("\n", i)
                j = n if j < 0 else j
                self._note(line, s[i + 2:j])
                blank(i, j)
                i = j
                continue
            if s.startswith("/*", i):
                j = s.find("*/", i + 2)
                j = n if j < 0 else j + 2
                body = s[i + 2:j - 2]
                for k, part in enumerate(body.split("\n")):
                    self._note(line + k, part)
                line += s.count("\n", i, j)
                blank(i, j)
                i = j
                continue
            m = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\n]{0,16})\(', s[i:i + 24])
            if m and (i == 0 or not (s[i - 1].isalnum() or s[i - 1] == "_")):
                end = s.find(")" + m.group(1) + '"', i + m.end())
                end = n if end < 0 else end + len(m.group(1)) + 2
                line += s.count("\n", i, end)
                blank(i + m.end(), end - len(m.group(1)) - 2)
                i = end
                continue
            if c in "\"'":
                if c == "'" and i > 0 and s[i - 1].isalnum() and i + 1 < n and s[i + 1].isalnum():
                    i += 1  # a digit separator, 1'000'000
                    continue
                j = i + 1
                while j < n and s[j] != c and s[j] != "\n":
                    j += 2 if s[j] == "\\" else 1
                blank(i + 1, min(j, n))
                i = j + 1
                continue
            i += 1
        self.code = "".join(out)
        self._starts = [0] + [m.end() for m in re.finditer("\n", self.code)]

    def _note(self, line: int, text: str) -> None:
        text = text.strip()
        if text:
            self.comments[line] = (self.comments.get(line, "") + " " + text).strip()

    # ------------------------------------------------------------------ positions
    def line_of(self, offset: int) -> int:
        return bisect.bisect_right(self._starts, offset)

    def line_text(self, line: int) -> str:
        lines = self.raw.split("\n")
        return lines[line - 1].strip() if 0 < line <= len(lines) else ""

    def suppression(self, line: int) -> str | None:
        """The reason given by `// memsentinel: ignore <reason>` on this line or the line above, else None."""
        for ln in (line, line - 1):
            m = SUPPRESS.search(self.comments.get(ln, ""))
            if m:
                return m.group(1).strip() or "(no reason given)"
        return None


def match_close(code: str, open_at: int) -> int:
    """Offset just past the bracket that closes the one at `open_at` ((, [ or {). Returns len(code) if it never closes."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    opener = code[open_at]
    closer = pairs[opener]
    depth = 0
    for k in range(open_at, len(code)):
        ch = code[k]
        if ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return k + 1
    return len(code)


def split_template_args(text: str) -> list[str]:
    """`A, TMap<B, C*>, D` -> ['A', 'TMap<B, C*>', 'D'] (top-level commas only)."""
    out, depth, cur = [], 0, []
    for ch in text:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def angle_body(text: str, lt_at: int) -> tuple[str, int]:
    """The text between `<` at lt_at and its matching `>`, and the offset just past the `>`."""
    depth = 0
    for k in range(lt_at, len(text)):
        if text[k] == "<":
            depth += 1
        elif text[k] == ">":
            depth -= 1
            if depth == 0:
                return text[lt_at + 1:k], k + 1
    return text[lt_at + 1:], len(text)
