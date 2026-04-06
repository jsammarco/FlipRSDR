from __future__ import annotations

import struct
from typing import Any, Iterable, Mapping


PROTOCOL_FORMAT_FLIPRSDR = "fliprsdr"
PROTOCOL_FORMAT_JSON = "json"

PROTOCOL_VERSION = 0x01
HEADER_SIZE = 8
REPLAY_COMMAND_MAX_LINE = 340

PACKET_BURST_START = 0x01
PACKET_TIMING_CHUNK = 0x02
PACKET_BURST_END = 0x03
PACKET_BURST_CAPTURE = 0x04

FLAG_TIMESTAMP = 1 << 0
FLAG_RSSI = 1 << 1
FLAG_TRUNCATED = 1 << 2
FLAG_OVERFLOW = 1 << 3


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
            continue
        out.append(byte)
        code += 1
        if code == 0xFF:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    index = 0
    length = len(data)
    while index < length:
        code = data[index]
        if code == 0:
            raise ValueError("COBS decode failed: zero byte inside frame")
        index += 1
        end = index + code - 1
        if end > length:
            raise ValueError("COBS decode failed: truncated code block")
        out.extend(data[index:end])
        index = end
        if code != 0xFF and index < length:
            out.append(0)
    return bytes(out)


def encode_uvarint(value: int) -> bytes:
    if value < 0:
        raise ValueError("Varint value must be non-negative")
    out = bytearray()
    current = int(value)
    while current >= 0x80:
        out.append((current & 0x7F) | 0x80)
        current >>= 7
    out.append(current & 0x7F)
    return bytes(out)


def decode_uvarint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    current = offset
    while current < len(data) and shift < 35:
        byte = data[current]
        current += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, current
        shift += 7
    raise ValueError("Invalid varint in packet payload")


def _build_header(packet_type: int, flags: int, payload: bytes, sequence: int) -> bytes:
    return struct.pack(
        "<BBBBHH",
        PROTOCOL_VERSION,
        packet_type & 0xFF,
        flags & 0xFF,
        HEADER_SIZE,
        len(payload) & 0xFFFF,
        sequence & 0xFFFF,
    )


def _encode_packet(packet_type: int, flags: int, payload: bytes, sequence: int = 0) -> bytes:
    header = _build_header(packet_type, flags, payload, sequence)
    packet = header + payload
    crc = crc16_xmodem(packet)
    return cobs_encode(packet + struct.pack("<H", crc)) + b"\x00"


def _packet_flags(message: Mapping[str, Any], *, include_timestamp: bool, include_rssi: bool) -> int:
    flags = 0
    if include_timestamp and message.get("timestamp") is not None:
        flags |= FLAG_TIMESTAMP
    if include_rssi and message.get("rssi") is not None:
        flags |= FLAG_RSSI
    if bool(message.get("truncated", False)):
        flags |= FLAG_TRUNCATED
    if bool(message.get("overflow", False)):
        flags |= FLAG_OVERFLOW
    return flags


def _encode_burst_start(message: Mapping[str, Any], sequence: int) -> bytes:
    flags = _packet_flags(message, include_timestamp=True, include_rssi=False)
    payload = bytearray()
    payload.extend(struct.pack("<HHI", int(message.get("session", 0)), int(message.get("burst", 0)), int(message.get("freq", 0))))
    payload.extend(bytes([1 if bool(message.get("first_level", True)) else 0, 0]))
    if flags & FLAG_TIMESTAMP:
        payload.extend(struct.pack("<I", int(message.get("timestamp", 0)) * 1000))
    return _encode_packet(PACKET_BURST_START, flags, bytes(payload), sequence)


def _encode_timing_chunk(
    session: int,
    burst: int,
    timings: Iterable[int],
    sequence: int,
) -> bytes:
    timing_list = [int(value) for value in timings]
    payload = bytearray(struct.pack("<HHH", session, burst, len(timing_list)))
    for timing in timing_list:
        payload.extend(encode_uvarint(timing))
    return _encode_packet(PACKET_TIMING_CHUNK, 0, bytes(payload), sequence)


def _encode_burst_end(message: Mapping[str, Any], sequence: int) -> bytes:
    flags = _packet_flags(message, include_timestamp=False, include_rssi=True)
    payload = bytearray()
    payload.extend(
        struct.pack(
            "<HHH",
            int(message.get("session", 0)),
            int(message.get("burst", 0)),
            int(message.get("count", len(message.get("timings", [])))),
        )
    )
    if flags & FLAG_RSSI:
        payload.extend(struct.pack("<h", int(round(float(message.get("rssi", 0.0)) * 100.0))))
    return _encode_packet(PACKET_BURST_END, flags, bytes(payload), sequence)


def _encode_burst_capture(message: Mapping[str, Any], sequence: int) -> bytes:
    flags = _packet_flags(message, include_timestamp=True, include_rssi=True)
    timings = [int(value) for value in message.get("timings", [])]
    payload = bytearray()
    payload.extend(
        struct.pack(
            "<HHI",
            int(message.get("session", 0)),
            int(message.get("burst", 0)),
            int(message.get("freq", 0)),
        )
    )
    payload.extend(bytes([1 if bool(message.get("first_level", True)) else 0, 0]))
    payload.extend(struct.pack("<H", int(message.get("count", len(timings)))))
    if flags & FLAG_TIMESTAMP:
        payload.extend(struct.pack("<I", int(message.get("timestamp", 0)) * 1000))
    if flags & FLAG_RSSI:
        payload.extend(struct.pack("<h", int(round(float(message.get("rssi", 0.0)) * 100.0))))
    for timing in timings:
        payload.extend(encode_uvarint(timing))
    return _encode_packet(PACKET_BURST_CAPTURE, flags, bytes(payload), sequence)


def encode_recording_burst(message: Mapping[str, Any], timings_per_chunk: int = 128) -> bytes:
    if message.get("type") != "burst_capture":
        raise ValueError("Recording encoder expects a burst_capture message")

    timings = [int(value) for value in message.get("timings", [])]
    total_count = int(message.get("count", len(timings)))
    truncated = bool(message.get("truncated", False))
    overflow = bool(message.get("overflow", False))
    full_capture = not truncated and len(timings) == total_count

    sequence = 0
    if full_capture and not overflow:
        return _encode_burst_capture(message, sequence)

    chunks: list[bytes] = [_encode_burst_start(message, sequence)]
    sequence += 1
    for index in range(0, len(timings), max(1, timings_per_chunk)):
        chunks.append(
            _encode_timing_chunk(
                int(message.get("session", 0)),
                int(message.get("burst", 0)),
                timings[index : index + max(1, timings_per_chunk)],
                sequence,
            )
        )
        sequence += 1
    chunks.append(_encode_burst_end(message, sequence))
    return b"".join(chunks)


def build_replay_commands(
    frequency_hz: int,
    first_level: bool,
    timings: Iterable[int],
    *,
    max_line_length: int = REPLAY_COMMAND_MAX_LINE,
) -> list[str]:
    timing_list = [int(value) for value in timings]
    if not timing_list:
        raise ValueError("Replay requires at least one timing value")
    if frequency_hz <= 0:
        raise ValueError("Replay requires a valid frequency")
    if max_line_length < 32:
        raise ValueError("Replay command line budget is too small")

    commands = [f"replay_begin {int(frequency_hz)} {1 if first_level else 0} {len(timing_list)}"]
    offset = 0
    chunk: list[str] = []
    chunk_len = 0

    for timing in timing_list:
        if timing <= 0:
            raise ValueError("Replay timings must be positive integers")
        token = str(timing)
        if not chunk:
            projected = len(f"replay_chunk {offset} {token}")
        else:
            projected = len(f"replay_chunk {offset} ") + chunk_len + 1 + len(token)

        if chunk and projected > max_line_length:
            commands.append(f"replay_chunk {offset} {','.join(chunk)}")
            offset += len(chunk)
            chunk = [token]
            chunk_len = len(token)
            continue

        chunk.append(token)
        chunk_len = len(",".join(chunk))

        if len(f"replay_chunk {offset} {chunk[0]}") > max_line_length:
            raise ValueError("Replay timing value exceeds the command line budget")

    if chunk:
        commands.append(f"replay_chunk {offset} {','.join(chunk)}")
    commands.append("replay_commit")
    return commands


def parse_packet(decoded_packet: bytes) -> dict[str, Any]:
    if len(decoded_packet) < HEADER_SIZE + 2:
        raise ValueError("Packet too short")

    version, packet_type, flags, header_size, payload_len, _sequence = struct.unpack(
        "<BBBBHH", decoded_packet[:HEADER_SIZE]
    )
    if version != PROTOCOL_VERSION:
        raise ValueError(f"Unsupported protocol version: {version}")
    if header_size != HEADER_SIZE:
        raise ValueError(f"Unsupported header size: {header_size}")
    if len(decoded_packet) != HEADER_SIZE + payload_len + 2:
        raise ValueError("Packet length does not match header")

    packet_without_crc = decoded_packet[:-2]
    packet_crc = struct.unpack("<H", decoded_packet[-2:])[0]
    expected_crc = crc16_xmodem(packet_without_crc)
    if packet_crc != expected_crc:
        raise ValueError("CRC check failed")

    payload = decoded_packet[HEADER_SIZE:-2]
    if packet_type == PACKET_BURST_START:
        if len(payload) not in {10, 14}:
            raise ValueError("Invalid BURST_START payload length")
        session, burst, freq = struct.unpack("<HHI", payload[:8])
        first_level = bool(payload[8])
        message: dict[str, Any] = {
            "type": "burst_start",
            "session": session,
            "burst": burst,
            "freq": freq,
            "first_level": first_level,
        }
        if flags & FLAG_TIMESTAMP:
            if len(payload) < 14:
                raise ValueError("Missing BURST_START timestamp")
            message["timestamp"] = struct.unpack("<I", payload[10:14])[0] // 1000
        return message

    if packet_type == PACKET_TIMING_CHUNK:
        if len(payload) < 6:
            raise ValueError("Invalid TIMING_CHUNK payload length")
        session, burst, count = struct.unpack("<HHH", payload[:6])
        timings: list[int] = []
        offset = 6
        for _ in range(count):
            timing, offset = decode_uvarint(payload, offset)
            timings.append(timing)
        if offset != len(payload):
            raise ValueError("Unexpected trailing bytes in TIMING_CHUNK")
        return {
            "type": "timing_chunk",
            "session": session,
            "burst": burst,
            "timings": timings,
        }

    if packet_type == PACKET_BURST_END:
        if len(payload) not in {6, 8}:
            raise ValueError("Invalid BURST_END payload length")
        session, burst, total_count = struct.unpack("<HHH", payload[:6])
        message = {
            "type": "burst_end",
            "session": session,
            "burst": burst,
            "count": total_count,
            "truncated": bool(flags & FLAG_TRUNCATED),
            "overflow": bool(flags & FLAG_OVERFLOW),
        }
        if flags & FLAG_RSSI:
            if len(payload) < 8:
                raise ValueError("Missing BURST_END RSSI")
            message["rssi"] = struct.unpack("<h", payload[6:8])[0] / 100.0
        return message

    if packet_type == PACKET_BURST_CAPTURE:
        if len(payload) < 12:
            raise ValueError("Invalid BURST_CAPTURE payload length")
        session, burst, freq = struct.unpack("<HHI", payload[:8])
        first_level = bool(payload[8])
        total_count = struct.unpack("<H", payload[10:12])[0]
        offset = 12
        message = {
            "type": "burst_capture",
            "session": session,
            "burst": burst,
            "freq": freq,
            "first_level": first_level,
            "count": total_count,
            "truncated": bool(flags & FLAG_TRUNCATED),
            "overflow": bool(flags & FLAG_OVERFLOW),
        }
        if flags & FLAG_TIMESTAMP:
            if offset + 4 > len(payload):
                raise ValueError("Missing BURST_CAPTURE timestamp")
            message["timestamp"] = struct.unpack("<I", payload[offset : offset + 4])[0] // 1000
            offset += 4
        if flags & FLAG_RSSI:
            if offset + 2 > len(payload):
                raise ValueError("Missing BURST_CAPTURE RSSI")
            message["rssi"] = struct.unpack("<h", payload[offset : offset + 2])[0] / 100.0
            offset += 2
        timings: list[int] = []
        while offset < len(payload):
            timing, offset = decode_uvarint(payload, offset)
            timings.append(timing)
        message["timings"] = timings
        return message

    raise ValueError(f"Unsupported packet type: {packet_type}")


class BinaryStreamDecoder:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> tuple[list[dict[str, Any]], list[str]]:
        self._buffer.extend(data)
        messages: list[dict[str, Any]] = []
        warnings: list[str] = []

        while True:
            try:
                delimiter = self._buffer.index(0)
            except ValueError:
                break

            frame = bytes(self._buffer[:delimiter])
            del self._buffer[: delimiter + 1]
            if not frame:
                continue

            try:
                decoded = cobs_decode(frame)
                messages.append(parse_packet(decoded))
            except ValueError as exc:
                warnings.append(str(exc))

        return messages, warnings


def reconstruct_burst_captures(messages: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    active: dict[tuple[int, int], dict[str, Any]] = {}
    completed: list[dict[str, Any]] = []

    for message in messages:
        message_type = message.get("type")
        if message_type == "burst_start":
            key = (int(message.get("session", 0)), int(message.get("burst", 0)))
            active[key] = {
                "type": "burst_capture",
                "session": key[0],
                "burst": key[1],
                "freq": int(message.get("freq", 0)),
                "timestamp": message.get("timestamp"),
                "first_level": 1 if bool(message.get("first_level", True)) else 0,
                "timings": [],
                "count": 0,
                "truncated": False,
            }
        elif message_type == "timing_chunk":
            key = (int(message.get("session", 0)), int(message.get("burst", 0)))
            current = active.setdefault(
                key,
                {
                    "type": "burst_capture",
                    "session": key[0],
                    "burst": key[1],
                    "freq": 0,
                    "timestamp": None,
                    "first_level": 1,
                    "timings": [],
                    "count": 0,
                    "truncated": False,
                },
            )
            current["timings"].extend(int(value) for value in message.get("timings", []))
            current["count"] = len(current["timings"])
        elif message_type == "burst_end":
            key = (int(message.get("session", 0)), int(message.get("burst", 0)))
            current = active.setdefault(
                key,
                {
                    "type": "burst_capture",
                    "session": key[0],
                    "burst": key[1],
                    "freq": 0,
                    "timestamp": None,
                    "first_level": 1,
                    "timings": [],
                    "count": 0,
                    "truncated": False,
                },
            )
            current["count"] = int(message.get("count", len(current["timings"])))
            current["truncated"] = bool(message.get("truncated", False))
            if "rssi" in message:
                current["rssi"] = float(message["rssi"])
            if "overflow" in message:
                current["overflow"] = bool(message["overflow"])
            completed.append(current)
            active.pop(key, None)
        elif message_type == "burst_capture":
            completed.append(
                {
                    "type": "burst_capture",
                    "session": int(message.get("session", 0)),
                    "burst": int(message.get("burst", 0)),
                    "freq": int(message.get("freq", 0)),
                    "timestamp": message.get("timestamp"),
                    "first_level": 1 if bool(message.get("first_level", True)) else 0,
                    "timings": [int(value) for value in message.get("timings", [])],
                    "count": int(message.get("count", len(message.get("timings", [])))),
                    "truncated": bool(message.get("truncated", False)),
                    **({"rssi": float(message["rssi"])} if "rssi" in message else {}),
                    **({"overflow": bool(message["overflow"])} if "overflow" in message else {}),
                }
            )

    return completed


def decode_binary_recording(data: bytes) -> tuple[list[dict[str, Any]], list[str]]:
    decoder = BinaryStreamDecoder()
    messages, warnings = decoder.feed(data)
    return reconstruct_burst_captures(messages), warnings


def format_message_log(message: Mapping[str, Any]) -> str:
    message_type = str(message.get("type", "?"))
    session = int(message.get("session", 0))
    burst = int(message.get("burst", 0))
    if message_type == "timing_chunk":
        return (
            f"[fliprsdr] {message_type} s={session} b={burst} "
            f"timings={len(message.get('timings', []))}"
        )
    if message_type == "burst_end":
        return (
            f"[fliprsdr] {message_type} s={session} b={burst} "
            f"count={int(message.get('count', 0))}"
        )
    if message_type == "burst_capture":
        return (
            f"[fliprsdr] {message_type} s={session} b={burst} "
            f"count={int(message.get('count', len(message.get('timings', []))))}"
        )
    return f"[fliprsdr] {message_type} s={session} b={burst}"
