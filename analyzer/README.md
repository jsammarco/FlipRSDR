# FlipRSDR Analyzer

FlipRSDR Analyzer is an offline desktop app for exploring the recordings produced by `FlipRSDR Receiver`.

## Features

- Loads saved `.fliprsdr` recordings and legacy `burst_capture` JSONL files
- Replays bursts over time with optional audio
- Shows per-burst waveform and a recording-wide waterfall
- Lets you drag across the waterfall to select a burst range
- Saves the selected burst range back out as `.fliprsdr` or `.jsonl`
- Connects to a Flipper over serial and replays the selected burst range on-device
- Adds a burst timeline so you can scrub through activity over time
- Summarizes repeated timing signatures and likely frame-like bursts
- Attempts a lightweight short/long symbol decode when the burst shape looks compatible

## Running from source

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r analyzer\requirements.txt
.venv\Scripts\python analyzer\main.py
```

## Building a Windows exe

```powershell
.\build_analyzer.ps1
```

The final executable will be written to:

```text
build\FlipRSDR Analyzer.exe
```
