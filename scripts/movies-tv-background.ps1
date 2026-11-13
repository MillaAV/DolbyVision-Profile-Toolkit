$ErrorActionPreference = 'Continue'
$base = 'C:\ProgramData\MillaAV\P7DV'
$injector = Join-Path $base 'P7InjectorMulti.exe'
$mediaProbe = Join-Path $base 'P7MediaProbe.exe'
$configFile = Join-Path $base 'player-targets.json'
$logFolder = Join-Path $env:LOCALAPPDATA 'DolbyVision-Profile-Enabler'
$log = Join-Path $logFolder 'player-autohook.log'
$seen = @{}
$lastMediaProbe = @{}
$lastMediaResult = @{}
$preflightPaths = @{}
$stageCache = @{}
function Log([string]$message) {
    if (-not (Test-Path -LiteralPath (Join-Path $base 'logging.on') -PathType Leaf)) { return }
    New-Item -Path $logFolder -ItemType Directory -Force | Out-Null
    Add-Content -LiteralPath $log -Value ('{0:o} {1}' -f (Get-Date), $message) -Encoding UTF8
}
Log 'Unified player monitor started.'
while ($true) {
    try {
        $config = Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json
        $live = @{}
        $active = @()
        $targets = @($config.targets | Where-Object { $_.enabled -eq $true } | Sort-Object { if ($_.package -eq 'Microsoft.ZuneVideo') { 0 } else { 1 } })
        foreach ($target in $targets) {
            $packageName = [string]$target.package
            if ($stageCache.ContainsKey($packageName)) {
                $stage = $stageCache[$packageName]
            } else {
                $pkg = Get-AppxPackage -Name $packageName -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $pkg) { continue }
                $stage = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState\P7Hook\P7ProfileHook.dll')
                $stageCache[$packageName] = $stage
            }
            if (-not (Test-Path -LiteralPath $stage)) { continue }
            foreach ($name in @($target.executables)) {
                $procName = [System.IO.Path]::GetFileNameWithoutExtension([string]$name)
                foreach ($player in @(Get-Process -Name $procName -ErrorAction SilentlyContinue)) {
                    $idText = [string]$player.Id
                    $started = 0
                    try { $started = $player.StartTime.ToUniversalTime().Ticks } catch {}
                    $key = $idText + ':' + $name + ':' + $started
                    $live[$key] = $true
                    $active += [pscustomobject]@{
                        Key = $key
                        Id = $idText
                        Name = $name
                        Package = $packageName
                        Stage = $stage
                        Player = $player
                    }
                    if ($seen.ContainsKey($key)) { continue }
                    $loaded = $false
                    try { $loaded = @($player.Modules | Where-Object ModuleName -eq 'P7ProfileHook.dll').Count -gt 0 } catch {}
                    if ($loaded) {
                        $seen[$key] = $true
                        Log ('Already hooked ' + $key)
                        continue
                    }
                    if (-not (Test-Path -LiteralPath $injector)) {
                        Log ('Injector missing for ' + $key)
                        continue
                    }
                    try {
                        $output = & $injector $stage $name $idText 2>&1 | Out-String
                        $code = $LASTEXITCODE
                        Log ('ATTACH_FIRST ' + $key + ' exit=' + $code + ' ' + $output.Trim())
                        if ($code -eq 0) { $seen[$key] = $true }
                    } catch { Log ('ATTACH_FAILED ' + $key + ' ' + $_.Exception.Message) }
                }
            }
        }
        if (Test-Path -LiteralPath $mediaProbe) {
            foreach ($entry in $active) {
                if ($entry.Package -eq 'Microsoft.Windows.Photos') { continue }
                $now = [DateTime]::UtcNow
                $previous = $lastMediaProbe[$entry.Key]
                if ($null -ne $previous -and ($now - $previous).TotalMilliseconds -lt 1000) { continue }
                $lastMediaProbe[$entry.Key] = $now
                $mediaDecision = Join-Path (Split-Path -Parent $entry.Stage) ('preflight-' + $entry.Id + '.flag')
                $preflightPaths[$entry.Key] = $mediaDecision
                try {
                    & $mediaProbe $entry.Id $mediaDecision 2>&1 | Out-Null
                    $probeStatus = $LASTEXITCODE
                    $statusText = switch ($probeStatus) {
                        0 { 'DOLBY_VISION' }
                        1 { 'NATIVE_HEVC' }
                        2 { 'UNKNOWN' }
                        default { 'FAILED_' + $probeStatus }
                    }
                    if ($lastMediaResult[$entry.Key] -ne $statusText) {
                        Log ($entry.Key + ' media-preflight=' + $statusText)
                        $lastMediaResult[$entry.Key] = $statusText
                    }
                } catch { Log ($entry.Key + ' media preflight error: ' + $_.Exception.Message) }
            }
        }
        foreach ($key in @($seen.Keys)) {
            if (-not $live.ContainsKey($key)) { $seen.Remove($key) }
        }
        foreach ($key in @($lastMediaProbe.Keys)) {
            if (-not $live.ContainsKey($key)) {
                $lastMediaProbe.Remove($key)
                $lastMediaResult.Remove($key)
                if ($preflightPaths.ContainsKey($key)) {
                    Remove-Item -LiteralPath $preflightPaths[$key] -Force -ErrorAction SilentlyContinue
                    $preflightPaths.Remove($key)
                }
            }
        }
    } catch { Log ('Monitor error: ' + $_.Exception.Message) }
    Start-Sleep -Milliseconds 100
}
