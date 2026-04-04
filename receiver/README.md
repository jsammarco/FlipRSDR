# FlipRSDR Receiver

FlipRSDR Receiver is the PC-side companion app for the Flipper Zero `FlipRSDR` external app.

## Features

- Live USB serial ingest from the Flipper Zero CDC stream
- Waveform view of the current pulse/gap timing burst
- Waterfall view using log-duration bins with SDR-style peak coloring
- Recording completed bursts to `.fliprsdr` by default, with JSONL still available
- Optional audible playback of completed bursts through the default Windows audio device

## Expected input

The receiver understands both Flipper output modes:

- `fliprsdr` binary packets framed with COBS and `0x00` delimiters
- JSON `burst_start`, `timing_chunk`, `burst_end`, and `burst_capture` messages

The protocol selector in the app defaults to `fliprsdr`.

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
