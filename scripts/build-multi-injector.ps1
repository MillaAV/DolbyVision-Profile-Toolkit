$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Find-Compiler {
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { return $null }
    $bat = Join-Path ([string]($path | Select-Object -First 1)) 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $bat) { return $bat }
    return $null
}
$vcvars=Find-Compiler
if (-not $vcvars) {
    Write-Host 'C++ Build Tools are missing. Installing Microsoft Visual Studio Build Tools...' -ForegroundColor Yellow
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'winget is unavailable. Install Microsoft App Installer and try again.' }
    & winget.exe install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --accept-source-agreements --accept-package-agreements --silent --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
    if ($LASTEXITCODE -ne 0) { throw ('Visual Studio Build Tools installation failed: ' + $LASTEXITCODE) }
    $vcvars=Find-Compiler
    if (-not $vcvars) { throw 'Build Tools installation completed, but an x64 C++ compiler was not found.' }
}
$source=Join-Path $root 'src\P7Injector.cpp'
$output=Join-Path $root 'bin\P7InjectorMulti.exe'
$command='"'+$vcvars+'" && cl /nologo /std:c++17 /EHsc /O2 /W3 /DUNICODE /D_UNICODE /Fe:"'+$output+'" "'+$source+'" /link /SUBSYSTEM:CONSOLE'
Push-Location (Join-Path $root 'bin')
try {
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output)) { throw 'Injector compilation failed.' }
    Write-Host 'Injector built successfully.' -ForegroundColor Green
} finally { Pop-Location }
