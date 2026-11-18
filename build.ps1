#Requires -Version 5.1
<#
  Builds the mod and assembles a release folder in dist\.
    .\build.ps1             # Release build + dist\
    .\build.ps1 -Test       # ...and run the end-to-end tests against the simulated game
    .\build.ps1 -Deploy     # ...and run the installer against your game

  The RAGE Plugin Hook plugin only gets built if RPH's SDK is in third_party\rph_sdk. See the
  note in tools\compile_rph_plugin.ps1. The SDK and RPH itself never go in dist\.
#>
param(
    [switch]$Test,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

# The commands page in docs\ takes its effects and numbers from the source, so it's brought up
# to date on every build.
& (Join-Path $root "tools\gen_commands_page.ps1")

# Terminals opened before CMake was installed won't have it on PATH yet.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $cmakeDir = @("$env:ProgramFiles\CMake\bin",
                  "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") |
                Where-Object { Test-Path (Join-Path $_ "cmake.exe") } | Select-Object -First 1
    if (-not $cmakeDir) { throw "CMake not found. Install it with: winget install Kitware.CMake" }
    $env:PATH = "$cmakeDir;$env:PATH"
}

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
cmake --build $build --config Release -- /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$rphPlugin = Join-Path $build "rph_plugin\ChatVsStreamer.dll"
$hasRphSdk = Test-Path -LiteralPath (Join-Path $root "third_party\rph_sdk\RagePluginHook.dll")
if ($hasRphSdk) {
    & (Join-Path $root "tools\compile_rph_plugin.ps1") -Source (Join-Path $root "src\rph\plugin\ChatVsStreamerPlugin.cs") -Out $rphPlugin
} else {
    Write-Host "RPH's SDK isn't in third_party\rph_sdk, so the RAGE Plugin Hook version won't be in dist\." -ForegroundColor Yellow
}

if ($Test) {
    $testHost = Join-Path $build "Release\TestHost.exe"
    $suitesRun = 0
    foreach ($suite in @(@{ Name = "Script Hook V build"; Mode = @() }, @{ Name = "RAGE Plugin Hook build"; Mode = @("--rph") })) {
        $mode = $suite.Mode
        & $testHost $build $root @mode
        # 3 means Windows wouldn't load that build, so its tests never ran. See TestHost.cpp.
        if ($LASTEXITCODE -eq 3) { Write-Host "The tests of the $($suite.Name) DID NOT RUN." -ForegroundColor Yellow; continue }
        if ($LASTEXITCODE -ne 0) { throw "Tests of the $($suite.Name) failed" }
        $suitesRun++
    }
    if ($suitesRun -eq 0) { throw "No tests ran." }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tests\InstallerTest.ps1")
    # 3 means GTA V or RPH is running, which the installer refuses to work beside.
    if ($LASTEXITCODE -eq 3) { Write-Host "The installer tests DID NOT RUN." -ForegroundColor Yellow }
    elseif ($LASTEXITCODE -ne 0) { throw "Installer tests failed" }
}

$dist = Join-Path $root "dist"
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dist | Out-Null
Copy-Item (Join-Path $build "Release\ChatVsStreamerGTAV.asi") $dist
Copy-Item (Join-Path $root "mod\overlay.html") $dist
Copy-Item (Join-Path $root "mod\panel.html") $dist
Copy-Item (Join-Path $root "installer\install.ps1") $dist
Copy-Item (Join-Path $root "installer\RunMe.bat") $dist
if ($hasRphSdk) {
    $rphDist = Join-Path $dist "rph"
    New-Item -ItemType Directory $rphDist | Out-Null
    Copy-Item $rphPlugin $rphDist
    Copy-Item (Join-Path $build "Release\ChatVsStreamerCore.dll") $rphDist
    Copy-Item (Join-Path $root "installer\rph-files.ps1") $dist
    Copy-Item (Join-Path $root "installer\StartGame.bat") $dist
}

Write-Host "Release assembled in $dist" -ForegroundColor Green
if ($Deploy) { & (Join-Path $dist "install.ps1") }
