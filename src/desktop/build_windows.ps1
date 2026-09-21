param(
    [string]$Python = "",
    [string]$DistDir = (Join-Path $PSScriptRoot "..\dist\windows")
)

$ErrorActionPreference = "Stop"
$supportedPython = $Python
if (-not $supportedPython) {
    $launchers = & py -0p 2>$null
    foreach ($line in $launchers) {
        if ($line -match '3\.(?:10|11|12|13).*?([A-Za-z]:\\.*python\.exe)\s*$') {
            $supportedPython = $Matches[1]
            break
        }
    }
}
if (-not $supportedPython) {
    throw "Python 3.10 through 3.13 is required to build the Windows application"
}
$pythonVersion = & $supportedPython -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
if ($LASTEXITCODE -ne 0 -or [version]$pythonVersion -lt [version]'3.10' -or
    [version]$pythonVersion -ge [version]'3.14') {
    throw "Python 3.10 through 3.13 is required; Python 3.14 is not compatible with the WinForms backend"
}
$buildRoot = Join-Path $env:LOCALAPPDATA "VitaContentManager\build"
$pythonTag = & $supportedPython -c "import sys; print(f'py{sys.version_info.major}{sys.version_info.minor}')"
if ($LASTEXITCODE -ne 0 -or -not $pythonTag) { throw "Could not identify Windows Python version" }
$venv = Join-Path $buildRoot "venv-$pythonTag"
$pythonInVenv = Join-Path $venv "Scripts\python.exe"
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

& $supportedPython -m venv $venv
if ($LASTEXITCODE -ne 0) { throw "Could not create the Windows build environment" }
& $pythonInVenv -m pip install -r (Join-Path $PSScriptRoot "requirements.txt") pyinstaller
if ($LASTEXITCODE -ne 0) { throw "Could not install desktop build dependencies" }
& $pythonInVenv -c "from PIL import Image, _imaging; import webview; print('Native Pillow and pywebview imports OK')"
if ($LASTEXITCODE -ne 0) { throw "Windows build dependencies are incompatible with this Python version" }

$wpdBuild = Join-Path $buildRoot "wpd"
New-Item -ItemType Directory -Force -Path $wpdBuild | Out-Null
Copy-Item (Join-Path $PSScriptRoot "usb_wpd\main.cpp") (Join-Path $wpdBuild "main.cpp") -Force
Copy-Item (Join-Path $PSScriptRoot "usb_wpd\build_windows.cmd") (Join-Path $wpdBuild "build_windows.cmd") -Force
$wpdExe = Join-Path $wpdBuild "vcm-wpd.exe"
Push-Location $wpdBuild
try {
    & (Join-Path $wpdBuild "build_windows.cmd") $wpdExe
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $wpdExe)) { throw "Windows Portable Devices helper build failed" }
} finally {
    Pop-Location
}

& $pythonInVenv -m PyInstaller --noconfirm --onedir --windowed `
    --name VitaContentManagerPro `
    --paths "$PSScriptRoot\.." `
    --icon "$PSScriptRoot\ui\assets\content-manager-pro.ico" `
    --add-data "$PSScriptRoot\ui;ui" `
    --add-binary "$wpdExe;usb_wpd" `
    --workpath (Join-Path $buildRoot "work") `
    --specpath $buildRoot `
    --distpath $DistDir `
    (Join-Path $PSScriptRoot "app.py")
if ($LASTEXITCODE -ne 0) { throw "Windows package build failed" }
$pillowBinary = Get-ChildItem (Join-Path $DistDir 'VitaContentManagerPro\_internal\PIL\_imaging*.pyd') -ErrorAction SilentlyContinue
if (-not $pillowBinary) { throw "Windows package is missing Pillow's _imaging extension" }
Write-Host "Built: $(Join-Path $DistDir 'VitaContentManagerPro\VitaContentManagerPro.exe')"
