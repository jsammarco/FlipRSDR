# FlipRSDR

FlipRSDR is a Flipper Zero external app that captures raw demodulated Sub-GHz pulse/gap timings and streams them to a PC over USB CDC or BLE serial. It is intentionally focused on pulse timing fidelity rather than protocol decoding.

## What it does

- Captures ordered pulse/gap durations in microseconds using the firmware async Sub-GHz receive path
- Detects burst start, burst continuation, and burst end from gap timing and idle timeout
- Preserves the full timing burst locally in buffered modes, including truncation and overflow flags
- Streams the compact `fliprsdr` binary protocol by default, with JSON kept as a debug option
- Supports USB CDC on dual-CDC channel `1` and BLE serial

## Screenshots

### Flipper app

![Set frequency page](https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/flipper%20screenshots/Set%20Freq%20Page..png)

<table width="100%">
  <tr>
    <td align="center" width="50%">
      <img src="https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/flipper%20screenshots/Freq%20Menu.png" alt="Frequency menu" width="100%" />
    </td>
    <td align="center" width="50%">
      <img src="https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/flipper%20screenshots/Menu%20Page%202.png" alt="Menu page 2" width="100%" />
    </td>
  </tr>
  <tr>
    <td align="center">
      Frequency menu
    </td>
    <td align="center">
      Menu page 2
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/flipper%20screenshots/Scanning%20Screen.png" alt="Scanning screen" width="100%" />
    </td>
    <td align="center" width="50%">
      <img src="https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/flipper%20screenshots/Scanning%20Idle.png" alt="Scanning idle" width="100%" />
    </td>
  </tr>
  <tr>
    <td align="center">
      Scanning screen
    </td>
    <td align="center">
      Scanning idle
    </td>
  </tr>
</table>

### Receiver app

![FlipRSDR Receiver screenshot](https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/receiver%20screeshots/receiver%20screenshot.jpg)

### Analyzer app

![FlipRSDR Analyzer screenshot](https://raw.githubusercontent.com/jsammarco/FlipRSDR/refs/heads/main/screenshots/analyzer/analyzer%20screenshot.jpg)

## Main flow

- `Start Capture`
  - `OK` starts or stops listening
  - `Left` clears the buffered burst
  - `Right` sends the buffered burst
  - `Back` leaves the screen and stops capture
- `Settings`
  - Frequency preset
  - Frequency fine-tune offset
  - Transport
  - Protocol format
  - Streaming mode
  - Auto-send after burst complete
  - Include RSSI
  - Include timestamp
  - Max pulse count
  - Capture timeout
  - Burst-end gap threshold
  - Debug send action
- `About`

## Streaming format

The default serial format is the custom `fliprsdr` binary protocol defined in `fliprsdr_binary_protocol_spec.md`. It uses:

- COBS framing with `0x00` delimiters
- CRC-16/XMODEM integrity checks
- little-endian metadata fields
- unsigned varints for timing arrays

JSON remains available as a compatibility/debug option. In either format, `first_level` is preserved so the PC side can reconstruct the exact pulse/gap ordering even if the first received interval is a gap.

## File structure

- `app/fliprsdr.c`
  - App allocation, view dispatcher, scene manager, and shared refresh helpers
- `app/fliprsdr.h`
  - Shared enums, constants, settings, burst, and snapshot structs
- `app/fliprsdr_app.h`
  - Internal app struct, view ids, and custom events
- `app/settings.c` / `app/settings.h`
  - Persistent settings using `saved_struct`
- `app/burst_buffer.c` / `app/burst_buffer.h`
  - Fixed-capacity burst storage with truncation tracking
- `app/capture.c` / `app/capture.h`
  - Raw async Sub-GHz receive worker and burst assembly
- `app/transport.c` / `app/transport.h`
  - Shared transport thread and backend selection
- `app/transport_usb.c`
  - USB CDC transport backend
- `app/transport_ble.c`
  - BLE serial transport backend
- `app/protocol.c` / `app/protocol.h`
  - `fliprsdr` binary encoding plus JSON compatibility formatting
- `views/capture_view.c` / `views/capture_view.h`
  - Custom capture status screen
- `scenes/`
  - Menu, capture, settings, and about scenes
- `application.fam`
  - Flipper manifest
- `build.ps1`
  - Sync-and-build helper for a local firmware checkout

## Firmware API assumptions

- Raw receive uses `furi_hal_subghz_start_async_rx(...)`
- The receive preset uses `subghz_device_cc1101_preset_ook_270khz_async_regs`
- USB streaming uses:
  - `furi_hal_usb_set_config(&usb_cdc_dual, NULL)`
  - `furi_hal_cdc_set_callbacks(...)`
  - `furi_hal_cdc_send(...)`
- BLE streaming uses:
  - `bt_profile_start(..., ble_profile_serial, NULL)`
  - `ble_profile_serial_set_event_callback(...)`
  - `ble_profile_serial_tx(...)`

## Notes and TODOs

- The app currently uses the public async raw receive API directly and does not depend on protocol decode paths.
- Buffered modes intentionally stop after one completed burst so the last burst can be preserved exactly and resent without being overwritten. This matches the v1 priority of “one complete burst at a time is acceptable”.
- `TODO`: the OOK raw receive preset may need tuning per target signal family or future firmware changes.
- `TODO`: USB transport currently assumes dual CDC channel `1` is the safest user-facing serial endpoint.
- `TODO`: BLE transport restores the default serial profile on exit because the public API does not expose the previously active profile template.

## Building

Default:

```powershell
.\build.ps1
```

Preview only:

```powershell
.\build.ps1 -PreviewSync
```

Skip build after sync:

```powershell
.\build.ps1 -SkipBuild
```

## PC receiver

The Windows desktop companion app lives under `receiver/` and is called `FlipRSDR Receiver`.

- Live serial ingest from the Flipper CDC stream
- Waveform view for the current pulse train
- Waterfall view using log-duration bins with SDR-style coloring
- Optional recording of completed bursts to `.fliprsdr` binary files or JSONL
- Optional audible playback of completed bursts
- Automatic port refresh while disconnected, plus last-port recall on restart

Build the Windows executable with:

```powershell
.\build_receiver.ps1
```

The build script writes the final executable to:

```text
build\FlipRSDR Receiver.exe
```

## PC analyzer

The offline analysis companion app lives under `analyzer/` and is called `FlipRSDR Analyzer`.

- Loads saved `.fliprsdr` recordings from the receiver, plus legacy JSONL
- Replays bursts over recording time with optional audio playback
- Shows waveform and a full recording waterfall view
- Highlights repeated timing signatures and likely frame-like bursts
- Attempts a lightweight short/long symbol decode for bursts that resemble simple PWM-style framing

Build the Windows executable with:

```powershell
.\build_analyzer.ps1
```

The build script writes the final executable to:

```text
build\FlipRSDR Analyzer.exe
```
