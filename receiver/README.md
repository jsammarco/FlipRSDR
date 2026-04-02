# FlipRSDR Receiver

FlipRSDR Receiver is the PC-side companion app for the Flipper Zero `FlipRSDR` external app.

## Features

- Live USB serial ingest from the Flipper Zero CDC stream
- Waveform view of the current pulse/gap timing burst
- Waterfall view using log-duration bins with SDR-style peak coloring
- Recording completed bursts to JSONL for later analysis
- Optional audible playback of completed bursts through the default Windows audio device

## Expected input

The receiver understands the JSON lines emitted by the Flipper app:

- `burst_start`
- `timing_chunk`
- `burst_end`
- `burst_capture`

It also tolerates serial monitor prefixes and extracts the JSON object from each line.

## Running from source

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r receiver\requirements.txt
.venv\Scripts\python receiver\main.py
```

## Building a Windows exe

Use the root build script:

```powershell
.\build_receiver.ps1
```

The final executable will be written to:

```text
build\FlipRSDR Receiver.exe
```
