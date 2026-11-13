#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$base = 'C:\ProgramData\MillaAV\P7DV'
$globalFlag = Join-Path $base 'logging.on'
$targetConfig = Join-Path $PSScriptRoot 'player-targets.json'
if (-not (Test-Path -LiteralPath $targetConfig)) { throw 'Missing player-targets.json' }
$config = Get-Content -LiteralPath $targetConfig -Raw | ConvertFrom-Json
$stages = @()
foreach ($target in $config.targets) {
    if ($target.enabled -ne $true) { continue }
    $pkg = Get-AppxPackage -Name $target.package -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pkg) { continue }
    $folder = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState\P7Hook')
    $dll = Join-Path $folder 'P7ProfileHook.dll'
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        $stages += [pscustomobject]@{ Name = $target.name; Folder = $folder; Flag = (Join-Path $folder 'logging.on') }
    }
}
if (-not $stages.Count) { throw 'No installed P7ProfileHook.dll found. Run INSTALL.cmd first.' }
$enabled = Test-Path -LiteralPath $globalFlag -PathType Leaf
if (-not $enabled) {
    $enabled = @($stages | Where-Object { Test-Path -LiteralPath $_.Flag -PathType Leaf }).Count -gt 0
}
if ($enabled) {
    Remove-Item -LiteralPath $globalFlag -Force -ErrorAction SilentlyContinue
    foreach ($stage in $stages) { Remove-Item -LiteralPath $stage.Flag -Force -ErrorAction Stop }
    Write-Host 'Logging: OFF' -ForegroundColor Green
} else {
    New-Item -ItemType Directory -Path $base -Force | Out-Null
    [IO.File]::WriteAllText($globalFlag, 'on')
    foreach ($stage in $stages) { [IO.File]::WriteAllText($stage.Flag, 'on') }
    Write-Host 'Logging: ON' -ForegroundColor Yellow
}
foreach ($stage in $stages) {
    $active = Test-Path -LiteralPath $stage.Flag -PathType Leaf
    $status = if ($active) { 'ON' } else { 'OFF' }
    Write-Host ('  ' + $stage.Name + ': ' + $status)
    if ($active) { Write-Host ('  Hook log: ' + (Join-Path $stage.Folder 'P7-PROFILE-HOOK.log')) }
}
Write-Host 'The hook checks logging state every 500 ms. Restart the player for a clean diagnostic session.'
