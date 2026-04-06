param(
    [string]$SourceFile = "C:\Users\Joe\Projects\flipperzero-firmware\build\f7-firmware-D\.extapps\fliprsdr.fap",
    [string]$FirmwareDir = "C:\Users\Joe\Projects\flipperzero-firmware",
    [string]$Port = "COM17",
    [string]$DestinationDir = "/ext/apps/Sub-GHz/_My_Apps",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Resolve-PythonCommand {
    if(Get-Command py -ErrorAction SilentlyContinue) {
        return @("py", "-3")
    }

    if(Get-Command python -ErrorAction SilentlyContinue) {
        return @("python")
    }

    throw "Python was not found in PATH. Install Python or use the firmware toolchain environment first."
}

$SourceFile = [System.IO.Path]::GetFullPath($SourceFile)
$FirmwareDir = [System.IO.Path]::GetFullPath($FirmwareDir)
$StorageScript = Join-Path $FirmwareDir "scripts\storage.py"

if(!(Test-Path $SourceFile -PathType Leaf)) {
    throw "Built app was not found: $SourceFile"
}

if(!(Test-Path $FirmwareDir -PathType Container)) {
    throw "FirmwareDir does not exist: $FirmwareDir"
}

if(!(Test-Path $StorageScript -PathType Leaf)) {
    throw "Flipper storage helper was not found: $StorageScript"
}

$destinationDir = ($DestinationDir -replace "\\", "/").Trim()
if([string]::IsNullOrWhiteSpace($destinationDir)) {
    throw "DestinationDir cannot be empty."
}

if(!$destinationDir.StartsWith("/")) {
    throw "DestinationDir must be an absolute Flipper path such as /ext/apps/Sub-GHz/_My_Apps"
}

$destinationDir = $destinationDir.TrimEnd("/")
$destinationFile = "$destinationDir/$([System.IO.Path]::GetFileName($SourceFile))"
$pythonCommand = Resolve-PythonCommand

Write-Host "SourceFile     : $SourceFile"
Write-Host "FirmwareDir    : $FirmwareDir"
Write-Host "Port           : $Port"
Write-Host "DestinationDir : $destinationDir"
Write-Host "DestinationFile: $destinationFile"

Push-Location $FirmwareDir
try {
    $pythonExe = $pythonCommand[0]
    $pythonArgs = @()
    if($pythonCommand.Count -gt 1) {
        $pythonArgs += $pythonCommand[1..($pythonCommand.Count - 1)]
    }

    $sendArgs = @($StorageScript, "-p", $Port, "send")
    if($Force) {
        $sendArgs += "--force"
    }
    $sendArgs += @($SourceFile, $destinationFile)

    & $pythonExe @pythonArgs @sendArgs
    if($LASTEXITCODE -ne 0) {
        throw "Upload failed."
    }
} finally {
    Pop-Location
}

Write-Host "Upload complete."
