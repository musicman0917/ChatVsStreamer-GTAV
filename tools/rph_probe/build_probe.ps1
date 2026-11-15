#Requires -Version 5.1
# Compiles CvsProbe.dll, the read-only test plugin for RAGE Plugin Hook, into build\rph_probe.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
& (Join-Path $root "tools\compile_rph_plugin.ps1") -Source (Join-Path $PSScriptRoot "CvsProbe.cs") -Out (Join-Path $root "build\rph_probe\CvsProbe.dll")
