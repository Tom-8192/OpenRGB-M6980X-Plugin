# Builds the plugin DLL and the protocol test with MSVC 2022 x64 and Qt 6.
#
#   .\scripts\build-windows.ps1 [-QtDir C:\Qt\6.8.3\msvc2022_64] [-OutputDirectory dist]
#
# Needs the OpenRGB submodule (git submodule update --init).

param(
    [string]$QtDir = $(if ($env:QT_ROOT_DIR) { $env:QT_ROOT_DIR } else { 'C:\Qt\6.8.3\msvc2022_64' }),
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $root 'OpenRGB\OpenRGBPluginInterface.h'))) {
    throw 'OpenRGB headers missing: run "git submodule update --init" first'
}
if (-not (Test-Path (Join-Path $QtDir 'bin\qmake.exe'))) {
    throw "qmake not found in $QtDir"
}

if (-not $env:VCINSTALLDIR) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'MSVC toolchain not found' }
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $build | Out-Null

# protocol test (no Qt)
Push-Location $build
try {
    & cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I "$root\src" "$root\tests\protocol_test.cpp" "$root\src\M6980XProtocol.cpp" /Fe:protocol_test.exe | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'building the test failed' }
    & .\protocol_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'protocol test failed' }

    & (Join-Path $QtDir 'bin\qmake.exe') "$root\OpenRGB-M6980X.pro" CONFIG+=release
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }
    & nmake /nologo
    if ($LASTEXITCODE -ne 0) { throw 'nmake failed' }
}
finally {
    Pop-Location
}

$out = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $build 'release\OpenRGB-M6980X.dll') $out -Force
Write-Host "Plugin: $(Join-Path $out 'OpenRGB-M6980X.dll')"
