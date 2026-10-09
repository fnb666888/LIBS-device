[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceProject,
    [Parameter(Mandatory=$true)][string]$PythonPath
)
$ErrorActionPreference = 'Stop'
$taskSource = Join-Path (Resolve-Path -LiteralPath $SourceProject).Path 'model/current'
$taskPython = (Resolve-Path -LiteralPath $PythonPath).Path
$taskManifest = Get-Content -LiteralPath (Join-Path $taskSource 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($taskManifest.format -notin @('current_mixed_v1','current_mixed_v2')) { throw 'Unsupported model format' }
# Validate the complete source package before replacing the destination assets.
foreach ($taskAsset in $taskManifest.asset_sha256.PSObject.Properties) {
    if ([IO.Path]::GetFileName($taskAsset.Name) -ne $taskAsset.Name) { throw 'Invalid model asset name' }
    $taskHash = (Get-FileHash -LiteralPath (Join-Path $taskSource $taskAsset.Name) -Algorithm SHA256).Hash
    if ($taskHash -ne $taskAsset.Value) { throw "Model hash mismatch: $($taskAsset.Name)" }
}
$taskDest = Join-Path $PSScriptRoot 'iron_ore/model/current'
New-Item -ItemType Directory -Force -Path $taskDest | Out-Null
foreach ($taskAsset in $taskManifest.asset_sha256.PSObject.Properties) {
    Copy-Item -LiteralPath (Join-Path $taskSource $taskAsset.Name) -Destination $taskDest
}
Copy-Item -LiteralPath (Join-Path $taskSource 'manifest.json') -Destination $taskDest
$taskConfig = @{python=$taskPython; model_dir='iron_ore/model/current'; timeout_ms=60000}
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'runtime.local.json'), ($taskConfig | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Write-Host "Model installed at $taskDest"
