#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'
$out = Join-Path $root 'bin'
$third = Join-Path $root 'third_party'
$mhRoot = Join-Path $third 'minhook'
$doviRoot = Join-Path $third 'libdovi'
$doviDll = Join-Path $out 'dovi.dll'
New-Item -ItemType Directory -Force -Path $out,$third | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools not found.' }
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw 'VC++ x64 tools not found.' }
$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

if (-not (Test-Path $doviDll)) {
    Write-Host 'Downloading libdovi 3.4.0...' -ForegroundColor Yellow
    $doviZip = Join-Path $third 'libdovi-3.4.0.zip'
    $doviTmp = Join-Path $third 'libdovi-temp'
    Remove-Item $doviZip,$doviTmp -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -UseBasicParsing 'https://github.com/quietvoid/dovi_tool/releases/download/2.3.3/libdovi-3.4.0-x86_64-pc-windows-msvc.zip' -OutFile $doviZip
    Expand-Archive $doviZip -DestinationPath $doviTmp -Force
    $foundDovi = Get-ChildItem $doviTmp -Recurse -File | Where-Object { $_.Name -match '^(lib)?dovi\.dll$' } | Select-Object -First 1
    if (-not $foundDovi) { throw 'libdovi DLL was not found in release archive.' }
    New-Item -ItemType Directory -Force -Path $doviRoot | Out-Null
    Copy-Item $foundDovi.FullName $doviDll -Force
    Copy-Item $foundDovi.FullName (Join-Path $doviRoot 'dovi.dll') -Force
    Remove-Item $doviZip,$doviTmp -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path (Join-Path $mhRoot 'include\MinHook.h'))) {
    Write-Host 'Downloading MinHook v1.3.4...' -ForegroundColor Yellow
    $zip = Join-Path $third 'minhook-v1.3.4.zip'
    $tmp = Join-Path $third 'minhook-temp'
    Remove-Item $zip,$tmp -Recurse -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -UseBasicParsing 'https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath $tmp -Force
    $downloaded = Get-ChildItem $tmp -Directory | Select-Object -First 1
    if (-not $downloaded) { throw 'MinHook archive has no directory.' }
    Remove-Item $mhRoot -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $downloaded.FullName $mhRoot
    Remove-Item $zip,$tmp -Recurse -Force -ErrorAction SilentlyContinue
}

$dllSrc = Join-Path $src 'P7ProfileHook.cpp'
$vpSrc = Join-Path $src 'Hdr10VideoProcessorBridge.cpp'
$vpAbiSrc = Join-Path $src 'Hdr10VideoProcessorCAbi.c'
$exeSrc = Join-Path $src 'P7Injector.cpp'
$dllOut = Join-Path $out 'P7ProfileHook.dll'
$exeOut = Join-Path $out 'P7Injector.exe'
$inc = Join-Path $mhRoot 'include'
$mhSrc = Join-Path $mhRoot 'src'
$hde = Join-Path $mhSrc 'hde'

$buffer = Join-Path $mhSrc 'buffer.c'
$hook = Join-Path $mhSrc 'hook.c'
$trampoline = Join-Path $mhSrc 'trampoline.c'
$hde64 = Join-Path $hde 'hde64.c'

$cmd1 = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /Oi /Ot /GL /arch:AVX2 /openmp /EHsc /W4 /LD /I"' + $inc + '" /I"' + $mhSrc + '" /I"' + $hde + '" "' + $dllSrc + '" "' + $vpSrc + '" "' + $vpAbiSrc + '" "' + $buffer + '" "' + $hook + '" "' + $trampoline + '" "' + $hde64 + '" /link /LTCG /OUT:"' + $dllOut + '" mfplat.lib mfuuid.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib dxguid.lib'
$hookBuildLog = Join-Path $out 'P7ProfileHook-build.log'
$hookBuildCommand = $cmd1 + ' 2>&1'
Write-Host ('Compiling hook; compiler output: ' + $hookBuildLog) -ForegroundColor Cyan
$hookBuildOutput = & cmd.exe /d /s /c $hookBuildCommand
$hookBuildExitCode = $LASTEXITCODE
@($hookBuildOutput) | Set-Content -LiteralPath $hookBuildLog -Encoding UTF8
$hookBuildOutput | Out-Host
if ($hookBuildExitCode -ne 0) {
    Write-Host ''
    Write-Host 'MSVC errors (from full compiler output):' -ForegroundColor Red
    $errors = @($hookBuildOutput | Select-String -Pattern '(^|\s)(fatal error|error) (C\d+|LNK\d+|D\d+)|fatal error|undefined reference|unresolved external|warning LNK')
    if ($errors.Count -gt 0) {
        $errors | Select-Object -First 40 | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    } else {
        Write-Host 'No compiler error line matched. Please send the complete log file.' -ForegroundColor Yellow
    }
    throw ('Hook DLL build failed: ' + $hookBuildExitCode + '. Full log: ' + $hookBuildLog)
}

$cmd2 = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /EHsc /W4 "' + $exeSrc + '" /link /SUBSYSTEM:CONSOLE /OUT:"' + $exeOut + '"'
cmd.exe /d /s /c $cmd2
if ($LASTEXITCODE -ne 0) { throw "Injector build failed: $LASTEXITCODE" }

$probeSrc = Join-Path $src 'P7MediaProbe.cpp'
$probeOut = Join-Path $out 'P7MediaProbe.exe'
$cmd3 = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /EHsc /W4 /I"' + $src + '" "' + $probeSrc + '" /link /SUBSYSTEM:CONSOLE /OUT:"' + $probeOut + '" ole32.lib shell32.lib uuid.lib'
cmd.exe /d /s /c $cmd3
if ($LASTEXITCODE -ne 0) { throw "Media preflight helper build failed: $LASTEXITCODE" }

$hdrSource = Join-Path $src 'P7HdrOutputCheck.cpp'
$hdrOutput = Join-Path $out 'P7HdrOutputCheck.exe'
$hdrCommand = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /EHsc /W4 "' + $hdrSource + '" /link /SUBSYSTEM:CONSOLE /OUT:"' + $hdrOutput + '" dxgi.lib'
cmd.exe /d /s /c $hdrCommand
if ($LASTEXITCODE -ne 0) { throw "HDR output checker build failed: $LASTEXITCODE" }

Write-Host ''
Write-Host 'BUILT Version 1.0-beta:'  -ForegroundColor Green
Write-Host $dllOut
Write-Host $exeOut
Write-Host $probeOut
Write-Host $hdrOutput
