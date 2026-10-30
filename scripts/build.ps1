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

$cmd1 = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /Oi /Ot /GL /arch:AVX2 /openmp /EHsc /W4 /LD /I"' + $inc + '" /I"' + $mhSrc + '" /I"' + $hde + '" "' + $dllSrc + '" "' + $buffer + '" "' + $hook + '" "' + $trampoline + '" "' + $hde64 + '" /link /LTCG /OUT:"' + $dllOut + '" mfplat.lib mfuuid.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib'
cmd.exe /d /s /c $cmd1
if ($LASTEXITCODE -ne 0) { throw "Hook DLL build failed: $LASTEXITCODE" }

$cmd2 = 'call "' + $vcvars + '" && cl /nologo /std:c++17 /O2 /EHsc /W4 "' + $exeSrc + '" /link /SUBSYSTEM:CONSOLE /OUT:"' + $exeOut + '"'
cmd.exe /d /s /c $cmd2
if ($LASTEXITCODE -ne 0) { throw "Injector build failed: $LASTEXITCODE" }

Write-Host ''
Write-Host 'BUILT Version 1.0-beta:' -ForegroundColor Green
Write-Host $dllOut
Write-Host $exeOut
