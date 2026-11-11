"""The visual report (UC-4.3, UC-5.3): every findings file merged into one HTML page that needs no server.

The page is viewer.html with viewer.css and viewer.js inlined and the merged data embedded, so it can be attached to a
build or mailed. The same viewer.js draws the sample report on the public manual page.
"""
from __future__ import annotations

import datetime as _dt
import json
from dataclasses import dataclass, field
from pathlib import Path

from . import findings as F

VIEWER = Path(__file__).resolve().parent / "viewer"


@dataclass
class Report:
    files: list[dict] = field(default_factory=list)
    findings: list[dict] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    @classmethod
    def from_files(cls, paths: list[Path]) -> "Report":
        rep = cls()
        for p in paths:
            try:
                data = F.load(p)
            except (ValueError, json.JSONDecodeError, OSError) as e:
                rep.problems.append(f"{p}: {e}")
                continue
            index = len(rep.files)
            rep.files.append({
                "name": p.name, "kind": data["kind"], "made": data.get("made", ""), "ok": data.get("ok", True),
                "failure": data.get("failure", ""), "target": data.get("target", {}), "checked": data.get("checked", {}),
                "counts": data.get("counts", {}), "blind_spots": data.get("blind_spots", []),
            })
            for f in data["findings"]:
                rep.findings.append({**f, "file": index})
        return rep

    def data(self) -> dict:
        return {
            "made": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat(),
            "problems": F.PROBLEMS,
            "rules": {k: {"problem": v[0], "title": v[1]} for k, v in F.RULES.items()},
            "files": self.files,
            "findings": self.findings,
            "unreadable": self.problems,
        }

    def write_html(self, out: Path) -> Path:
        html = (VIEWER / "viewer.html").read_text(encoding="utf-8")
        css = (VIEWER / "viewer.css").read_text(encoding="utf-8")
        js = (VIEWER / "viewer.js").read_text(encoding="utf-8")
        data = json.dumps(self.data()).replace("</", "<\\/")
        html = (html.replace("<!--CSS-->", "<style>\n" + css + "\n</style>")
                    .replace("<!--DATA-->", "<script>window.MEMSENTINEL = " + data + ";</script>")
                    .replace("<!--JS-->", "<script>\n" + js.replace("</script", "<\\/script") + "\n</script>"))
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(html, encoding="utf-8")
        return out

    def write_json(self, out: Path) -> Path:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(self.data(), indent=1), encoding="utf-8")
        return out
