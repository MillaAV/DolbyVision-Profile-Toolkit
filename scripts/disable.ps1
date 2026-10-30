#Requires -RunAsAdministrator
$ErrorActionPreference='Stop'
$name='MillaAV P7 Dolby Vision AutoHook'
$other = 'MillaAV Dolby Vision Movies TV AutoHook'
$otherTask = Get-ScheduledTask -TaskName $other -ErrorAction SilentlyContinue
if ($otherTask) { Stop-ScheduledTask -TaskName $other -ErrorAction SilentlyContinue; Disable-ScheduledTask -TaskName $other | Out-Null }

$task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
if ($task) { Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue; Disable-ScheduledTask -TaskName $name | Out-Null }
Write-Host 'AutoHook disabled. Restart Media Player to stop hook injection.' -ForegroundColor Green
