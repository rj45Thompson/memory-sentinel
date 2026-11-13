<#
.SYNOPSIS
Installs the Memory Sentinel plugin into an Unreal Engine project.

.DESCRIPTION
Copies the plugin into <project>\Plugins\MemorySentinel and adds it to the project's .uproject plugin list.

- With no -Project, a file picker asks for the .uproject. -Project also takes the project's folder.
- If the project already has Plugins\MemorySentinel, it is moved to <project>\MemorySentinel.backup-<date-time> first.
  The backup sits outside Plugins, so Unreal never loads two copies.
- Before the .uproject is changed it is saved as <name>.uproject.bak. Only the plugin entry is added (or switched on);
  every other byte of the file stays as it was, and the result is read back and compared before the installer finishes.
- If the project is not set to Unreal 5.7, it says so and asks before going on. -Yes answers every question with yes.
- A read-only .uproject (in Perforce, one that is not checked out) stops the install before anything is copied.

Exit codes: 0 installed, 1 stopped (you said no, picked nothing, or something needs you first), 2 could not install
(the reason is printed). When it stops or fails, it says what, if anything, it had already changed.

.EXAMPLE
Install-MemorySentinel.cmd
Double-click it: a file picker asks for your .uproject.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File Install-MemorySentinel.ps1 -Project "C:\Projects\MyGame\MyGame.uproject" -Yes
No questions: for scripts, CI and AI assistants.
#>
[CmdletBinding()]
param(
    [string]$Project,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$PluginName = 'MemorySentinel'
$PluginVersion = '1.0'
$TestedEngine = '5.7'
# Build outputs and caches are never copied: Unreal rebuilds the plugin for your project.
$SkipDirs = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '__pycache__', '.git', '.vs')
$Done = New-Object System.Collections.Generic.List[string]

function Say([string]$Text) { Write-Host $Text }

function Stop-Install([int]$Code, [string]$Why) {
    Write-Host ''
    if ($Code -eq 1) { Write-Host "STOPPED: $Why" -ForegroundColor Yellow } else { Write-Host "FAILED: $Why" -ForegroundColor Red }
    if ($Done.Count -eq 0) {
        Write-Host 'Nothing in your project was changed.'
    } else {
        Write-Host 'Already done before it stopped:'
        foreach ($d in $Done) { Write-Host "  - $d" }
    }
    exit $Code
}

# Anything that goes wrong without a check of its own (a write Windows refuses, a file another program holds) ends the
# install here, through Stop-Install, so it always says what it had already changed. $Step says what it was doing.
$Step = 'starting'
trap {
    $e = $_.Exception
    while ($e.InnerException) { $e = $e.InnerException }
    Stop-Install 2 "Could not finish $Step`: $($e.Message)"
}

function Ask([string]$Question) {
    if ($Yes) {
        Say "$Question yes (-Yes was given)"
        return $true
    }
    $answer = Read-Host "$Question (y/n)"
    return ($answer -match '^\s*(y|yes)\s*$')
}

function Get-Prop($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $p = $Object.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}

# ------------------------------------------------------------------------------------------------ the project file

function Select-ProjectFile {
    if ($Yes) { Stop-Install 2 '-Yes needs -Project <path to your .uproject>, because nobody is there to pick the file.' }
    if ([System.Threading.Thread]::CurrentThread.GetApartmentState() -eq 'STA') {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Pick the Unreal project to install Memory Sentinel into'
        $dialog.Filter = 'Unreal project (*.uproject)|*.uproject'
        $unrealProjects = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Unreal Projects'
        if (Test-Path -LiteralPath $unrealProjects) { $dialog.InitialDirectory = $unrealProjects }
        $owner = New-Object System.Windows.Forms.Form
        $owner.TopMost = $true
        try { $result = $dialog.ShowDialog($owner) } finally { $owner.Dispose() }
        if ($result -ne [System.Windows.Forms.DialogResult]::OK) { Stop-Install 1 'No project was picked.' }
        return $dialog.FileName
    }
    # A file picker needs a single-threaded console; Install-MemorySentinel.cmd starts one. Elsewhere, ask for the path.
    $typed = Read-Host "Path to your project's .uproject file (or its folder)"
    if (-not $typed) { Stop-Install 1 'No project was given.' }
    return $typed.Trim().Trim('"')
}

function Resolve-ProjectFile([string]$Given) {
    if (-not $Given) { $Given = Select-ProjectFile }
    if (-not (Test-Path -LiteralPath $Given)) { Stop-Install 2 "There is no file or folder at $Given." }
    $item = Get-Item -LiteralPath $Given
    if ($item.PSIsContainer) {
        $found = @(Get-ChildItem -LiteralPath $item.FullName -File | Where-Object { $_.Extension -eq '.uproject' })
        if ($found.Count -eq 0) { Stop-Install 2 "The folder $($item.FullName) has no .uproject file in it." }
        if ($found.Count -gt 1) {
            Stop-Install 2 ("The folder $($item.FullName) has more than one .uproject file (" + (($found | ForEach-Object { $_.Name }) -join ', ') +
                            '). Pass the one you want with -Project.')
        }
        $item = $found[0]
    }
    if ($item.Extension -ne '.uproject') { Stop-Install 2 "$($item.FullName) is not a .uproject file." }
    return $item.FullName
}

function Read-ProjectText([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $start = 0
    if ($bom) { $start = 3 }
    $text = (New-Object System.Text.UTF8Encoding($false)).GetString($bytes, $start, $bytes.Length - $start)
    return @{ Bytes = $bytes; Bom = $bom; Text = $text }
}

# ------------------------------------------------------------------------------------------------ the engine version

function Get-EngineInfo([string]$Association) {
    # Version and folder of the engine the project is set to, each read from where Unreal itself records it, or left empty.
    $info = @{ Version = $null; Root = $null; HowKnown = '' }
    if ($Association -match '^\d+\.\d+$') {
        $info.Version = $Association
        $info.HowKnown = "the .uproject's EngineAssociation is $Association"
        $dat = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
        if (Test-Path -LiteralPath $dat) {
            $list = Get-Prop (Get-Content -LiteralPath $dat -Raw | ConvertFrom-Json) 'InstallationList'
            $hit = @($list | Where-Object { (Get-Prop $_ 'AppName') -eq "UE_$Association" })
            if ($hit.Count -ge 1 -and (Test-Path -LiteralPath (Join-Path $hit[0].InstallLocation 'Engine'))) { $info.Root = $hit[0].InstallLocation }
        }
    } elseif ($Association) {
        # A source or custom engine build: its setup registers the folder under this key, named by the GUID.
        $key = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
        if (Test-Path $key) {
            $root = Get-Prop (Get-ItemProperty -LiteralPath $key) $Association
            if ($root) {
                $info.Root = $root
                $versionFile = Join-Path $root 'Engine\Build\Build.version'
                if (Test-Path -LiteralPath $versionFile) {
                    $v = Get-Content -LiteralPath $versionFile -Raw | ConvertFrom-Json
                    $info.Version = "$($v.MajorVersion).$($v.MinorVersion)"
                    $info.HowKnown = "the engine build it names ($root) says $($info.Version) in Engine\Build\Build.version"
                }
            }
        }
    }
    return $info
}

# ------------------------------------------------------------------------------------------------ editing the .uproject as text

function Find-Close([string]$Text, [int]$Open) {
    # Index of the bracket that closes the one at $Open, skipping anything inside strings; -1 if it never closes.
    $depth = 0
    $inString = $false
    for ($i = $Open; $i -lt $Text.Length; $i++) {
        $c = $Text[$i]
        if ($inString) {
            if ($c -eq '\') { $i++ } elseif ($c -eq '"') { $inString = $false }
            continue
        }
        if ($c -eq '"') { $inString = $true }
        elseif ($c -eq '{' -or $c -eq '[') { $depth++ }
        elseif ($c -eq '}' -or $c -eq ']') {
            $depth--
            if ($depth -eq 0) { return $i }
        }
    }
    return -1
}

function Find-Key([string]$Text, [string]$Key) {
    # Where the value of "Key" starts, for each time the key appears directly inside the object that $Text starts with.
    $hits = New-Object System.Collections.Generic.List[int]
    $depth = 0
    $i = 0
    while ($i -lt $Text.Length) {
        $c = $Text[$i]
        if ($c -eq '"') {
            $j = $i + 1
            while ($j -lt $Text.Length -and $Text[$j] -ne '"') {
                if ($Text[$j] -eq '\') { $j++ }
                $j++
            }
            if ($depth -eq 1 -and $Text.Substring($i + 1, $j - $i - 1) -ceq $Key) {
                $k = $j + 1
                while ($k -lt $Text.Length -and [char]::IsWhiteSpace($Text[$k])) { $k++ }
                if ($k -lt $Text.Length -and $Text[$k] -eq ':') {
                    $k++
                    while ($k -lt $Text.Length -and [char]::IsWhiteSpace($Text[$k])) { $k++ }
                    $hits.Add($k)
                }
            }
            $i = $j + 1
            continue
        }
        if ($c -eq '{' -or $c -eq '[') { $depth++ } elseif ($c -eq '}' -or $c -eq ']') { $depth-- }
        $i++
    }
    return ,$hits
}

function Get-LastContent([string]$Text, [int]$Before) {
    # Index of the last character before $Before that is not white space.
    $k = $Before - 1
    while ($k -ge 0 -and [char]::IsWhiteSpace($Text[$k])) { $k-- }
    return $k
}

function Edit-ProjectText([string]$Text, $Json) {
    # Returns @{ Text = new text; Did = what changed } or $null when the plugin is already listed and switched on.
    $nl = "`n"
    if ($Text.Contains("`r`n")) { $nl = "`r`n" }
    $unit = "`t"   # Unreal writes .uproject files with tabs; a file with no indented line keeps that style
    $m = [regex]::Match($Text, '\n([ \t]+)"')
    if ($m.Success) { $unit = $m.Groups[1].Value }
    $entry = '{' + $nl + $unit * 3 + '"Name": "' + $PluginName + '",' + $nl + $unit * 3 + '"Enabled": true' + $nl + $unit * 2 + '}'

    $top = $Text.IndexOf('{')
    $topClose = Find-Close $Text $top
    if ($top -lt 0 -or $topClose -lt 0) { Stop-Install 2 'The .uproject does not hold one JSON object.' }
    $object = $Text.Substring($top, $topClose - $top + 1)
    $at = Find-Key $object 'Plugins'
    if ($at.Count -gt 1) { Stop-Install 2 'The .uproject lists "Plugins" more than once. Fix that by hand first.' }

    if ($at.Count -eq 0) {
        $last = Get-LastContent $Text $topClose
        $block = $unit + '"Plugins": [' + $nl + $unit * 2 + $entry + $nl + $unit + ']'
        if ($Text[$last] -eq '{') { $insert = $nl + $block + $nl } else { $insert = ',' + $nl + $block }
        return @{ Text = $Text.Insert($last + 1, $insert); Did = 'added a "Plugins" list with MemorySentinel switched on' }
    }

    $open = $top + $at[0]
    if ($Text[$open] -ne '[') { Stop-Install 2 'The .uproject has a "Plugins" entry that is not a list. Fix that by hand first.' }
    $close = Find-Close $Text $open

    # Is MemorySentinel already in the list? Walk the list's objects one by one.
    $i = $open + 1
    while ($i -lt $close) {
        if ($Text[$i] -eq '{') {
            $end = Find-Close $Text $i
            $item = $Text.Substring($i, $end - $i + 1)
            if ((Get-Prop ($item | ConvertFrom-Json) 'Name') -eq $PluginName) {
                $enabledAt = Find-Key $item 'Enabled'
                if ($enabledAt.Count -eq 0) {
                    $new = $Text.Insert($i + 1, ' "Enabled": true,')
                    return @{ Text = $new; Did = 'switched on the MemorySentinel entry that was already listed (it had no "Enabled")' }
                }
                $v = $i + $enabledAt[0]
                $lit = [regex]::Match($Text.Substring($v), '^(true|false|null|"[^"]*"|[-0-9.eE+]+)')
                if ($lit.Value -eq 'true') { return $null }
                $new = $Text.Substring(0, $v) + 'true' + $Text.Substring($v + $lit.Length)
                return @{ Text = $new; Did = "switched on the MemorySentinel entry that was already listed (it said `"Enabled`": $($lit.Value))" }
            }
            $i = $end + 1
            continue
        }
        $i++
    }

    $last = Get-LastContent $Text $close
    if ($last -eq $open) {
        $new = $Text.Substring(0, $open + 1) + $nl + $unit * 2 + $entry + $nl + $unit + $Text.Substring($close)
    } else {
        $new = $Text.Insert($last + 1, ',' + $nl + $unit * 2 + $entry)
    }
    return @{ Text = $new; Did = 'added MemorySentinel to the "Plugins" list, switched on' }
}

function Test-ProjectEdit($Before, [string]$AfterText) {
    # The edited file must parse, list MemorySentinel once and switched on, and be equal to the old one everywhere else.
    try { $after = $AfterText | ConvertFrom-Json } catch { return "the edited file is not valid JSON ($($_.Exception.Message))" }
    $mine = @(@(Get-Prop $after 'Plugins') | Where-Object { (Get-Prop $_ 'Name') -eq $PluginName })
    if ($mine.Count -ne 1) { return "the edited file lists MemorySentinel $($mine.Count) times" }
    if ((Get-Prop $mine[0] 'Enabled') -ne $true) { return 'the edited file does not switch MemorySentinel on' }
    $restBefore = $Before | Select-Object -Property * -ExcludeProperty Plugins | ConvertTo-Json -Depth 50 -Compress
    $restAfter = $after | Select-Object -Property * -ExcludeProperty Plugins | ConvertTo-Json -Depth 50 -Compress
    if ($restBefore -cne $restAfter) { return 'something other than the plugin list changed' }
    $othersBefore = @(@(Get-Prop $Before 'Plugins') | Where-Object { $null -ne $_ -and (Get-Prop $_ 'Name') -ne $PluginName } | ForEach-Object { $_ | ConvertTo-Json -Depth 50 -Compress })
    $othersAfter = @(@(Get-Prop $after 'Plugins') | Where-Object { $null -ne $_ -and (Get-Prop $_ 'Name') -ne $PluginName } | ForEach-Object { $_ | ConvertTo-Json -Depth 50 -Compress })
    if (($othersBefore -join "`n") -cne ($othersAfter -join "`n")) { return 'another plugin entry changed' }
    $old = @(@(Get-Prop $Before 'Plugins') | Where-Object { $null -ne $_ -and (Get-Prop $_ 'Name') -eq $PluginName })
    if ($old.Count -eq 1) {
        $a = $old[0] | Select-Object -Property * -ExcludeProperty Enabled | ConvertTo-Json -Depth 50 -Compress
        $b = $mine[0] | Select-Object -Property * -ExcludeProperty Enabled | ConvertTo-Json -Depth 50 -Compress
        if ($a -cne $b) { return 'the MemorySentinel entry changed in more than its "Enabled" field' }
    }
    return $null
}

# ------------------------------------------------------------------------------------------------ the install

Say "Memory Sentinel $PluginVersion installer (built and tested on Unreal $TestedEngine, Windows x64)"
Say ''

$Step = 'finding the project file'
$uproject = Resolve-ProjectFile $Project
$projectDir = Split-Path -Parent $uproject
$projectName = [System.IO.Path]::GetFileNameWithoutExtension($uproject)
Say "Project: $uproject"

$Step = "reading $uproject"
$original = Read-ProjectText $uproject
try { $json = $original.Text | ConvertFrom-Json } catch { Stop-Install 2 "$uproject is not valid JSON: $($_.Exception.Message)" }

# 1. The engine version.
$Step = 'checking the engine version'
$association = [string](Get-Prop $json 'EngineAssociation')
$engine = Get-EngineInfo $association
if ($engine.Version -eq $TestedEngine) {
    Say "Engine: Unreal $($engine.Version) ($($engine.HowKnown))."
} else {
    Say ''
    if ($engine.Version) {
        Say "WARNING: this project uses Unreal $($engine.Version) ($($engine.HowKnown))."
    } else {
        Say "WARNING: could not tell which Unreal version this project uses (its EngineAssociation is '$association', and no engine registered under that name was found)."
    }
    Say "Memory Sentinel $PluginVersion is built and tested on Unreal $TestedEngine only. On another version it may not compile."
    if (-not (Ask 'Install anyway?')) { Stop-Install 1 'You chose not to install on this engine version.' }
}

# 2. A C++ project?
$Step = 'looking for the Source folder'
$sourceDir = Join-Path $projectDir 'Source'
if (-not (Test-Path -LiteralPath $sourceDir)) {
    Say ''
    Say 'WARNING: this project has no Source folder, so it has no C++ code of its own.'
    Say 'Memory Sentinel is a C++ source plugin and was tested in C++ projects only.'
    if (-not (Ask 'Install anyway?')) { Stop-Install 1 'You chose not to install into a project without C++ code.' }
}

# 3. Not while Unreal has this project open: it holds the old plugin's files and would not load the new one.
$Step = 'checking whether Unreal has this project open'
$projectForms = @($uproject, $uproject.Replace('\', '/'))
try {
    $editors = @(Get-CimInstance -ClassName Win32_Process -Filter "Name LIKE 'UnrealEditor%'" | Where-Object {
        $cmd = $_.CommandLine
        $cmd -and @($projectForms | Where-Object { $cmd.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count -gt 0 })
} catch {
    Say "Could not check whether Unreal has this project open: $($_.Exception.Message)"
    if (-not (Ask 'Is Unreal closed (or not using this project), so the install can go on?')) { Stop-Install 1 'Close Unreal, then run the installer again.' }
    $editors = @()
}
if ($editors.Count -gt 0) {
    Stop-Install 1 ("Unreal has this project open (process " + (($editors | ForEach-Object { $_.ProcessId }) -join ', ') +
                    '). Close it, then run the installer again.')
}

# 4. The plugin that came with this installer: Plugins\MemorySentinel in the download, unreal\MemorySentinel in the repository.
$Step = 'finding the plugin next to the installer'
$here = $PSScriptRoot
$sources = @(@((Join-Path $here "Plugins\$PluginName"), (Join-Path $here "unreal\$PluginName")) |
             Where-Object { Test-Path -LiteralPath (Join-Path $_ "$PluginName.uplugin") })
if ($sources.Count -eq 0) {
    Stop-Install 2 ("The plugin is not next to this installer: looked for $PluginName.uplugin in $(Join-Path $here "Plugins\$PluginName") " +
                    "and $(Join-Path $here "unreal\$PluginName"). Unzip the whole download, then run the installer from the unzipped folder.")
}
if ($sources.Count -gt 1) { Stop-Install 2 "There are two copies of the plugin next to this installer ($($sources -join ' and ')). Keep one." }
$source = (Get-Item -LiteralPath $sources[0]).FullName

$pluginsDir = Join-Path $projectDir 'Plugins'
$dest = Join-Path $pluginsDir $PluginName
if ($source.TrimEnd('\') -ieq $dest.TrimEnd('\')) {
    Stop-Install 2 "The installer is inside the project it would install into ($source). Run it from the unzipped download instead."
}

# 5. Another copy somewhere else under Plugins would make Unreal find the plugin twice.
$Step = "looking for another copy of the plugin under $pluginsDir"
if (Test-Path -LiteralPath $pluginsDir) {
    $others = @(Get-ChildItem -LiteralPath $pluginsDir -Filter "$PluginName.uplugin" -Recurse -Depth 4 -File |
                Where-Object { $_.DirectoryName.TrimEnd('\') -ine $dest.TrimEnd('\') })
    if ($others.Count -gt 0) {
        Stop-Install 1 ("Memory Sentinel is already in this project at " + (($others | ForEach-Object { $_.DirectoryName }) -join ', ') +
                        ". Remove or move that copy first, so Unreal does not find the plugin twice.")
    }
}

# 6. Work out the change to the .uproject now, while nothing has been touched, so a project file that cannot take it
#    stops the install here instead of after the copy. A read-only one is what Perforce leaves when it is not checked out.
$Step = "working out the change to $projectName.uproject"
$edit = Edit-ProjectText $original.Text $json
if ($null -ne $edit -and (Get-Item -LiteralPath $uproject -Force).IsReadOnly) {
    Stop-Install 1 "The project file $uproject is read-only (in Perforce: not checked out). Check it out, then run the installer again."
}

# 7. An existing Plugins\MemorySentinel goes to a dated backup outside Plugins.
$backup = $null
if (Test-Path -LiteralPath $dest) {
    $existing = Get-Item -LiteralPath $dest -Force
    if ($existing.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        Stop-Install 1 "$dest is a link to another folder, not a copy. The installer does not move or write through links: remove the link yourself if you want a copied plugin there."
    }
    $backup = Join-Path $projectDir ("$PluginName.backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $Step = "moving $dest to $backup"
    Move-Item -LiteralPath $dest -Destination $backup
    $Done.Add("Moved the Memory Sentinel that was in Plugins to $backup")
}

# 8. Copy the plugin.
$Step = "copying the plugin to $dest (what was copied before the error is still there)"
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File -Force | Where-Object {
    $rel = $_.FullName.Substring($source.Length).TrimStart('\')
    $parts = $rel.Split('\')
    -not (@($parts | Where-Object { $SkipDirs -contains $_ }).Count) -and $_.Extension -ne '.pyc' })
foreach ($f in $files) {
    $rel = $f.FullName.Substring($source.Length).TrimStart('\')
    $to = Join-Path $dest $rel
    $toDir = Split-Path -Parent $to
    if (-not (Test-Path -LiteralPath $toDir)) { New-Item -ItemType Directory -Path $toDir -Force | Out-Null }
    Copy-Item -LiteralPath $f.FullName -Destination $to
}
$copied = @(Get-ChildItem -LiteralPath $dest -Recurse -File -Force).Count
if ($copied -ne $files.Count) { Stop-Install 2 "Copied $copied files to $dest but the plugin has $($files.Count)." }
$Done.Add("Copied the plugin to $dest ($copied files, source only: Unreal builds it for your project)")

# 9. List the plugin in the .uproject (the change was worked out in step 6).
if ($null -eq $edit) {
    $Done.Add("Left $projectName.uproject as it was: it already lists MemorySentinel, switched on")
} else {
    $bak = "$uproject.bak"
    if (Test-Path -LiteralPath $bak) { $bak = "$uproject." + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak' }
    $Step = "saving the old project file as $bak"
    [System.IO.File]::WriteAllBytes($bak, $original.Bytes)
    $Done.Add("Saved the old project file as $bak")
    $Step = "writing $uproject (the old one is in $bak)"
    [System.IO.File]::WriteAllText($uproject, $edit.Text, (New-Object System.Text.UTF8Encoding($original.Bom)))
    $Step = "reading $uproject back to compare it"
    $problem = Test-ProjectEdit $json (Read-ProjectText $uproject).Text
    if ($problem) {
        $Step = "putting $uproject back as it was (the old one is in $bak)"
        [System.IO.File]::WriteAllBytes($uproject, $original.Bytes)
        $Done.Add("Put $projectName.uproject back as it was, because $problem")
        Stop-Install 2 "Could not add the plugin to $projectName.uproject safely: $problem. Add {`"Name`": `"$PluginName`", `"Enabled`": true} to its Plugins list by hand."
    }
    $Done.Add("In $projectName.uproject: $($edit.Did); the rest of the file is unchanged (read back and compared)")
}

# 10. Tell the user what happened and what comes next.
$Step = 'printing what was done (the install itself is finished)'
Say ''
Say "Memory Sentinel $PluginVersion is installed in $projectDir."
Say 'What the installer did:'
foreach ($d in $Done) { Say "  - $d" }
Say ''
Say 'Next step, pick one:'
Say "  1. Open $projectName.uproject. Unreal says the MemorySentinel modules are missing or were built for another version and asks to rebuild them: click Yes. Then open Tools > Memory Sentinel."
$editorTargets = @()
if (Test-Path -LiteralPath $sourceDir) {
    $editorTargets = @(Get-ChildItem -LiteralPath $sourceDir -Filter '*.Target.cs' -File | Where-Object {
        (Get-Content -LiteralPath $_.FullName -Raw) -match 'TargetType\.Editor' } | ForEach-Object { $_.Name -replace '\.Target\.cs$', '' })
}
if ($engine.Root -and $editorTargets.Count -eq 1) {
    $buildBat = Join-Path $engine.Root 'Engine\Build\BatchFiles\Build.bat'
    Say '  2. Or build it from PowerShell first (needs Visual Studio with its C++ game development workload):'
    Say "     & `"$buildBat`" $($editorTargets[0]) Win64 Development -Project=`"$uproject`" -WaitMutex"
} else {
    Say '  2. Or build your editor target from your IDE first.'
}
$aiPrompt = Join-Path $here 'AI_INSTALL.md'
if (Test-Path -LiteralPath $aiPrompt) {
    Say "  3. Or let an AI coding assistant build it, run the scan and explain the findings: paste the prompt in $aiPrompt"
    Say '     into Claude Code, Copilot agent mode or Cursor, opened in your project folder.'
}
exit 0
