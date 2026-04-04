# Android Receiver

Android Receiver is the Android companion app for FlipRSDR. It mirrors the desktop receiver workflow with:

- USB serial ingest for Flipper CDC serial
- BLE serial ingest with Nordic UART fallback and generic write/notify characteristic discovery
- `fliprsdr` binary and JSON protocol parsing
- Start/stop scan, frequency, and RSSI remote commands
- Live waveform and waterfall views
- Optional audio playback for completed bursts
- Recording to app-scoped `.fliprsdr` or `.jsonl` files

## Build

From the repo root:

```powershell
.\build_android_receiver.ps1
```

The script builds `android_receiver/app` and copies the generated APK into:

```text
build\Android Receiver-debug.apk
```

## Requirements

- Android SDK installed locally
- Gradle available on `PATH`, or a Gradle wrapper added under `android_receiver`

If `ANDROID_SDK_ROOT` is not set, the build script will also try `%LOCALAPPDATA%\Android\Sdk`.
