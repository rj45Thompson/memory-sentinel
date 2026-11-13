"""The one-click installer (UC-6), run for real: Install-MemorySentinel.ps1 under Windows PowerShell 5.1 (the PowerShell
Install-MemorySentinel.cmd starts), against throwaway projects whose .uproject files have every shape it must handle.

Each test checks the bytes: the .uproject must change only by the plugin entry, the .bak must be the old file exactly,
the plugin copy must be the plugin's source files and nothing else, and every refusal must leave the project untouched.
"""
import contextlib
import ctypes
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "Install-MemorySentinel.ps1"
WRAPPER = ROOT / "Install-MemorySentinel.cmd"
PLUGIN = ROOT / "unreal" / "MemorySentinel"
POWERSHELL = shutil.which("powershell")
SKIP = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", "__pycache__", ".git", ".vs"}

ENTRY_TABS = '\t\t{\n\t\t\t"Name": "MemorySentinel",\n\t\t\t"Enabled": true\n\t\t}'
BASE = '{\n\t"FileVersion": 3,\n\t"EngineAssociation": "5.7",\n\t"Modules": [ { "Name": "Game", "Type": "Runtime" } ]'


@contextlib.contextmanager
def held_open_for_reading(path: Path):
    """Holds the file open the way another program can (an editor, a sync tool): anyone may read it, nobody may write it."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_void_p]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    generic_read, file_share_read, open_existing = 0x80000000, 0x1, 3
    handle = k32.CreateFileW(str(path), generic_read, file_share_read, None, open_existing, 0, None)
    if handle is None or handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), f"could not hold {path} open")
    try:
        yield
    finally:
        k32.CloseHandle(handle)


def plugin_source_files() -> set:
    out = set()
    for p in PLUGIN.rglob("*"):
        rel = p.relative_to(PLUGIN)
        if p.is_file() and not (SKIP & set(rel.parts)) and p.suffix != ".pyc":
            out.add(rel.as_posix())
    return out


@unittest.skipUnless(sys.platform == "win32" and POWERSHELL, "the installer is a Windows PowerShell script")
class Installer(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ms-install-"))
        self.links = []
        self.read_only_files = []

    def tearDown(self):
        for link in self.links:  # remove a junction itself, never through it
            if os.path.lexists(link):
                os.rmdir(link)
        for f in self.read_only_files:
            if f.exists():
                os.chmod(f, stat.S_IREAD | stat.S_IWRITE)
        shutil.rmtree(self.tmp)

    def make_read_only(self, path: Path):
        os.chmod(path, stat.S_IREAD)  # what Perforce does to every file that is not checked out
        self.read_only_files.append(path)

    def project(self, text, name="Game", source=True, bom=False) -> Path:
        d = self.tmp / name
        d.mkdir()
        data = text.encode("utf-8")
        (d / f"{name}.uproject").write_bytes((b"\xef\xbb\xbf" if bom else b"") + data)
        if source:
            (d / "Source").mkdir()
            (d / "Source" / f"{name}Editor.Target.cs").write_text("public class X : TargetRules { Type = TargetType.Editor; }")
        return d / f"{name}.uproject"

    def run_installer(self, *args, stdin=None):
        return subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(INSTALLER), *args],
                              input=stdin, capture_output=True, text=True, timeout=180)

    def assert_installed(self, r, uproject: Path):
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        dest = uproject.parent / "Plugins" / "MemorySentinel"
        got = {p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file()}
        self.assertEqual(got, plugin_source_files())

    def test_adds_a_plugins_list_and_keeps_every_other_byte(self):
        old = BASE + "\n}\n"
        up = self.project(old)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_text(encoding="utf-8"), BASE + ',\n\t"Plugins": [\n' + ENTRY_TABS + "\n\t]\n}\n")
        self.assertEqual((up.parent / "Game.uproject.bak").read_bytes(), old.encode())
        self.assertIn("added a \"Plugins\" list", r.stdout)

    def test_appends_to_an_unreal_style_list_with_windows_line_endings(self):
        other = '\t\t{\r\n\t\t\t"Name": "ModelingToolsEditorMode",\r\n\t\t\t"Enabled": true,\r\n\t\t\t"TargetAllowList": [\r\n\t\t\t\t"Editor"\r\n\t\t\t]\r\n\t\t}'
        head = BASE.replace("\n", "\r\n") + ',\r\n\t"Plugins": [\r\n' + other
        up = self.project(head + "\r\n\t]\r\n}\r\n")
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_bytes().decode(), head + ",\r\n" + ENTRY_TABS.replace("\n", "\r\n") + "\r\n\t]\r\n}\r\n")

    def test_fills_an_empty_list(self):
        up = self.project(BASE + ',\n\t"Plugins": []\n}\n')
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_text(encoding="utf-8"), BASE + ',\n\t"Plugins": [\n' + ENTRY_TABS + "\n\t]\n}\n")

    def test_switches_on_a_disabled_entry_and_changes_nothing_else(self):
        old = BASE + ',\n\t"Plugins": [ { "Name": "MemorySentinel", "Enabled": false, "MarketplaceURL": "" } ]\n}\n'
        up = self.project(old)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_text(encoding="utf-8"), old.replace('"Enabled": false', '"Enabled": true'))
        self.assertIn("switched on", r.stdout)

    def test_already_listed_leaves_the_file_alone(self):
        old = BASE + ',\n\t"Plugins": [ { "Name": "MemorySentinel", "Enabled": true } ]\n}\n'
        up = self.project(old)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_text(encoding="utf-8"), old)
        self.assertFalse((up.parent / "Game.uproject.bak").exists())
        self.assertIn("already lists MemorySentinel", r.stdout)

    def test_keeps_a_byte_order_mark(self):
        up = self.project(BASE + "\n}\n", bom=True)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertTrue(up.read_bytes().startswith(b"\xef\xbb\xbf{"))

    def test_an_existing_copy_goes_to_a_backup_outside_plugins(self):
        up = self.project(BASE + "\n}\n")
        old = up.parent / "Plugins" / "MemorySentinel"
        old.mkdir(parents=True)
        (old / "MemorySentinel.uplugin").write_text("old")
        (old / "mine.txt").write_text("keep me")
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        backups = list(up.parent.glob("MemorySentinel.backup-*"))
        self.assertEqual(len(backups), 1, r.stdout)
        self.assertEqual((backups[0] / "mine.txt").read_text(), "keep me")
        self.assertEqual((backups[0] / "MemorySentinel.uplugin").read_text(), "old")
        self.assertIn(str(backups[0]), r.stdout)

    def test_a_link_is_refused_and_left_alone(self):
        up = self.project(BASE + "\n}\n")
        target = self.tmp / "elsewhere"
        target.mkdir()
        (target / "MemorySentinel.uplugin").write_text("linked")
        link = up.parent / "Plugins" / "MemorySentinel"
        link.parent.mkdir()
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
        self.links.append(link)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("is a link", r.stdout)
        self.assertEqual(sorted(p.name for p in target.iterdir()), ["MemorySentinel.uplugin"])
        self.assertEqual(up.read_text(encoding="utf-8"), BASE + "\n}\n")
        self.assertFalse(list(up.parent.glob("MemorySentinel.backup-*")))

    def test_a_second_copy_elsewhere_under_plugins_is_refused(self):
        up = self.project(BASE + "\n}\n")
        other = up.parent / "Plugins" / "Tools" / "MemorySentinel"
        other.mkdir(parents=True)
        (other / "MemorySentinel.uplugin").write_text("{}")
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("already in this project", r.stdout)
        self.assertFalse((up.parent / "Plugins" / "MemorySentinel").exists())

    def test_a_read_only_project_file_stops_before_anything_is_copied(self):
        old = BASE + "\n}\n"
        up = self.project(old)
        self.make_read_only(up)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("is read-only", r.stdout)
        self.assertIn("Check it out", r.stdout)
        self.assertIn("Nothing in your project was changed.", r.stdout)
        self.assertNotIn("Install-MemorySentinel.ps1", r.stderr)  # an unhandled PowerShell error names the script
        self.assertFalse((up.parent / "Plugins").exists())
        self.assertFalse(list(up.parent.glob("*.bak")))
        self.assertEqual(up.read_bytes(), old.encode())

    def test_a_read_only_project_file_that_already_lists_the_plugin_is_not_written(self):
        old = BASE + ',\n\t"Plugins": [ { "Name": "MemorySentinel", "Enabled": true } ]\n}\n'
        up = self.project(old)
        self.make_read_only(up)
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertEqual(up.read_bytes(), old.encode())
        self.assertFalse(list(up.parent.glob("*.bak")))

    def test_a_write_that_fails_stops_through_stop_install_and_lists_what_was_done(self):
        old = BASE + "\n}\n"
        up = self.project(old)
        with held_open_for_reading(up):
            r = self.run_installer("-Project", str(up), "-Yes")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("FAILED:", r.stdout)
        self.assertIn("Game.uproject", r.stdout.split("FAILED:", 1)[1].splitlines()[0])
        self.assertIn("Already done before it stopped:", r.stdout)
        self.assertIn("Copied the plugin to", r.stdout)
        self.assertIn("Saved the old project file as", r.stdout)
        self.assertNotIn("Install-MemorySentinel.ps1", r.stderr)  # an unhandled PowerShell error names the script
        self.assertEqual(up.read_bytes(), old.encode())
        self.assertEqual((up.parent / "Game.uproject.bak").read_bytes(), old.encode())

    def test_another_engine_version_asks_and_no_changes_nothing(self):
        old = BASE.replace('"5.7"', '"5.3"') + "\n}\n"
        up = self.project(old)
        r = self.run_installer("-Project", str(up), stdin="n\n")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("this project uses Unreal 5.3", r.stdout)
        self.assertIn("Nothing in your project was changed.", r.stdout)
        self.assertEqual(up.read_text(encoding="utf-8"), old)
        self.assertFalse((up.parent / "Plugins").exists())

    def test_another_engine_version_with_yes_warns_and_installs(self):
        up = self.project(BASE.replace('"5.7"', '"5.3"') + "\n}\n")
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assert_installed(r, up)
        self.assertIn("WARNING: this project uses Unreal 5.3", r.stdout)

    def test_a_project_folder_works_as_project(self):
        up = self.project(BASE + "\n}\n")
        r = self.run_installer("-Project", str(up.parent), "-Yes")
        self.assert_installed(r, up)

    def test_yes_without_a_project_fails(self):
        r = self.run_installer("-Yes")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("-Yes needs -Project", r.stdout)

    def test_not_json_fails_and_changes_nothing(self):
        up = self.project("{ this is not json")
        r = self.run_installer("-Project", str(up), "-Yes")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertFalse((up.parent / "Plugins").exists())

    def test_the_cmd_wrapper_passes_the_exit_code_back(self):
        r = subprocess.run(["cmd", "/c", str(WRAPPER), "-Project", str(self.tmp / "missing.uproject"), "-Yes"],
                           capture_output=True, text=True, timeout=180)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("There is no file or folder at", r.stdout)


if __name__ == "__main__":
    unittest.main()
