"""Publish Memory Sentinel: the public manual page's files, the three downloads, and the public repository, from their
single sources.

    py tools/publish_site.py --site <anchor-demo checkout> --repo <public repository folder>

Into <site>/memsentinel/:
- viewer.js and viewer.css from the plugin (the same files the plugin's own visual report inlines),
- sample.js: the merged report of site/sample/ (real findings files from the test game, unedited but for this machine's
  folders, which are shortened),
- the panel screenshots as JPEG, and the front page's thumbnail (<site>/thumbs/memsentinel.jpg),
- MemorySentinel-UE5.7-v<V>.zip: the plugin, as Plugins/MemorySentinel (source, no build outputs),
- MemorySentinel-Install-UE5.7-v<V>.zip: the same plugin plus the one-click installer and the AI assistant prompt,
- memsentinel-source-v<V>.zip: everything public, the same files as the public repository.
The two plugin zips carry the README as ZipReadme makes it (it names the zip's own Plugins/MemorySentinel).
The page itself (<site>/memsentinel/index.html) is written by hand and is not changed here, but it is read through the
privacy gate with everything else, so a page that names a private word or a machine folder stops the publish.

Into <repo>: the public repository's working tree, exactly PublicSet plus its .gitignore and .gitattributes. A new folder
is made a git repository; nothing is committed or pushed here.

Nothing is written until PrivacyGate has passed every byte that goes out, and every zip is read back through the gate
before it replaces the published one. spec/ never goes out: its notes are private working notes.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unreal" / "MemorySentinel" / "Content" / "Python"))

from memsentinel.report import VIEWER, Report  # noqa: E402
from memsentinel.unreal import EngineNotFound, find_engine  # noqa: E402

VERSION = "1.0"
ENGINE = "5.7"
SKIP_PARTS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", "__pycache__", ".git", ".vs", "archive", "runs"}
PLUGIN_ZIP = f"MemorySentinel-UE{ENGINE}-v{VERSION}.zip"
INSTALL_ZIP = f"MemorySentinel-Install-UE{ENGINE}-v{VERSION}.zip"
SOURCE_ZIP = f"memsentinel-source-v{VERSION}.zip"


class Refused(Exception):
    """Publishing stopped before anything wrong went out; the message says what and where."""


class PublicSet:
    """The curated public files: ONE list, read by every public output (the public repository, the source zip, and, for its
    plugin part, the plugin and install zips), so the downloads and the repository can never drift apart.

    Breadcrumb: the private working copy is this repository; spec/ (the idea notes and the spec check), site/ (the sample
    findings and screenshots the page is built from) and unreal/test/ (build logs) stay private."""

    TREES = ("unreal/MemorySentinel", "unreal/SentinelTest/Source", "unreal/SentinelTest/Config", "tests", "tools")
    FILES = ("LICENSE", "README.md", "USE_CASES.md", "AI_INSTALL.md", "Install-MemorySentinel.cmd",
             "Install-MemorySentinel.ps1", "unreal/SentinelTest/SentinelTest.uproject")
    PLUGIN = "unreal/MemorySentinel"
    INSTALLER = ("Install-MemorySentinel.cmd", "Install-MemorySentinel.ps1", "AI_INSTALL.md", "README.md", "LICENSE")

    @staticmethod
    def wanted(rel: Path) -> bool:
        if SKIP_PARTS & set(rel.parts) or rel.suffix in (".log", ".pyc"):
            return False
        return not (rel.suffix == ".json" and "Saved" in rel.parts)

    def tree(self, top: str) -> list[str]:
        base = ROOT / top
        out = []
        for p in sorted(base.rglob("*")):
            rel = p.relative_to(ROOT)
            if p.is_dir() or p.is_symlink() or not self.wanted(rel):
                continue
            out.append(rel.as_posix())
        if not out:
            raise Refused(f"the public tree {top} is empty or missing")
        return out

    def files(self) -> list[str]:
        """Every public file, as a path relative to the repository root (posix separators)."""
        out = [f for t in self.TREES for f in self.tree(t)]
        for f in self.FILES:
            if not (ROOT / f).is_file():
                raise Refused(f"the public file {f} is missing")
            out.append(f)
        return sorted(set(out))

    def plugin_entries(self) -> dict[str, bytes]:
        """The plugin as it is unzipped into a project: Plugins/MemorySentinel/..., with the licence and readme beside it."""
        entries = {"Plugins/" + f[len("unreal/"):]: (ROOT / f).read_bytes() for f in self.tree(self.PLUGIN)}
        for extra in ("LICENSE", "README.md"):
            entries[f"Plugins/MemorySentinel/{extra}"] = self.zip_file(extra)
        return entries

    @staticmethod
    def zip_file(rel: str) -> bytes:
        """A top-level public file as it goes into the plugin and install zips: README.md as ZipReadme makes it, the rest as is."""
        data = (ROOT / rel).read_bytes()
        return ZipReadme.from_repo(data) if rel == "README.md" else data


class ZipReadme:
    """The README inside the plugin and install zips. The repository's README is written for the repository, where the
    plugin is unreal/MemorySentinel; both zips hold it at Plugins/MemorySentinel, so the "Install by hand" step must name
    that folder instead (the source zip is the repository's layout and keeps the repository's README).

    Only that one step changes, and it must be found exactly once: a README whose step was reworded stops the publish
    rather than shipping a zip whose README points at a folder the zip does not hold."""

    REPO_STEP = b"Copy `unreal/MemorySentinel` into your project's `Plugins/` folder"
    ZIP_STEP = b"Copy this zip's `Plugins/MemorySentinel` folder into your project's `Plugins/` folder"

    @classmethod
    def from_repo(cls, data: bytes) -> bytes:
        n = data.count(cls.REPO_STEP)
        if n != 1:
            raise Refused(f"README.md has the by-hand step {cls.REPO_STEP.decode()!r} {n} times, not once, so the zips' "
                          "README cannot name the zip's own Plugins/MemorySentinel; update ZipReadme to the new wording")
        return data.replace(cls.REPO_STEP, cls.ZIP_STEP)


class PrivacyGate:
    """Refuses any public byte that names a private word or this machine's folders. The word list is spec/public_refuse.txt
    (private, so it never leaks itself); this account's name and home folder and this checkout's folder are added at run
    time. A plain word matches as a whole word, anything else anywhere; case never matters. Binary files are read too."""

    LIST = ROOT / "spec" / "public_refuse.txt"

    def __init__(self):
        if not self.LIST.is_file():
            raise Refused(f"the private word list {self.LIST} is missing, so nothing can be checked; it is not published")
        words = [w.strip() for w in self.LIST.read_text(encoding="utf-8").splitlines()]
        words = [w for w in words if w and not w.startswith("#")]
        home = Path.home()
        for p in (home, ROOT):
            words += [str(p), str(p).replace("\\", "/"), str(p).replace("\\", "\\\\")]
        words.append(home.name)
        self.patterns = []
        for w in sorted(set(words)):
            rx = r"\b" + re.escape(w) + r"\b" if re.fullmatch(r"\w+", w) else re.escape(w)
            self.patterns.append((w, re.compile(rx.encode("latin-1"), re.IGNORECASE)))

    def hits(self, name: str, data: bytes) -> list[str]:
        out = []
        for word, rx in self.patterns:
            for m in rx.finditer(data):
                line = data.count(b"\n", 0, m.start()) + 1
                out.append(f"{name}:{line}: {word!r}")
        return out

    def check(self, items: dict[str, bytes], what: str) -> None:
        bad = [h for name, data in sorted(items.items()) for h in self.hits(name, data) + self.hits(name + " (path)", name.encode())]
        if bad:
            raise Refused(f"{what} names private words or this machine's folders:\n  " + "\n  ".join(bad))

    def check_zip(self, path: Path) -> int:
        with zipfile.ZipFile(path) as z:
            items = {f"{path.name}!{n}": z.read(n) for n in z.namelist()}
        self.check(items, path.name)
        return len(items)

    def check_tree(self, folder: Path) -> int:
        items = {p.relative_to(folder).as_posix(): p.read_bytes() for p in folder.rglob("*")
                 if p.is_file() and ".git" not in p.relative_to(folder).parts}
        self.check(items, str(folder.name) + " (public repository tree)")
        return len(items)


def write_zip(out: Path, entries: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(entries):
            z.writestr(name, entries[name])
    return out


class PublicRepo:
    """The public repository's working tree: exactly PublicSet plus a .gitignore and a .gitattributes for the public
    layout. A file that left the set is deleted; .git is kept. It refuses a non-empty folder that is not an earlier public
    tree (MARK missing), and any folder inside this checkout or holding it, so it can never sweep somebody else's files."""

    MARK = "unreal/MemorySentinel/MemorySentinel.uplugin"
    GITIGNORE = (
        "# build outputs: Unreal writes these beside the project and the plugin\n"
        "Binaries/\nIntermediate/\nSaved/\nDerivedDataCache/\n.vs/\n__pycache__/\n*.pyc\n"
        "# the test game's Plugins folder is filled by running the installer on it (see README)\n"
        "unreal/SentinelTest/Plugins/\n"
        "# the installer's backups and .uproject copies, if you run it on the test game\n"
        "*.backup-*/\n*.uproject.bak\n"
    )
    GITATTRIBUTES = (
        "* text=auto\n"
        "# Windows scripts keep Windows line endings on every machine\n"
        "*.cmd text eol=crlf\n*.bat text eol=crlf\n*.ps1 text eol=crlf\n"
        "*.png binary\n*.jpg binary\n*.zip binary\n"
    )

    def __init__(self, folder: Path):
        self.folder = folder.resolve()
        if self.folder == ROOT or ROOT in self.folder.parents or self.folder in ROOT.parents:
            raise Refused(f"the public repository folder {self.folder} overlaps this checkout")
        if self.folder.is_dir() and any(p.name != ".git" for p in self.folder.iterdir()) and not (self.folder / self.MARK).is_file():
            raise Refused(f"{self.folder} has files in it but is not an earlier public tree (no {self.MARK}); pick an empty folder")

    def entries(self, public: PublicSet) -> dict[str, bytes]:
        items = {f: (ROOT / f).read_bytes() for f in public.files()}
        items[".gitignore"] = self.GITIGNORE.encode()
        items[".gitattributes"] = self.GITATTRIBUTES.encode()
        return items

    def write(self, items: dict[str, bytes]) -> dict[str, int]:
        self.folder.mkdir(parents=True, exist_ok=True)
        counts = {"added": 0, "changed": 0, "removed": 0, "same": 0}
        for p in sorted(self.folder.rglob("*"), reverse=True):
            rel = p.relative_to(self.folder)
            if ".git" in rel.parts and rel.parts[0] == ".git":
                continue
            if p.is_file() and rel.as_posix() not in items:
                p.unlink()
                counts["removed"] += 1
            elif p.is_dir() and not any(p.iterdir()):
                p.rmdir()
        for rel, data in sorted(items.items()):
            dst = self.folder / rel
            if dst.is_file():
                if dst.read_bytes() == data:
                    counts["same"] += 1
                    continue
                counts["changed"] += 1
            else:
                counts["added"] += 1
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
        if not (self.folder / ".git").exists():
            subprocess.run(["git", "init", "-b", "main"], cwd=self.folder, check=True, stdout=subprocess.DEVNULL)
        return counts


def machine_prefixes() -> list[str]:
    """This machine's folders as a findings file writes them (JSON-escaped backslashes, or forward slashes), longest first:
    the test game's folder, this checkout, and the folder the engine is installed in (read from the Epic launcher's list
    for the test game's engine). Read at run time, so no machine path is written in this file."""
    engine_parent = find_engine(ROOT / "unreal" / "SentinelTest" / "SentinelTest.uproject").parent
    out = []
    for base in (ROOT / "unreal", ROOT, engine_parent):
        s = str(base)
        out += [s.replace("\\", "\\\\") + "\\\\", s.replace("\\", "/") + "/"]
    return out


def publish(site: Path, repo_dir: Path) -> str:
    out = site / "memsentinel"
    out.mkdir(parents=True, exist_ok=True)
    gate = PrivacyGate()
    public = PublicSet()

    # Everything that goes out is gated before anything is written.
    repo = PublicRepo(repo_dir)
    repo_items = repo.entries(public)
    plugin = public.plugin_entries()
    install = dict(plugin)
    for extra in PublicSet.INSTALLER:
        install[extra] = public.zip_file(extra)
    source = {f"memsentinel/{rel}": data for rel, data in repo_items.items() if not rel.startswith(".git")}
    page = out / "index.html"
    if not page.is_file():
        raise Refused(f"the page {page} is missing")
    for name, items in (("the public repository", repo_items), (PLUGIN_ZIP, plugin), (INSTALL_ZIP, install), (SOURCE_ZIP, source),
                        ("the page", {"memsentinel/index.html": page.read_bytes()})):
        gate.check(items, name)

    sample = sorted((ROOT / "site" / "sample").glob("*.json"))
    rep = Report.from_files(sample)
    if rep.problems or not rep.files:
        raise Refused(f"the sample set is unreadable: {rep.problems}")
    # the files are published as they were written, except this machine's folders, which are shortened (the page says so)
    data = json.dumps(rep.data())
    for prefix in machine_prefixes():
        data = re.sub(re.escape(prefix), "", data, flags=re.IGNORECASE)
    left = [w for w in (ROOT.name, Path.home().name, "Program Files") if w in data]
    if left:
        raise Refused(f"machine paths left in the sample: {left}")
    data = "window.MEMSENTINEL_SAMPLE = " + data.replace("</", "<\\/") + ";\n"
    gate.check({"sample.js": data.encode("utf-8")}, "sample.js")

    for name in ("viewer.js", "viewer.css"):
        (out / name).write_text((VIEWER / name).read_text(encoding="utf-8"), encoding="utf-8")
    (out / "sample.js").write_text(data, encoding="utf-8")

    from PIL import Image
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    for png in sorted((ROOT / "site" / "shots").glob("*.png")):
        Image.open(png).convert("RGB").save(shots / (png.stem + ".jpg"), "JPEG", quality=86, optimize=True)
    thumb = Image.open(ROOT / "site" / "shots" / "panel-leak.png").convert("RGB")
    w, h = thumb.size
    box = (int(w * 0.02), int(h * 0.03), int(w * 0.98), int(h * 0.03) + int(w * 0.96 * 400 / 640))
    thumb.crop(box).resize((640, 400), Image.LANCZOS).save(site / "thumbs" / "memsentinel.jpg", "JPEG", quality=86, optimize=True)

    # The zips are written beside, read back through the gate, and only then replace the published ones.
    sizes = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, items in ((PLUGIN_ZIP, plugin), (INSTALL_ZIP, install), (SOURCE_ZIP, source)):
            z = write_zip(Path(tmp) / name, items)
            if gate.check_zip(z) != len(items):
                raise Refused(f"{name} read back with a different number of files than were written")
            shutil.move(str(z), out / name)
            sizes[name] = ((out / name).stat().st_size, len(items))

    counts = repo.write(repo_items)
    n_tree = gate.check_tree(repo.folder)
    if n_tree != len(repo_items):
        raise Refused(f"the public repository tree has {n_tree} files but the set has {len(repo_items)}")

    lines = [f"published to {out}: viewer, sample.js ({len(rep.files)} files, {len(rep.findings)} findings), "
             f"{len(list(shots.glob('*.jpg')))} shots"]
    lines += [f"  {name}: {n} files, {size:,} bytes" for name, (size, n) in sizes.items()]
    lines.append(f"public repository {repo.folder}: {len(repo_items)} files ({counts['added']} added, {counts['changed']} changed, "
                 f"{counts['removed']} removed, {counts['same']} unchanged); not committed")
    lines.append(f"privacy gate: {len(gate.patterns)} words and paths, 0 hits in the repository tree, the three zips, sample.js "
                 "and the page")
    return "\n".join(lines)


def main(argv):
    a = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    a.add_argument("--site", required=True, help="the anchor-demo checkout (the page lives in its memsentinel/ folder)")
    a.add_argument("--repo", required=True, help="the public repository's folder (made and kept in step with PublicSet)")
    args = a.parse_args(argv)
    try:
        print(publish(Path(args.site), Path(args.repo)))
    except (Refused, EngineNotFound) as e:
        print(f"REFUSED: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
