# Install Memory Sentinel with an AI assistant

Open your AI coding assistant (Claude Code, GitHub Copilot in agent mode, or Cursor) in your Unreal project's folder, the
folder that holds your `.uproject`. Then paste everything between the two lines below. The assistant installs the plugin,
builds your editor, runs the static scan, opens the report and explains what it found. It needs Windows, Unreal 5.7 and a
C++ project.

---

Install the Unreal Engine plugin Memory Sentinel into this project, build the project, scan its C++ and explain what the
scan found. Run every command in PowerShell from this project's folder, and show me each command's output. If a step
fails, stop and tell me what failed and why.

1. Download and unpack the installer. (If I already unzipped MemorySentinel-Install-UE5.7-v1.0.zip, ask me where, use
   that folder in step 2 instead, and skip this step.)

       Invoke-WebRequest -UseBasicParsing https://rj45thompson.github.io/anchor-demo/memsentinel/MemorySentinel-Install-UE5.7-v1.0.zip -OutFile "$env:TEMP\MemorySentinel-Install.zip"
       Expand-Archive "$env:TEMP\MemorySentinel-Install.zip" "$env:TEMP\MemorySentinel-Install" -Force

2. Run the installer without questions. It copies the plugin to Plugins\MemorySentinel (an older copy there is first
   moved to a dated backup beside the .uproject), saves the .uproject as .uproject.bak, adds the plugin to it, and prints
   what it did.

       powershell -NoProfile -ExecutionPolicy Bypass -File "$env:TEMP\MemorySentinel-Install\Install-MemorySentinel.ps1" -Project . -Yes

   Exit code 0 means it is installed. If it printed a WARNING (another engine version, or no C++ code), tell me before
   you go on.

3. Find the engine from the project's EngineAssociation, using the Epic launcher's list of installed engines:

       $assoc = (Get-Content (Get-ChildItem *.uproject).FullName -Raw | ConvertFrom-Json).EngineAssociation
       $engine = ((Get-Content "$env:ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat" -Raw | ConvertFrom-Json).InstallationList | Where-Object AppName -eq "UE_$assoc").InstallLocation
       $engine

   If $engine comes out empty (for example because EngineAssociation is a {GUID}, which means a source build), ask me
   for the engine folder. Steps 4 and 5 use $engine. If your shell does not keep variables from one command to the next
   (many assistants start a new shell for every command), begin each of those commands with
   `$engine = '<the folder this step printed>';`.

4. Build the editor target. Its name is the Source\*.Target.cs file that says TargetType.Editor, without .Target.cs:

       Get-ChildItem Source\*.Target.cs | Select-String -List 'TargetType.Editor' | ForEach-Object { $_.Filename -replace '\.Target\.cs$' }
       & "$engine\Engine\Build\BatchFiles\Build.bat" <EditorTarget> Win64 Development -Project="$((Get-ChildItem *.uproject).FullName)" -WaitMutex

   A good build ends with "Result: Succeeded". If it fails, fix only what is wrong with this project's own setup (for
   example a missing Visual Studio C++ workload, or a module this project's own Build.cs names). Never edit anything
   under Plugins\MemorySentinel to make a build error or a finding go away: tell me about it instead.

5. Run the static scan with the engine's own Python, build the HTML report, and open it:

       & "$engine\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" Plugins\MemorySentinel\Tools\ms.py scan .
       & "$engine\Engine\Binaries\ThirdParty\Python3\Win64\python.exe" Plugins\MemorySentinel\Tools\ms.py report Saved\MemorySentinel -o Saved\MemorySentinel\report.html
       Start-Process Saved\MemorySentinel\report.html

   The scan's exit code is 0 when it found no errors, 1 when it found some (that is a result, not a failure), and 2 when
   it could not run, which is never a clean result. It writes every finding to Saved\MemorySentinel\scan.json.

6. Summarise for me: the counts by severity, then each finding as file:line, what is wrong, and the fix the scan gives
   (the "why" and "fix" fields in Saved\MemorySentinel\scan.json). Do not change any code unless I ask.

---

The installer's other options: `Install-MemorySentinel.ps1 -Project <path to .uproject, or its folder> [-Yes]`. With
no `-Project` it opens a file picker. Exit codes: 0 installed, 1 stopped (you said no, or something needs you first),
2 could not install (it prints why and what, if anything, it had already changed).
