param([Parameter(Mandatory=$true)][ValidateSet('ON','OFF','DIFF')][string]$Mode)
$ErrorActionPreference = 'Stop'
$targets = New-Object System.Collections.Generic.List[string]
$central = 'C:\ProgramData\MillaAV\P7DV'
if (Test-Path -LiteralPath $central) { $targets.Add((Join-Path $central 'fel-split-mode.txt')) }
$packages = Join-Path $env:LOCALAPPDATA 'Packages'
if (Test-Path -LiteralPath $packages) {
    Get-ChildItem -LiteralPath $packages -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $stage = Join-Path $_.FullName 'LocalState\P7Hook'
        if (Test-Path -LiteralPath $stage) { $targets.Add((Join-Path $stage 'fel-split-mode.txt')) }
    }
}
$written = 0
foreach ($path in ($targets | Select-Object -Unique)) {
    try {
        Set-Content -LiteralPath $path -Value $Mode -Encoding ASCII -ErrorAction Stop
        $saved = (Get-Content -LiteralPath $path -Raw -ErrorAction Stop).Trim()
        if ($saved -ne $Mode) { throw 'Readback mismatch' }
        Write-Host "OK: $path" -ForegroundColor Green
        $written++
    } catch {
        Write-Host "FAIL: $path : $_" -ForegroundColor Red
    }
}
if ($written -eq 0) {
    Write-Host 'Split mode not saved. No active stage was writable.' -ForegroundColor Red
    exit 1
}
Write-Host "Split-screen: $Mode (applies during playback within about 1 second)" -ForegroundColor Cyan
