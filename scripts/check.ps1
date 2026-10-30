$ErrorActionPreference = 'Stop'
$reportDir = Join-Path $env:LOCALAPPDATA 'DolbyVision-Profile-Enabler'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$report = Join-Path $reportDir 'dolby-vision-report.txt'
$results = [System.Collections.Generic.List[string]]::new()
function Print([string]$message) { Write-Host $message; $script:results.Add($message) }
function Matches($rows,[string]$pattern) { return @($rows | Where-Object { $_ -match $pattern }).Count }
$targets = @(
    @{ Package='Microsoft.ZuneMusic'; Name='Media Player'; Exes=@('Microsoft.Media.Player') },
    @{ Package='Microsoft.ZuneVideo'; Name='Movies & TV'; Exes=@('Video.UI') },
    @{ Package='Microsoft.Windows.Photos'; Name='Photos'; Exes=@('Microsoft.Photos','Photos') }
)
$sessions = @()
foreach ($target in $targets) {
    $pkg = Get-AppxPackage -Name $target.Package -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pkg) { continue }
    $state = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState')
    foreach ($subdir in @('P7Hook', 'P7Hook-Probe')) {
        $path = Join-Path $state ($subdir + '\P7-PROFILE-HOOK.log')
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try {
            $file = Get-Item -LiteralPath $path
            $rows = @(Get-Content -LiteralPath $path -Encoding Unicode -Tail 50000 -ErrorAction Stop)
            $start = -1
            $pidText = ''
            for ($i=0; $i -lt $rows.Count; $i++) {
                if ($rows[$i] -match '\bpid=(\d+)\s+(?:v\d|Version\s+1\.0-beta)') {
                    if ($start -ge 0 -and $i -gt $start) {
                        $chunk = @($rows[$start..($i-1)])
                        $sessions += [pscustomobject]@{ Name=$target.Name; Pid=$pidText; Rows=$chunk; Path=$path; Time=([datetime]::ParseExact(($rows[$start].Substring(0,23)),'yyyy-MM-dd HH:mm:ss.fff',$null)) }
                    }
                    $start=$i; $pidText=$Matches[1]
                }
            }
            if ($start -ge 0) {
                $chunk = @($rows[$start..($rows.Count-1)])
                $sessions += [pscustomobject]@{ Name=$target.Name; Pid=$pidText; Rows=$chunk; Path=$path; Time=([datetime]::ParseExact(($rows[$start].Substring(0,23)),'yyyy-MM-dd HH:mm:ss.fff',$null)) }
            }
        } catch { Print ('Unable to read log: ' + $_.Exception.Message) }
    }
}
$active = @()
foreach ($session in $sessions) {
    $p = Get-Process -Id ([int]$session.Pid) -ErrorAction SilentlyContinue
    if (-not $p) { continue }
    $expected = switch ($session.Name) { 'Media Player' { @('Microsoft.Media.Player') } 'Movies & TV' { @('Video.UI') } default { @('Microsoft.Photos','Photos') } }
    if ($expected -contains $p.ProcessName) { $active += $session }
}
$pool = if ($active.Count) { $active } else { $sessions }
$selected = $pool | Sort-Object Time -Descending | Select-Object -First 1
Print 'Dolby Vision - playback status'
if (-not $selected) { Print 'No playback session recorded.'; $results | Set-Content -LiteralPath $report -Encoding UTF8; exit 2 }
$rows=@($selected.Rows)
$in = Matches $rows 'FEL DOLBY_IN frame=.*ProcessInput original hr=0x00000000'
$out = Matches $rows 'FEL DOLBY_OUT ProcessOutput call=.*hr=0x00000000'
$latest = $rows | Where-Object { $_ -match 'FEL RPU FRAME=' } | Select-Object -Last 1
$profile='Not confirmed'
if ($latest -and $latest -match '\bprofile=(5|7|8|9)\b') { $profile='P'+$Matches[1] }
Print ('Player: ' + $selected.Name)
Print ('Session: ' + $(if ($active.Count) {'Active'} else {'Last recorded'}))
Print ('Profile: ' + $profile)
Print ('Dolby input/output: ' + $in + ' / ' + $out)
if ($profile -eq 'P7') {
    $layer = 'Not confirmed'
    if ($latest -match '\bel=FEL\b') { $layer='FEL' } elseif ($latest -match '\bel=MEL\b') { $layer='MEL' }
    Print ('Enhancement: ' + $layer)
    if ($layer -eq 'FEL') {
        $gpu=Matches $rows 'FEL GPU RECON DISPATCH'
        $applied=Matches $rows 'FEL DOLBY_OUT frame=.*postDolbyFEL=YES'
        $missed=Matches $rows 'FEL DOLBY_OUT frame=.*postDolbyFEL=NO'
        Print ('FEL GPU / applied / skipped: ' + $gpu + ' / ' + $applied + ' / ' + $missed)
        if ($gpu -gt 0 -and $applied -gt 0) { Print 'Status: FEL pipeline active (visual accuracy unverified)' } else { Print 'Status: FEL not confirmed' }
    }
}
if ($in -gt 0 -and $out -gt 0) { Print 'Dolby processing: ACTIVE' } else { Print 'Dolby processing: NOT CONFIRMED' }
Print ('Session time: ' + $selected.Time.ToString('yyyy-MM-dd HH:mm:ss'))
Print ('Log: ' + $selected.Path)
$results | Set-Content -LiteralPath $report -Encoding UTF8
