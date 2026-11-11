"""The command line (UC-5). Exit codes: 0 no finding at or above --fail-on, 1 there is one, 2 the detector could not run.

    ms.py scan <project or folder> [-o file] [--json] [--fail-on error|warning|info]
    ms.py leaktest --project P.uproject --map /Game/Maps/X [--seconds 5] [--return /Engine/Maps/Entry] [--engine DIR]
    ms.py heap --project P.uproject [--map M] [--seconds 20] [--sample-every 64] [--suspect 1234] [--guard-every N]
    ms.py report <findings dir or files...> -o report.html
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from . import findings as F
from .scanner import Scanner

ORDER = {"error": 0, "warning": 1, "info": 2}


def _exit_code(datas: list[dict], fail_on: str) -> int:
    if not datas or any(not d.get("ok", True) for d in datas):
        return 2
    limit = ORDER[fail_on]
    bad = [f for d in datas for f in d["findings"] if not f.get("suppressed") and ORDER[f["severity"]] <= limit]
    return 1 if bad else 0


def _print_summary(data: dict, out: Path | None) -> None:
    c = data["counts"]
    if not data.get("ok", True):
        print(f"FAILED: {data.get('failure')}")
    print(f"{data['kind']}: checked {json.dumps(data['checked'])}")
    print(f"  {c['error']} error, {c['warning']} warning, {c['info']} info, {c['suppressed']} suppressed")
    for f in data["findings"]:
        w = f["where"]
        loc = f"{w['file']}:{w['line']}" if "file" in w else w.get("object", "")
        mark = "  (suppressed: " + f["suppressed"] + ")" if f.get("suppressed") else ""
        print(f"  {f['severity']:7} {f['id']}  {loc}  {f['title']}{mark}")
    if out:
        print(f"wrote {out}")


def cmd_scan(a) -> int:
    ff = Scanner(Path(a.path)).run()
    data = ff.to_json()
    out = Path(a.out) if a.out else None
    if out is None and any(Path(a.path).glob("*.uproject")):
        out = Path(a.path) / "Saved" / "MemorySentinel" / "scan.json"
    if out:
        ff.write(out)
    if a.json:
        print(json.dumps(data, indent=1))
    else:
        _print_summary(data, out)
    return _exit_code([data], a.fail_on)


def _run_result(result, fail_on: str) -> int:
    from .unreal import RunResult  # noqa: F401  (documents the type)
    print(f"run: exit code {result.exit_code}, {result.seconds:.1f} s, log {result.log}")
    if not result.files:
        print("FAILED: the run wrote no findings file")
        return 2
    datas = []
    for p in result.files:
        d = F.load(p)
        datas.append(d)
        _print_summary(d, p)
    return _exit_code(datas, fail_on)


def cmd_leaktest(a) -> int:
    from . import unreal
    uproject = Path(a.project)
    engine = unreal.find_engine(uproject, a.engine)
    r = unreal.leaktest(uproject, engine, a.map, a.seconds, a.return_map, a.args.split() if a.args else None, a.timeout)
    return _run_result(r, a.fail_on)


def cmd_heap(a) -> int:
    from . import unreal
    uproject = Path(a.project)
    engine = unreal.find_engine(uproject, a.engine)
    r = unreal.heap(uproject, engine, a.map, a.seconds, a.sample_every, a.suspect, a.guard_every, a.guard_size,
                    a.args.split() if a.args else None, a.timeout)
    return _run_result(r, a.fail_on)


def cmd_report(a) -> int:
    from .report import Report
    paths: list[Path] = []
    for p in a.inputs:
        p = Path(p)
        paths += sorted(p.glob("*.json")) if p.is_dir() else [p]
    rep = Report.from_files(paths)
    out = Path(a.out)
    rep.write_html(out)
    print(f"report: {len(rep.files)} findings files, {len(rep.findings)} findings, {len(rep.problems)} unreadable -> {out}")
    for why in rep.problems:
        print(f"  could not read: {why}")
    return 2 if not rep.files else 0


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="ms.py", description="Memory Sentinel: find Unreal memory bugs.")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("scan", help="the static pass over a project's (or a folder's) C++ (UC-1)")
    s.add_argument("path", help="an Unreal project folder (with a .uproject) or any folder of C++")
    s.add_argument("-o", "--out", help="findings file to write (default: <project>/Saved/MemorySentinel/scan.json)")
    s.add_argument("--json", action="store_true", help="print the findings file instead of a summary")
    s.add_argument("--fail-on", choices=list(ORDER), default="error")
    s.set_defaults(fn=cmd_scan)

    for name, fn, helptext in (("leaktest", cmd_leaktest, "the level leak test, headless (UC-2)"),
                               ("heap", cmd_heap, "a headless run with the heap sampler on (UC-3)")):
        r = sub.add_parser(name, help=helptext)
        r.add_argument("--project", required=True, help="the .uproject file")
        r.add_argument("--engine", help="the engine folder (default: the project's engine from the Epic launcher)")
        r.add_argument("--map", default="/Engine/Maps/Entry" if name == "heap" else None, required=name == "leaktest")
        r.add_argument("--seconds", type=float, default=5.0 if name == "leaktest" else 20.0)
        r.add_argument("--timeout", type=float, default=300.0)
        r.add_argument("--args", help="more command line arguments for the game, in one string")
        r.add_argument("--fail-on", choices=list(ORDER), default="error")
        if name == "leaktest":
            r.add_argument("--return", dest="return_map", default="/Engine/Maps/Entry")
        else:
            r.add_argument("--sample-every", type=int, default=256)
            r.add_argument("--suspect", type=int, action="append", help="a block size to sample and watch first (repeat)")
            r.add_argument("--guard-every", type=int, default=0)
            r.add_argument("--guard-size", type=int, action="append", help="a block size to put against a guard page")
        r.set_defaults(fn=fn)

    rp = sub.add_parser("report", help="merge findings files into one HTML visual report (UC-4.3)")
    rp.add_argument("inputs", nargs="+", help="findings files, or folders of them")
    rp.add_argument("-o", "--out", required=True)
    rp.set_defaults(fn=cmd_report)

    a = p.parse_args(argv)
    return a.fn(a)
