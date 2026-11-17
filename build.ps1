# build.ps1 - build aerialsync.exe (requires MSVC / VS Build Tools)
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# locate MSVC via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; install Visual Studio Build Tools" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "MSVC toolset not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found" }

# compile inside vcvars environment (single cmd invocation)
$src = Join-Path $root "src"
$out = Join-Path $root "aerialsync.exe"
$cmd = "call `"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /utf-8 /O2 /W3 `"$src\main.cpp`" `"$src\AppleSync.cpp`" `"$src\AppleSource.cpp`" /Fe:`"$out`" /Fo:`"$root\\`""
Write-Host "Compiling..."
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "Build OK: $out"
