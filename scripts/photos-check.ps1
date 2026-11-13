$ErrorActionPreference = 'Continue'
Write-Host 'Photos / Dolby Vision status' -ForegroundColor Cyan
$package = Get-AppxPackage -Name 'Microsoft.Windows.Photos' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($package) {
    Write-Host ('Package: ' + $package.Name + ' ' + $package.Version)
    $stage = Join-Path $env:LOCALAPPDATA ('Packages\' + $package.PackageFamilyName + '\LocalState\P7Hook')
    $dll = Join-Path $stage 'P7ProfileHook.dll'
    Write-Host ('Staged DLL: ' + $(if (Test-Path -LiteralPath $dll) { 'YES' } else { 'NO' }))
    $hookLog = Join-Path $stage 'P7-PROFILE-HOOK.log'
} else {
    Write-Host 'Microsoft.Windows.Photos package not found' -ForegroundColor Yellow
    $hookLog = $null
}
Write-Host ''
Write-Host 'Photos-related running processes:' -ForegroundColor Cyan
$processes = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'Photo|Image' })
if ($processes.Count -eq 0) {
    Write-Host 'None found. Open a Dolby Vision video in Photos, then run this check again.'
} else {
    foreach ($process in $processes) {
        $loaded = 'UNKNOWN'
        try {
            $loaded = if (@($process.Modules | Where-Object { $_.ModuleName -eq 'P7ProfileHook.dll' }).Count -gt 0) { 'YES' } else { 'NO' }
        } catch {
            $loaded = 'ACCESS_DENIED'
        }
        Write-Host ('PID=' + $process.Id + ' EXE=' + $process.ProcessName + '.exe HOOK_LOADED=' + $loaded)
        try { Write-Host ('  Path=' + $process.Path) } catch {}
    }
}
Write-Host ''
$monitor = Join-Path $env:LOCALAPPDATA 'DolbyVision-Profile-Enabler\player-autohook.log'
if (Test-Path -LiteralPath $monitor) {
    Write-Host 'Autohook / Photos recent entries:' -ForegroundColor Cyan
    @(Get-Content -LiteralPath $monitor -Tail 200 | Where-Object { $_ -match 'Photo|ATTACH_FAILED|Monitor error' } | Select-Object -Last 25) | ForEach-Object { Write-Host $_ }
} else {
    Write-Host 'Autohook log is not present.'
}
Write-Host ''
if ($hookLog -and (Test-Path -LiteralPath $hookLog)) {
    Write-Host 'Photos HEVC / Dolby recent entries:' -ForegroundColor Cyan
    @(Get-Content -LiteralPath $hookLog -Encoding Unicode -Tail 1200 | Where-Object { $_ -match 'Version 1.0-beta|PHOTOS_|HEVC SetInputType|DOLBY RPU FORWARD|FEL DOLBY_IN|FEL DOLBY_OUT|INLINE MFTEnumEx|HOOKS ENABLED' } | Select-Object -Last 45) | ForEach-Object { Write-Host $_ }
    Write-Host ('Full Photos log: ' + $hookLog)
} else {
    Write-Host 'Photos decoder log is not present. Turn on LOGGING.cmd and restart Photos.' -ForegroundColor Yellow
}
