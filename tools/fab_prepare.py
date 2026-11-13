"""Make the plugin meet Fab's code plugin rules, idempotently, and build the .zip that is uploaded to Fab.

Fab Technical Requirements (4.3.6 and 4.3.7.3):
- 4.3.6.1.b  every source and header file starts with a commented copyright notice naming the publisher and the year;
- 4.3.6.a    the .uplugin has an "EngineVersion" key for the engine it is built for;
- 4.3.6.b    every module has a PlatformAllowList;
- 4.3.7.3.b  Config/FilterPlugin.ini lists the files outside Content, Resources and Source that ship (README.md, LICENSE);
- the plugin browser icon, Resources/Icon128.png.
Fab "Asset File Format and Structure Requirements", Unreal Engine Code Plugins and Python Scripts:
- a code plugin holds the .uplugin and the Config, Content, Resources and Source folders;
- a plugin's Python lives in Content/Python and nowhere else ("Do not place Python files outside of directories listed
  above"), so the plugin's command line and its package are Content/Python/ms.py and Content/Python/memsentinel/, and the
  .uplugin says CanContainContent.

The "FabURL" key (4.3.6.c) needs the product ID the publisher portal gives a draft listing, so it is set with --fab-id once
that draft exists, never guessed.

    py tools/fab_prepare.py --publisher "RJ Thompson" --engine 5.7.0 [--fab-id <id from the portal URL>] [--check]
    py tools/fab_prepare.py --publisher "RJ Thompson" --engine 5.7.0 --check --zip <out.zip> --gallery <folder>

--gallery writes the listing's images from site/shots (Fab: at least 1920x1080, under 3 MB each, under 25 MB in all); it
refuses a smaller capture instead of upscaling it.

--zip writes the upload: one top folder, MemorySentinel/, holding the plugin exactly as the public downloads carry it
(publish_site.PublicSet: no build outputs, the licence and readme beside the .uplugin), passed through the privacy gate
before it is written and read back through it after. It refuses while any rule above is not met. Fab then builds the zip
itself for each engine version picked; do the same before uploading, on the unzipped copy (4.3.6.2, from PowerShell):

    RunUAT.bat BuildPlugin -Plugin=<unzipped>\\MemorySentinel\\MemorySentinel.uplugin -Package=<out> -Rocket -TargetPlatforms=Win64
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / "unreal" / "MemorySentinel"
YEAR = 2026
NOTICE = re.compile(r"^// Copyright \(c\) \d{4} .+\. Released under the MIT License; see LICENSE\.\r?\n")
FILTER = "[FilterPlugin]\n; Files outside Content, Resources and Source that ship with the plugin (Fab 4.3.7.3.b).\n/README.md\n/LICENSE\n"
FOLDERS = ("Config", "Content", "Resources", "Source")   # Fab: Unreal Engine Code Plugins, "must contain"
PYTHON_HOME = ("Content", "Python")                      # Fab: Python Scripts, "Plugin Python code: Content/Python"
BUILD_OUTPUTS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache"}


def sources() -> list[Path]:
    return sorted(p for p in (PLUGIN / "Source").rglob("*") if p.suffix in (".h", ".cpp", ".cs"))


def misplaced_python(paths: list[tuple[str, ...]]) -> list[str]:
    """Every .py whose path (as parts below the plugin folder) is not under Content/Python."""
    return ["/".join(p) for p in paths if p[-1].endswith(".py") and tuple(p[:2]) != PYTHON_HOME]


def plugin_paths() -> list[tuple[str, ...]]:
    return [p.relative_to(PLUGIN).parts for p in PLUGIN.rglob("*")
            if p.is_file() and not BUILD_OUTPUTS & set(p.relative_to(PLUGIN).parts)]


def notice_line(publisher: str) -> str:
    return f"// Copyright (c) {YEAR} {publisher}. Released under the MIT License; see LICENSE.\n"


def problems(publisher: str, engine: str, fab_id: str | None) -> list[str]:
    out = []
    for p in sources():
        head = p.read_text(encoding="utf-8-sig").split("\n", 1)[0] + "\n"
        if head != notice_line(publisher):
            out.append(f"{p.relative_to(PLUGIN)}: no copyright notice for {publisher} on its first line")
    desc = json.loads((PLUGIN / "MemorySentinel.uplugin").read_text(encoding="utf-8"))
    if desc.get("EngineVersion") != engine:
        out.append(f"MemorySentinel.uplugin: EngineVersion is {desc.get('EngineVersion')!r}, not {engine!r}")
    for m in desc["Modules"]:
        if not m.get("PlatformAllowList") and not m.get("PlatformDenyList"):
            out.append(f"MemorySentinel.uplugin: module {m['Name']} has no PlatformAllowList")
    if fab_id and desc.get("FabURL") != f"com.epicgames.launcher://ue/Fab/product/{fab_id}":
        out.append("MemorySentinel.uplugin: FabURL does not name the portal's product ID")
    if (PLUGIN / "Config" / "FilterPlugin.ini").read_text(encoding="utf-8") != FILTER if (PLUGIN / "Config" / "FilterPlugin.ini").exists() else True:
        out.append("Config/FilterPlugin.ini is missing or differs")
    if not (PLUGIN / "Resources" / "Icon128.png").exists():
        out.append("Resources/Icon128.png is missing")
    for d in FOLDERS:
        if not (PLUGIN / d).is_dir():
            out.append(f"{d}/ is missing; a Fab code plugin holds {', '.join(FOLDERS)}")
    for p in misplaced_python(plugin_paths()):
        out.append(f"{p}: Python outside Content/Python, which Fab refuses")
    if (PLUGIN / "Content").is_dir() and desc.get("CanContainContent") is not True:
        out.append("MemorySentinel.uplugin: CanContainContent is not true, but the plugin has a Content folder")
    return out


GALLERY_MIN = (1920, 1080)          # Fab: Media Gallery Images, minimum size
GALLERY_MAX_BYTES = 3 * 1024 * 1024  # each image "less than 3MB"
GALLERY_MAX_TOTAL = 25 * 1024 * 1024 # "less than 25MB in total"


def gallery(shots: Path, out_dir: Path) -> list[tuple[str, tuple[int, int], int]]:
    """The listing's media gallery: every panel screenshot (site/shots/*.png, real captures of the panel in Unreal) as a
    JPEG that meets Fab's image rules. Refuses (SystemExit 2) rather than upscaling a smaller capture."""
    from PIL import Image
    pngs = sorted(shots.glob("*.png"))
    if not pngs:
        print(f"REFUSED: no screenshots in {shots}")
        raise SystemExit(2)
    sizes = {p: Image.open(p).size for p in pngs}
    small = [f"{p.name} is {w}x{h}" for p, (w, h) in sizes.items() if w < GALLERY_MIN[0] or h < GALLERY_MIN[1]]
    if small:
        print(f"REFUSED: Fab's gallery needs at least {GALLERY_MIN}; retake, never upscale: " + "; ".join(small))
        raise SystemExit(2)
    out_dir.mkdir(parents=True, exist_ok=True)
    made = []
    for p in pngs:
        jpg = out_dir / (p.stem + ".jpg")
        Image.open(p).convert("RGB").save(jpg, "JPEG", quality=92, optimize=True)
        made.append((jpg.name, Image.open(jpg).size, jpg.stat().st_size))
    big = [f"{n} is {b // 1024} KB" for n, _, b in made if b >= GALLERY_MAX_BYTES]
    if big or sum(b for *_, b in made) >= GALLERY_MAX_TOTAL:
        print("REFUSED: over Fab's size limits: " + "; ".join(big or ["the total is 25 MB or more"]))
        raise SystemExit(2)
    return made


def fab_zip(out_path: Path) -> int:
    """Write the Fab upload; returns the number of files in it. Refuses (SystemExit 2) on anything Fab would refuse."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from publish_site import PrivacyGate, PublicSet, Refused, write_zip
    try:
        gate = PrivacyGate()
        src = PublicSet().plugin_entries()
        prefix = "Plugins/MemorySentinel/"
        if not all(n.startswith(prefix) for n in src):
            raise Refused("the plugin entries are not all under Plugins/MemorySentinel/")
        entries = {"MemorySentinel/" + n[len(prefix):]: b for n, b in src.items()}
        parts = [tuple(n.split("/")[1:]) for n in entries]
        bad = [f"{'/'.join(p)}: build output" for p in parts if BUILD_OUTPUTS & set(p)]
        bad += [f"{p}: Python outside Content/Python" for p in misplaced_python(parts)]
        bad += [f"{d}/ is not in the zip" for d in FOLDERS if not any(p[0] == d for p in parts if len(p) > 1)]
        if sum(1 for p in parts if p[-1].endswith(".uplugin")) != 1:
            bad.append("the zip does not hold exactly one .uplugin")
        if bad:
            raise Refused("the Fab zip would break Fab's rules:\n  " + "\n  ".join(bad))
        gate.check(entries, out_path.name)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        write_zip(out_path, entries)
        return gate.check_zip(out_path)
    except Refused as e:
        print(f"REFUSED: {e}")
        raise SystemExit(2)


def read_as_is(p: Path) -> tuple[str, str]:
    """The file's text with its own line endings untouched, and that ending (CRLF or LF). Reading with Python's newline
    translation and writing back would turn every CRLF file into LF, which this tool did once to 19 files."""
    with open(p, encoding="utf-8-sig", newline="") as fh:
        text = fh.read()
    return text, "\r\n" if "\r\n" in text else "\n"


def apply(publisher: str, engine: str, fab_id: str | None) -> None:
    for p in sources():
        text, eol = read_as_is(p)
        text = NOTICE.sub("", text, count=1)
        p.write_text(notice_line(publisher).rstrip("\n") + eol + text, encoding="utf-8", newline="")
    up = PLUGIN / "MemorySentinel.uplugin"
    raw, eol = read_as_is(up)
    desc = json.loads(raw)
    # keep the file's own layout: change or add single keys after VersionName
    def put(key: str, value: str) -> None:
        nonlocal raw
        if key in desc:
            raw = re.sub(rf'("{key}"\s*:\s*)"[^"]*"', rf'\g<1>"{value}"', raw, count=1)
        else:
            raw = re.sub(r'(\t"VersionName": "[^"]*",\r?\n)', lambda m: m.group(1) + f'\t"{key}": "{value}",' + eol, raw,
                         count=1)
        desc[key] = value
    put("EngineVersion", engine)
    put("SupportURL", "https://github.com/rj45Thompson/memory-sentinel/issues")
    if fab_id:
        put("FabURL", f"com.epicgames.launcher://ue/Fab/product/{fab_id}")
    json.loads(raw)  # still valid JSON
    up.write_text(raw, encoding="utf-8", newline="")
    (PLUGIN / "Config").mkdir(exist_ok=True)
    (PLUGIN / "Config" / "FilterPlugin.ini").write_text(FILTER, encoding="utf-8", newline="")
    icon = PLUGIN / "Resources" / "Icon128.png"
    if not icon.exists():
        draw_icon(icon)


def draw_icon(path: Path) -> None:
    """A dark tile, a ring for the heap, four watch points for DR0-DR3, one of them caught (red)."""
    from PIL import Image, ImageDraw
    s = 512
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((16, 16, s - 16, s - 16), radius=96, fill=(14, 17, 22, 255), outline=(42, 51, 64, 255), width=8)
    d.ellipse((104, 104, s - 104, s - 104), outline=(92, 200, 255, 255), width=28)
    for k, (x, y) in enumerate(((256, 104), (408, 256), (256, 408), (104, 256))):
        col = (255, 92, 92, 255) if k == 1 else (61, 214, 140, 255)
        d.ellipse((x - 34, y - 34, x + 34, y + 34), fill=col, outline=(14, 17, 22, 255), width=10)
    d.rectangle((236, 196, 276, 316), fill=(231, 234, 238, 255))
    d.ellipse((232, 332, 280, 380), fill=(231, 234, 238, 255))
    path.parent.mkdir(exist_ok=True)
    img.resize((128, 128), Image.LANCZOS).save(path)


def main(argv: list[str]) -> int:
    a = argparse.ArgumentParser()
    a.add_argument("--publisher", required=True)
    a.add_argument("--engine", required=True, help="e.g. 5.7.0")
    a.add_argument("--fab-id")
    a.add_argument("--check", action="store_true", help="change nothing; exit 1 when a rule is not met")
    a.add_argument("--zip", type=Path, help="then write the Fab upload .zip here (only when every rule is met)")
    a.add_argument("--gallery", type=Path, help="then write the listing's gallery JPEGs (from site/shots) into this folder")
    args = a.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+\.0", args.engine):
        print(f"REFUSED: --engine {args.engine!r} is not a major version like 5.7.0 (Fab 4.3.6.a)")
        return 2
    if not args.check:
        apply(args.publisher, args.engine, args.fab_id)
    left = problems(args.publisher, args.engine, args.fab_id)
    for p in left:
        print("NOT MET:", p)
    print(f"{len(sources())} source files, {len(left)} rule(s) not met")
    if left:
        return 1
    if args.zip:
        n = fab_zip(args.zip)
        print(f"wrote {args.zip} ({n} files, {args.zip.stat().st_size // 1024} KB), read back through the privacy gate")
    if args.gallery:
        for name, size, nbytes in gallery(ROOT / "site" / "shots", args.gallery):
            print(f"gallery {name}: {size[0]}x{size[1]}, {nbytes // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
