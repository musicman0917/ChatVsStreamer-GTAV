#Requires -Version 5.1
<#
  Tests the installer scripts against made-up game folders. Nothing here touches a real game,
  RAGE Plugin Hook, LSPDFR's zip or your data folder. It all happens in a folder under %TEMP%,
  which is deleted afterwards.

    powershell -NoProfile -ExecutionPolicy Bypass -File tests\InstallerTest.ps1

  Part one runs install.ps1 beside a stand-in for rph-files.ps1 that only records how it was
  called, so every branch of the RAGE Plugin Hook walkthrough runs without admin rights. Part
  two runs the real rph-files.ps1 with a made-up zip.

  Exits 0 if every check passed and 1 if any failed. Exits 3, having run nothing, while GTA V
  or RAGE Plugin Hook is running, because the installer refuses to work then.
#>
param(
    # Leaves the work folder behind, to look at.
    [switch]$Keep,
    # The folder holding the install.ps1 and rph-files.ps1 to test.
    [string]$Installer = (Join-Path (Split-Path -Parent $PSScriptRoot) "installer")
)

$ErrorActionPreference = "Stop"
$work = Join-Path ([IO.Path]::GetTempPath()) ("cvs_installer_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$appData = Join-Path $work "appdata"
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$manifestName = "ChatVsStreamer_RPH_manifest.txt"
$script:checks = 0
$script:failed = 0
$script:games = 0
$script:locked = @()

function Section($name) { Write-Host $name }

# A failed check prints what its run printed, once, because that's where the reason is.
function Check($ok, $what, $run) {
    $script:checks++
    if ($ok) { return }
    $script:failed++
    Write-Host "  FAILED: $what" -ForegroundColor Red
    if ($run -and -not $run.Shown) {
        $run.Shown = $true
        Write-Host "    exit code $($run.ExitCode), calls: $($run.Calls -join ' ; ')"
        foreach ($line in ($run.Output -split "`n")) { Write-Host "    | $line" }
    }
}

# A made-up game folder. A version gives its exe that file version. Locked means no new files
# or folders can go in it, which is what Program Files is like without admin rights.
function New-Game([string]$Version, [switch]$WithRph, [switch]$WithSkipDsCheck, [switch]$Locked) {
    $script:games++
    $dir = Join-Path $work "game$($script:games)"
    New-Item -ItemType Directory -Path $dir | Out-Null
    $exe = Join-Path $dir "GTA5_Enhanced.exe"
    if ($Version) {
        $built = Join-Path $work "exe_$Version.exe"
        if (-not (Test-Path -LiteralPath $built)) {
            $source = "[assembly: System.Reflection.AssemblyFileVersion(`"$Version`")] public static class NotAGame { public static void Main() { } }"
            Add-Type -TypeDefinition $source -OutputAssembly $built -OutputType ConsoleApplication
        }
        Copy-Item -LiteralPath $built -Destination $exe
    } else {
        Set-Content -LiteralPath $exe -Value "not a game" -Encoding ASCII
    }
    if ($WithRph) { Set-Content -LiteralPath (Join-Path $dir "RAGEPluginHook.exe") -Value "not RPH" -Encoding ASCII }
    if ($WithSkipDsCheck) { Set-Content -LiteralPath (Join-Path $dir "skipdscheck") -Value "" -Encoding ASCII }
    if ($Locked) {
        & icacls.exe $dir /deny "*${sid}:(WD,AD)" /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Couldn't lock $dir" }
        $script:locked += $dir
    }
    return $dir
}

# Runs a script with nobody at the keyboard and the data folder under the work folder.
function Invoke-Script($file, [string[]]$arguments, [switch]$RphFails) {
    $savedAppData = $env:LOCALAPPDATA
    $savedPreference = $ErrorActionPreference
    $env:LOCALAPPDATA = $appData
    if ($RphFails) { $env:CVS_TEST_RPH_FAILS = "1" }
    # Windows PowerShell turns what a program writes to its error output into errors, and "Stop" would throw on them.
    $ErrorActionPreference = "Continue"
    try {
        $lines = $null | & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $file @arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
        $env:LOCALAPPDATA = $savedAppData
        Remove-Item Env:\CVS_TEST_RPH_FAILS -ErrorAction SilentlyContinue
    }
    return [pscustomobject]@{ Output = (@($lines) -join "`n"); ExitCode = $code; Calls = @(); Shown = $false }
}

# install.ps1, beside the stand-in. Calls holds how the stand-in was called, one line each time.
function Install($game, [string[]]$arguments, [switch]$RphFails) {
    $run = Invoke-Script (Join-Path $release "install.ps1") (@("-GamePath", $game, "-SkipTwitchSetup") + $arguments) -RphFails:$RphFails
    $log = Join-Path $release "calls.txt"
    if (Test-Path -LiteralPath $log) {
        $run.Calls = @(Get-Content -LiteralPath $log)
        Remove-Item -LiteralPath $log
    }
    return $run
}

# The real rph-files.ps1.
function Rph($game, [string[]]$arguments) {
    return Invoke-Script (Join-Path $realRelease "rph-files.ps1") (@("-GamePath", $game) + $arguments)
}

# What's in a file, or nothing if it isn't there, so that a missing file fails a check instead of
# stopping the run.
function Lines($path) {
    if (Test-Path -LiteralPath $path) { return @(Get-Content -LiteralPath $path) }
    return @()
}

function Text($path) {
    if (Test-Path -LiteralPath $path) { return [IO.File]::ReadAllText($path) }
    return ""
}

function Names($dir) { return @(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object { $_.Name } | Sort-Object) -join ", " }

if (@(Get-Process -Name "GTA5_Enhanced", "GTA5", "RAGEPluginHook" -ErrorAction SilentlyContinue).Count -gt 0) {
    Write-Host "GTA V or RAGE Plugin Hook is running, and the installer refuses to work then." -ForegroundColor Yellow
    exit 3
}

try {
    New-Item -ItemType Directory -Path $work, $appData | Out-Null

    # A release folder with the real install.ps1 and a stand-in for rph-files.ps1.
    $release = Join-Path $work "release"
    New-Item -ItemType Directory -Path $release | Out-Null
    Copy-Item -LiteralPath (Join-Path $installer "install.ps1") -Destination $release
    foreach ($name in "ChatVsStreamerGTAV.asi", "overlay.html", "panel.html") {
        Set-Content -LiteralPath (Join-Path $release $name) -Value "made up: $name" -Encoding ASCII
    }
    Set-Content -LiteralPath (Join-Path $release "rph-files.ps1") -Encoding ASCII -Value @'
param([switch]$Install, [switch]$Mod, [switch]$SkipDsCheck, [switch]$AllowWrite, [string]$GamePath, [string]$ZipPath)
$parts = @()
if ($AllowWrite) { $parts += "AllowWrite" }
if ($Install) { $parts += "Install zip=$ZipPath" }
if ($SkipDsCheck) { $parts += "SkipDsCheck" }
if ($Mod) { $parts += "Mod" }
Add-Content -LiteralPath (Join-Path $PSScriptRoot "calls.txt") -Value ($parts -join " | ")
Write-Host "stand-in for rph-files.ps1: $($parts -join ' | ')"
if ($env:CVS_TEST_RPH_FAILS) { exit 1 }
exit 0
'@

    # Stand-ins for Script Hook V's page and LSPDFR's zip. install.ps1 only looks for the zip.
    $pageWithout = Join-Path $work "scripthook_without.html"
    $pageWith = Join-Path $work "scripthook_with.html"
    Set-Content -LiteralPath $pageWithout -Value "Supported: 1.0.3889.0 and 1.0.1158.13" -Encoding ASCII
    Set-Content -LiteralPath $pageWith -Value "Supported: 1.0.3889.0 and 1.0.1158.16" -Encoding ASCII
    $zip = Join-Path $work "lspdfr_made_up.zip"
    Set-Content -LiteralPath $zip -Value "not a zip" -Encoding ASCII
    $noZip = Join-Path $work "not_downloaded_yet.zip"
    $rphVersion = "1.0.1158.16"

    # ── install.ps1 ─────────────────────────────────────────────────────────────

    Section "Installer: through Script Hook V"
    $game = New-Game
    $run = Install $game @()
    Check ($run.ExitCode -eq 0) "it finishes" $run
    Check (Test-Path -LiteralPath (Join-Path $game "ChatVsStreamerGTAV.asi")) "the .asi is in the game folder" $run
    Check ($run.Calls.Count -eq 0) "rph-files.ps1 isn't used" $run
    Check ($run.Output -match "Nearly there") "it says what's left to do" $run
    Check (Test-Path -LiteralPath (Join-Path $appData "ChatVsStreamerGTAV\overlay.html")) "the overlay is in the data folder" $run

    $game = New-Game -Version $rphVersion
    $run = Install $game @("-ScriptHookPage", $pageWith)
    Check ($run.ExitCode -eq 0 -and $run.Output -match "Script Hook V's page lists this version") "a version Script Hook V lists goes that way" $run
    Check ($run.Output -notmatch "RAGE Plugin Hook \(RPH\)" -and $run.Calls.Count -eq 0) "with no mention of RPH" $run

    $game = New-Game -Locked
    $run = Install $game @()
    Check ($run.ExitCode -ne 0 -and $run.Output -match "Run as administrator") "a folder it can't write to says to run as administrator" $run

    Section "...when Script Hook V doesn't list the game version"
    $game = New-Game -Version $rphVersion
    $run = Install $game @("-ScriptHookPage", $pageWithout, "-RphZip", $zip, "-Answers", "y")
    Check ($run.Output -match "doesn't list version $rphVersion" -and $run.Output -match "Set the mod up through RAGE Plugin Hook\?") "it offers RAGE Plugin Hook" $run
    Check ($run.ExitCode -eq 0 -and $run.Calls.Count -eq 1 -and $run.Calls[0] -match "^Install zip=.*lspdfr_made_up\.zip" -and $run.Calls[0] -match "Mod$") "yes installs RPH and the mod in one go" $run
    Check (-not (Test-Path -LiteralPath (Join-Path $game "ChatVsStreamerGTAV.asi"))) "and leaves the .asi out" $run

    $game = New-Game -Version $rphVersion
    $run = Install $game @("-ScriptHookPage", $pageWithout, "-RphZip", $zip, "-Answers", "n")
    Check ($run.ExitCode -eq 0 -and $run.Calls.Count -eq 0 -and (Test-Path -LiteralPath (Join-Path $game "ChatVsStreamerGTAV.asi"))) "no copies the .asi, ready for when Script Hook V catches up" $run
    Check ($run.Output -match "Keep checking") "and says to keep checking its page" $run

    $game = New-Game -Version $rphVersion
    $run = Install $game @("-ScriptHookPage", $pageWithout, "-RphZip", $zip)
    Check ($run.ExitCode -eq 0 -and $run.Calls.Count -eq 0 -and $run.Output -match "install\.ps1 -Rph") "with nobody to ask, it stays with Script Hook V and names the switch" $run

    $game = New-Game -Version "1.0.9999.0"
    $run = Install $game @("-ScriptHookPage", $pageWithout, "-RphZip", $zip, "-Answers", "y")
    Check ($run.Output -notmatch "Set the mod up through RAGE Plugin Hook\?" -and $run.Calls.Count -eq 0) "a version RPH's copy wasn't built for gets no offer" $run

    Rename-Item -LiteralPath (Join-Path $release "rph-files.ps1") -NewName "rph-files.ps1.off"
    $game = New-Game -Version $rphVersion
    $run = Install $game @("-ScriptHookPage", $pageWithout, "-RphZip", $zip, "-Answers", "y")
    Rename-Item -LiteralPath (Join-Path $release "rph-files.ps1.off") -NewName "rph-files.ps1"
    Check ($run.ExitCode -eq 0 -and $run.Output -notmatch "RAGE Plugin Hook" -and (Test-Path -LiteralPath (Join-Path $game "ChatVsStreamerGTAV.asi"))) "a release without the RPH build doesn't offer it either" $run

    Section "...through RAGE Plugin Hook"
    $game = New-Game
    $run = Install $game @("-Rph", "-RphZip", $zip)
    Check ($run.ExitCode -eq 0 -and $run.Output -match "Found lspdfr_made_up\.zip") "-Rph finds the zip" $run
    Check ($run.Calls.Count -eq 1 -and $run.Calls[0] -match "^Install zip=" -and $run.Calls[0] -match "Mod$") "and installs RPH and the mod in one go" $run
    Check (($run.Output -match "skipdscheck") -eq ($run.Calls[0] -match "SkipDsCheck")) "with the DirectStorage file whenever it says so" $run
    Check ($run.Output -match "nothing needs admin rights" -and $run.Output -notmatch "Type 1 or 2") "a folder it can write to needs no question about admin rights" $run
    Check ($run.Output -match "StartGame\.bat") "it says to start the game with StartGame.bat" $run

    $game = New-Game -WithRph -WithSkipDsCheck
    $run = Install $game @()
    Check ($run.ExitCode -eq 0 -and $run.Output -match "RAGE Plugin Hook is installed") "with RPH already there, it's used without asking" $run
    Check ($run.Calls.Count -eq 1 -and $run.Calls[0] -eq "Mod") "and only the mod goes in" $run

    $game = New-Game
    $run = Install $game @("-Rph", "-RphZip", $noZip)
    Check ($run.ExitCode -eq 1 -and $run.Calls.Count -eq 0) "without the zip and nobody to ask, it stops and changes nothing" $run
    Check ($run.Output -match "lcpdfr\.com" -and $run.Output -match "not_downloaded_yet\.zip" -and $run.Output -match "Not finished") "having said where to get it" $run

    $run = Install $game @("-Rph", "-RphZip", $noZip, "-Answers", ",skip")
    Check ($run.Output -match "It isn't there yet") "Enter without the zip there asks again" $run
    Check ($run.ExitCode -eq 1 -and $run.Calls.Count -eq 0 -and $run.Output -match "Not finished") "and skip stops for now" $run

    $game = New-Game -Version "1.0.9999.0"
    $run = Install $game @("-Rph", "-RphZip", $zip)
    Check ($run.ExitCode -eq 1 -and $run.Calls.Count -eq 0 -and $run.Output -match "built for game version $rphVersion, and yours is 1\.0\.9999\.0") "a game version RPH's copy wasn't built for stops it" $run

    $game = New-Game
    $run = Install $game @("-Rph", "-RphZip", $zip) -RphFails
    Check ($run.ExitCode -ne 0 -and $run.Output -match "Couldn't add the mod for RAGE Plugin Hook") "a failure in rph-files.ps1 stops the installer" $run
    Check ($run.Output -notmatch "Nearly there") "before it says it's nearly done" $run

    Section "...when the game folder needs admin rights"
    $game = New-Game -Locked
    $run = Install $game @("-Rph", "-RphZip", $zip, "-Answers", "1")
    Check ($run.Output -match "Admin rights" -and $run.Output -match "Type 1 or 2") "it asks how the game should get to write there" $run
    Check ($run.Calls.Count -eq 2 -and $run.Calls[0] -eq "AllowWrite" -and $run.Calls[1] -match "^Install zip=") "1 gives the account write access first, then installs" $run
    Check ($run.Output -match "still can't write to the game folder") "and says so if that didn't work" $run

    $game = New-Game -Locked
    $run = Install $game @("-Rph", "-RphZip", $zip, "-Answers", "2")
    Check ($run.ExitCode -eq 0 -and $run.Calls.Count -eq 1 -and $run.Calls[0] -match "^Install zip=") "2 leaves the folder alone and installs" $run

    $game = New-Game -Locked
    $run = Install $game @("-Rph", "-RphZip", $zip, "-Answers", "maybe,2")
    Check (([regex]::Matches($run.Output, "Type 1 or 2")).Count -eq 2 -and $run.Calls.Count -eq 1) "anything else asks again" $run

    $game = New-Game -Locked
    $run = Install $game @("-Rph", "-RphZip", $zip)
    Check ($run.ExitCode -eq 0 -and $run.Calls.Count -eq 1 -and $run.Calls[0] -notmatch "AllowWrite") "with nobody to ask, the folder's permissions are left alone" $run
    Check ($run.Output -match "rph-files\.ps1 -AllowWrite") "and it names the command" $run

    # ── rph-files.ps1 ───────────────────────────────────────────────────────────

    # A release folder with the real rph-files.ps1, and a zip laid out like LSPDFR's.
    $realRelease = Join-Path $work "real_release"
    $modFolder = Join-Path $realRelease "rph"
    $dataDir = Join-Path $work "data"
    New-Item -ItemType Directory -Path $realRelease, $modFolder | Out-Null
    Copy-Item -LiteralPath (Join-Path $installer "rph-files.ps1") -Destination $realRelease
    foreach ($name in "overlay.html", "panel.html", "rph\ChatVsStreamer.dll", "rph\ChatVsStreamerCore.dll") {
        Set-Content -LiteralPath (Join-Path $realRelease $name) -Value "made up: $name" -Encoding ASCII
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $fakeZip = Join-Path $work "made_up_lspdfr.zip"
    $archive = [IO.Compression.ZipFile]::Open($fakeZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in "RAGEPluginHook.exe", "XInput1_4.dll", "SomeLibrary.dll", "lspdfr/readme.txt", "plugins/LSPD First Response.dll") {
            $writer = New-Object IO.StreamWriter($archive.CreateEntry($name).Open())
            try { $writer.Write("made up: $name") } finally { $writer.Dispose() }
        }
    } finally {
        $archive.Dispose()
    }
    $hash = (Get-FileHash -LiteralPath $fakeZip -Algorithm SHA256).Hash
    $install = @("-Install", "-ZipPath", $fakeZip, "-ExpectedSha256", $hash)
    $everything = $install + @("-SkipDsCheck", "-Mod", "-ModFolder", $modFolder, "-DataDir", $dataDir)

    Section "RPH files: write access first, then everything else"
    $game = New-Game
    $manifest = Join-Path $game $manifestName
    Set-Content -LiteralPath (Join-Path $game "readme.txt") -Value "the player's own" -Encoding ASCII
    $run = Rph $game @("-AllowWrite")
    Check ($run.ExitCode -eq 0 -and @(Lines $manifest) -contains "grant`t$sid") "-AllowWrite records whose access it changed" $run
    # RPH would write a file of this name. This one is the player's, and appeared after the record began.
    Set-Content -LiteralPath (Join-Path $game "commandline.txt") -Value "the player's own" -Encoding ASCII
    $run = Rph $game $everything
    Check ($run.ExitCode -eq 0) "-Install still works after it" $run
    $missing = @("RAGEPluginHook.exe", "XInput1_4.dll", "SomeLibrary.dll", "skipdscheck", "startup.rphs", "Plugins\ChatVsStreamer.dll", "Plugins\ChatVsStreamer\ChatVsStreamerCore.dll") |
               Where-Object { -not (Test-Path -LiteralPath (Join-Path $game $_)) }
    Check (@($missing).Count -eq 0) "RPH, the DirectStorage file and the mod are all in (missing: $($missing -join ', '))" $run
    Check (-not (Test-Path -LiteralPath (Join-Path $game "lspdfr")) -and -not (Test-Path -LiteralPath (Join-Path $game "Plugins\LSPD First Response.dll"))) "LSPDFR itself is left out" $run
    Check ((Text (Join-Path $game "startup.rphs")) -eq "LoadPlugin ChatVsStreamer.dll`r`n") "RPH is told to load the mod" $run
    Check (Test-Path -LiteralPath (Join-Path $dataDir "panel.html")) "the control panel is in the data folder" $run
    $lines = @(Lines $manifest)
    Check (($lines -contains "grant`t$sid") -and ($lines -contains "existing`treadme.txt") -and ($lines -contains "existing`tcommandline.txt")) "the record keeps the access change and what was the player's" $run
    Check (@($lines | Where-Object { $_ -like "file`tRAGEPluginHook.exe`t*" }).Count -eq 1) "and now lists RPH" $run

    $run = Rph $game $install
    Check ($run.ExitCode -eq 1 -and $run.Output -match "already installed RPH here") "a second -Install is refused" $run

    $run = Rph $game @("-Remove")
    Check ($run.ExitCode -eq 0 -and $run.Output -match "Everything this script added is out") "-Remove takes it all out again" $run
    Check ((Names $game) -eq "commandline.txt, GTA5_Enhanced.exe, readme.txt") "leaving the game and the player's files (left: $(Names $game))" $run

    Section "...in the other order"
    $game = New-Game
    $manifest = Join-Path $game $manifestName
    $run = Rph $game $everything
    Check ($run.ExitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $game "RAGEPluginHook.exe"))) "-Install works on a folder with no record" $run
    $run = Rph $game @("-AllowWrite")
    Check ($run.ExitCode -eq 0 -and @(Lines $manifest) -contains "grant`t$sid") "-AllowWrite adds to the same record" $run
    $run = Rph $game @("-Remove")
    Check ($run.ExitCode -eq 0 -and (Names $game) -eq "GTA5_Enhanced.exe") "-Remove leaves only the game (left: $(Names $game))" $run

    Section "...with a zip that wasn't checked"
    $game = New-Game
    $run = Rph $game @("-Install", "-ZipPath", $fakeZip, "-ExpectedSha256", ("0" * 64))
    Check ($run.ExitCode -eq 1 -and $run.Output -match "isn't the zip that was checked") "it's refused" $run
    Check ((Names $game) -eq "GTA5_Enhanced.exe") "and nothing is changed (there: $(Names $game))" $run
} finally {
    foreach ($dir in $script:locked) { & icacls.exe $dir /remove:d "*$sid" /Q | Out-Null }
    if ($Keep) { Write-Host "Left in place: $work" }
    elseif (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "`n$($script:checks) checks, $($script:failed) failed."
exit ([int]($script:failed -gt 0))
