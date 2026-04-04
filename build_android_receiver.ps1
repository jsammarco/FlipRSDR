param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$AndroidRoot = Join-Path $ProjectRoot "android_receiver"
$BuildRoot = Join-Path $ProjectRoot "build"
$OutputApk = Join-Path $BuildRoot ("Android Receiver-" + $Configuration.ToLowerInvariant() + ".apk")
$GradleWrapper = Join-Path $AndroidRoot "gradlew.bat"
$AndroidStudioJava = "C:\Program Files\Android\Android Studio\jbr"
$GradleCommand = $null

if((!(Get-Command java -ErrorAction SilentlyContinue)) -and (Test-Path (Join-Path $AndroidStudioJava "bin\java.exe"))) {
    $env:JAVA_HOME = $AndroidStudioJava
    $env:PATH = (Join-Path $AndroidStudioJava "bin") + ";" + $env:PATH
}

if(Test-Path $GradleWrapper) {
    $GradleCommand = $GradleWrapper
} else {
    $Gradle = Get-Command gradle -ErrorAction SilentlyContinue
    if($Gradle) {
        $GradleCommand = $Gradle.Source
    }
}

if(-not $GradleCommand) {
    throw "Gradle was not found. Install Android Studio/Gradle or add a Gradle wrapper under android_receiver."
}

if(-not $env:ANDROID_SDK_ROOT -and -not $env:ANDROID_HOME) {
    $DefaultSdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
    if(Test-Path $DefaultSdk) {
        $env:ANDROID_SDK_ROOT = $DefaultSdk
    }
}

if(-not (Test-Path $BuildRoot)) {
    New-Item -ItemType Directory -Path $BuildRoot | Out-Null
}

$TaskName = if($Configuration -eq "Release") { "assembleRelease" } else { "assembleDebug" }
Write-Host "Building Android Receiver ($Configuration)..."
& $GradleCommand -p $AndroidRoot ":app:$TaskName"
if($LASTEXITCODE -ne 0) {
    throw "Gradle build failed with exit code $LASTEXITCODE"
}

$VariantDir = if($Configuration -eq "Release") { "release" } else { "debug" }
$BuiltApk = Join-Path $AndroidRoot "app\build\outputs\apk\$VariantDir\app-$($VariantDir).apk"
if(!(Test-Path $BuiltApk)) {
    throw "Gradle completed but the expected APK was not found at $BuiltApk"
}

Copy-Item -Force $BuiltApk $OutputApk
Write-Host "Build complete: $OutputApk"
