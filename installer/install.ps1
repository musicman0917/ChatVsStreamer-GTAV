#Requires -Version 5.1
<#
  Chat vs Streamer for GTA V installer. Double-click RunMe.bat, or run this directly.

  What it does:
    <game>\ChatVsStreamerGTAV.asi          the plugin, loaded by Script Hook V's ASI loader
    %LOCALAPPDATA%\ChatVsStreamerGTAV\     overlay, control panel, settings, points, your token

  If RAGE Plugin Hook is in the game folder, the mod runs through that instead, for game
  versions Script Hook V doesn't support yet. rph-files.ps1 then adds the plugin in place
  of the .asi. See "Which version of GTA V?" in the README.

  What it doesn't do: download Script Hook V or RAGE Plugin Hook. Each game patch needs a
  matching build, so you get them from their own pages. For Script Hook V, this checks that
  it's there, and whether the page lists your game's version.

  Run from the release folder (next to ChatVsStreamerGTAV.asi).
#>
param(
    [string]$GamePath,
    [string]$Channel,
    [string]$BotUsername,
    [string]$OAuthToken,
    [string]$BroadcasterToken,
    [switch]$SkipTwitchSetup
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptHookPage = "https://www.dev-c.com/gtav/scripthookv/"

$gameExes = "GTA5_Enhanced.exe", "GTA5.exe", "PlayGTAV.exe"

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
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $page = Invoke-WebRequest -Uri $scriptHookPage -UseBasicParsing -TimeoutSec 8
        return [bool]($page.Content -match [regex]::Escape($version.Trim()))
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

$usesRph = Test-Path (Join-Path $GamePath "RAGEPluginHook.exe")
if ($usesRph) {
    Write-Host "   [OK] RAGE Plugin Hook is installed, so the mod will run through it" -ForegroundColor Green
    $rphFiles = Join-Path $here "rph-files.ps1"
    if (-not (Test-Path $rphFiles)) { throw "This release doesn't include the RAGE Plugin Hook version of the mod." }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $rphFiles -Mod -GamePath $GamePath
    if ($LASTEXITCODE -ne 0) { throw "Couldn't add the mod for RAGE Plugin Hook. The reason is above." }
} else {
    $asi = Join-Path $here "ChatVsStreamerGTAV.asi"
    if (-not (Test-Path $asi)) { throw "ChatVsStreamerGTAV.asi not found next to install.ps1" }

    # Script Hook V has to be there for the plugin to load at all, and it only runs on game
    # versions it knows about, which is the usual reason a mod stops working after a patch.
    $supported = Test-ScriptHookSupport $version
    if ($supported -eq $false) {
        Write-Host "   [!!] Script Hook V's page doesn't list version $version." -ForegroundColor Yellow
        Write-Host "        It won't run on this version until a new release adds it, and neither will this mod."
        Write-Host "        Keep checking: $scriptHookPage"
        Write-Host "        Nothing here needs reinstalling after that, apart from replacing Script Hook V's files."
    } elseif ($supported -eq $true) {
        Write-Host "   [OK] Script Hook V's page lists this version" -ForegroundColor Green
    } elseif ($version) {
        Write-Host "   [??] Couldn't check Script Hook V's page. Make sure it lists version $version." -ForegroundColor Yellow
    }

    $hasHook = Test-Path (Join-Path $GamePath "ScriptHookV.dll")
    $hasLoader = (Test-Path (Join-Path $GamePath "dinput8.dll")) -or (Test-Path (Join-Path $GamePath "xinput1_4.dll"))
    if ($hasHook -and $hasLoader) {
        Write-Host "   [OK] Script Hook V is installed" -ForegroundColor Green
    } else {
        Write-Host "   [!!] Script Hook V isn't installed. The mod won't load without it." -ForegroundColor Yellow
        if ($supported -ne $false) {
            Write-Host "        1. Download the latest version from $scriptHookPage"
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
if ($usesRph) {
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
if ($usesRph) {
    Write-Host "Before going online, run rph-files.ps1 -Remove and turn BattlEye back on.`n" -ForegroundColor Yellow
} else {
    Write-Host "Before going online, delete $GamePath\ChatVsStreamerGTAV.asi and Script Hook V's files.`n" -ForegroundColor Yellow
}
