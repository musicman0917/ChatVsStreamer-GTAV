#Requires -Version 5.1
<#
  Compiles one C# file into a RAGE Plugin Hook plugin.

    compile_rph_plugin.ps1 -Source <file.cs> -Out <file.dll>

  Needs RPH's SDK at third_party\rph_sdk\RagePluginHook.dll. That's from the NuGet package
  RagePluginHook 1.124.0. Its license allows using it to develop plugins but forbids uploading
  or redistributing it, so third_party\rph_sdk is git-ignored and never goes in dist.

  This compiles against the .NET Framework 4.8 runtime already on the machine, so it doesn't
  need a targeting pack, and it doesn't copy the SDK next to the output.
#>
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Out,
    [string]$RphSdk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $RphSdk) { $RphSdk = Join-Path $root "third_party\rph_sdk\RagePluginHook.dll" }
if (-not (Test-Path -LiteralPath $RphSdk)) { throw "RPH's SDK isn't at $RphSdk. See the note at the top of this script." }

$framework = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319"
if (-not (Test-Path -LiteralPath (Join-Path $framework "mscorlib.dll"))) { throw ".NET Framework 4.x isn't at $framework" }

$dotnetRoot = Split-Path -Parent (Get-Command dotnet).Source
$csc = Get-ChildItem -LiteralPath (Join-Path $dotnetRoot "sdk") -Directory |
    Sort-Object { try { [version]$_.Name } catch { [version]"0.0" } } -Descending |
    ForEach-Object { Join-Path $_.FullName "Roslyn\bincore\csc.dll" } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $csc) { throw "No C# compiler found under $dotnetRoot\sdk" }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null

$references = "mscorlib", "System", "System.Core", "Microsoft.CSharp", "System.Drawing", "System.Windows.Forms" |
    ForEach-Object { "/reference:" + (Join-Path $framework "$_.dll") }

& dotnet $csc /noconfig /nologo /nostdlib+ /target:library /platform:x64 /optimize+ /unsafe+ /langversion:10 `
    $references "/reference:$RphSdk" "/out:$Out" $Source
if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Source) failed to compile" }

Write-Host "Built $Out" -ForegroundColor Green
