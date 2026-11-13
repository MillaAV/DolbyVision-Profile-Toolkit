#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$base = 'C:\ProgramData\MillaAV\P7DV'
$taskName = 'MillaAV Dolby Vision Movies TV AutoHook'
$oldTaskPaused = $false
trap {
    if ($oldTaskPaused) {
        try { Start-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue } catch {}
    }
    break
}
$legacyName = 'MillaAV Dolby Vision Movies TV AutoHook'
New-Item -ItemType Directory -Path $base -Force | Out-Null
Remove-Item -LiteralPath (Join-Path $base 'logging.on') -Force -ErrorAction SilentlyContinue
Write-Host 'Logging: OFF (default). Run LOGGING.cmd to toggle.'
$sourceInjector = Join-Path $root 'bin\P7InjectorMulti.exe'
Write-Host 'Configuring unified player integration...'
if ($true) {
    Write-Host 'Building updated player injector...'
    & (Join-Path $root 'scripts\build-multi-injector.ps1')
    if (-not (Test-Path -LiteralPath $sourceInjector)) { throw 'Injector build did not produce an executable.' }
    Write-Host 'Building P7ProfileHook.dll from current source...'
    & (Join-Path $root 'scripts\build.ps1')
    if (-not (Test-Path -LiteralPath (Join-Path $root 'bin\P7ProfileHook.dll'))) { throw 'Hook DLL build did not produce a DLL.' }
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

$changedTargets = @()
foreach ($targetStage in $stageList) {
    foreach ($name in @('P7ProfileHook.dll','dovi.dll')) {
        $from = Join-Path $root ('bin\' + $name)
        $to = Join-Path $targetStage.Path $name
        if (-not (Test-Path -LiteralPath $from)) { throw ('Required file missing: ' + $from) }
        if (-not (Test-Path -LiteralPath $to) -or
            (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash) {
            $changedTargets += $targetStage.Name
            break
        }
    }
}
$changedTargets = @($changedTargets | Select-Object -Unique)
if ($changedTargets.Count -gt 0) {
    $oldMonitor = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($oldMonitor -and $oldMonitor.State -eq 'Running') {
        Write-Host 'Pausing the player monitor for DLL updates...'
        Stop-ScheduledTask -TaskName $taskName -ErrorAction Stop
        $oldTaskPaused = $true
        Start-Sleep -Milliseconds 800
    }
    $targetProcesses = @()
    foreach ($target in $config.targets) {
        if ($changedTargets -notcontains $target.name) { continue }
        foreach ($exe in $target.executables) {
            $targetProcesses += [System.IO.Path]::GetFileNameWithoutExtension([string]$exe)
        }
    }
    $targetProcesses = @($targetProcesses | Select-Object -Unique)
    $running = @($targetProcesses | ForEach-Object { Get-Process -Name $_ -ErrorAction SilentlyContinue } | Sort-Object Id -Unique)
    if ($running.Count -gt 0) {
        Write-Host 'Closing supported players to update their loaded DLLs...' -ForegroundColor Cyan
        foreach ($proc in $running) {
            try {
                if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { [void]$proc.CloseMainWindow() }
            } catch {}
        }
        Start-Sleep -Seconds 3
        $stillRunning = @($running | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
        foreach ($proc in $stillRunning) {
            if ($proc.ProcessName -match 'Photos') {
                throw 'Microsoft Photos is still open. Save any unsaved edits and close Photos, then run INSTALL.cmd again.'
            }
            Write-Host ('Closing background player process: ' + $proc.ProcessName + ' (' + $proc.Id + ')')
            Stop-Process -Id $proc.Id -Force -ErrorAction Stop
        }
        Start-Sleep -Milliseconds 900
    }
}

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
        if (Test-Path -LiteralPath $to) {
            $incomingHash = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
            $installedHash = (Get-FileHash -LiteralPath $to -Algorithm SHA256).Hash
            if ($incomingHash -eq $installedHash) {
                Write-Host ('Already installed (unchanged): ' + $targetStage.Name + ' / ' + $name) -ForegroundColor DarkGray
                continue
            }
        }
        $copied = $false
        $copyError = $null
        for ($attempt = 0; $attempt -lt 12 -and -not $copied; $attempt++) {
            try {
                Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop
                $copied = $true
            }
            catch [System.IO.IOException] {
                $copyError = $_.Exception.Message
                Start-Sleep -Milliseconds 500
            }
        }
        if (-not $copied) {
            throw ('Cannot update ' + $targetStage.Name + ' / ' + $name + ': another process is still using the DLL. Existing file preserved. ' + $copyError)
        }
    }
    Remove-Item -LiteralPath (Join-Path $targetStage.Path 'logging.on') -Force -ErrorAction SilentlyContinue
    Write-Host ('Staged: ' + $targetStage.Name) -ForegroundColor Cyan
}
$sourceProbe = Join-Path $root 'bin\P7MediaProbe.exe'
if (-not (Test-Path -LiteralPath $sourceProbe)) { throw 'Missing media preflight helper after build.' }
Copy-Item -LiteralPath $sourceProbe -Destination (Join-Path $base 'P7MediaProbe.exe') -Force
Copy-Item -LiteralPath (Join-Path $root 'bin\dovi.dll') -Destination (Join-Path $base 'dovi.dll') -Force
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
$oldTaskPaused = $false
Write-Host 'Player background integration installed. No CMD needed for future launches.' -ForegroundColor Green
Write-Host 'Unified early attach and 2000 ms preflight are enabled for Microsoft Media Player and Movies & TV. Photos retains its original media-type path.' -ForegroundColor Cyan
Write-Host 'Automatic early hooking still depends on Windows process timing and cannot be guaranteed on every launch.' -ForegroundColor Yellow
Write-Host 'Microsoft Media Player, Movies & TV and Photos use the rebuilt hook DLL.' -ForegroundColor Green
