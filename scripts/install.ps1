#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$base = 'C:\ProgramData\MillaAV\P7DV'
$taskName = 'MillaAV Dolby Vision Movies TV AutoHook'
$legacyName = 'MillaAV Dolby Vision Movies TV AutoHook'
New-Item -ItemType Directory -Path $base -Force | Out-Null
$sourceInjector = Join-Path $root 'bin\P7InjectorMulti.exe'
Write-Host 'Configuring Movies & TV automatic integration...'
if ($true) {
    Write-Host 'Building updated player injector...'
    & (Join-Path $root 'scripts\build-multi-injector.ps1')
    if (-not (Test-Path -LiteralPath $sourceInjector)) { throw 'Injector build did not produce an executable.' }
}
$config = Get-Content -LiteralPath (Join-Path $root 'scripts\player-targets.json') -Raw | ConvertFrom-Json
$stageList = @()
foreach ($target in $config.targets) {
    if (-not $target.enabled) { continue }
    $pkg = Get-AppxPackage -Name $target.package -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pkg) { Write-Warning ($target.name + ' is not installed; skipping.'); continue }
    $stageList += [pscustomobject]@{ Name = $target.name; Path = (Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState\P7Hook')) }
}
if (-not $stageList.Count) { throw 'No supported player was found.' }
$backup = Join-Path $base ('backup-movies-tv-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach ($targetStage in $stageList) {
    New-Item -ItemType Directory -Path $targetStage.Path -Force | Out-Null
    foreach ($name in @('P7ProfileHook.dll','dovi.dll')) {
        $from = Join-Path $root ('bin\' + $name)
        $to = Join-Path $targetStage.Path $name
        if (-not (Test-Path -LiteralPath $from)) { throw ('Required file missing: ' + $from) }
        if (Test-Path -LiteralPath $to) {
            $safeName = ($targetStage.Name -replace '[^A-Za-z0-9]', '') + '-' + $name
            Copy-Item -LiteralPath $to -Destination (Join-Path $backup $safeName) -Force
        }
        if ($targetStage.Name -eq 'Microsoft Media Player' -and (Test-Path -LiteralPath $to)) {
            Write-Host ('Preserved existing Media Player file: ' + $name) -ForegroundColor Cyan
            continue
        }
        if (Test-Path -LiteralPath $to) {
            $incomingHash = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
            $installedHash = (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash
            if ($incomingHash -eq $installedHash) {
                Write-Host ('Already installed (unchanged): ' + $targetStage.Name + ' / ' + $name) -ForegroundColor DarkGray
                continue
            }
        }
        try {
            Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop
        }
        catch [System.IO.IOException] {
            throw ('Cannot update ' + $targetStage.Name + ' / ' + $name + ': the file is in use. Close the player and run INSTALL.cmd again. Existing DLL left intact. ' + $_.Exception.Message)
        }
    }
    Write-Host ('Staged: ' + $targetStage.Name) -ForegroundColor Cyan
}
Copy-Item -LiteralPath $sourceInjector -Destination (Join-Path $base 'P7InjectorMulti.exe') -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\player-targets.json') -Destination (Join-Path $base 'player-targets.json') -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\movies-tv-background.ps1') -Destination (Join-Path $base 'movies-tv-background.ps1') -Force
$old = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($old) {
    Export-ScheduledTask -TaskName $taskName | Set-Content -LiteralPath (Join-Path $backup 'task.xml') -Encoding Unicode
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
$scriptPath = Join-Path $base 'movies-tv-background.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $scriptPath + '"')
$trigger = New-ScheduledTaskTrigger -AtLogOn -User ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name)
$principal = New-ScheduledTaskPrincipal -UserId ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host 'Player background integration installed. No CMD needed for future launches.' -ForegroundColor Green
Write-Host 'Important: automatic early hooking depends on process timing; Dolby Vision activation is not guaranteed on every launch.' -ForegroundColor Yellow
Write-Host 'Microsoft Media Player, Movies & TV and Photos are monitored. Existing Media Player hook DLLs remain unchanged.' -ForegroundColor Green
