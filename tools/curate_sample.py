"""Pick the newest findings file of each test-game scenario into site/sample/, the set the manual's report and the panel
screenshots show. Every file is a real run's output, copied byte for byte; nothing is edited or made up here.

    py tools/curate_sample.py [--to-project]   (--to-project also puts the set in the test game's Saved/MemorySentinel,
                                                 moving what was there into Saved/MemorySentinel/archive/)
"""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOUND = ROOT / "unreal" / "SentinelTest" / "Saved" / "MemorySentinel"
SAMPLE = ROOT / "site" / "sample"


def scenario(d: dict) -> str:
    cmd = d.get("target", {}).get("command_line", "") + " " + d.get("target", {}).get("command", "")
    for part in cmd.split():
        if part.startswith("-SentinelScenario="):
            return part.split("=", 1)[1]
    return ""


def main(argv):
    files = sorted(FOUND.glob("*.json"), key=lambda p: p.stat().st_mtime)
    pick = {}
    for p in files:
        d = json.loads(p.read_text(encoding="utf-8"))
        kind, n = d["kind"], len(d["findings"])
        if kind == "scan":
            key = "scan"
        elif kind == "leaktest":
            key = "leak" if n else "leakclean"
        elif kind == "heap":
            key = "heap-" + (scenario(d) or "unnamed")
        else:
            key = "run-crash"
        pick[key] = p          # newest wins
    want = ["scan", "leak", "leakclean", "heap-stomp", "heap-stompfree", "heap-uaf", "heap-doublefree", "heap-guard",
            "run-crash", "heap-heapclean"]
    missing = [k for k in want if k not in pick]
    if missing:
        print("missing runs:", ", ".join(missing))
        return 2
    if SAMPLE.exists():
        shutil.rmtree(SAMPLE)
    SAMPLE.mkdir(parents=True)
    for k in want:
        shutil.copy2(pick[k], SAMPLE / pick[k].name)
        print(f"{k:16} {pick[k].name}")
    if "--to-project" in argv:
        archive = FOUND / "archive"
        archive.mkdir(exist_ok=True)
        for p in FOUND.glob("*.json"):
            shutil.move(str(p), archive / p.name)
        for p in SAMPLE.glob("*.json"):
            shutil.copy2(p, FOUND / p.name)
        print(f"the test game's findings folder now holds the {len(want)} sample files; the rest are in {archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
