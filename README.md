# Memory Sentinel

Find Unreal Engine 5 memory bugs before your players do: UObject pointers the garbage collector cannot see, shared pointer
cycles, objects that outlive the level that made them, and heap stomps (writes past the end of a block, after a free, or a
double free), each reported with the file and line, how it is known, and what to change.

## Install in one click

1. Download [MemorySentinel-Install-UE5.7-v1.0.zip](https://rj45thompson.github.io/anchor-demo/memsentinel/MemorySentinel-Install-UE5.7-v1.0.zip)
   and unzip it anywhere.
2. Double-click `Install-MemorySentinel.cmd` and pick your project's `.uproject` file.
3. Open the project in Unreal. The plugin comes as source, so Unreal asks to rebuild its modules: click Yes. Then open
   Tools > Memory Sentinel.

The installer copies the plugin into your project's `Plugins/MemorySentinel`. If one is already there, it first moves it
to `MemorySentinel.backup-<date-time>` beside your `.uproject`. It saves your `.uproject` as `.uproject.bak`, adds the
plugin to its plugin list and changes nothing else in it. If the project is not on Unreal 5.7, it says so and asks
before going on. It prints everything it did. From a script or CI:
`.\Install-MemorySentinel.cmd -Project <path to your .uproject> -Yes` (from the unzipped folder).

Source code: https://github.com/rj45Thompson/memory-sentinel

### Or let your AI assistant install it

Open [AI_INSTALL.md](AI_INSTALL.md), copy its prompt, and paste it into Claude Code, GitHub Copilot (agent mode) or
Cursor, opened in your project's folder. The assistant runs the installer, builds your editor target, runs the static
scan, opens the report, and walks you through each finding with its file and line.

## What it finds

- **Static pass** (Python, standard library; runs on the Python inside every Unreal install): reads your project's C++ and
  reports raw `UObject*` / `TObjectPtr` members without `UPROPERTY`, UObject pointers in plain structs and `FGCObject`s that
  forget one, lambdas handed to timers and delegates that capture a UObject unguarded, UObjects hidden in `void*`, and
  `TSharedPtr` cycles between types (including through a base class).
- **Level leak test**: opens a map, leaves it, collects garbage twice, and lists what survived. For each survivor, the
  engine's own reference search (the code behind `obj refs`) gives the chain from a root.
- **Heap sampler** (`-MemorySentinel`): one allocation in N, and every allocation of a size you suspect, gets a canary after
  its end; freed blocks wait in quarantine filled with a pattern; the CPU's four data breakpoints rotate over the likeliest
  blocks and stop the exact write; optional GWP-ASan style guard pages fault at the exact instruction.
- **Panel**: Tools > Memory Sentinel in the editor, or `ms.Show` in a running game, with a diagram for every finding. **Visual
  report**: one HTML file for the build server.

Manual and a live report: https://rj45thompson.github.io/anchor-demo/memsentinel/

## Install by hand

1. Copy `unreal/MemorySentinel` into your project's `Plugins/` folder, and add `{ "Name": "MemorySentinel", "Enabled": true }`
   to the `Plugins` list in your `.uproject`.
2. Regenerate project files and build your editor target (Windows x64, Unreal 5.7).
3. Tools > Memory Sentinel.

From a script or CI (any Python 3.9 or newer; the engine's own is
`Engine/Binaries/ThirdParty/Python3/Win64/python.exe`):

```
python Plugins/MemorySentinel/Content/Python/ms.py scan <project folder>
python Plugins/MemorySentinel/Content/Python/ms.py leaktest --project <P.uproject> --map /Game/Maps/YourMap
python Plugins/MemorySentinel/Content/Python/ms.py heap --project <P.uproject> --map /Game/Maps/YourMap --seconds 60
python Plugins/MemorySentinel/Content/Python/ms.py report <project>/Saved/MemorySentinel -o report.html
```

Exit codes: 0 nothing at or above `--fail-on`, 1 findings, 2 the detector could not run (never a clean pass).

## What is in this repository

- `unreal/MemorySentinel`: the plugin (three modules: `MemorySentinelHeap`, `MemorySentinel`, `MemorySentinelEditor`) and
  its Python tools (`Content/Python/ms.py`, `Content/Python/memsentinel/`).
- `unreal/SentinelTest`: the test game. Every bug the tool must find is planted there on purpose, beside a clean copy of each
  pattern that must produce nothing. To build it, first run the installer on it:
  `.\Install-MemorySentinel.cmd -Project unreal\SentinelTest\SentinelTest.uproject -Yes` (from the repository folder).
- `Install-MemorySentinel.cmd` and `Install-MemorySentinel.ps1`: the one-click installer. From this repository it installs
  `unreal/MemorySentinel`; from the install zip, the `Plugins/MemorySentinel` beside it.
- `AI_INSTALL.md`: the prompt that has an AI coding assistant install, build, scan and explain.
- `tests/`: the static pass held to the test game both ways (every plant found at its line, nothing else reported), edge
  cases one snippet at a time, and the installer run against project files of every shape it must handle.
- `tools/publish_site.py`: builds the downloads and this repository's tree from the working copy, and refuses to publish
  anything that names a private word or a local folder.
- `USE_CASES.md`: the use cases, requirements and the classes that serve each step.
- Not here: `spec/`, the design notes the tool was built from and the spec check (rules quoted from the notes, one repro per
  rule, each run against the shipped tool). The notes are private working notes, so they stay in the working copy.

## Measured

- Test game, static pass: 18 planted bugs, 18 reported at their lines, 0 reports in the clean control.
- Level leak test: the planted per-level cache is reported with its full chain (GameEngine, GameInstance, the subsystem's
  `Caches` array); the same map with nothing planted reports no survivor out of about 43,000 live objects.
- Heap sampler: the planted overrun, write after free, double free and guarded overrun are each reported, the overrun and the
  write after free at the exact writing line; with nothing planted, sampling 1 in 4 with guard pages over two map travels
  (19 million allocations, 67,355 canaries, 5,067 guard pages, 27 register rotations), no finding.
- Spec check: 13 rules quoted from the notes, 19 repros, every one kept; three planted faults in a copy of the tool each turn
  their repros red.

What it cannot see is listed in every report it writes.

MIT licence.
