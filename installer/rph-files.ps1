#Requires -Version 5.1
<#
  Adds RAGE Plugin Hook and this mod to the game folder, or takes them back out.

    rph-files.ps1 -Install              copy RPH out of your own LSPDFR download
    rph-files.ps1 -Mod                  add this mod, and have RPH load it when the game starts
    rph-files.ps1 -Plugin <file.dll>    add a different plugin, and have RPH load that instead
    rph-files.ps1 -SkipDsCheck          add the empty file that silences RPH's DirectStorage message
    rph-files.ps1 -Remove               delete what the others added
    rph-files.ps1 -Start                start the game through RPH (what StartGame.bat runs)
    rph-files.ps1 -AllowWrite           let your account write to the game folder, so nothing needs admin rights
    rph-files.ps1 -RevokeWrite          undo -AllowWrite

  -Install, -Mod, -Plugin and -SkipDsCheck can go together, except that -Mod and -Plugin both set
  which plugin loads, so use one of them. RPH's readme forbids redistributing it, so this mod
  never ships it.

  -AllowWrite is the safer way to run this mod from Program Files, because the alternative runs
  the game and the code that reads Twitch chat as admin. The cost is that any program running
  under your account can then change the game's files, so after this, never start RPH or the
  game with Run as administrator. -Remove undoes it too.

  -Install copies only the zip's top-level files: RAGEPluginHook.exe and its libraries. It also
  creates an empty Plugins folder. LSPDFR itself (the zip's lspdfr and plugins folders) stays
  out. If any of those file names already exist in the game folder, it stops without changing
  anything.

  -Mod also works with a copy of RPH this script didn't install, and -Remove then leaves that
  copy alone. It takes its files from the rph folder next to this script.

  -SkipDsCheck only silences RPH's message. It doesn't fix whatever caused it.

  -Remove deletes a file only if it's still what this script put there, plus the files RPH
  creates as it runs, if they weren't already there before the install. It reports anything else
  that has appeared since, and leaves it alone.

  -Start exists because RPH writes its log into the game folder from inside the game. In Program
  Files that takes admin rights, and the game only gets them if the launcher that starts it has
  them, so RPH has to open the launcher itself. -Start checks for that and handles the prompt.
  It also clears what the last run left behind, because on an NVMe drive RPH crashes on start-up
  if its old log is still there. The log and any crash report go to the data folder.

  Changing the game folder needs admin rights, so Windows will ask for permission.
  Run -Remove before turning BattlEye back on or going into GTA Online.
#>
param(
    [switch]$Install,
    [switch]$Remove,
    [switch]$Mod,
    [switch]$Start,
    [switch]$AllowWrite,
    [switch]$RevokeWrite,
    # Whose access -AllowWrite and -RevokeWrite change. Passed along so that the admin prompt
    # can't change the answer if it's accepted with a different account.
    [string]$ForUser = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
    [string]$GamePath = (Join-Path $env:ProgramFiles "Rockstar Games\Grand Theft Auto V Enhanced"),
    [string]$ZipPath,
    [string]$Plugin,
    [switch]$SkipDsCheck,
    # Where -Mod finds its two DLLs, and where it puts the overlay and control panel.
    [string]$ModFolder = (Join-Path $PSScriptRoot "rph"),
    [string]$DataDir = (Join-Path $env:LOCALAPPDATA "ChatVsStreamerGTAV"),
    # The zip that was scanned before this script was written. A different zip needs its own check.
    [string]$ExpectedSha256 = "8524E534F219C7F33634ED45CED055F3C7B0B461FA8302FF2F0AC46470C7ED34",
    [switch]$PauseAtEnd
)

$ErrorActionPreference = "Stop"
$zipName = "lspdfr_049_9695_Manual_Install_Enhanced_1158_16.zip"
$manifestName = "ChatVsStreamer_RPH_manifest.txt"
$required = "RAGEPluginHook.exe", "XInput1_4.dll"
$modPlugin = "ChatVsStreamer.dll"
$modCore = "ChatVsStreamerCore.dll"
$webPages = "overlay.html", "panel.html"
# RPH creates these in the game folder as it runs.
$rphRuntimeItems = "RagePluginHook.log", "RagePluginHook.ini", "commandline.txt", "rphhs", "ExecuteCode", "crashreport"

function Write-Ok($text) { Write-Host "   [OK] $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "   [!!] $text" -ForegroundColor Yellow }

function Test-Admin {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-Writable($dir) {
    $probe = Join-Path $dir ("cvs_write_test_{0}.tmp" -f [guid]::NewGuid().ToString("N"))
    try {
        $stream = [IO.File]::Open($probe, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Dispose()
        [IO.File]::Delete($probe)
        return $true
    } catch {
        $inner = $_.Exception.InnerException
        if ($_.Exception -is [UnauthorizedAccessException] -or $inner -is [UnauthorizedAccessException]) { return $false }
        throw
    }
}

function Invoke-Elevated {
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -GamePath `"$GamePath`" -PauseAtEnd"
    if ($Install) { $argLine += " -Install -ZipPath `"$ZipPath`" -ExpectedSha256 $ExpectedSha256" }
    if ($Mod) { $argLine += " -Mod -ModFolder `"$ModFolder`" -DataDir `"$DataDir`"" }
    if ($Plugin) { $argLine += " -Plugin `"$Plugin`"" }
    if ($SkipDsCheck) { $argLine += " -SkipDsCheck" }
    if ($Remove) { $argLine += " -Remove" }
    if ($AllowWrite) { $argLine += " -AllowWrite -ForUser $ForUser" }
    if ($RevokeWrite) { $argLine += " -RevokeWrite -ForUser $ForUser" }
    if ($Start) {
        $argLine += " -Start -DataDir `"$DataDir`""
        Write-Host "Windows will ask for permission, because RAGE Plugin Hook needs admin rights in this folder."
    } else {
        Write-Host "Windows will ask for permission to change the game folder. The results appear in a new window."
    }
    try {
        # -Start doesn't wait, because the game it starts could keep this window open until you quit.
        # If it fails, the admin window stays open to say why.
        if ($Start) {
            Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argLine
            return
        }
        $process = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argLine -Wait -PassThru
    } catch {
        throw "Didn't get admin rights, so nothing was changed. ($($_.Exception.Message))"
    }
    if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) { throw "That didn't finish. The admin window said why." }
}

function Invoke-Install {
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $manifest = Join-Path $GamePath $manifestName
    if (Test-Path -LiteralPath $manifest) { throw "This script has already installed RPH here. Run -Remove first." }
    if (-not (Test-Path -LiteralPath $ZipPath)) { throw "Can't find the LSPDFR zip at: $ZipPath" }

    $hash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
    if ($hash -ne $ExpectedSha256) {
        throw "This isn't the zip that was checked (SHA-256 $hash). Nothing was changed."
    }
    Write-Ok "The zip matches the copy that was scanned"

    $pluginsDir = Join-Path $GamePath "Plugins"
    $makePlugins = -not (Test-Path -LiteralPath $pluginsDir)

    $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entries = @($zip.Entries | Where-Object { $_.Name -and $_.FullName -notmatch '[\\/]' })
        foreach ($name in $required) {
            if (-not ($entries | Where-Object { $_.Name -eq $name })) { throw "$name isn't at the top of the zip. Nothing was changed." }
        }

        $clashes = @($entries | Where-Object { Test-Path -LiteralPath (Join-Path $GamePath $_.Name) } | ForEach-Object { $_.Name })
        if ($clashes.Count -gt 0) {
            throw "Already in the game folder, so nothing was changed: $($clashes -join ', ')"
        }

        # Written before copying, so -Remove can still clean up if a copy fails part-way.
        $lines = New-Object Collections.Generic.List[string]
        foreach ($item in Get-ChildItem -LiteralPath $GamePath -Force) { $lines.Add("existing`t$($item.Name)") }
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            foreach ($entry in $entries) {
                $stream = $entry.Open()
                try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "") }
                finally { $stream.Dispose() }
                $lines.Add("file`t$($entry.Name)`t$digest")
            }
        } finally { $sha.Dispose() }
        if ($makePlugins) { $lines.Add("dir`tPlugins") }
        Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII

        foreach ($entry in $entries) {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $GamePath $entry.Name), $false)
        }
        Write-Ok "Copied $($entries.Count) files. LSPDFR itself was left out"
    } finally {
        $zip.Dispose()
    }

    if ($makePlugins) {
        New-Item -ItemType Directory -Path $pluginsDir | Out-Null
        Write-Ok "Created an empty Plugins folder"
    }
}

# For a copy of RPH this script didn't install: everything already there counts as not ours.
function New-ManifestIfMissing {
    $manifest = Join-Path $GamePath $manifestName
    if (Test-Path -LiteralPath $manifest) { return }
    $lines = @(Get-ChildItem -LiteralPath $GamePath -Force | ForEach-Object { "existing`t$($_.Name)" })
    Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII
}

# Writes a file this script is responsible for, recording it first so -Remove knows about it.
# Returns $false, changing nothing, if the path holds a file this script didn't put there.
function Set-TrackedFile($relative, [byte[]]$bytes) {
    $manifest = Join-Path $GamePath $manifestName
    $path = Join-Path $GamePath $relative
    $lines = @(Get-Content -LiteralPath $manifest)
    $tracked = @($lines | Where-Object { $parts = $_ -split "`t"; $parts[0] -eq "file" -and $parts[1] -eq $relative })
    if ((Test-Path -LiteralPath $path) -and $tracked.Count -eq 0) { return $false }

    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace("-", "") }
    finally { $sha.Dispose() }

    $lines = @($lines | Where-Object { $tracked -notcontains $_ }) + "file`t$relative`t$digest"
    Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII
    [IO.File]::WriteAllBytes($path, $bytes)
    return $true
}

function Set-TrackedDir($relative) {
    $path = Join-Path $GamePath $relative
    if (Test-Path -LiteralPath $path) { return }
    Add-Content -LiteralPath (Join-Path $GamePath $manifestName) -Value "dir`t$relative" -Encoding ASCII
    New-Item -ItemType Directory -Path $path | Out-Null
}

function Set-StartupPlugin($name) {
    if (Set-TrackedFile "startup.rphs" ([Text.Encoding]::ASCII.GetBytes("LoadPlugin $name`r`n"))) {
        Write-Ok "RPH should load it when the game starts. If it doesn't, press F4 in game and type: LoadPlugin $name"
    } else {
        Write-Warn "startup.rphs already exists and was left alone. Press F4 in game and type: LoadPlugin $name"
    }
}

function Install-Mod {
    New-ManifestIfMissing
    Set-TrackedDir "Plugins"
    # In its own folder, so RPH doesn't take the core for a plugin.
    Set-TrackedDir "Plugins\ChatVsStreamer"

    $files = @{ "Plugins\$modPlugin" = $modPlugin; "Plugins\ChatVsStreamer\$modCore" = $modCore }
    foreach ($target in $files.Keys) {
        if (-not (Set-TrackedFile $target ([IO.File]::ReadAllBytes((Join-Path $ModFolder $files[$target]))))) {
            throw "$target is already there and this script didn't add it, so it was left alone."
        }
    }
    Write-Ok "Added the mod to the Plugins folder"
    Set-StartupPlugin $modPlugin

    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    foreach ($page in $webPages) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $page) -Destination $DataDir -Force }
    Write-Ok "Updated the overlay and control panel in $DataDir"
}

function Install-Plugin {
    if (-not (Test-Path -LiteralPath (Join-Path $GamePath "Plugins"))) { throw "There's no Plugins folder in $GamePath" }

    $name = Split-Path -Leaf $Plugin
    if (-not (Set-TrackedFile "Plugins\$name" ([IO.File]::ReadAllBytes($Plugin)))) {
        throw "Plugins\$name is already there and this script didn't add it, so it was left alone."
    }
    Write-Ok "Added Plugins\$name"
    Set-StartupPlugin $name
}

# RPH's own message names this empty file as the way to silence it.
function Install-SkipDsCheck {
    New-ManifestIfMissing
    if (Set-TrackedFile "skipdscheck" ([byte[]]@())) {
        Write-Ok "Added skipdscheck, so RPH shouldn't stop at its DirectStorage message"
    } else {
        Write-Warn "skipdscheck is already there and this script didn't add it, so it was left alone"
    }
}

function Start-Game {
    # With DirectStorage on (NVMe drives), the game holds open every file already in its folder.
    # RPH's crash report shows what that does: starting up, it opens the last run's log, gets
    # refused, and the game crashes with a misleading "Could not hook game process". So what RPH
    # writes from inside the game has to be gone before the game starts. The log and crash report
    # are worth keeping, so they go to the data folder.
    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $keep = @{ "RagePluginHook.log" = "RagePluginHook.previous.log"; "crashreport" = "crashreport.previous.txt" }
    foreach ($name in $keep.Keys) {
        $path = Join-Path $GamePath $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try {
            # Copied rather than moved, so the copy gets the data folder's permissions.
            Copy-Item -LiteralPath $path -Destination (Join-Path $DataDir $keep[$name]) -Force
            Remove-Item -LiteralPath $path -Force
        } catch {
            Write-Warn "Couldn't clear the last run's $name. ($($_.Exception.Message))"
        }
    }
    foreach ($name in "ExecuteCode", "rphhs") {
        $path = Join-Path $GamePath $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try { Remove-Item -LiteralPath $path -Recurse -Force }
        catch { Write-Warn "Couldn't clear the last run's $name. ($($_.Exception.Message))" }
    }
    Start-Process -FilePath (Join-Path $GamePath "RAGEPluginHook.exe") -WorkingDirectory $GamePath
    Write-Ok "Started RAGE Plugin Hook. It opens the launcher and the game by itself."
}

# Returns $false if Windows refuses, which is what to expect without admin rights.
function Grant-WriteAccess($sid) {
    & icacls.exe $GamePath /grant "*${sid}:(OI)(CI)M" /Q *> $null
    return $LASTEXITCODE -eq 0
}

function Revoke-WriteAccess($sid) {
    & icacls.exe $GamePath /remove:g "*$sid" /Q *> $null
    return $LASTEXITCODE -eq 0
}

function Get-RecordedGrants {
    $manifest = Join-Path $GamePath $manifestName
    if (-not (Test-Path -LiteralPath $manifest)) { return @() }
    return @(Get-Content -LiteralPath $manifest | Where-Object { $_ -like "grant`t*" } | ForEach-Object { ($_ -split "`t")[1] })
}

function Set-GrantRecord($sid, [bool]$present) {
    New-ManifestIfMissing
    $manifest = Join-Path $GamePath $manifestName
    $lines = @(Get-Content -LiteralPath $manifest | Where-Object { $_ -ne "grant`t$sid" })
    if ($present) { $lines += "grant`t$sid" }
    Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII
}

# RPH rewrites startup.rphs when it runs, so a changed copy is still ours if it only loads plugins.
function Test-OnlyLoadsPlugins($path) {
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line.Trim() -and $line.Trim() -notmatch '^LoadPlugin\s') { return $false }
    }
    return $true
}

function Invoke-Remove {
    $manifest = Join-Path $GamePath $manifestName
    if (-not (Test-Path -LiteralPath $manifest)) {
        Write-Host "Nothing to remove: this script has no install record in $GamePath"
        return
    }

    $known = @{}
    $removed = 0
    $left = 0
    $dirs = New-Object Collections.Generic.List[string]

    foreach ($line in Get-Content -LiteralPath $manifest) {
        $parts = $line -split "`t"
        $known[$parts[1]] = $true
        if ($parts[0] -eq "dir") { $dirs.Add($parts[1]); continue }
        if ($parts[0] -ne "file") { continue }

        $path = Join-Path $GamePath $parts[1]
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $parts[2] -and
            -not ($parts[1] -eq "startup.rphs" -and (Test-OnlyLoadsPlugins $path))) {
            Write-Warn "Left in place because it has changed: $($parts[1])"
            $left++
            continue
        }
        try {
            Remove-Item -LiteralPath $path
            $removed++
        } catch {
            Write-Warn "Couldn't delete $($parts[1]). Is the game or RPH still running? ($($_.Exception.Message))"
            $left++
        }
    }

    foreach ($name in $rphRuntimeItems) {
        $path = Join-Path $GamePath $name
        if ($known.ContainsKey($name) -or -not (Test-Path -LiteralPath $path)) { continue }
        try {
            Remove-Item -LiteralPath $path -Recurse -Force
            $removed++
            $known[$name] = $true
        } catch {
            Write-Warn "Couldn't delete $name. Is the game or RPH still running? ($($_.Exception.Message))"
            $left++
            $known[$name] = $true
        }
    }

    # Deepest first, because a folder can only go once what's inside it has.
    foreach ($dir in ($dirs | Sort-Object Length -Descending)) {
        $path = Join-Path $GamePath $dir
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $contents = @(Get-ChildItem -LiteralPath $path -Force)
        if ($contents.Count -eq 0) {
            Remove-Item -LiteralPath $path
            Write-Ok "Removed the empty $dir folder"
        } else {
            Write-Warn "Left the $dir folder because it isn't empty: $(($contents | ForEach-Object { $_.Name }) -join ', ')"
            $left++
        }
    }

    Write-Ok "Deleted $removed items"

    $others = @(Get-ChildItem -LiteralPath $GamePath -Force | Where-Object { -not $known.ContainsKey($_.Name) -and $_.Name -ne $manifestName })
    if ($others.Count -gt 0) {
        Write-Warn "These appeared after the install and weren't touched: $(($others | ForEach-Object { $_.Name }) -join ', ')"
    }

    if ($left -eq 0) {
        Remove-Item -LiteralPath $manifest
        Write-Ok "Everything this script added is out of the game folder"
    } else {
        Write-Warn "It isn't fully out yet. Sort out the items above, then run -Remove again before going online."
    }
}

$exitCode = 0
try {
    if (-not ($Install -or $Mod -or $Plugin -or $SkipDsCheck -or $Remove -or $Start -or $AllowWrite -or $RevokeWrite)) { throw "Use -Install, -Mod, -Plugin, -SkipDsCheck, -Remove, -Start, -AllowWrite or -RevokeWrite." }
    if (($AllowWrite -or $RevokeWrite) -and ($Install -or $Mod -or $Plugin -or $SkipDsCheck -or $Remove -or $Start)) { throw "Use -AllowWrite or -RevokeWrite on its own." }
    if ($AllowWrite -and $RevokeWrite) { throw "Use -AllowWrite or -RevokeWrite, not both." }
    if ($Remove -and ($Install -or $Mod -or $Plugin -or $SkipDsCheck -or $Start)) { throw "Use -Remove on its own." }
    if ($Start -and ($Install -or $Mod -or $Plugin -or $SkipDsCheck)) { throw "Use -Start on its own." }
    if ($Mod -and $Plugin) { throw "-Mod and -Plugin both set which plugin loads, so use one of them." }

    $GamePath = $GamePath.Trim('"', ' ')
    $exe = @("GTA5_Enhanced.exe", "GTA5.exe") | Where-Object { Test-Path -LiteralPath (Join-Path $GamePath $_) } | Select-Object -First 1
    if (-not $exe) { throw "No GTA V executable in: $GamePath" }

    if ($Install -and -not $ZipPath) {
        $downloads = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path
        $ZipPath = Join-Path $downloads $zipName
    }

    # Don't ask for admin rights when there's nothing to do.
    $installed = Test-Path -LiteralPath (Join-Path $GamePath $manifestName)
    if ($Install -and -not (Test-Path -LiteralPath $ZipPath)) {
        throw "Can't find the LSPDFR zip at: $ZipPath"
    }
    if ($Mod) {
        if (-not $Install -and -not (Test-Path -LiteralPath (Join-Path $GamePath "RAGEPluginHook.exe"))) { throw "RPH isn't in the game folder. Add -Install, or install it first." }
        foreach ($name in $modPlugin, $modCore) {
            if (-not (Test-Path -LiteralPath (Join-Path $ModFolder $name))) { throw "Can't find $name in: $ModFolder" }
        }
        foreach ($page in $webPages) {
            if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $page))) { throw "Can't find $page next to this script. Run the copy in the dist folder." }
        }
        # Windows won't replace a DLL that's in use.
        if (@(Get-Process -Name "GTA5_Enhanced", "GTA5", "RAGEPluginHook" -ErrorAction SilentlyContinue).Count -gt 0) {
            throw "Close the game and RAGE Plugin Hook first. Nothing was changed."
        }
    }
    if ($Plugin) {
        if (-not (Test-Path -LiteralPath $Plugin -PathType Leaf)) { throw "Can't find the plugin at: $Plugin" }
        $Plugin = (Resolve-Path -LiteralPath $Plugin).Path
        if (-not $Install -and -not $installed) { throw "This script hasn't installed RPH here yet. Add -Install." }
    }
    if ($SkipDsCheck -and -not $Install -and -not (Test-Path -LiteralPath (Join-Path $GamePath "RAGEPluginHook.exe"))) { throw "RPH isn't in the game folder. Add -Install, or install it first." }
    if ($Start) {
        if (-not (Test-Path -LiteralPath (Join-Path $GamePath "RAGEPluginHook.exe"))) { throw "RPH isn't in: $GamePath" }
        if (@(Get-Process -Name "GTA5_Enhanced", "GTA5", "RAGEPluginHook" -ErrorAction SilentlyContinue).Count -gt 0) {
            throw "The game or RAGE Plugin Hook is already running."
        }
        # RPH hands the launch to a launcher that's already open, and the game inherits its rights.
        # From a shell without admin rights, Path can only be read for a launcher without them too.
        if ((Test-Writable $GamePath) -and -not (Test-Admin)) {
            if (@(Get-Process -Name "Launcher" -ErrorAction SilentlyContinue | Where-Object { -not $_.Path }).Count -gt 0) {
                throw "Quit the Rockstar launcher first. If its icon is by the clock, right-click it and exit from there. It's still running with admin rights from an earlier launch, and the game would inherit them."
            }
        }
    }

    # -Remove takes the access back first, because after that it may be the admin prompt that finishes the job.
    $accessRefused = $false
    if ($Remove) {
        foreach ($sid in (Get-RecordedGrants)) {
            if (-not (Revoke-WriteAccess $sid)) { $accessRefused = $true }
        }
    }

    if ($AllowWrite -or $RevokeWrite) {
        $changed = if ($AllowWrite) { Grant-WriteAccess $ForUser } else { Revoke-WriteAccess $ForUser }
        if ($changed) {
            Set-GrantRecord $ForUser ([bool]$AllowWrite)
            if ($AllowWrite) {
                Write-Ok "Your account can now write to the game folder, so nothing needs admin rights."
                Write-Host "Start the game with StartGame.bat. From now on, don't start RPH or the game with Run as administrator."
                Write-Host "To undo this, run this script with -RevokeWrite."
            } else {
                Write-Ok "Your account's write access to the game folder has been taken away."
                Write-Host "StartGame.bat will ask for admin rights again, and the game will run as admin."
            }
        } elseif (Test-Admin) {
            throw "Even with admin rights, Windows wouldn't change the permissions on: $GamePath"
        } else {
            Invoke-Elevated
        }
    } elseif ($accessRefused -and -not (Test-Admin)) {
        Invoke-Elevated
    } elseif ($accessRefused) {
        throw "Even with admin rights, Windows wouldn't change the permissions on: $GamePath. Nothing was removed."
    } elseif ($Remove -and -not $installed) {
        Write-Host "Nothing to remove: this script has no install record in $GamePath"
    } elseif (Test-Writable $GamePath) {
        $version = (Get-Item -LiteralPath (Join-Path $GamePath $exe)).VersionInfo.FileVersion
        Write-Host "`n=== RAGE Plugin Hook ===`n" -ForegroundColor Cyan
        Write-Host "Game: $GamePath"
        if ($version) { Write-Host "Version: $version" }
        if ($Install) { Invoke-Install }
        if ($Mod) { Install-Mod }
        if ($Plugin) { Install-Plugin }
        if ($SkipDsCheck) { Install-SkipDsCheck }
        if ($Start) { Start-Game }
        elseif ($Remove) {
            Invoke-Remove
        } else {
            Write-Host "`nStory mode only. Run this script with -Remove before turning BattlEye back on or going into GTA Online." -ForegroundColor Yellow
        }
    } elseif (Test-Admin) {
        throw "Even with admin rights, Windows won't allow changes in: $GamePath"
    } else {
        if ($Start) {
            # From here, Path is only readable for a launcher running without admin rights. RPH
            # would hand the launch to it, and the game would crash when RPH tried to write its log.
            $plainLauncher = @(Get-Process -Name "Launcher" -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "*\Rockstar Games\*" })
            if ($plainLauncher.Count -gt 0) {
                throw "Quit the Rockstar launcher first. If its icon is by the clock, right-click it and exit from there. RAGE Plugin Hook opens it again by itself, with the admin rights the game needs in this folder."
            }
        }
        Invoke-Elevated
    }
} catch {
    Write-Host "`n$($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}

# A successful -Start has nothing to read, and the window would only be in the way of the game.
if ($PauseAtEnd -and (-not $Start -or $exitCode -ne 0)) {
    Write-Host ""
    Read-Host "Press Enter to close" | Out-Null
}
exit $exitCode
