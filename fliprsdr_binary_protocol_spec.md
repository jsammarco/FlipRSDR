# FlipRSDR Binary Serial Protocol Specification

## Overview

This document defines a compact binary serial protocol for FlipRSDR to replace the current JSON-based transport used between the Flipper Zero app and the PC receiver/analyzer.

The protocol is designed for:

- compact serial transport over USB CDC and BLE serial
- preservation of exact ordered pulse/gap timing data
- robust framing and recovery from partial or corrupt packets
- simple implementation in C on Flipper Zero and in desktop receiver software
- compatibility with both live streaming and buffered burst transfer modes

This protocol is intended to carry the same logical information currently represented by JSON messages such as `burst_start`, `timing_chunk`, `burst_end`, and `burst_capture`, while using far fewer bytes and less CPU overhead. The existing app currently streams raw demodulated Sub-GHz pulse/gap timings to a PC over USB CDC or BLE serial, with support for burst detection, buffered capture, truncation flags, and optional RSSI/timestamps. fileciteturn0file0L1-L8 The binary protocol below mirrors those capabilities while remaining focused on timing fidelity rather than protocol decoding. fileciteturn0file0L1-L3

---

## Goals

### Primary goals

- reduce transport size compared to JSON
- reduce formatting/parsing overhead on both Flipper and PC
- preserve exact pulse/gap ordering and timing values
- support both streaming and buffered modes
- make packet boundaries explicit and recoverable
- allow optional metadata without wasting bytes

### Non-goals

- this protocol does not attempt to decode RF protocols
- this protocol does not compress aggressively at the cost of timing fidelity
- this protocol does not depend on external serialization libraries such as CBOR or MessagePack

---

## Protocol summary

The protocol uses a small custom binary packet format with:

- packet framing suitable for serial transport
- fixed header fields
- packet-type-specific payloads
- unsigned varint encoding for timing durations
- CRC16 for corruption detection

Recommended transport framing:

- **COBS framing** for binary packets
- **0x00 delimiter** between encoded packets

Recommended payload encoding:

- fixed-width integers for metadata
- unsigned varints for timing arrays

This combination provides good compactness, simple parsing, and reliable stream recovery.

---

## Transport framing

### Recommended framing: COBS

Each binary packet shall be:

1. constructed as raw packet bytes
2. COBS-encoded
3. terminated with a single `0x00` byte on the serial stream

This provides:

- safe packet boundaries over byte-stream transports
- recovery after corruption or dropped bytes
- no need to escape arbitrary payload bytes manually

### Serial packet layout on the wire

```text
[COBS-encoded packet bytes] [0x00]
```

### Receiver behavior

The PC receiver shall:

- accumulate bytes until `0x00` is seen
- COBS-decode the accumulated bytes
- validate minimum packet size
- validate CRC16
- dispatch packet by type

If COBS decode fails or CRC check fails, the receiver shall discard that packet and continue scanning for the next `0x00` delimiter.

---

## Endianness and integer encoding

Unless otherwise noted:

- all fixed-width multi-byte integers are **little-endian**
- timing durations are encoded as **unsigned LEB128-style varints**
- signed fixed-width integers use two's complement representation

### Rationale

Little-endian matches common desktop and embedded environments and is straightforward to implement in C.

Unsigned varints are used for timing durations because the durations are non-negative and often small enough to fit in 1 to 2 bytes.

---

## Raw packet structure

Before COBS encoding, each packet shall have this structure:

```text
+----------------+----------------------+----------------+
| Header         | Payload              | CRC16          |
+----------------+----------------------+----------------+
```

### Header structure

```text
Offset  Size  Field
0       1     version
1       1     type
2       1     flags
3       1     header_size
4       2     payload_len
6       2     sequence
```

Total header size for v1: **8 bytes**

### Header fields

#### `version` (`u8`)
Protocol version.

For this specification:

- `0x01` = protocol version 1

#### `type` (`u8`)
Packet type identifier.

Defined packet types:

- `0x01` = `BURST_START`
- `0x02` = `TIMING_CHUNK`
- `0x03` = `BURST_END`
- `0x04` = `BURST_CAPTURE`
- `0x05` = `STATUS` (optional)
- `0x06` = `ACK` (optional, future use)

Only the first four are required for initial implementation.

#### `flags` (`u8`)
Packet-level flags.

For v1:

- bit 0 = timestamp present in payload
- bit 1 = RSSI present in payload
- bit 2 = truncated
- bit 3 = overflow
- bit 4 = reserved
- bit 5 = reserved
- bit 6 = reserved
- bit 7 = reserved

Meaning depends slightly on packet type, but this keeps a common compact representation.

#### `header_size` (`u8`)
Header size in bytes.

For v1, set to `8`.

This makes future extension easier if extra header fields are added.

#### `payload_len` (`u16`)
Length of payload only, not including header or CRC16.

#### `sequence` (`u16`)
Packet sequence number incremented by sender for each emitted packet.

Recommended behavior:

- increment for every packet sent
- wrap naturally at 65535 to 0

This is optional for basic operation but recommended for diagnostics and loss detection.

---

## CRC

### CRC field

After the payload, append:

- `crc16` (`u16`, little-endian)

### CRC coverage

CRC16 shall be computed across:

```text
header + payload
```

It shall not include the CRC bytes themselves.

### Recommended CRC variant

Use **CRC-16/CCITT-FALSE** or **CRC-16/XMODEM**.

Pick one and keep it fixed in both Flipper and PC implementations.

Recommended for simplicity:

- **CRC-16/XMODEM**
- polynomial `0x1021`
- init `0x0000`
- no reflection
- xorout `0x0000`

### Receiver behavior on CRC failure

- discard the decoded packet
- optionally increment an error counter
- continue receiving subsequent packets

---

## Packet types

## 1. `BURST_START` (`type = 0x01`)

Indicates that a new burst has started.

This packet is emitted when the receiver identifies the beginning of a burst and wants the PC side to know the metadata before timing chunks arrive.

### Payload layout

```text
Offset  Size  Field
0       2     session_id (u16)
2       2     burst_id (u16)
4       4     frequency_hz (u32)
8       1     first_level (u8)
9       1     reserved
10      4     timestamp_us (u32)   if flags bit0 set
```

### Fields

#### `session_id` (`u16`)
Monotonic or reset-on-start capture session identifier.

#### `burst_id` (`u16`)
Burst identifier within the session.

#### `frequency_hz` (`u32`)
Configured receive frequency in Hz.

Example:

- 433920000
- 315000000
- 914990000

#### `first_level` (`u8`)
Represents the logical starting level of the first timing interval.

Values:

- `0` = first timing entry begins with low/gap
- `1` = first timing entry begins with high/pulse

This matches the current requirement that the PC side must be able to reconstruct exact pulse/gap ordering even when the first received interval is a gap. fileciteturn0file0L24-L27

#### `timestamp_us` (`u32`, optional)
Timestamp for burst start in microseconds relative to session start or device uptime reference.

Included only when `flags.bit0 = 1`.

### Notes

- `BURST_START` is most useful in live streaming mode.
- Buffered-only workflows may omit this packet if a full `BURST_CAPTURE` packet is used instead.

---

## 2. `TIMING_CHUNK` (`type = 0x02`)

Carries part of the ordered pulse/gap timing sequence for a burst.

This is the core live-stream packet type.

### Payload layout

```text
Offset  Size   Field
0       2      session_id (u16)
2       2      burst_id (u16)
4       2      count (u16)
6       N      timings[count] encoded as unsigned varints
```

### Fields

#### `session_id` (`u16`)
Matches the `BURST_START` packet.

#### `burst_id` (`u16`)
Matches the current burst.

#### `count` (`u16`)
Number of timing entries encoded in this chunk.

#### `timings[]`
Ordered timing durations in microseconds.

Each timing is encoded as an unsigned varint.

### Varint encoding

Use unsigned LEB128-style varints:

- lower 7 bits per byte are data
- high bit set means continuation follows

Encoding examples:

- `32` → 1 byte
- `66` → 1 byte
- `127` → 1 byte
- `128` → 2 bytes
- `350` → 2 bytes
- `1050` → 2 bytes

### Timing semantics

The `timings[]` array preserves exact ordering as captured:

- pulse, gap, pulse, gap...
- or gap, pulse, gap, pulse...

The interpretation of the first entry depends on `first_level` from `BURST_START` or `BURST_CAPTURE`.

### Chunk sizing recommendation

Choose chunk sizes so packets remain manageable for both USB CDC and BLE serial.

Suggested target:

- 32 to 128 timing values per chunk

For BLE, smaller chunks may reduce latency and buffering problems.

---

## 3. `BURST_END` (`type = 0x03`)

Signals that the burst is complete.

### Payload layout

```text
Offset  Size  Field
0       2     session_id (u16)
2       2     burst_id (u16)
4       2     total_count (u16)
6       2     rssi_q8_8 (i16)     if flags bit1 set
```

### Fields

#### `session_id` (`u16`)
Matches the burst.

#### `burst_id` (`u16`)
Matches the burst.

#### `total_count` (`u16`)
Total number of timing entries in the completed burst.

#### `rssi_q8_8` (`i16`, optional)
RSSI in fixed-point signed Q8.8 format.

Examples:

- `-58.0 dBm` → `-14848` if using exact Q8.8
- alternatively, sender may use signed centi-dBm (`dBm * 100`) if preferred

For implementation simplicity, centi-dBm is often easier to reason about than Q8.8. If you prefer that, rename field to `rssi_cdbm` and document it clearly.

### Flags use

For `BURST_END`:

- bit 1 = RSSI present
- bit 2 = burst truncated
- bit 3 = overflow occurred

### Truncation and overflow

These flags match the app’s current behavior of preserving truncation/overflow state in buffered modes. fileciteturn0file0L5-L8

---

## 4. `BURST_CAPTURE` (`type = 0x04`)

Carries a complete buffered burst in one packet sequence.

This is intended for buffered send mode, where the last completed burst is preserved locally and transmitted as one complete record. The current app already supports buffered capture and sending of a complete burst including truncation and overflow flags. fileciteturn0file0L5-L8

### Payload layout

```text
Offset  Size   Field
0       2      session_id (u16)
2       2      burst_id (u16)
4       4      frequency_hz (u32)
8       1      first_level (u8)
9       1      reserved
10      2      total_count (u16)
12      4      timestamp_us (u32)   if flags bit0 set
16/12   2      rssi_cdbm (i16)      if flags bit1 set
...     N      timings[total_count] as unsigned varints
```

Because timestamp and RSSI are optional, actual offsets after `total_count` depend on flags.

### Fields

Includes all metadata required for full reconstruction of the burst without any separate `BURST_START` or `BURST_END` packet.

### Use cases

- manual buffered send
- auto-send after burst complete
- saved-capture export pipeline

This matches the current app model where buffered modes intentionally preserve one completed burst for exact resend rather than continuous overwrite. fileciteturn0file0L51-L53

---

## Optional packet types

## 5. `STATUS` (`type = 0x05`)

Optional human- and machine-readable status packet for diagnostics.

Suggested payload:

```text
code:u16
value:u16
```

Example codes:

- transport connected
- transport disconnected
- capture started
- capture stopped
- buffer cleared
- packet CRC failure count

Not required for first implementation.

## 6. `ACK` (`type = 0x06`)

Optional future packet if reliability or command-response behavior is desired.

Not required for first implementation.

---

## Flag definitions

Common packet `flags` byte:

```text
bit 0  TIMESTAMP_PRESENT
bit 1  RSSI_PRESENT
bit 2  TRUNCATED
bit 3  OVERFLOW
bit 4  RESERVED
bit 5  RESERVED
bit 6  RESERVED
bit 7  RESERVED
```

### Meaning by context

- `TIMESTAMP_PRESENT` means a timestamp field is included in the payload layout for that packet type
- `RSSI_PRESENT` means an RSSI field is included in the payload layout for that packet type
- `TRUNCATED` means not all timings could be stored or sent
- `OVERFLOW` means capture/queue/buffer overflow occurred during burst handling

---

## Varint specification

Timing durations shall be encoded as unsigned varints using this algorithm.

### Encoding

For each unsigned integer value:

1. take the low 7 bits
2. write them to a byte
3. set bit 7 if more bits remain
4. continue until value is fully encoded

### Decoding

1. read bytes sequentially
2. append lower 7 bits at increasing 7-bit shifts
3. stop when a byte is read with high bit clear

### Pseudocode: encode

```c
size_t write_uvarint(uint8_t* out, uint32_t value) {
    size_t i = 0;
    while(value >= 0x80) {
        out[i++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out[i++] = (uint8_t)(value & 0x7F);
    return i;
}
```

### Pseudocode: decode

```c
bool read_uvarint(const uint8_t* in, size_t in_len, size_t* pos, uint32_t* value_out) {
    uint32_t value = 0;
    uint8_t shift = 0;

    while(*pos < in_len && shift < 35) {
        uint8_t b = in[(*pos)++];
        value |= ((uint32_t)(b & 0x7F)) << shift;
        if((b & 0x80) == 0) {
            *value_out = value;
            return true;
        }
        shift += 7;
    }

    return false;
}
```

### Recommended timing width

Use `uint32_t` during decode even if current timings usually fit well below that.

This avoids needless future constraints.

---

## Session and burst identifiers

### `session_id`

Recommended behavior:

- increment when capture is started
- reset to `1` at app launch if desired

### `burst_id`

Recommended behavior:

- start at `1` for each session
- increment for each completed or started burst

These IDs allow the PC side to correlate start, chunk, and end packets.

---

## Suggested sender behavior

## Live streaming mode

For live streaming, send:

1. `BURST_START`
2. one or more `TIMING_CHUNK`
3. `BURST_END`

### Example sequence

```text
BURST_START(session=1, burst=14, freq=433920000, first_level=1)
TIMING_CHUNK(session=1, burst=14, count=64, timings=[...])
TIMING_CHUNK(session=1, burst=14, count=59, timings=[...])
BURST_END(session=1, burst=14, total_count=123, rssi=-58.0, truncated=0)
```

## Buffered mode

For buffered send mode, send either:

- one `BURST_CAPTURE`

or, if packet size becomes too large:

- `BURST_START`
- one or more `TIMING_CHUNK`
- `BURST_END`

### Recommendation

Prefer `BURST_CAPTURE` for reasonably sized bursts because it maps directly to the user’s manual send flow and preserved exact-burst model. fileciteturn0file0L14-L20

If a burst becomes too large for practical transport buffering, fall back to the live-style segmented sequence.

---

## Suggested receiver behavior

The PC receiver should:

1. read serial bytes continuously
2. split frames by `0x00`
3. COBS-decode each frame
4. validate header size and payload length
5. validate CRC16
6. parse by packet type
7. reconstruct bursts using `session_id` + `burst_id`

### Live reconstruction

For a live burst:

- `BURST_START` creates a new active burst object
- each `TIMING_CHUNK` appends timings to that burst
- `BURST_END` finalizes the burst and marks metadata such as RSSI/truncation

### Buffered reconstruction

For `BURST_CAPTURE`:

- create a completed burst directly from one packet

### Error handling

If a chunk arrives for an unknown active burst:

- either discard it
- or create a temporary incomplete burst and mark it as missing start metadata

Simplest v1 behavior: discard orphan chunks and increment a diagnostic counter.

---

## Size considerations

This protocol is expected to be significantly smaller than JSON.

### Example JSON timing chunk

```json
{"type":"timing_chunk","session":1,"burst":1,"timings":[350,1050,350,350,1050,350]}
```

This includes repeated field names and ASCII numeric overhead.

### Binary equivalent

Equivalent binary payload would carry roughly:

- packet type in header
- session `u16`
- burst `u16`
- count `u16`
- six timings as varints

Typical timings like 350 and 1050 usually encode in 2 bytes each, making the packet far smaller than its JSON equivalent.

---

## Recommended limits

For v1 implementation, define conservative limits in both sender and receiver.

### Suggested constants

```c
#define FLIPRSDR_PROTO_VERSION              1
#define FLIPRSDR_HEADER_SIZE                8
#define FLIPRSDR_MAX_TIMINGS_PER_CHUNK      128
#define FLIPRSDR_MAX_BURST_TIMINGS          4096
#define FLIPRSDR_MAX_DECODED_PACKET_SIZE    4096
```

Adjust based on current app buffer limits and BLE/USB transport constraints.

---

## Versioning and compatibility

### Protocol version field

The `version` field allows future changes while keeping backward compatibility manageable.

### v1 rules

A v1 receiver shall:

- accept only packets with `version == 1`
- discard unsupported packet types gracefully
- reject malformed packets safely

### Future extension ideas

Possible future versions may add:

- stronger burst metadata
- symbol-table compression for repeated durations
- transport commands from PC to Flipper
- negotiated capabilities
- optional acknowledgments or retransmission

---

## Recommended implementation plan

## Phase 1: parallel support

Add binary support while keeping JSON available as a debug mode.

Suggested setting:

- Protocol format: `JSON` or `Binary`

This allows side-by-side testing during migration.

## Phase 2: implement core encoder/decoder

On Flipper:

- add packet builder
- add varint encoder
- add CRC16 helper
- add COBS encoder
- replace JSON formatter path in protocol/transport layer with binary path when selected

On PC receiver:

- add byte stream framing for `0x00`
- add COBS decode
- add CRC16 validation
- add packet parser and burst reconstruction

## Phase 3: buffered and live test coverage

Test cases should include:

- short burst
- long burst over multiple timing chunks
- gap-first burst using `first_level = 0`
- burst with truncation flag
- burst with overflow flag
- packet CRC corruption
- partial frame loss and re-sync
- USB CDC and BLE serial transport validation

The app currently supports both USB CDC and BLE serial, so both transports should be validated with the same packet model. fileciteturn0file0L1-L8

---

## Example packet definitions in C

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t header_size;
    uint16_t payload_len;
    uint16_t sequence;
} FlipRSDRPacketHeader;
#pragma pack(pop)
```

Suggested type constants:

```c
#define FLIPRSDR_PKT_BURST_START    0x01
#define FLIPRSDR_PKT_TIMING_CHUNK   0x02
#define FLIPRSDR_PKT_BURST_END      0x03
#define FLIPRSDR_PKT_BURST_CAPTURE  0x04
#define FLIPRSDR_PKT_STATUS         0x05
```

Suggested flag constants:

```c
#define FLIPRSDR_FLAG_TIMESTAMP     (1u << 0)
#define FLIPRSDR_FLAG_RSSI          (1u << 1)
#define FLIPRSDR_FLAG_TRUNCATED     (1u << 2)
#define FLIPRSDR_FLAG_OVERFLOW      (1u << 3)
```

---

## Example flow: live burst

Example logical burst:

- session = 3
- burst = 18
- frequency = 433920000
- first_level = 1
- timings = [350, 1050, 350, 350, 1050, 350]
- total_count = 6
- rssi = -58.0 dBm

Example send sequence:

1. send `BURST_START`
2. send `TIMING_CHUNK` with 6 timings
3. send `BURST_END`

The receiver reconstructs the exact pulse/gap sequence using `first_level` and the ordered timing list.

---

## Design decisions recap

This protocol chooses:

- **custom binary** instead of JSON, CBOR, or MessagePack
- **COBS framing** for reliable stream packet boundaries
- **CRC16** for corruption detection
- **fixed-width metadata** for simplicity
- **varints for timings** for compactness
- **shared logical packet types** that mirror the current app design

This makes it well suited for FlipRSDR’s core requirement of preserving raw burst timing fidelity while improving throughput and compactness over serial. fileciteturn0file0L1-L3

---

## Final recommendation

For the first implementation in your app:

- implement protocol version 1 exactly as specified here
- keep JSON as a temporary debug option
- use `BURST_START`, `TIMING_CHUNK`, `BURST_END`, and `BURST_CAPTURE`
- use COBS framing with `0x00` delimiter
- use unsigned varints for microsecond timings
- use CRC-16/XMODEM for packet validation

This will give you a compact, robust, and easy-to-maintain protocol that is a strong fit for both USB CDC and BLE serial transport in FlipRSDR.

