# Memory Sentinel: use cases

Memory Sentinel finds memory bugs in an Unreal Engine 5 C++ project before a player meets them: UObject pointers the garbage
collector cannot see, shared pointer cycles, writes past the end of a block or after it was freed, and objects that outlive the
level that made them. It has three detectors and one place to read what they found:

- a **static pass** over the project's own C++ (Python, standard library; it runs on the Python every Unreal install ships),
- a **level leak test** that loads a map, leaves it, collects garbage and asks the engine who still holds each survivor,
- a **heap sampler** that gives sampled blocks a canary, holds freed ones in quarantine, rotates the CPU's four data breakpoints
  over the likeliest blocks, and can put a guard page behind a sample of blocks,
- a **panel** (an editor tab, and the same widget as an in-game overlay) and a **visual report** (one HTML file) that read the
  same findings files.

The written requirement it serves is the idea notes the tool was built from (in `spec/` in the working copy; they are private
working notes, so they are not in the public repository). Requirements below cite its
lines as `N:line`; steps cite requirements; code cites steps.

## Requirements

- R1 (N:7, N:14) A UObject pointer member the garbage collector cannot see is reported with its file and line.
- R2 (N:15) Pointers the simple scan misses are covered where the source shows them: in plain structs, in lambda captures handed
  to delegates and timers, and hidden behind `void*`.
- R3 (N:9) A strong shared pointer cycle between types is reported with its whole path, and the fix names the side to weaken.
- R4 (N:11, N:18, N:42) Objects that survive a level unload are reported with the chain of references that keeps each one alive.
- R5 (N:10) A write past the end of a sampled block, or into a freed sampled block, is reported with the block's allocation stack.
- R6 (N:32, N:36) The four debug registers rotate over the likeliest addresses: suspects first, then the newest sampled blocks. Never
  more than four at once, never more than eight bytes each.
- R7 (N:33) A watched write is reported with the stack of the instruction that wrote it.
- R8 (N:38) Canaries are checked on free and on a per-frame budget, and a corrupted block's size becomes a suspect so the
  next block of that size is watched by a data breakpoint.
- R9 (N:37, N:44) Guard pages are optional, off by default, and sampled.
- R10 (iron law) Every finding says how it is known. A detector that could not run says so; it is never a clean pass.
- R11 (N:15, N:19) Every report lists what its detector cannot see.

## UC-1: a developer scans the project's source

Primary actor: an Unreal C++ developer. Precondition: the plugin is in the project's `Plugins/` folder.

1. Tools > Memory Sentinel > **Scan source** (or `python Plugins/MemorySentinel/Tools/ms.py scan <project>`) -> every `.h` and
   `.cpp` under `Source/` and each project plugin's `Source/` is read, and the panel says how many files, types and members it read.
2. A UCLASS or USTRUCT member that holds a raw `UObject*`, a `TObjectPtr` or a container of them with no `UPROPERTY` -> one
   **error** finding at that line (R1).
3. A plain class or struct (no UCLASS/USTRUCT) that holds a UObject pointer and is not an `FGCObject` that reports it -> one
   **error** finding (R2).
4. A lambda that captures `this` or a UObject pointer and is handed to a delegate or a timer -> one **warning**, fix
   `CreateWeakLambda` / `AddUObject` / a captured `TWeakObjectPtr` (R2).
5. A UObject pointer cast or assigned into a `void*` -> one **warning** (R2).
6. Types that hold each other through `TSharedPtr`/`TSharedRef`, directly or through a derived type -> one **error** per cycle
   with its path; a type that holds its own type -> one **warning**; a lambda that captures `SharedThis`/`AsShared` -> one
   **warning** (R3).
7. Double-click a finding -> the IDE opens at that line.

Alternates:
- 1a No `Source/` folder or no C++ -> the scan **fails** ("nothing to scan"), exit 2. It never reports a clean project.
- 2a The member is a `TWeakObjectPtr`, `TSoftObjectPtr`, `TStrongObjectPtr`, or is reported by the type's own
  `AddReferencedObjects` -> no finding.
- 2b The line (or the line above) carries `// memsentinel: ignore <reason>` -> the finding is kept as **suppressed** with the
  reason, and counted.
- 2c A type's base is not in the scanned code -> it is judged a UObject only by the Unreal naming rule (U or A prefix), and the
  finding says so in `how_known`.

## UC-2: a developer runs the level leak test

1. Tools > Memory Sentinel > **Leak test** with a map (or `ms.LeakTest /Game/Maps/X` in a dev build, or
   `ms.py leaktest --project P --map /Game/Maps/X` headless) -> the plugin records every live object, opens the map, lets it run
   (default 5 s), opens the return map (default `/Engine/Maps/Entry`) and collects garbage twice.
2. Every object still alive that was made after step 1 and belonged to the left map (its package, its world or its level), or is
   an actor or component outside the current world -> one **error** finding "survived the level unload" (R4).
3. For each survivor the engine's own reference search (`FReferenceChainSearch`, the code behind `obj refs`) gives the shortest
   chain from a root -> the finding lists it root first, each link with the property or `AddReferencedObjects` that holds it (R4).
4. The run's counts are written: objects before, after, survivors, chains found.

Alternates:
- 1a The map does not load -> the test **fails** with the engine's reason. No findings file claims a pass.
- 2a Nothing survived -> a findings file with zero findings and the counts that prove the test ran.
- 3a The search finds no chain (the object is only kept by the root set) -> the finding says "rooted, no referencer".

## UC-3: a developer runs with the heap sampler

1. Start the game with `-MemorySentinel` (and optionally `-MemorySentinelSampleEvery=N`, `-MemorySentinelSuspectSizes=a,b`,
   `-MemorySentinelGuardEvery=N`) -> the plugin wraps the engine's allocator at startup and logs what it sampled.
2. One allocation in N, and every allocation of a suspect size, gets 16 to 23 bytes of canary after its end and a recorded
   allocation stack; its reported size stays the size asked for.
3. On free, the canary is checked; the block is filled with a pattern and held in quarantine instead of being reused (R8).
4. Each frame, a budget of live and quarantined blocks is checked (R8). A changed canary -> one **error** "heap overrun" with the
   size, the bytes past the end that changed and the allocation stack (R5). A changed quarantine pattern -> one **error** "write
   after free" with the allocation and free stacks (R5). Either way the block's size becomes a suspect size (R8).
5. Every K frames the rotor picks at most four 8-byte words (suspect canaries, suspect quarantined blocks, then the newest
   sampled canaries) and loads them into DR0-DR3 on every thread (R6). A write to a watched word -> the stack of the writing
   instruction is attached to that block's finding (R7).
6. With `-MemorySentinelGuardEvery=N`, one allocation in N (and suspect sizes) is placed against a no-access page; on free its page
   becomes no-access. A touch past the end or after free faults at that instruction -> the finding is written with its stack,
   then the crash continues to the engine's crash reporter (R9).
7. Findings are written when found and at exit.

Alternates:
- 1a Not Windows x64 -> the sampler does not install and says why; no data breakpoints, no guard pages.
- 5a A debugger is attached -> the rotor stays off (the debugger owns the debug registers) and says so.
- 2a The block table is full -> sampling pauses until frees make room; the count of skipped samples is reported.

## UC-4: a developer reads the findings

1. The panel (editor tab, or `ms.Show` in a running game) lists every findings file under `Saved/MemorySentinel/`, newest
   first, with error and warning counts per kind.
2. Selecting a finding -> its why, fix and how it is known, and a diagram: the reference chain for a survivor, the ring for a
   cycle, the block's bytes for a stomp, the source line for a pointer.
3. **Open visual report** -> `ms.py report` merges the files into one HTML file (no server) with the same diagrams and an
   overview of every finding, and opens it in the browser.

## UC-5: a team runs it in CI

1. `ms.py scan <project> --fail-on error` -> exit 1 when there is an error finding, 0 when there is none, 2 when it could not run.
2. `ms.py leaktest ...` and `ms.py heap ...` start the engine headless (`-game -RenderOffScreen -unattended`) and return the same
   exit codes from the findings files they wrote.
3. `ms.py report <dir> -o report.html` -> one file to attach to the build.

## UC-6: an engineer installs it into a project

Primary actor: an Unreal C++ developer trying the plugin. Precondition: the install zip is unzipped (or the repository is cloned).

1. Double-click `Install-MemorySentinel.cmd` -> a file picker asks for the `.uproject` (`-Project <path> -Yes` for scripts, CI and
   AI assistants) -> the installer names the project and the engine version it is set to.
2. The project is not set to Unreal 5.7, or has no `Source/` folder -> it says so and asks; no -> exit 1, nothing changed.
3. Unreal has the project open -> exit 1, nothing changed.
4. `Plugins/MemorySentinel` already exists -> it is moved to `MemorySentinel.backup-<date-time>` beside the `.uproject`, outside
   `Plugins/` so Unreal never loads two copies. A link there, or another copy elsewhere under `Plugins/` -> exit 1, untouched.
5. The plugin's source (no build outputs) is copied to `Plugins/MemorySentinel` and the file count checked.
6. The `.uproject` is saved as `.uproject.bak`, then the plugin entry is added (or switched on) as text, so every other byte
   stays as it was. The result is parsed and compared with the old file; if anything else changed it is put back (exit 2).
7. It prints what it did and the next step: open the project and accept the rebuild, build with `Build.bat`, or paste
   `AI_INSTALL.md` into an AI coding assistant, which installs, builds, scans and explains the findings with file and line.

## Classes (the bones) and the steps they serve

Python, `unreal/MemorySentinel/Tools/memsentinel/`:
- `cpp.SourceText`: comments and strings blanked, lines kept (UC-1.1).
- `scanner.TypeIndex`: every class/struct with its kind, bases and body (UC-1.1, 1.2, 2c).
- `scanner.MemberReader`, `scanner.PointerType`: a member declaration and what it holds (UC-1.2, 1.3).
- `scanner.CaptureFinder`, `scanner.VoidHideFinder`: lambdas into delegates and timers, `void*` hiding (UC-1.4, 1.5).
- `cycles.SharedGraph`: strong shared edges and their cycles (UC-1.6).
- `findings.Finding`, `findings.FindingsFile`: the one schema every detector writes (UC-1..4, R10, R11).
- `report.Report`: merge and the HTML visual report (UC-4.3, UC-5.3).
- `unreal.HeadlessRun`: start the engine for UC-2 and UC-3 from a script (UC-5.2).

C++, `unreal/MemorySentinel/Source/`:
- `MemorySentinelHeap` (loads at PostConfigInit, no UObjects): `FSentinelHeap` the allocator proxy (UC-3.1-3.4),
  `FBlockTable` (UC-3.2), `FQuarantine` (UC-3.3), `FGuardPool` (UC-3.6), `FRegisterRotor` (UC-3.5), `FFaultHandler`
  (UC-3.5, 3.6), `FHeapEvents` (UC-3.4-3.7).
- `MemorySentinel`: `FLevelLeakTest` (UC-2), `FHeapFindingsWriter` (UC-3.7), `FFindingsJson` (the schema in C++),
  `SSentinelPanel` and `SSentinelDiagram` (UC-4.1, 4.2), `FSentinelOverlay` (`ms.Show`).
- `MemorySentinelEditor`: the Tools menu entry and the tab that hosts `SSentinelPanel` (UC-4.1), the scan and leak test buttons
  (UC-1.1, UC-2.1).

Installer and publishing:
- `Install-MemorySentinel.ps1`: `Get-EngineInfo` (UC-6.1, 6.2), `Edit-ProjectText` and `Test-ProjectEdit` (UC-6.6); the
  `.cmd` beside it starts it with the execution policy relaxed for that one process.
- `tools/publish_site.py`: `PublicSet` the one list of public files (the public repository and all three zips read it),
  `PrivacyGate` (refuses any public byte naming a private word or a local folder), `PublicRepo` (the public repository tree).
