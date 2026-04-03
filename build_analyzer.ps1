param(
    [string]$PythonExe = "python",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$AnalyzerRoot = Join-Path $ProjectRoot "analyzer"
$BuildRoot = Join-Path $ProjectRoot "build"
$VenvRoot = Join-Path $BuildRoot "analyzer_venv"
$WorkRoot = Join-Path $BuildRoot "analyzer-pyinstaller-work"
$SpecRoot = Join-Path $BuildRoot "analyzer-pyinstaller-spec"
$DistRoot = Join-Path $BuildRoot "analyzer-pyinstaller-dist"
$EntryPoint = Join-Path $AnalyzerRoot "main.py"
$Requirements = Join-Path $AnalyzerRoot "requirements.txt"
$VenvPython = Join-Path $VenvRoot "Scripts\\python.exe"
$FinalExe = Join-Path $BuildRoot "FlipRSDR Analyzer.exe"

if(!(Test-Path $BuildRoot)) {
    New-Item -ItemType Directory -Path $BuildRoot | Out-Null
}

if($Clean) {
    @($WorkRoot, $SpecRoot, $DistRoot, $FinalExe) | ForEach-Object {
        if(Test-Path $_) {
            Remove-Item -Recurse -Force $_
        }
    }
}

if(!(Test-Path $VenvPython)) {
    Write-Host "Creating analyzer build virtual environment..."
    & $PythonExe -m venv $VenvRoot
}

Write-Host "Installing analyzer build dependencies..."
& $VenvPython -m pip install --upgrade pip setuptools wheel
& $VenvPython -m pip install -r $Requirements pyinstaller

Write-Host "Building $FinalExe ..."
& $VenvPython -m PyInstaller `
    --noconfirm `
    --clean `
    --windowed `
    --onefile `
    --name "FlipRSDRAnalyzer" `
    --distpath $DistRoot `
    --workpath $WorkRoot `
    --specpath $SpecRoot `
    $EntryPoint

$BuiltExe = Join-Path $DistRoot "FlipRSDRAnalyzer.exe"
if(!(Test-Path $BuiltExe)) {
    throw "PyInstaller did not produce the expected executable."
}

Copy-Item -Force $BuiltExe $FinalExe
Write-Host "Build complete: $FinalExe"
