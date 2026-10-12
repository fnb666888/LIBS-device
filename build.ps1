[CmdletBinding()]
param([switch]$Release, [switch]$SkipTests, [switch]$WithUi)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'backend/build.ps1') -Release:$Release -SkipTests:$SkipTests
if ($WithUi) { & (Join-Path $PSScriptRoot 'ui/build.ps1') }
