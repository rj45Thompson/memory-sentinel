"""Headless Unreal runs for the runtime detectors (UC-2, UC-3, UC-5.2).

`HeadlessRun` starts the project's game with no window (`-game -RenderOffScreen -unattended`), no sound to the speakers
(`-DeterministicAudio`), and its own log, waits for it, and returns every findings file the run wrote. When the game
crashes, it reads the crash out of the log and writes one more findings file (kind "run", rule run-crash), so a crash is a
finding and never an empty pass.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import findings as F


class EngineNotFound(Exception):
    pass


def find_engine(uproject: Path, engine: str | None = None) -> Path:
    """The engine folder (the one holding Engine/). From --engine, else the project's EngineAssociation looked up in the
    Epic launcher's install list. Never a guess: raises EngineNotFound naming what was looked at."""
    if engine:
        root = Path(engine)
        if (root / "Engine" / "Binaries").is_dir():
            return root
        raise EngineNotFound(f"{root} has no Engine/Binaries folder")
    assoc = json.loads(uproject.read_text(encoding="utf-8-sig")).get("EngineAssociation", "")
    dat = Path(os.environ.get("PROGRAMDATA", "C:/ProgramData")) / "Epic" / "UnrealEngineLauncher" / "LauncherInstalled.dat"
    looked = [str(dat)]
    if dat.is_file():
        for item in json.loads(dat.read_text(encoding="utf-8-sig")).get("InstallationList", []):
            if item.get("AppName") == f"UE_{assoc}" and (Path(item["InstallLocation"]) / "Engine").is_dir():
                return Path(item["InstallLocation"])
    raise EngineNotFound(f"no Unreal {assoc!r} install found (looked in {', '.join(looked)}); pass --engine")


@dataclass
class RunResult:
    exit_code: int
    seconds: float
    log: Path
    files: list[Path] = field(default_factory=list)
    crashed: bool = False
    crash: str = ""
    timed_out: bool = False

    def loaded(self) -> list[dict]:
        return [F.load(p) for p in self.files]

    def findings(self, rule: str | None = None) -> list[dict]:
        return [f for d in self.loaded() for f in d["findings"] if rule is None or f["rule"] == rule]


CRASH_START = re.compile(r"(=== Critical error: ===|Unhandled Exception: |Fatal error!|Assertion failed: )")
FRAME = re.compile(r"\[Callstack\]\s+0x[0-9a-fA-F]+\s+(\S+)!(.+?)(?:\s+\[(.+?)\])?\s*$")


class HeadlessRun:
    def __init__(self, uproject: Path, engine: Path, map_path: str = "/Engine/Maps/Entry", exec_cmds: list[str] | None = None,
                 extra: list[str] | None = None, timeout: float = 300.0, run_dir: Path | None = None):
        self.uproject = uproject.resolve()
        self.engine = engine
        self.map = map_path
        self.exec_cmds = exec_cmds or []
        self.extra = extra or []
        self.timeout = timeout
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.run_dir = run_dir or (self.uproject.parent / "Saved" / "MemorySentinel" / "runs" / stamp)

    @property
    def findings_dir(self) -> Path:
        return self.uproject.parent / "Saved" / "MemorySentinel"

    def command(self, log: Path) -> str:
        """One command line string. Unreal reads -ExecCmds="a b,c" only when the quote follows the '=', which a list
        handed to subprocess would not give it (it quotes the whole token), so the string is built here."""
        exe = self.engine / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
        if not exe.is_file():
            raise EngineNotFound(f"no {exe}")
        parts = [f'"{exe}"', f'"{self.uproject}"', self.map, "-game", "-RenderOffScreen", "-unattended", "-nosplash",
                 "-DeterministicAudio", "-ResX=1280", "-ResY=720", "-ForceRes", f'-ABSLOG="{log}"']
        if self.exec_cmds:
            parts.append('-ExecCmds="' + ",".join(self.exec_cmds) + '"')
        return " ".join(parts + self.extra)

    def run(self) -> RunResult:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        log = self.run_dir / "game.log"
        before = {p: p.stat().st_mtime for p in self.findings_dir.glob("*.json")} if self.findings_dir.is_dir() else {}
        start = time.time()
        timed_out = False
        proc = subprocess.Popen(self.command(log), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            code = proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
            timed_out = True
        seconds = time.time() - start
        files = sorted(p for p in self.findings_dir.glob("*.json") if p.stat().st_mtime >= start - 1
                       and before.get(p) != p.stat().st_mtime)
        result = RunResult(exit_code=code, seconds=seconds, log=log, files=files, timed_out=timed_out)
        self._read_crash(result)
        if result.crashed or timed_out:
            result.files.append(self._crash_file(result))
        return result

    def _read_crash(self, result: RunResult) -> None:
        if not result.log.is_file():
            result.crashed = result.exit_code != 0
            result.crash = f"the game exited with code {result.exit_code} and wrote no log at {result.log}"
            return
        lines = result.log.read_text(encoding="utf-8", errors="replace").splitlines()
        for k, line in enumerate(lines):
            if CRASH_START.search(line):
                result.crashed = True
                result.crash = "\n".join(lines[k:k + 40])
                return

    def _crash_file(self, result: RunResult) -> Path:
        frames = []
        for line in result.crash.splitlines():
            m = FRAME.search(line)
            if m:
                frames.append({"module": m.group(1), "function": m.group(2).strip(), "at": m.group(3) or ""})
        first = next((f for f in frames if not re.search(r"(?i)kernel|ntdll|-Core\.dll|MemorySentinelHeap", f["module"])), None)
        where = {"object": str(result.log)}
        if first and first["at"]:
            m = re.match(r"(.+?):(\d+)$", first["at"])
            if m:
                where = {"file": m.group(1), "line": int(m.group(2)), "code": first["function"]}
        reason = "it ran past its time limit and was stopped" if result.timed_out else (
            result.crash.splitlines()[0].strip() if result.crash else f"exit code {result.exit_code}")
        fnd = F.Finding(
            rule="run-crash", severity="error",
            title=("The run timed out" if result.timed_out else "The game crashed during the run"),
            where=where,
            why=f"The headless run did not end normally: {reason}. Anything after that point was not tested.",
            fix=("Read the crash stack: the first frame in your code is listed. If a guard-page finding was written by the "
                 "same run, the crash is that finding's write, stopped on purpose."),
            how_known=f"READ: {result.log} (exit code {result.exit_code})",
            evidence={"exit_code": result.exit_code, "frames": frames[:30], "log_excerpt": result.crash[:4000]})
        ff = F.FindingsFile(kind="run", target={"project": str(self.uproject), "map": self.map,
                                                "command": self.command(result.log)},
                            checked={"seconds": round(result.seconds, 1), "exit_code": result.exit_code},
                            blind_spots=["A crash ends the run: code after the crash point was not exercised."],
                            findings=[fnd])
        ff.number("RUN")
        return ff.write(self.findings_dir / f"run-crash-{time.strftime('%Y%m%d-%H%M%S')}.json")


def leaktest(uproject: Path, engine: Path, map_path: str, seconds: float = 5.0, return_map: str = "/Engine/Maps/Entry",
             extra: list[str] | None = None, timeout: float = 300.0) -> RunResult:
    cmds = [f"ms.LeakTest {map_path} -seconds={seconds:g} -return={return_map} -quit"]
    return HeadlessRun(uproject, engine, return_map, cmds, extra, timeout).run()


def heap(uproject: Path, engine: Path, map_path: str, seconds: float = 20.0, sample_every: int = 256,
         suspects: list[int] | None = None, guard_every: int = 0, guard_sizes: list[int] | None = None,
         extra: list[str] | None = None, timeout: float = 300.0) -> RunResult:
    args = ["-MemorySentinel", f"-MemorySentinelSampleEvery={sample_every}"]
    if suspects:
        args.append("-MemorySentinelSuspectSizes=" + ",".join(str(s) for s in suspects))
    if guard_every:
        args.append(f"-MemorySentinelGuardEvery={guard_every}")
    if guard_sizes:
        args.append("-MemorySentinelGuardSizes=" + ",".join(str(s) for s in guard_sizes))
    return HeadlessRun(uproject, engine, map_path, [f"ms.QuitAfter {seconds:g}"], args + (extra or []), timeout).run()
