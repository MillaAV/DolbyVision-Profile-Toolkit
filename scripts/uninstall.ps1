param([string]$RootFile)
$ErrorActionPreference = 'Stop'

if (-not $RootFile) {
    try {
        $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        if ((Split-Path $root -Leaf) -cne 'DolbyVision-Profile-Enabler-1.0-beta' -or
            -not (Test-Path -LiteralPath (Join-Path $root 'src\P7ProfileHook.cpp') -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $root 'INSTALL.cmd') -PathType Leaf)) {
            throw 'The DolbyVision project directory could not be validated.'
        }
        $job = Join-Path $env:TEMP ('DVE-remove-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $job -Force | Out-Null
        $worker = Join-Path $job 'uninstall.ps1'
        $config = Join-Path $job 'root.txt'
        Copy-Item -LiteralPath $PSCommandPath -Destination $worker -ErrorAction Stop
        [IO.File]::WriteAllText($config, $root, [Text.UTF8Encoding]::new($false))
        $args = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -RootFile "{1}"' -f $worker, $config
        $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        if ($isAdmin) {
            Start-Process -FilePath 'powershell.exe' -ArgumentList $args -WorkingDirectory $env:SystemRoot -ErrorAction Stop | Out-Null
        } else {
            Start-Process -FilePath 'powershell.exe' -ArgumentList $args -Verb RunAs -WorkingDirectory $env:SystemRoot -ErrorAction Stop | Out-Null
        }
        exit 0
    } catch {
        Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
        exit 1
    }
}

$log = Join-Path $env:TEMP ('DolbyVision-uninstall-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))
$errors = [Collections.Generic.List[string]]::new()
function Note([string]$message) {
    Write-Host $message
    try { Add-Content -LiteralPath $log -Value $message -Encoding UTF8 } catch {}
}
function Remove-Path([string]$target) {
    if (-not (Test-Path -LiteralPath $target)) { return }
    try {
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
        Note ('Removed: ' + $target)
    } catch {
        $errors.Add($target + ' | ' + $_.Exception.Message)
        Note ('FAILED: ' + $target)
    }
}
try {
    Set-Location -LiteralPath $env:SystemRoot
    Start-Sleep -Milliseconds 1400
    $root = [IO.Path]::GetFullPath([IO.File]::ReadAllText($RootFile, [Text.Encoding]::UTF8).Trim())
    if ((Split-Path $root -Leaf) -cne 'DolbyVision-Profile-Enabler-1.0-beta' -or
        -not (Test-Path -LiteralPath (Join-Path $root 'src\P7ProfileHook.cpp') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $root 'INSTALL.cmd') -PathType Leaf)) {
        throw 'Project root verification failed. Nothing was removed.'
    }
    Note ('Uninstalling: ' + $root)
    Note ('Log: ' + $log)
    $taskNames = @('MillaAV P7 Dolby Vision AutoHook', 'MillaAV Dolby Vision Movies TV AutoHook', 'MillaAV Dolby Vision Player AutoHook')
    foreach ($taskName in $taskNames) {
        $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        if ($task) {
            try {
                Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
                Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction Stop
                Note ('Task removed: ' + $taskName)
            } catch { $errors.Add('Task ' + $taskName + ' | ' + $_.Exception.Message) }
        }
    }
    foreach ($processName in @('Microsoft.Media.Player', 'Video.UI', 'Microsoft.Photos', 'Photos', 'P7Injector', 'P7InjectorMulti')) {
        Get-Process -Name $processName -ErrorAction SilentlyContinue | ForEach-Object {
            try { Stop-Process -Id $_.Id -Force -ErrorAction Stop } catch { $errors.Add('PID ' + $_.Id + ' | ' + $_.Exception.Message) }
        }
    }
    Start-Sleep -Milliseconds 900
    foreach ($packageName in @('Microsoft.ZuneMusic', 'Microsoft.ZuneVideo', 'Microsoft.Windows.Photos')) {
        $packages = @(Get-AppxPackage -Name $packageName -ErrorAction SilentlyContinue)
        foreach ($package in $packages) {
            $state = Join-Path $env:LOCALAPPDATA ('Packages\' + $package.PackageFamilyName + '\LocalState')
            foreach ($sub in @('P7Hook','P7Hook-Probe')) {
                Remove-Path (Join-Path $state $sub)
            }
        }
    }
    Remove-Path (Join-Path $env:ProgramData 'MillaAV\P7DV')
    Remove-Path (Join-Path $env:LOCALAPPDATA 'DolbyVision-Profile-Enabler')
    Remove-Path $root
    if (Test-Path -LiteralPath $root) { $errors.Add('Project directory still exists.') }
    if ($errors.Count -gt 0) {
        foreach ($failure in $errors) { Note ('ISSUE: ' + $failure) }
        Note ('INCOMPLETE - ' + $errors.Count + ' error(s).')
    } else { Note 'UNINSTALL COMPLETED.' }
} catch { Note ('FATAL: ' + $_.Exception.Message) }
finally {
    Note ('Log saved to: ' + $log)
    Start-Sleep -Seconds 5
    try { Remove-Item -LiteralPath (Split-Path $RootFile -Parent) -Recurse -Force -ErrorAction SilentlyContinue } catch {}
}
