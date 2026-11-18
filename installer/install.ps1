#Requires -Version 5.1
<#
  Chat vs Streamer for GTA V installer. Double-click RunMe.bat, or run this directly.

  What it does:
    <game>\ChatVsStreamerGTAV.asi          the plugin, loaded by Script Hook V's ASI loader
    %LOCALAPPDATA%\ChatVsStreamerGTAV\     overlay, control panel, settings, points, your token

  If Script Hook V doesn't support your game version yet, the mod can run through RAGE Plugin
  Hook (RPH) instead, and this walks you through that: copying RPH out of your own LSPDFR
  download, the file that stops RPH's DirectStorage error on NVMe drives, and whether your
  account gets write access to the game folder. rph-files.ps1 does the copying, and adds the
  plugin in place of the .asi. Each step is skipped once it's done, so running this again
  only does what's left. See "Which version of GTA V?" in the README.

  What it doesn't do: download Script Hook V or RAGE Plugin Hook. Each game patch needs a
  matching build, and RPH's license forbids bundling it, so you get them from their own pages.
  For Script Hook V, this checks that it's there, and whether the page lists your game's version.

  The RPH questions are only asked when someone can answer them: at a console, or through
  -Answers. Otherwise this changes nothing a question would have decided, and says what to run.

  Run from the release folder (next to ChatVsStreamerGTAV.asi).
#>
param(
    [string]$GamePath,
    [string]$Channel,
    [string]$BotUsername,
    [string]$OAuthToken,
    [string]$BroadcasterToken,
    [switch]$SkipTwitchSetup,
    # Set the mod up through RAGE Plugin Hook, whatever Script Hook V's page says.
    [switch]$Rph,
    # LSPDFR's zip, if it isn't in your Downloads folder.
    [string]$RphZip,
    # Answers to the RPH questions, in the order they're asked, separated by commas. An empty one
    # is Enter. For running this with nobody at the keyboard, as the tests do.
    [string]$Answers,
    # Where to read which game versions Script Hook V supports: its page, or a saved copy of it.
    [string]$ScriptHookPage = "https://www.dev-c.com/gtav/scripthookv/"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptHookSite = "https://www.dev-c.com/gtav/scripthookv/" # where players get it, whatever -ScriptHookPage reads

$gameExes = "GTA5_Enhanced.exe", "GTA5.exe", "PlayGTAV.exe"

# What rph-files.ps1 -Install accepts: one exact zip, whose copy of RPH was built for one game
# version. Keep these in step with that script.
$rphZipName = "lspdfr_049_9695_Manual_Install_Enhanced_1158_16.zip"
$rphGameVersion = "1.0.1158.16"
$lspdfrPage = "https://www.lcpdfr.com/downloads/gta5mods/g17media/52970-lspd-first-response-enhanced-preview/"

function Write-Ok($text) { Write-Host "   [OK] $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "   [!!] $text" -ForegroundColor Yellow }

$atConsole = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
$givenAnswers = New-Object Collections.Generic.Queue[string]
if ($PSBoundParameters.ContainsKey("Answers")) {
    foreach ($answer in ($Answers -split ",")) { $givenAnswers.Enqueue($answer) }
}
$canAsk = $atConsole -or $givenAnswers.Count -gt 0

# The next answer: one given ahead of time, or what gets typed. $null means nobody is there to ask.
function Read-Answer($prompt) {
    if ($givenAnswers.Count -gt 0) {
        $answer = $givenAnswers.Dequeue()
        Write-Host "${prompt}: $answer"
        return $answer
    }
    if (-not $atConsole) { return $null }
    return Read-Host $prompt
}

# Enter means yes. With nobody there to ask, it's no.
function Read-YesNo($question) {
    while ($true) {
        $answer = Read-Answer "$question [Y/n]"
        if ($null -eq $answer) { return $false }
        if (-not $answer.Trim() -or $answer.Trim() -match '^(y|yes)$') { return $true }
        if ($answer.Trim() -match '^(n|no)$') { return $false }
        Write-Host "   Type y or n."
    }
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

# $true if the folder is on an NVMe drive, $false if it's on another kind, $null if Windows
# doesn't say, as with RAID or a network share.
function Test-NvmeDrive($path) {
    try {
        $letter = [IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $path).ProviderPath).Substring(0, 1)
        $bus = "$((Get-Partition -DriveLetter $letter -ErrorAction Stop | Get-Disk -ErrorAction Stop).BusType)"
    } catch {
        return $null
    }
    if ($bus -eq "NVMe") { return $true }
    if ($bus -in "SATA", "ATA", "ATAPI", "USB", "SD", "MMC", "SAS", "SCSI") { return $false }
    return $null
}

function Test-GameFolder($path) {
    if (-not $path -or -not (Test-Path $path)) { return $false }
    foreach ($exe in $gameExes) { if (Test-Path (Join-Path $path $exe)) { return $true } }
    return $false
}

function Find-GtaV {
    $candidates = New-Object System.Collections.ArrayList

    # Steam: every library folder.
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $lib = $m.Groups[1].Value -replace '\\\\', '\'
                [void]$candidates.Add((Join-Path $lib "steamapps\common\Grand Theft Auto V Enhanced"))
                [void]$candidates.Add((Join-Path $lib "steamapps\common\Grand Theft Auto V"))
            }
        }
    }

    # Rockstar launcher: registry, then the default folders.
    Get-ChildItem "HKLM:\SOFTWARE\WOW6432Node\Rockstar Games" -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -match "GTA|Grand Theft Auto" } |
        ForEach-Object {
            $dir = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).InstallFolder
            if ($dir) { [void]$candidates.Add($dir) }
        }
    [void]$candidates.Add("$env:ProgramFiles\Rockstar Games\Grand Theft Auto V Enhanced")
    [void]$candidates.Add("$env:ProgramFiles\Rockstar Games\Grand Theft Auto V")

    # Epic.
    $manifests = "C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path $manifests) {
        Get-ChildItem $manifests -Filter *.item -ErrorAction SilentlyContinue | ForEach-Object {
            $item = Get-Content $_.FullName -Raw | ConvertFrom-Json
            if ($item.DisplayName -match "Grand Theft Auto") { [void]$candidates.Add($item.InstallLocation) }
        }
    }

    $candidates | ForEach-Object { "$_".TrimEnd('\') } | Where-Object { Test-GameFolder $_ } | Select-Object -Unique
}

function Get-GameVersion($path) {
    foreach ($exe in "GTA5_Enhanced.exe", "GTA5.exe") {
        $file = Join-Path $path $exe
        if (Test-Path $file) { return (Get-Item $file).VersionInfo.FileVersion }
    }
    return $null
}

# $true if Script Hook V's page lists this game version, $false if it doesn't, $null if unknown.
function Test-ScriptHookSupport($version) {
    if (-not $version) { return $null }
    try {
        if ($ScriptHookPage -notmatch '^https?://') {
            $content = Get-Content -LiteralPath $ScriptHookPage -Raw
        } else {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $content = (Invoke-WebRequest -Uri $ScriptHookPage -UseBasicParsing -TimeoutSec 8).Content
        }
        return [bool]($content -match [regex]::Escape($version.Trim()))
    } catch {
        return $null
    }
}

Write-Host "`n=== Chat vs Streamer for GTA V ===`n" -ForegroundColor Cyan

if (-not $GamePath) {
    $found = @(Find-GtaV)
    if ($found.Count -eq 1) {
        $GamePath = $found[0]
    } elseif ($found.Count -gt 1) {
        Write-Host "Found more than one copy of GTA V:"
        for ($i = 0; $i -lt $found.Count; $i++) {
            Write-Host ("  {0}. {1} (version {2})" -f ($i + 1), $found[$i], (Get-GameVersion $found[$i]))
        }
        $choice = Read-Host "Install into which one? Enter a number"
        $index = 0
        if (-not [int]::TryParse($choice, [ref]$index) -or $index -lt 1 -or $index -gt $found.Count) { throw "'$choice' isn't one of the numbers listed." }
        $GamePath = $found[$index - 1]
    } else {
        $GamePath = Read-Host "Couldn't find GTA V. Paste its install folder (the one with the game's .exe in it)"
    }
}
$GamePath = $GamePath.Trim('"', ' ')
if (-not (Test-GameFolder $GamePath)) { throw "No GTA V executable in: $GamePath. Looked for: $($gameExes -join ', ')" }
Write-Host "Game: $GamePath"

$version = Get-GameVersion $GamePath
if ($version) { Write-Host "Version: $version" }

# Gets the mod into the game through RAGE Plugin Hook, doing whichever steps are still to do.
# Sets $script:rphReady once the mod is in. Otherwise it has stopped, having said why, for
# something only the player can do. It's called as a statement, never for a value, so that what
# rph-files.ps1 prints goes straight to the window.
function Install-ThroughRph {
    $rphFiles = Join-Path $here "rph-files.ps1"
    if (-not (Test-Path $rphFiles)) { throw "This release doesn't include the RAGE Plugin Hook version of the mod." }
    # Windows won't replace files that are in use.
    if (@(Get-Process -Name "GTA5_Enhanced", "GTA5", "RAGEPluginHook" -ErrorAction SilentlyContinue).Count -gt 0) {
        throw "Close the game and RAGE Plugin Hook first, then run this again. Nothing was changed."
    }
    $todo = @()

    # RPH itself, out of the player's own LSPDFR download.
    if (Test-Path (Join-Path $GamePath "RAGEPluginHook.exe")) {
        Write-Ok "RAGE Plugin Hook is installed, so the mod will run through it"
    } else {
        Write-Host "`nRAGE Plugin Hook" -ForegroundColor Cyan
        if ($version -and $version -ne $rphGameVersion) {
            Write-Warn "This installer can only add the copy of RPH built for game version $rphGameVersion, and yours is $version."
            Write-Host "        Look for a build that supports $version on LSPDFR's page: $lspdfrPage"
            Write-Host "        Copy RAGEPluginHook.exe and the files beside it into the game folder yourself, then run this again."
            return
        }
        if ($RphZip) {
            $zip = $RphZip.Trim('"', ' ')
        } else {
            $downloads = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path
            $zip = Join-Path $downloads $rphZipName
        }
        if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) {
            Write-Host "   RPH's license doesn't allow bundling it with this mod, so it comes out of your own download:"
            Write-Host "     1. Open LSPDFR's Enhanced Preview page: $lspdfrPage"
            Write-Host "     2. Download the manual install, named $(Split-Path -Leaf $zip)"
            Write-Host "     3. Leave it zipped, in: $(Split-Path -Parent $zip)"
            Write-Host "   Only RPH is copied out of it. LSPDFR itself stays out of your game."
            while (-not (Test-Path -LiteralPath $zip -PathType Leaf)) {
                $answer = Read-Answer "   Press Enter when the zip is there, or type skip to stop for now"
                if ($null -eq $answer -or $answer.Trim() -match '^(s|skip)$') { return }
                if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { Write-Warn "It isn't there yet: $zip" }
            }
        }
        Write-Ok "Found $(Split-Path -Leaf $zip)"
        $todo += "-Install", "-ZipPath", $zip
    }

    # With DirectStorage, which the game uses on NVMe drives, RPH stops at an error unless this file is there.
    if (-not (Test-Path (Join-Path $GamePath "skipdscheck"))) {
        $nvme = Test-NvmeDrive $GamePath
        if ($nvme) {
            Write-Host "   The game is on an NVMe drive, where RPH stops with a DirectStorage error. A file named skipdscheck prevents that, so it goes in too."
        } elseif ($null -eq $nvme) {
            Write-Host "   Windows didn't say what kind of drive the game is on. On an NVMe drive RPH stops with a DirectStorage error, so the file that prevents it, skipdscheck, goes in too."
        }
        if ($nvme -ne $false) { $todo += "-SkipDsCheck" }
    }

    # RPH writes into the game folder from inside the game, which Program Files only lets admins do.
    if (Test-Writable $GamePath) {
        Write-Ok "Your account can write to the game folder, so nothing needs admin rights"
    } else {
        Write-Host "`nAdmin rights" -ForegroundColor Cyan
        Write-Host "   RPH writes its log into the game folder from inside the game, and Windows only lets admins write there."
        Write-Host "     1. Give your account write access to the game folder. Recommended: nothing needs admin rights"
        Write-Host "        after that. The cost is that any program running under your account could then change the"
        Write-Host "        game's files, so never start the game or RPH with Run as administrator afterwards."
        Write-Host "        rph-files.ps1 -RevokeWrite undoes it."
        Write-Host "     2. Leave the folder as it is. StartGame.bat then asks for admin rights every time, and the"
        Write-Host "        whole game runs as admin, including the code that reads Twitch chat."
        $choice = $null
        while ($choice -notin "1", "2") {
            $choice = Read-Answer "   Type 1 or 2"
            if ($null -eq $choice) { break }
            $choice = $choice.Trim()
        }
        if ($choice -eq "1") {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $rphFiles -AllowWrite -GamePath $GamePath
            if ($LASTEXITCODE -ne 0 -or -not (Test-Writable $GamePath)) {
                Write-Warn "Your account still can't write to the game folder, so the next step asks for admin rights instead."
            }
        } elseif ($null -eq $choice) {
            Write-Host "   Nobody is here to choose, so the folder stays as it is. For the first option, run: rph-files.ps1 -AllowWrite"
        }
    }

    # One call, so there's at most one more admin prompt.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $rphFiles @todo -Mod -GamePath $GamePath
    if ($LASTEXITCODE -ne 0) { throw "Couldn't add the mod for RAGE Plugin Hook. The reason is above." }
    $script:rphReady = $true
}

# Script Hook V has to be there for the plugin to load at all, and it only runs on game versions
# it knows about, which is the usual reason a mod stops working after a patch. RPH is the other
# way in.
$useRph = $Rph -or (Test-Path (Join-Path $GamePath "RAGEPluginHook.exe"))
$supported = $null
if (-not $useRph) {
    $supported = Test-ScriptHookSupport $version
    if ($supported -eq $false) {
        Write-Warn "Script Hook V's page doesn't list version $version, so it can't load mods into this game yet."
        # A release built without RPH's SDK has no RPH version of the mod to offer.
        $hasRphBuild = Test-Path (Join-Path $here "rph-files.ps1")
        if ($hasRphBuild -and $version -eq $rphGameVersion) {
            Write-Host "`nRAGE Plugin Hook (RPH) is another way in, and this mod runs through it too. It takes a few more"
            Write-Host "steps, which this installer does with you: getting RPH out of LSPDFR's download, a fix for NVMe"
            Write-Host "drives, and letting the game run without admin rights."
            if ($canAsk) { $useRph = Read-YesNo "Set the mod up through RAGE Plugin Hook?" }
            else { Write-Host "To do that, run: install.ps1 -Rph" }
        } elseif ($hasRphBuild) {
            Write-Host "        If you add a copy of RAGE Plugin Hook that supports $version yourself, run this again and the mod will use it."
        }
        if (-not $useRph) {
            Write-Host "        Until a new release of Script Hook V adds this version, the mod won't load through it."
            Write-Host "        Keep checking: $scriptHookSite"
            Write-Host "        Nothing here needs reinstalling after that, apart from replacing Script Hook V's files."
        }
    }
}

if ($useRph) {
    $script:rphReady = $false
    Install-ThroughRph
    if (-not $script:rphReady) {
        Write-Host "`nNot finished: the mod isn't in the game yet. Run this again when you're ready, and it carries on from here.`n" -ForegroundColor Yellow
        exit 1
    }
} else {
    $asi = Join-Path $here "ChatVsStreamerGTAV.asi"
    if (-not (Test-Path $asi)) { throw "ChatVsStreamerGTAV.asi not found next to install.ps1" }

    if ($supported -eq $true) {
        Write-Host "   [OK] Script Hook V's page lists this version" -ForegroundColor Green
    } elseif ($null -eq $supported -and $version) {
        Write-Host "   [??] Couldn't check Script Hook V's page. Make sure it lists version $version." -ForegroundColor Yellow
    }

    $hasHook = Test-Path (Join-Path $GamePath "ScriptHookV.dll")
    $hasLoader = (Test-Path (Join-Path $GamePath "dinput8.dll")) -or (Test-Path (Join-Path $GamePath "xinput1_4.dll"))
    if ($hasHook -and $hasLoader) {
        Write-Host "   [OK] Script Hook V is installed" -ForegroundColor Green
    } else {
        Write-Host "   [!!] Script Hook V isn't installed. The mod won't load without it." -ForegroundColor Yellow
        if ($supported -ne $false) {
            Write-Host "        1. Download the latest version from $scriptHookSite"
            Write-Host "        2. Copy ScriptHookV.dll and dinput8.dll from its bin folder into:"
            Write-Host "           $GamePath"
        }
    }

    # The plugin. The default Rockstar folder needs admin rights to write to.
    try {
        Copy-Item $asi $GamePath -Force
        Write-Host "   [OK] ChatVsStreamerGTAV.asi copied" -ForegroundColor Green
    } catch [System.UnauthorizedAccessException] {
        throw "Windows wouldn't let me write to $GamePath. Right-click RunMe.bat, choose Run as administrator, and try again."
    }
}

# Data folder: keep the user's settings, points and token. Refresh the web pages.
$dataDir = Join-Path $env:LOCALAPPDATA "ChatVsStreamerGTAV"
New-Item -ItemType Directory $dataDir -Force | Out-Null
Copy-Item (Join-Path $here "overlay.html") $dataDir -Force
Copy-Item (Join-Path $here "panel.html") $dataDir -Force
Write-Host "   [OK] Data folder: $dataDir" -ForegroundColor Green

# The mod writes a blank copy of this file on its first run, so it being there doesn't mean it's filled in.
$ini = Join-Path $dataDir "ChatVsStreamer.ini"
$twitchIsSet = (Test-Path $ini) -and
               @(Get-Content $ini | Where-Object { $_ -match '^\s*Channel\s*=\s*\S' }).Count -gt 0 -and
               @(Get-Content $ini | Where-Object { $_ -match '^\s*OAuthToken\s*=\s*\S' }).Count -gt 0
if (-not $twitchIsSet -and -not $SkipTwitchSetup) {
    Write-Host "`nTwitch setup" -ForegroundColor Cyan
    if (-not $Channel) { $Channel = Read-Host "  Your Twitch channel name" }
    if (-not $BotUsername) { $BotUsername = Read-Host "  Account the mod replies as (Enter = same as channel)" }
    if (-not $BotUsername) { $BotUsername = $Channel }
    if (-not $OAuthToken) {
        Write-Host "  Get a chat token for '$BotUsername' with the chat:read and chat:edit scopes (for example at https://twitchtokengenerator.com)."
        Write-Host "  The token stays hidden as you paste it, in case this window is on stream."
        $secure = Read-Host "  Paste the OAuth token" -AsSecureString
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        try { $OAuthToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
    if (-not $BroadcasterToken) {
        Write-Host "  Optional: for channel point rewards to give chaos points, a token for '$Channel' itself (not the bot)"
        Write-Host "  with the channel:read:redemptions scope. Press Enter to skip. You can add it to the file later."
        $secure = Read-Host "  Paste the channel's token" -AsSecureString
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        try { $BroadcasterToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
    # ASCII on purpose: Windows' INI reader trips over a UTF-8 byte order mark.
    @"
; Chat vs Streamer for GTA V: boot settings. Keep this file off stream, it holds your tokens.
; Prices, toggles and economy settings are in the control panel: http://localhost:7373/panel

[Twitch]
Channel=$Channel
BotUsername=$BotUsername
OAuthToken=$OAuthToken
; Optional, so channel point rewards give chaos points: a token for your channel's own account
; (not the bot's) with the channel:read:redemptions scope. The rewards are in channel_point_rewards.txt.
BroadcasterToken=$BroadcasterToken

[Ports]
Web=7373
"@ | Set-Content -Path $ini -Encoding ASCII
    Write-Host "   [OK] Saved $ini" -ForegroundColor Green
} elseif ($twitchIsSet -and @(Get-Content $ini | Where-Object { $_ -match '^\s*BroadcasterToken\s*=\s*\S' }).Count -eq 0) {
    Write-Host "`n  Channel point rewards can give chaos points: add a BroadcasterToken= line under [Twitch] in" -ForegroundColor Cyan
    Write-Host "  $ini (off stream), with a token for your channel's own account and the"
    Write-Host "  channel:read:redemptions scope. The rewards and amounts are in channel_point_rewards.txt."
}

Write-Host "`nNearly there:" -ForegroundColor Cyan
if ($useRph) {
    Write-Host "  1. Story mode only. Turn BattlEye off in the Rockstar Games Launcher's settings for GTA V."
    Write-Host "     Start the game with StartGame.bat in this folder, not the launcher's Play button."
} else {
    Write-Host "  1. Story mode only. Script Hook V closes the game if you go online."
    Write-Host "     If story mode won't load mods, turn BattlEye off in the Rockstar Games Launcher's"
    Write-Host "     settings for GTA V. Online play needs it back on, and the .asi removed first."
}
Write-Host "  2. OBS browser source:  http://localhost:7373/       (1920x1080)"
Write-Host "  3. Control panel:       http://localhost:7373/panel  (prices, toggles, test buttons)"
Write-Host "     Both come up shortly after the game starts."
Write-Host "  4. Log: $dataDir\ChatVsStreamer.log`n"
if ($useRph) {
    Write-Host "Before going online, run rph-files.ps1 -Remove and turn BattlEye back on.`n" -ForegroundColor Yellow
} else {
    Write-Host "Before going online, delete $GamePath\ChatVsStreamerGTAV.asi and Script Hook V's files.`n" -ForegroundColor Yellow
}
