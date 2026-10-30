$ErrorActionPreference = 'Continue'
$base = 'C:\ProgramData\MillaAV\P7DV'
$injector = Join-Path $base 'P7InjectorMulti.exe'
$configFile = Join-Path $base 'player-targets.json'
$logFolder = Join-Path $env:LOCALAPPDATA 'DolbyVision-Profile-Enabler'
New-Item -Path $logFolder -ItemType Directory -Force | Out-Null
$log = Join-Path $logFolder 'player-autohook.log'
$seen = @{}
function Log([string]$message) {
    Add-Content -LiteralPath $log -Value ('{0:o} {1}' -f (Get-Date), $message) -Encoding UTF8
}
Log 'Player monitor started.'
while ($true) {
    try {
        $config = Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json
        $live = @{}
        foreach ($target in @($config.targets | Where-Object { $_.enabled -eq $true })) {
            $pkg = Get-AppxPackage -Name $target.package -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $pkg) { continue }
            $stage = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState\P7Hook\P7ProfileHook.dll')
            if (-not (Test-Path -LiteralPath $stage)) { continue }
            foreach ($name in @($target.executables)) {
                $procName = [System.IO.Path]::GetFileNameWithoutExtension([string]$name)
                foreach ($player in @(Get-Process -Name $procName -ErrorAction SilentlyContinue)) {
                    $idText = [string]$player.Id
                    $key = $idText + ':' + $name
                    $live[$key] = $true
                    if ($seen.ContainsKey($key)) { continue }
                    $loaded = $false
                    try { $loaded = @($player.Modules | Where-Object ModuleName -eq 'P7ProfileHook.dll').Count -gt 0 } catch {}
                    if ($loaded) { $seen[$key] = $true; Log ('Already hooked ' + $key); continue }
                    if (-not (Test-Path -LiteralPath $injector)) { Log 'Injector missing'; continue }
                    $output = & $injector $stage $name $idText 2>&1 | Out-String
                    $code = $LASTEXITCODE
                    Log ($key + ' exit=' + $code + ' ' + $output.Trim())
                    if ($code -eq 0) { $seen[$key] = $true }
                }
            }
        }
        foreach ($key in @($seen.Keys)) { if (-not $live.ContainsKey($key)) { $seen.Remove($key) } }
    } catch { Log ('Monitor error: ' + $_.Exception.Message) }
    Start-Sleep -Milliseconds 100
}
