from __future__ import annotations

import csv
import io
import json
import math
import sys
import tempfile
import threading
import time
import wave
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import numpy as np
import pyqtgraph as pg
import serial
import serial.tools.list_ports
from PySide6.QtCore import QSettings, QTimer, Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from fliprsdr_protocol import build_replay_commands, decode_binary_recording, encode_recording_burst

try:
    import winsound
except ImportError:  # pragma: no cover
    winsound = None


APP_NAME = "FlipRSDR Analyzer"
WATERFALL_BINS = 512
WATERFALL_MIN_US = 16.0
WATERFALL_MAX_US = 65536.0
WATERFALL_X_TICKS_US = [16, 32, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000, 32000, 65536]
DEFAULT_PLAYBACK_SPEED = 1.0
PLAYBACK_SPEED_OPTIONS = [0.25, 0.5, 1.0, 2.0, 4.0]
MAX_WAVEFORM_POINTS = 4000
MAX_AUDIO_BURST_US = 2_000_000
MAX_AUDIO_TIMINGS = 4096
DEFAULT_REMOTE_BAUD_RATE = 9600
REMOTE_BAUD_OPTIONS = ["9600", "115200", "230400"]


class WaterfallPlotWidget(pg.PlotWidget):
    def __init__(self) -> None:
        super().__init__()
        self.selection_callback = None
        self._dragging_selection = False
        self._drag_start_row = 0.0

    def mousePressEvent(self, event) -> None:  # type: ignore[override]
        if event.button() == Qt.LeftButton:
            self._dragging_selection = True
            self._drag_start_row = self._event_row(event)
            if self.selection_callback is not None:
                self.selection_callback(self._drag_start_row, self._drag_start_row, False)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:  # type: ignore[override]
        if self._dragging_selection:
            if self.selection_callback is not None:
                self.selection_callback(self._drag_start_row, self._event_row(event), False)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:  # type: ignore[override]
        if self._dragging_selection and event.button() == Qt.LeftButton:
            self._dragging_selection = False
            if self.selection_callback is not None:
                self.selection_callback(self._drag_start_row, self._event_row(event), True)
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def _event_row(self, event) -> float:
        scene_pos = self.mapToScene(event.position().toPoint())
        return float(self.plotItem.vb.mapSceneToView(scene_pos).y())


@dataclass
class BurstData:
    session: int
    burst: int
    frequency_hz: int = 0
    timestamp: int | None = None
    first_level: bool = True
    timings: list[int] = field(default_factory=list)
    count: int = 0
    rssi: float | None = None
    truncated: bool = False

    @property
    def timing_count(self) -> int:
        return self.count if self.count else len(self.timings)

    @property
    def total_duration_us(self) -> int:
        return int(sum(self.timings))


@dataclass
class RecordingAnalysis:
    total_bursts: int
    sessions: int
    frequencies_hz: list[int]
    time_span_ms: int
    avg_duration_us: float
    avg_timings_per_burst: float
    repeated_signatures: list[tuple[str, int]]
    likely_frame_bursts: list[int]
    notes: list[str]


@dataclass
class DecodedFrame:
    label: str
    bit_string: str
    confidence: float
    sync_us: int | None
    short_us: int
    long_us: int
    hex_preview: str

    @property
    def family_key(self) -> tuple[int, int]:
        return (int(round(self.short_us / 50.0) * 50), int(round(self.long_us / 50.0) * 50))

    def format_text(self) -> str:
        sync_text = f"Sync {self.sync_us} us, " if self.sync_us is not None else ""
        return (
            f"{self.label}. {sync_text}"
            f"symbols ~{self.short_us}/{self.long_us} us, {len(self.bit_string)} bits. "
            f"Bits: {self.bit_string[:96]} Hex: {self.hex_preview} (confidence {self.confidence:.0%})"
        )


def load_recording(path: Path) -> list[BurstData]:
    suffix = path.suffix.lower()
    if suffix == ".fliprsdr":
        return _load_binary_recording(path)
    if suffix == ".jsonl":
        return _load_json_recording(path)

    try:
        return _load_json_recording(path)
    except (UnicodeDecodeError, ValueError):
        return _load_binary_recording(path)


def _optional_int(value: object) -> int | None:
    return int(value) if value is not None else None


def _optional_float(value: object) -> float | None:
    return float(value) if value is not None else None


def _load_json_recording(path: Path) -> list[BurstData]:
    bursts: list[BurstData] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            text = raw_line.strip()
            if not text:
                continue
            try:
                message = json.loads(text)
            except json.JSONDecodeError as exc:
                raise ValueError(f"Invalid JSON on line {line_number}: {exc}") from exc

            if message.get("type") != "burst_capture":
                continue

            bursts.append(
                BurstData(
                    session=int(message.get("session", 0)),
                    burst=int(message.get("burst", 0)),
                    frequency_hz=int(message.get("freq", 0)),
                    timestamp=_optional_int(message.get("timestamp")),
                    first_level=bool(int(message.get("first_level", 1))),
                    timings=[int(value) for value in message.get("timings", [])],
                    count=int(message.get("count", len(message.get("timings", [])))),
                    rssi=_optional_float(message.get("rssi")),
                    truncated=bool(message.get("truncated", False)),
                )
            )

    bursts.sort(
        key=lambda burst: (
            burst.timestamp if burst.timestamp is not None else sys.maxsize,
            burst.session,
            burst.burst,
        )
    )
    return bursts


def _load_binary_recording(path: Path) -> list[BurstData]:
    bursts: list[BurstData] = []
    capture_messages, warnings = decode_binary_recording(path.read_bytes())
    if warnings and not capture_messages:
        raise ValueError("; ".join(warnings[:4]))

    for message in capture_messages:
        bursts.append(
            BurstData(
                session=int(message.get("session", 0)),
                burst=int(message.get("burst", 0)),
                frequency_hz=int(message.get("freq", 0)),
                timestamp=_optional_int(message.get("timestamp")),
                first_level=bool(int(message.get("first_level", 1))),
                timings=[int(value) for value in message.get("timings", [])],
                count=int(message.get("count", len(message.get("timings", [])))),
                rssi=_optional_float(message.get("rssi")),
                truncated=bool(message.get("truncated", False)),
            )
        )

    bursts.sort(
        key=lambda burst: (
            burst.timestamp if burst.timestamp is not None else sys.maxsize,
            burst.session,
            burst.burst,
        )
    )
    return bursts


def burst_histogram_row(burst: BurstData) -> np.ndarray:
    if not burst.timings:
        return np.zeros(WATERFALL_BINS, dtype=np.float32)

    durations = np.array(burst.timings, dtype=np.float32)
    durations = np.clip(durations, WATERFALL_MIN_US, WATERFALL_MAX_US)
    histogram, _ = np.histogram(
        np.log2(durations),
        bins=WATERFALL_BINS,
        range=(math.log2(WATERFALL_MIN_US), math.log2(WATERFALL_MAX_US)),
    )
    row = histogram.astype(np.float32)
    if row.max() > 0:
        row /= row.max()
    return np.sqrt(row)


def burst_signature(burst: BurstData) -> str:
    if not burst.timings:
        return "empty"
    quantized = [int(round(math.log2(max(16, value)) * 8.0)) for value in burst.timings[:48]]
    return ",".join(str(value) for value in quantized)


def cluster_durations_us(timings: list[int], tolerance: float = 0.28) -> list[tuple[int, int]]:
    clusters: list[list[int]] = []
    for value in sorted(v for v in timings if v > 0):
        placed = False
        for cluster in clusters:
            center = sum(cluster) / len(cluster)
            if abs(value - center) / max(center, 1.0) <= tolerance:
                cluster.append(value)
                placed = True
                break
        if not placed:
            clusters.append([value])

    return [(int(round(sum(cluster) / len(cluster))), len(cluster)) for cluster in clusters]


def _classify_duration(value: int, short_us: int, long_us: int, tolerance: float = 0.38) -> str:
    short_error = abs(value - short_us) / max(short_us, 1)
    long_error = abs(value - long_us) / max(long_us, 1)
    best_error = min(short_error, long_error)
    if best_error > tolerance:
        return "?"
    return "S" if short_error <= long_error else "L"


def _valid_symbol_pair(first_kind: str, second_kind: str) -> str | None:
    if first_kind == "S" and second_kind == "L":
        return "0"
    if first_kind == "L" and second_kind == "S":
        return "1"
    return None


def _parse_symbol_pairs(values: list[int], short_us: int, long_us: int) -> tuple[str, float, int]:
    bits: list[str] = []
    decoded_pairs = 0
    skipped_values = 0
    index = 0
    glitch_threshold = max(80, int(short_us * 0.45))

    while index + 1 < len(values):
        first_kind = _classify_duration(values[index], short_us, long_us)
        second_kind = _classify_duration(values[index + 1], short_us, long_us)
        bit = _valid_symbol_pair(first_kind, second_kind)
        if bit is not None:
            bits.append(bit)
            decoded_pairs += 1
            index += 2
            continue

        next_bit = None
        if index + 2 < len(values):
            next_first = _classify_duration(values[index + 1], short_us, long_us)
            next_second = _classify_duration(values[index + 2], short_us, long_us)
            next_bit = _valid_symbol_pair(next_first, next_second)

        if next_bit is not None and (
            values[index] <= glitch_threshold or first_kind == "?"
        ):
            skipped_values += 1
            index += 1
            continue

        if first_kind in {"S", "L"} and second_kind in {"S", "L"}:
            bits.append("?")
            index += 2
            continue

        skipped_values += 1
        index += 1

    confidence = decoded_pairs / max(len(bits), 1)
    return "".join(bits), confidence, skipped_values


def _bit_string_to_hex(bit_string: str) -> str:
    padded = "".join(bit if bit in {"0", "1"} else "0" for bit in bit_string)
    hex_groups = [
        f"{int(padded[index : index + 4], 2):X}"
        for index in range(0, len(padded) - (len(padded) % 4), 4)
    ]
    return "".join(hex_groups[:16]) if hex_groups else "n/a"


def _build_decoded_frame(
    *,
    label: str,
    bit_string: str,
    confidence: float,
    sync_us: int | None,
    short_us: int,
    long_us: int,
) -> DecodedFrame:
    return DecodedFrame(
        label=label,
        bit_string=bit_string,
        confidence=confidence,
        sync_us=sync_us,
        short_us=short_us,
        long_us=long_us,
        hex_preview=_bit_string_to_hex(bit_string),
    )


def decode_burst_details(burst: BurstData) -> DecodedFrame | None:
    timings = [value for value in burst.timings if value > 0]
    if len(timings) < 8:
        return None
    return _decode_sync_pair_frame(timings)


def _decode_sync_pair_frame(timings: list[int]) -> DecodedFrame | None:
    positive = [value for value in timings if value > 0]
    if len(positive) < 20:
        return None

    candidate_clusters = [
        (center, count)
        for center, count in cluster_durations_us(positive, tolerance=0.22)
        if 80 <= center <= 2500
    ]
    if len(candidate_clusters) < 2:
        return None

    symbol_clusters = sorted(candidate_clusters, key=lambda item: item[1], reverse=True)[:2]
    short_us, long_us = sorted(center for center, _count in symbol_clusters)
    ratio = long_us / max(short_us, 1)
    if ratio < 1.45 or ratio > 3.5:
        return None

    sync_threshold = max(int(long_us * 4.0), 3000)
    sync_candidates = [
        index for index, value in enumerate(timings) if sync_threshold <= value < sync_threshold * 4
    ]

    candidate_streams: list[tuple[str, int | None, list[int]]] = []
    for sync_index in sync_candidates[:4]:
        payload = [value for value in timings[sync_index + 1 :] if 0 < value < sync_threshold]
        if len(payload) >= 24:
            candidate_streams.append(("Sync/pulse-width frame detected", timings[sync_index], payload))

    leading_window = [value for value in timings if 0 < value < sync_threshold]
    if len(leading_window) >= 24:
        candidate_streams.append(("Pulse-width frame detected", None, leading_window))

    best_result: tuple[str, float, int, int | None, str] | None = None
    for label, sync_us, payload in candidate_streams:
        for start_offset in range(min(6, len(payload) // 2)):
            bit_string, confidence, skipped_values = _parse_symbol_pairs(payload[start_offset:], short_us, long_us)
            decoded_bits = sum(1 for bit in bit_string if bit in {"0", "1"})
            if decoded_bits < 12:
                continue
            score = decoded_bits * 100 - bit_string.count("?") * 30 - skipped_values * 12 - start_offset * 4
            if best_result is None or score > best_result[2]:
                best_result = (bit_string, confidence, score, sync_us, label)

    if best_result is None:
        return None

    bit_string, confidence, _score, sync_us, label = best_result
    if confidence < 0.75:
        return None

    return _build_decoded_frame(
        label=label,
        bit_string=bit_string,
        confidence=confidence,
        sync_us=sync_us,
        short_us=short_us,
        long_us=long_us,
    )


def try_decode_burst(burst: BurstData) -> str:
    if len(burst.timings) < 8:
        return "Too short to decode confidently."

    timings = [value for value in burst.timings if value > 0]
    if len(timings) < 8:
        return "Not enough non-zero timings."

    sync_pair_decode = _decode_sync_pair_frame(timings)
    if sync_pair_decode is not None:
        return sync_pair_decode.format_text()

    clusters = cluster_durations_us(timings)
    if len(clusters) < 2:
        return "Single timing cluster only; likely noise or a carrier gap."

    durations = [center for center, _count in clusters]
    shortest = min(durations)
    longest = max(durations)
    ratio = longest / max(shortest, 1)
    if ratio < 1.6:
        return "Timing spread is too narrow for a simple short/long symbol guess."

    threshold = int(round((shortest + longest) / 2.0))
    bits: list[str] = []
    pair_count = min(len(burst.timings) // 2, 64)
    for pair_index in range(pair_count):
        first = burst.timings[pair_index * 2]
        second = burst.timings[pair_index * 2 + 1]
        if first <= 0 or second <= 0:
            continue
        if first < threshold and second >= threshold:
            bits.append("0")
        elif first >= threshold and second < threshold:
            bits.append("1")
        else:
            bits.append("?")

    if not bits:
        return "No pulse pairs suitable for a bit guess."

    bit_string = "".join(bits)
    confident_bits = [bit for bit in bits if bit in {"0", "1"}]
    confidence = len(confident_bits) / len(bits)
    if confidence < 0.65:
        return f"Possible PWM/PPM framing, but confidence is low. Candidate bits: {bit_string[:64]}"

    padded = "".join(bit if bit in {"0", "1"} else "0" for bit in bits)
    hex_groups: list[str] = []
    for index in range(0, len(padded) - (len(padded) % 4), 4):
        nibble = padded[index : index + 4]
        hex_groups.append(f"{int(nibble, 2):X}")
    hex_preview = "".join(hex_groups[:16]) if hex_groups else ""
    return (
        "Candidate short/long decode found. "
        f"Bits: {bit_string[:96]} Hex: {hex_preview or 'n/a'} (confidence {confidence:.0%})"
    )


def analyze_recording(bursts: list[BurstData]) -> RecordingAnalysis:
    if not bursts:
        return RecordingAnalysis(0, 0, [], 0, 0.0, 0.0, [], [], ["No bursts loaded."])

    timestamps = [burst.timestamp for burst in bursts if burst.timestamp is not None]
    frequencies = sorted({burst.frequency_hz for burst in bursts if burst.frequency_hz})
    signatures = Counter(burst_signature(burst) for burst in bursts if burst.timings)
    repeated_signatures = [(signature, count) for signature, count in signatures.most_common(6) if count > 1]
    likely_frame_bursts = [
        burst.burst
        for burst in bursts
        if len(cluster_durations_us(burst.timings)) >= 2 and len(burst.timings) >= 12 and not burst.truncated
    ][:12]

    notes: list[str] = []
    if repeated_signatures:
        notes.append("Repeated timing shapes were found, which usually means at least one transmitter is repeating a frame.")
    if len(frequencies) > 1:
        notes.append("Multiple center frequencies are present in the file.")
    if any(burst.truncated for burst in bursts):
        notes.append("Some bursts were truncated, so decoded output may be incomplete.")
    if not notes:
        notes.append("The recording looks usable for exploratory protocol work, but decoding will depend on signal family.")

    durations = [burst.total_duration_us for burst in bursts]
    timing_counts = [burst.timing_count for burst in bursts]
    return RecordingAnalysis(
        total_bursts=len(bursts),
        sessions=len({burst.session for burst in bursts}),
        frequencies_hz=frequencies,
        time_span_ms=(max(timestamps) - min(timestamps)) if len(timestamps) >= 2 else 0,
        avg_duration_us=float(sum(durations)) / len(durations),
        avg_timings_per_burst=float(sum(timing_counts)) / len(timing_counts),
        repeated_signatures=repeated_signatures,
        likely_frame_bursts=likely_frame_bursts,
        notes=notes,
    )


class AnalyzerMainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.resize(1580, 980)

        self._settings = QSettings("FlipRSDR", "Analyzer")
        self._bursts: list[BurstData] = []
        self._waterfall_rows: list[np.ndarray] = []
        self._decode_cache: dict[int, str] = {}
        self._decode_detail_cache: dict[int, DecodedFrame | None] = {}
        self._recording_path: Path | None = None
        self._current_index = -1
        self._playback_speed = DEFAULT_PLAYBACK_SPEED
        self._playback_timer = QTimer(self)
        self._playback_timer.setSingleShot(True)
        self._playback_timer.timeout.connect(self._advance_playback)
        self._audio_temp_dir = Path(tempfile.gettempdir()) / "FlipRSDRAnalyzerAudio"
        self._audio_temp_dir.mkdir(parents=True, exist_ok=True)
        self._audio_playback_index = 0
        self._audio_lock = threading.Lock()
        self._last_audio_index = -1
        self._selection_start = -1
        self._selection_end = -1
        self._remote_serial: serial.Serial | None = None

        pg.setConfigOptions(antialias=True, background="#05070a", foreground="#d8e1eb")
        self._build_ui()
        self._load_ui_settings()

    def _build_ui(self) -> None:
        central = QWidget(self)
        self.setCentralWidget(central)

        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(12, 12, 12, 12)
        main_layout.setSpacing(10)

        controls = QHBoxLayout()
        controls.setSpacing(8)

        self.open_button = QPushButton("Open Recording")
        self.export_button = QPushButton("Export CSV")
        self.save_selection_button = QPushButton("Save Selection")
        self.prev_button = QPushButton("Prev")
        self.play_button = QPushButton("Play")
        self.next_button = QPushButton("Next")
        self.audio_checkbox = QCheckBox("Audio")
        self.follow_checkbox = QCheckBox("Follow Replay")
        self.follow_checkbox.setChecked(True)
        self.speed_combo = QComboBox()
        self.speed_combo.addItems([f"{speed:g}x" for speed in PLAYBACK_SPEED_OPTIONS])
        self.view_combo = QComboBox()
        self.view_combo.addItems(["Waveform", "Waterfall"])

        self.open_button.clicked.connect(self._open_recording)
        self.export_button.clicked.connect(self._export_decoded_csv)
        self.save_selection_button.clicked.connect(self._save_selected_bursts)
        self.prev_button.clicked.connect(self._select_previous_burst)
        self.play_button.clicked.connect(self._toggle_playback)
        self.next_button.clicked.connect(self._select_next_burst)
        self.speed_combo.currentTextChanged.connect(self._on_speed_changed)
        self.view_combo.currentTextChanged.connect(self._sync_view_mode)

        controls.addWidget(self.open_button)
        controls.addWidget(self.export_button)
        controls.addWidget(self.save_selection_button)
        controls.addSpacing(8)
        controls.addWidget(self.prev_button)
        controls.addWidget(self.play_button)
        controls.addWidget(self.next_button)
        controls.addSpacing(12)
        controls.addWidget(QLabel("Speed"))
        controls.addWidget(self.speed_combo)
        controls.addWidget(self.audio_checkbox)
        controls.addWidget(self.follow_checkbox)
        controls.addSpacing(12)
        controls.addWidget(QLabel("View"))
        controls.addWidget(self.view_combo)
        controls.addStretch(1)

        main_layout.addLayout(controls)

        remote_controls = QHBoxLayout()
        remote_controls.setSpacing(8)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(180)
        self.refresh_ports_button = QPushButton("Refresh Ports")
        self.remote_baud_combo = QComboBox()
        self.remote_baud_combo.setEditable(True)
        self.remote_baud_combo.addItems(REMOTE_BAUD_OPTIONS)
        self.remote_connect_button = QPushButton("Connect Flipper")
        self.replay_selection_button = QPushButton("Replay Selection")
        self.remote_status_value = QLabel("Flipper disconnected")
        self.remote_status_value.setStyleSheet("color: #ff9c74;")

        self.refresh_ports_button.clicked.connect(self._refresh_ports)
        self.remote_connect_button.clicked.connect(self._toggle_remote_connection)
        self.replay_selection_button.clicked.connect(self._replay_selected_bursts)
        self.port_combo.currentTextChanged.connect(self._on_remote_port_changed)
        self.remote_baud_combo.currentTextChanged.connect(self._on_remote_baud_changed)

        remote_controls.addWidget(QLabel("Flipper"))
        remote_controls.addWidget(self.port_combo)
        remote_controls.addWidget(self.refresh_ports_button)
        remote_controls.addWidget(QLabel("Baud"))
        remote_controls.addWidget(self.remote_baud_combo)
        remote_controls.addWidget(self.remote_connect_button)
        remote_controls.addWidget(self.replay_selection_button)
        remote_controls.addStretch(1)
        remote_controls.addWidget(self.remote_status_value)
        main_layout.addLayout(remote_controls)

        stats_layout = QFormLayout()
        stats_layout.setHorizontalSpacing(18)
        stats_layout.setVerticalSpacing(6)

        self.file_value = QLabel("No file loaded")
        self.playback_value = QLabel("Idle")
        self.session_value = QLabel("-")
        self.burst_value = QLabel("-")
        self.freq_value = QLabel("-")
        self.count_value = QLabel("0")
        self.duration_value = QLabel("0 ms")
        self.rssi_value = QLabel("-")
        self.selection_value = QLabel("-")
        self.decode_value = QLabel("-")
        self.decode_value.setWordWrap(True)
        self.copy_decode_button = QPushButton("Copy")
        self.copy_decode_button.clicked.connect(self._copy_decode_guess)

        decode_row = QWidget()
        decode_row_layout = QHBoxLayout(decode_row)
        decode_row_layout.setContentsMargins(0, 0, 0, 0)
        decode_row_layout.setSpacing(8)
        decode_row_layout.addWidget(self.decode_value, 1)
        decode_row_layout.addWidget(self.copy_decode_button, 0, Qt.AlignRight)

        stats_layout.addRow("File", self.file_value)
        stats_layout.addRow("Playback", self.playback_value)
        stats_layout.addRow("Session", self.session_value)
        stats_layout.addRow("Burst", self.burst_value)
        stats_layout.addRow("Frequency", self.freq_value)
        stats_layout.addRow("Timings", self.count_value)
        stats_layout.addRow("Duration", self.duration_value)
        stats_layout.addRow("RSSI", self.rssi_value)
        stats_layout.addRow("Selection", self.selection_value)
        stats_layout.addRow("Decode Guess", decode_row)

        stats_container = QWidget()
        stats_container.setLayout(stats_layout)
        main_layout.addWidget(stats_container)

        self.waveform_plot = pg.PlotWidget()
        self.waveform_plot.setLabel("bottom", "Elapsed time", units="ms")
        self.waveform_plot.setLabel("left", "Signal state")
        self.waveform_plot.showGrid(x=True, y=True, alpha=0.28)
        self.waveform_curve = self.waveform_plot.plot(pen=pg.mkPen("#5dd4ff", width=1.6))

        self.waterfall_plot = WaterfallPlotWidget()
        self.waterfall_plot.setLabel("bottom", "Pulse / gap duration", units="us")
        self.waterfall_plot.setLabel("left", "Bursts over time")
        self.waterfall_plot.showGrid(x=False, y=False)
        self.waterfall_plot.setMouseEnabled(x=False, y=False)
        self.waterfall_plot.setMenuEnabled(False)
        self.waterfall_image = pg.ImageItem(axisOrder="row-major")
        self.waterfall_plot.addItem(self.waterfall_image)
        self.waterfall_cursor = pg.InfiniteLine(angle=0, movable=False, pen=pg.mkPen("#ffffff", width=1))
        self.waterfall_plot.addItem(self.waterfall_cursor)
        self.waterfall_selection_region = pg.LinearRegionItem(
            values=(0.0, 1.0),
            orientation=getattr(pg.LinearRegionItem, "Horizontal", "horizontal"),
            movable=False,
            brush=pg.mkBrush(93, 212, 255, 44),
            pen=pg.mkPen("#5dd4ff", width=1.2),
        )
        self.waterfall_plot.addItem(self.waterfall_selection_region)
        self.waterfall_image.setLookupTable(self._build_sdr_lut())
        self.waterfall_image.setLevels((0.0, 1.0))
        self.waterfall_plot.selection_callback = self._on_waterfall_drag_selection
        self._configure_waterfall_axes()

        plot_stack = QWidget()
        plot_layout = QVBoxLayout(plot_stack)
        plot_layout.setContentsMargins(0, 0, 0, 0)
        plot_layout.addWidget(self.waveform_plot)
        plot_layout.addWidget(self.waterfall_plot)
        self.waterfall_plot.hide()

        self.timeline_plot = pg.PlotWidget()
        self.timeline_plot.setLabel("bottom", "Recording time", units="ms")
        self.timeline_plot.setLabel("left", "Burst duration", units="ms")
        self.timeline_plot.showGrid(x=True, y=True, alpha=0.28)
        self.timeline_scatter = pg.ScatterPlotItem(size=7, brush=pg.mkBrush("#6bd0ff"), pen=pg.mkPen(None))
        self.timeline_plot.addItem(self.timeline_scatter)
        self.timeline_cursor = pg.InfiniteLine(angle=90, movable=False, pen=pg.mkPen("#ffd166", width=1.2))
        self.timeline_plot.addItem(self.timeline_cursor)

        self.analysis_view = QPlainTextEdit()
        self.analysis_view.setReadOnly(True)
        self.analysis_view.setStyleSheet(
            "QPlainTextEdit { background: #090d13; color: #d6dee8; font-family: Consolas, monospace; }"
        )

        bottom_splitter = QSplitter(Qt.Horizontal)
        bottom_splitter.addWidget(self.timeline_plot)
        bottom_splitter.addWidget(self.analysis_view)
        bottom_splitter.setSizes([860, 520])

        splitter = QSplitter(Qt.Vertical)
        splitter.addWidget(plot_stack)
        splitter.addWidget(bottom_splitter)
        splitter.setSizes([620, 280])
        main_layout.addWidget(splitter, 1)

        self.statusBar().showMessage("Open a fliprsdr or JSON recording to begin")

        open_action = QAction("Open Recording", self)
        open_action.triggered.connect(self._open_recording)
        self.addAction(open_action)

        export_action = QAction("Export CSV", self)
        export_action.triggered.connect(self._export_decoded_csv)
        self.addAction(export_action)

        save_selection_action = QAction("Save Selection", self)
        save_selection_action.triggered.connect(self._save_selected_bursts)
        self.addAction(save_selection_action)

        self._refresh_ports()
        self._update_remote_ui()
        self._update_selection_ui()

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self._save_ui_settings()
        self._stop_playback()
        self._stop_audio_playback()
        self._cleanup_audio_temp_files()
        self._disconnect_remote()
        super().closeEvent(event)

    def _build_sdr_lut(self) -> np.ndarray:
        stops = np.array([0.0, 0.12, 0.26, 0.48, 0.68, 0.82, 1.0], dtype=np.float32)
        colors = np.array(
            [
                (0, 0, 10),
                (0, 32, 86),
                (0, 112, 185),
                (0, 199, 150),
                (232, 222, 63),
                (255, 98, 30),
                (255, 255, 255),
            ],
            dtype=np.float32,
        )
        x = np.linspace(0.0, 1.0, 256, dtype=np.float32)
        lut = np.zeros((256, 3), dtype=np.uint8)
        for channel in range(3):
            lut[:, channel] = np.interp(x, stops, colors[:, channel]).astype(np.uint8)
        return lut

    def _configure_waterfall_axes(self) -> None:
        tick_spacing = math.log2(WATERFALL_MAX_US) - math.log2(WATERFALL_MIN_US)
        ticks: list[tuple[float, str]] = []
        for duration_us in WATERFALL_X_TICKS_US:
            position = WATERFALL_BINS * (
                (math.log2(float(duration_us)) - math.log2(WATERFALL_MIN_US)) / tick_spacing
            )
            label = f"{duration_us / 1000:.0f} ms" if duration_us >= 1000 else f"{duration_us:g} us"
            ticks.append((position, label))
        self.waterfall_plot.getAxis("bottom").setTicks([ticks])

    def _open_recording(self) -> None:
        last_path = str(self._settings.value("file/last_path", "", type=str)).strip()
        selected, _ = QFileDialog.getOpenFileName(
            self,
            "Open FlipRSDR recording",
            last_path or str((Path(__file__).resolve().parent.parent / "recordings")),
            "FlipRSDR Binary (*.fliprsdr);;JSON Lines (*.jsonl);;All Files (*)",
        )
        if not selected:
            return

        path = Path(selected)
        try:
            bursts = load_recording(path)
        except ValueError as exc:
            QMessageBox.critical(self, APP_NAME, str(exc))
            return

        self._settings.setValue("file/last_path", str(path.parent))
        self._load_bursts(path, bursts)

    def _load_bursts(self, path: Path, bursts: list[BurstData]) -> None:
        self._stop_playback()
        self._recording_path = path
        self._bursts = bursts
        self._waterfall_rows = [burst_histogram_row(burst) for burst in bursts]
        self._decode_cache.clear()
        self._decode_detail_cache.clear()
        self._current_index = 0 if bursts else -1
        self._selection_start = self._current_index
        self._selection_end = self._current_index
        self._last_audio_index = -1
        self.file_value.setText(str(path))
        self._apply_consensus_decodes()
        self._render_timeline()
        self._render_waterfall()
        self._render_analysis()
        self._show_current_burst()
        self._update_selection_ui()
        self.statusBar().showMessage(f"Loaded {len(bursts)} burst(s) from {path.name}", 4000)

    def _render_timeline(self) -> None:
        if not self._bursts:
            self.timeline_scatter.setData([], [])
            self.timeline_cursor.setValue(0.0)
            return

        base_timestamp = self._bursts[0].timestamp if self._bursts[0].timestamp is not None else 0
        x_values = []
        y_values = []
        brushes = []
        for burst in self._bursts:
            relative_ms = (
                (burst.timestamp - base_timestamp)
                if burst.timestamp is not None and base_timestamp is not None
                else len(x_values)
            )
            x_values.append(float(relative_ms))
            y_values.append(float(burst.total_duration_us) / 1000.0)
            strength = 1.0
            if burst.rssi is not None:
                strength = min(1.0, max(0.15, (burst.rssi + 120.0) / 60.0))
            color = pg.intColor(int(strength * 255), hues=256, values=1, maxValue=255)
            brushes.append(pg.mkBrush(color))
        self.timeline_scatter.setData(x=x_values, y=y_values, brush=brushes)
        self.timeline_plot.enableAutoRange()

    def _render_waterfall(self) -> None:
        if not self._waterfall_rows:
            canvas = np.zeros((1, WATERFALL_BINS), dtype=np.float32)
            self.waterfall_image.setImage(canvas, autoLevels=False)
            self.waterfall_image.setRect(0.0, 0.0, float(WATERFALL_BINS), 1.0)
            self.waterfall_cursor.setValue(0.0)
            self.waterfall_selection_region.hide()
            return

        canvas = np.vstack(self._waterfall_rows)
        self.waterfall_image.setImage(canvas, autoLevels=False)
        self.waterfall_image.setRect(0.0, 0.0, float(WATERFALL_BINS), float(canvas.shape[0]))
        self.waterfall_plot.setLimits(xMin=0, xMax=WATERFALL_BINS, yMin=0, yMax=canvas.shape[0])
        self.waterfall_plot.setRange(xRange=(0, WATERFALL_BINS), yRange=(0, canvas.shape[0]), padding=0.0)
        self.waterfall_cursor.setValue(float(self._current_index if self._current_index >= 0 else 0))
        self._update_selection_ui()

    def _render_waveform(self, burst: BurstData | None) -> None:
        if burst is None or not burst.timings:
            self.waveform_curve.setData([], [])
            return

        x_values: list[float] = [0.0]
        y_values: list[float] = [1.0 if burst.first_level else 0.0]
        level = y_values[0]
        elapsed_ms = 0.0
        for duration_us in burst.timings:
            elapsed_ms += duration_us / 1000.0
            x_values.append(elapsed_ms)
            y_values.append(level)
            level = 0.0 if level > 0.5 else 1.0
            x_values.append(elapsed_ms)
            y_values.append(level)

        if len(x_values) > MAX_WAVEFORM_POINTS:
            indices = np.linspace(0, len(x_values) - 1, MAX_WAVEFORM_POINTS, dtype=np.int32)
            x_data = np.array(x_values, dtype=np.float32)[indices]
            y_data = np.array(y_values, dtype=np.float32)[indices]
        else:
            x_data = np.array(x_values, dtype=np.float32)
            y_data = np.array(y_values, dtype=np.float32)

        self.waveform_curve.setData(x_data, y_data)
        self.waveform_plot.setXRange(0.0, max(elapsed_ms, 1.0), padding=0.02)
        self.waveform_plot.setYRange(-0.15, 1.15, padding=0.0)

    def _render_analysis(self) -> None:
        analysis = analyze_recording(self._bursts)
        lines = [
            f"Bursts: {analysis.total_bursts}",
            f"Sessions: {analysis.sessions}",
            f"Frequencies: {', '.join(f'{freq / 1_000_000:.3f} MHz' for freq in analysis.frequencies_hz) or 'n/a'}",
            f"Time span: {analysis.time_span_ms / 1000.0:.3f} s",
            f"Average burst duration: {analysis.avg_duration_us / 1000.0:.3f} ms",
            f"Average timings per burst: {analysis.avg_timings_per_burst:.1f}",
            "",
            "Notes:",
        ]
        lines.extend(f"- {note}" for note in analysis.notes)
        lines.append("")
        lines.append("Repeated signatures:")
        if analysis.repeated_signatures:
            for signature, count in analysis.repeated_signatures:
                preview = signature[:96] + ("..." if len(signature) > 96 else "")
                lines.append(f"- x{count}: {preview}")
        else:
            lines.append("- none")
        lines.append("")
        lines.append("Likely frame bursts:")
        lines.append(
            "- "
            + (
                ", ".join(str(burst_id) for burst_id in analysis.likely_frame_bursts)
                if analysis.likely_frame_bursts
                else "none"
            )
        )
        self.analysis_view.setPlainText("\n".join(lines))

    @staticmethod
    def _known_bit_count(bit_string: str) -> int:
        return sum(1 for bit in bit_string if bit in {"0", "1"})

    @staticmethod
    def _align_bit_strings(reference_bits: str, candidate_bits: str) -> tuple[int, int, int]:
        best_shift = 0
        best_matches = -1
        best_overlap = 0
        min_shift = -max(0, len(candidate_bits) - 12)
        max_shift = max(0, len(reference_bits) - 12)
        for shift in range(min_shift, max_shift + 1):
            matches = 0
            overlap = 0
            for index, bit in enumerate(candidate_bits):
                ref_index = index + shift
                if ref_index < 0 or ref_index >= len(reference_bits):
                    continue
                ref_bit = reference_bits[ref_index]
                if bit not in {"0", "1"} or ref_bit not in {"0", "1"}:
                    continue
                overlap += 1
                if bit == ref_bit:
                    matches += 1
            if (
                matches > best_matches
                or (matches == best_matches and overlap > best_overlap)
                or (matches == best_matches and overlap == best_overlap and abs(shift) < abs(best_shift))
            ):
                best_shift = shift
                best_matches = matches
                best_overlap = overlap
        return best_shift, best_matches, best_overlap

    def _decode_detail_for_index(self, index: int) -> DecodedFrame | None:
        if index not in self._decode_detail_cache and 0 <= index < len(self._bursts):
            self._decode_detail_cache[index] = decode_burst_details(self._bursts[index])
        return self._decode_detail_cache.get(index)

    def _apply_consensus_decodes(self) -> None:
        groups: dict[tuple[int, int], list[tuple[int, DecodedFrame]]] = {}
        for index, burst in enumerate(self._bursts):
            detail = decode_burst_details(burst)
            self._decode_detail_cache[index] = detail
            if detail is None or self._known_bit_count(detail.bit_string) < 24:
                continue
            groups.setdefault(detail.family_key, []).append((index, detail))

        for entries in groups.values():
            if len(entries) < 2:
                continue

            sorted_entries = sorted(
                entries,
                key=lambda item: (
                    self._known_bit_count(item[1].bit_string),
                    len(item[1].bit_string),
                    item[1].confidence,
                ),
                reverse=True,
            )
            clusters: list[list[tuple[int, DecodedFrame, int]]] = []
            cluster_references: list[str] = []

            for index, detail in sorted_entries:
                assigned = False
                for cluster_index, reference_bits in enumerate(cluster_references):
                    shift, matches, overlap = self._align_bit_strings(reference_bits, detail.bit_string)
                    if overlap < max(20, min(len(detail.bit_string), len(reference_bits)) // 2):
                        continue
                    if matches < max(14, int(overlap * 0.82)):
                        continue
                    clusters[cluster_index].append((index, detail, shift))
                    assigned = True
                    break
                if not assigned:
                    cluster_references.append(detail.bit_string)
                    clusters.append([(index, detail, 0)])

            for cluster in clusters:
                if len(cluster) < 2:
                    continue

                reference_index, reference, _ = max(
                    cluster,
                    key=lambda item: (
                        self._known_bit_count(item[1].bit_string),
                        len(item[1].bit_string),
                        item[1].confidence,
                    ),
                )
                reference_bits = reference.bit_string
                aligned_entries: list[tuple[int, DecodedFrame, int]] = []
                for index, detail, _shift in cluster:
                    shift, matches, overlap = self._align_bit_strings(reference_bits, detail.bit_string)
                    if overlap < max(20, min(len(detail.bit_string), len(reference_bits)) // 2):
                        continue
                    if matches < max(14, int(overlap * 0.82)):
                        continue
                    aligned_entries.append((index, detail, shift))

                if len(aligned_entries) < 2:
                    continue

                zero_votes = [0] * len(reference_bits)
                one_votes = [0] * len(reference_bits)
                for _index, detail, shift in aligned_entries:
                    for bit_index, bit in enumerate(detail.bit_string):
                        ref_index = bit_index + shift
                        if ref_index < 0 or ref_index >= len(reference_bits):
                            continue
                        if bit == "0":
                            zero_votes[ref_index] += 1
                        elif bit == "1":
                            one_votes[ref_index] += 1

                consensus_bits: list[str] = []
                for ref_index, ref_bit in enumerate(reference_bits):
                    if zero_votes[ref_index] > one_votes[ref_index]:
                        consensus_bits.append("0")
                    elif one_votes[ref_index] > zero_votes[ref_index]:
                        consensus_bits.append("1")
                    elif ref_bit in {"0", "1"}:
                        consensus_bits.append(ref_bit)
                    else:
                        consensus_bits.append("?")
                consensus_string = "".join(consensus_bits)

                for index, detail, shift in aligned_entries:
                    resolved = list(detail.bit_string)
                    for bit_index, bit in enumerate(resolved):
                        if bit != "?":
                            continue
                        ref_index = bit_index + shift
                        if ref_index < 0 or ref_index >= len(reference_bits):
                            continue
                        consensus_bit = consensus_string[ref_index]
                        if consensus_bit in {"0", "1"}:
                            resolved[bit_index] = consensus_bit
                    resolved_string = "".join(resolved)
                    original_unknown = detail.bit_string.count("?")
                    resolved_unknown = resolved_string.count("?")
                    if resolved_unknown >= original_unknown:
                        continue
                    resolved_confidence = self._known_bit_count(resolved_string) / max(len(resolved_string), 1)
                    resolved_frame = _build_decoded_frame(
                        label=f"Consensus pulse-width frame detected (x{len(aligned_entries)})",
                        bit_string=resolved_string,
                        confidence=max(detail.confidence, resolved_confidence),
                        sync_us=detail.sync_us,
                        short_us=detail.short_us,
                        long_us=detail.long_us,
                    )
                    self._decode_cache[index] = resolved_frame.format_text()
                    self._decode_detail_cache[index] = resolved_frame

    def _decode_text_for_index(self, index: int) -> str:
        decode_text = self._decode_cache.get(index)
        if decode_text is None and 0 <= index < len(self._bursts):
            detail = self._decode_detail_for_index(index)
            decode_text = detail.format_text() if detail is not None else try_decode_burst(self._bursts[index])
            self._decode_cache[index] = decode_text
        return decode_text or "-"

    def _show_current_burst(self) -> None:
        burst = self._current_burst()
        if burst is None:
            self.playback_value.setText("Idle")
            self.session_value.setText("-")
            self.burst_value.setText("-")
            self.freq_value.setText("-")
            self.count_value.setText("0")
            self.duration_value.setText("0 ms")
            self.rssi_value.setText("-")
            self.decode_value.setText("-")
            self._render_waveform(None)
            self._update_selection_ui()
            return

        self.playback_value.setText(
            f"Burst {self._current_index + 1} / {len(self._bursts)}"
            + (" (playing)" if self._playback_timer.isActive() else "")
        )
        self.session_value.setText(str(burst.session))
        self.burst_value.setText(str(burst.burst))
        self.freq_value.setText(f"{burst.frequency_hz / 1_000_000:.3f} MHz" if burst.frequency_hz else "-")
        self.count_value.setText(str(burst.timing_count))
        self.duration_value.setText(f"{burst.total_duration_us / 1000.0:.3f} ms")
        self.rssi_value.setText(f"{burst.rssi:.1f} dBm" if burst.rssi is not None else "-")
        self.decode_value.setText(self._decode_text_for_index(self._current_index))
        self._render_waveform(burst)
        self._update_timeline_cursor()
        self.waterfall_cursor.setValue(float(self._current_index))
        if self.follow_checkbox.isChecked() and self.view_combo.currentText() == "Waterfall":
            row_min = max(0, self._current_index - 24)
            row_max = min(len(self._bursts), self._current_index + 24)
            self.waterfall_plot.setYRange(float(row_min), float(max(row_max, row_min + 1)), padding=0.0)
        self._update_selection_ui()

    def _selection_range(self) -> tuple[int, int]:
        if not self._bursts:
            return (-1, -1)
        if self._selection_start < 0 or self._selection_end < 0:
            return (self._current_index, self._current_index)
        start = max(0, min(self._selection_start, self._selection_end))
        end = min(len(self._bursts) - 1, max(self._selection_start, self._selection_end))
        return (start, end)

    def _selected_bursts(self) -> list[tuple[int, BurstData]]:
        start, end = self._selection_range()
        if start < 0 or end < 0:
            return []
        return [(index, self._bursts[index]) for index in range(start, end + 1)]

    def _set_selection(self, start: int, end: int, *, sync_current: bool) -> None:
        if not self._bursts:
            self._selection_start = -1
            self._selection_end = -1
            self._update_selection_ui()
            return
        self._selection_start = max(0, min(start, len(self._bursts) - 1))
        self._selection_end = max(0, min(end, len(self._bursts) - 1))
        if sync_current:
            self._current_index = min(self._selection_start, self._selection_end)
            self._show_current_burst()
            return
        self._update_selection_ui()

    def _update_selection_ui(self) -> None:
        selected = self._selected_bursts()
        if not selected:
            self.selection_value.setText("-")
            self.save_selection_button.setEnabled(False)
            self.replay_selection_button.setEnabled(False)
            self.waterfall_selection_region.hide()
            return

        start, end = self._selection_range()
        first_burst = selected[0][1]
        last_burst = selected[-1][1]
        if start == end:
            text = f"Burst {first_burst.burst} (1 selected)"
        else:
            text = f"Bursts {first_burst.burst}-{last_burst.burst} ({len(selected)} selected)"
        self.selection_value.setText(text)
        self.save_selection_button.setEnabled(True)
        self.replay_selection_button.setEnabled(self._remote_serial is not None)
        self.waterfall_selection_region.setRegion((float(start), float(end + 1)))
        self.waterfall_selection_region.show()

    def _on_waterfall_drag_selection(self, start_row: float, end_row: float, finished: bool) -> None:
        if not self._bursts:
            return
        start = int(max(0, min(len(self._bursts) - 1, math.floor(min(start_row, end_row)))))
        end = int(max(0, min(len(self._bursts) - 1, math.floor(max(start_row, end_row)))))
        self._selection_start = start
        self._selection_end = end
        if finished:
            self._set_selection(start, end, sync_current=True)
        else:
            self._update_selection_ui()

    def _update_timeline_cursor(self) -> None:
        burst = self._current_burst()
        if burst is None:
            self.timeline_cursor.setValue(0.0)
            return
        base_timestamp = self._bursts[0].timestamp if self._bursts and self._bursts[0].timestamp is not None else 0
        relative_ms = (
            (burst.timestamp - base_timestamp)
            if burst.timestamp is not None and base_timestamp is not None
            else self._current_index
        )
        self.timeline_cursor.setValue(float(relative_ms))

    def _current_burst(self) -> BurstData | None:
        if self._current_index < 0 or self._current_index >= len(self._bursts):
            return None
        return self._bursts[self._current_index]

    def _select_previous_burst(self) -> None:
        if not self._bursts:
            return
        self._current_index = max(0, self._current_index - 1)
        if self._selection_start == self._selection_end:
            self._selection_start = self._current_index
            self._selection_end = self._current_index
        self._show_current_burst()
        self._play_current_audio_if_needed()

    def _select_next_burst(self) -> None:
        if not self._bursts:
            return
        self._current_index = min(len(self._bursts) - 1, self._current_index + 1)
        if self._selection_start == self._selection_end:
            self._selection_start = self._current_index
            self._selection_end = self._current_index
        self._show_current_burst()
        self._play_current_audio_if_needed()

    def _toggle_playback(self) -> None:
        if not self._bursts:
            QMessageBox.information(self, APP_NAME, "Open a recording first.")
            return
        if self._playback_timer.isActive():
            self._stop_playback()
        else:
            self._start_playback()

    def _start_playback(self) -> None:
        if not self._bursts:
            return
        if self._current_index < 0:
            self._current_index = 0
        self._last_audio_index = self._current_index - 1
        self.play_button.setText("Pause")
        self._show_current_burst()
        self._play_current_audio_if_needed()
        self._schedule_next_playback_step()

    def _stop_playback(self) -> None:
        self._playback_timer.stop()
        self.play_button.setText("Play")
        self._show_current_burst()

    def _finish_playback(self) -> None:
        self._playback_timer.stop()
        self.play_button.setText("Play")
        if self._bursts:
            self._current_index = 0
            self._last_audio_index = -1
        self._show_current_burst()

    def _schedule_next_playback_step(self) -> None:
        if not self._bursts or self._current_index < 0 or self._current_index >= len(self._bursts):
            return
        delay_ms = self._playback_delay_ms(self._current_index)
        self._playback_timer.start(delay_ms)

    def _playback_delay_ms(self, index: int) -> int:
        current = self._bursts[index]
        delay_ms = max(30.0, current.total_duration_us / 1000.0)
        if index + 1 < len(self._bursts):
            current_ts = current.timestamp if current.timestamp is not None else 0
            next_ts = self._bursts[index + 1].timestamp if self._bursts[index + 1].timestamp is not None else current_ts
            timestamp_gap_ms = max(0.0, float(next_ts - current_ts))
            if timestamp_gap_ms > 0:
                delay_ms = min(1000.0, max(delay_ms, timestamp_gap_ms))
        scaled_delay_ms = delay_ms / max(self._playback_speed, 0.01)
        return int(max(15, round(scaled_delay_ms)))

    def _copy_decode_guess(self) -> None:
        if not self._bursts:
            QMessageBox.information(self, APP_NAME, "Open a recording first.")
            return
        decode_text = self._decode_text_for_index(self._current_index)
        QApplication.clipboard().setText(decode_text)
        self.statusBar().showMessage("Decode guess copied to clipboard", 2500)

    def _export_decoded_csv(self) -> None:
        if not self._bursts:
            QMessageBox.information(self, APP_NAME, "Open a recording first.")
            return

        default_name = (
            f"{self._recording_path.stem}_decoded.csv"
            if self._recording_path is not None
            else "fliprsdr_decoded.csv"
        )
        default_dir = str(self._recording_path.parent) if self._recording_path is not None else str(Path.cwd())
        selected, _ = QFileDialog.getSaveFileName(
            self,
            "Export decoded bursts to CSV",
            str(Path(default_dir) / default_name),
            "CSV Files (*.csv);;All Files (*)",
        )
        if not selected:
            return

        output_path = Path(selected)
        try:
            with output_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(
                    [
                        "index",
                        "session",
                        "burst",
                        "frequency_hz",
                        "timestamp_ms",
                        "timing_count",
                        "duration_us",
                        "rssi_dbm",
                        "truncated",
                        "first_level",
                        "decode_guess",
                        "timings_us",
                    ]
                )
                for index, burst in enumerate(self._bursts, start=1):
                    writer.writerow(
                        [
                            index,
                            burst.session,
                            burst.burst,
                            burst.frequency_hz,
                            burst.timestamp if burst.timestamp is not None else "",
                            burst.timing_count,
                            burst.total_duration_us,
                            burst.rssi if burst.rssi is not None else "",
                            int(burst.truncated),
                            int(burst.first_level),
                            self._decode_text_for_index(index - 1),
                            " ".join(str(value) for value in burst.timings),
                        ]
                    )
        except OSError as exc:
            QMessageBox.critical(self, APP_NAME, f"Unable to export CSV:\n{exc}")
            return

        self.statusBar().showMessage(f"Exported decoded bursts to {output_path.name}", 4000)

    def _save_selected_bursts(self) -> None:
        selected = self._selected_bursts()
        if not selected:
            QMessageBox.information(self, APP_NAME, "Select one or more bursts first.")
            return

        start, end = self._selection_range()
        default_stem = self._recording_path.stem if self._recording_path is not None else "fliprsdr_selection"
        default_name = f"{default_stem}_bursts_{start + 1:03d}_{end + 1:03d}.fliprsdr"
        default_dir = str(self._recording_path.parent) if self._recording_path is not None else str(Path.cwd())
        selected_path, _ = QFileDialog.getSaveFileName(
            self,
            "Save selected bursts",
            str(Path(default_dir) / default_name),
            "FlipRSDR Binary (*.fliprsdr);;JSON Lines (*.jsonl);;All Files (*)",
        )
        if not selected_path:
            return

        output_path = Path(selected_path)
        try:
            if output_path.suffix.lower() == ".jsonl":
                with output_path.open("w", encoding="utf-8") as handle:
                    for _index, burst in selected:
                        handle.write(
                            json.dumps(self._burst_to_capture_dict(burst), separators=(",", ":")) + "\n"
                        )
            else:
                payload = bytearray()
                for _index, burst in selected:
                    payload.extend(encode_recording_burst(self._burst_to_capture_dict(burst)))
                output_path.write_bytes(bytes(payload))
        except OSError as exc:
            QMessageBox.critical(self, APP_NAME, f"Unable to save selection:\n{exc}")
            return

        self.statusBar().showMessage(f"Saved {len(selected)} selected burst(s) to {output_path.name}", 4000)

    def _burst_to_capture_dict(self, burst: BurstData) -> dict[str, object]:
        message: dict[str, object] = {
            "type": "burst_capture",
            "session": burst.session,
            "burst": burst.burst,
            "freq": burst.frequency_hz,
            "first_level": 1 if burst.first_level else 0,
            "timings": list(burst.timings),
            "count": burst.timing_count,
            "truncated": burst.truncated,
        }
        if burst.timestamp is not None:
            message["timestamp"] = burst.timestamp
        if burst.rssi is not None:
            message["rssi"] = burst.rssi
        return message

    def _refresh_ports(self) -> None:
        preferred_port = self.port_combo.currentText().strip()
        if not preferred_port:
            preferred_port = str(self._settings.value("remote/last_port", "", type=str)).strip()

        current_port = self.port_combo.currentText().strip()
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if preferred_port and preferred_port in ports:
            self.port_combo.setCurrentText(preferred_port)
        elif current_port and current_port in ports:
            self.port_combo.setCurrentText(current_port)
        elif ports:
            self.port_combo.setCurrentIndex(0)

    def _set_remote_status(self, text: str, *, ok: bool) -> None:
        self.remote_status_value.setText(text)
        self.remote_status_value.setStyleSheet(f"color: {'#7CFC98' if ok else '#ff9c74'};")

    def _update_remote_ui(self) -> None:
        connected = self._remote_serial is not None
        self.remote_connect_button.setText("Disconnect Flipper" if connected else "Connect Flipper")
        self.port_combo.setEnabled(not connected)
        self.refresh_ports_button.setEnabled(not connected)
        self.remote_baud_combo.setEnabled(not connected)
        self.replay_selection_button.setEnabled(connected and bool(self._selected_bursts()))
        if connected:
            port_name = self.port_combo.currentText().strip()
            self._set_remote_status(f"Connected to {port_name}", ok=True)
        else:
            self._set_remote_status("Flipper disconnected", ok=False)

    def _toggle_remote_connection(self) -> None:
        if self._remote_serial is None:
            self._connect_remote()
        else:
            self._disconnect_remote()

    def _connect_remote(self) -> None:
        port_name = self.port_combo.currentText().strip()
        if not port_name:
            QMessageBox.warning(self, APP_NAME, "Select a serial port first.")
            return

        baud_text = self.remote_baud_combo.currentText().strip()
        try:
            baud_rate = int(baud_text)
        except ValueError:
            QMessageBox.warning(self, APP_NAME, f"Invalid baud rate: {baud_text}")
            return

        try:
            self._remote_serial = serial.Serial(port_name, baud_rate, timeout=0.2, write_timeout=1.0)
            try:
                self._remote_serial.dtr = True
                self._remote_serial.rts = True
            except serial.SerialException:
                pass
            self._remote_serial.reset_input_buffer()
            self._settings.setValue("remote/last_port", port_name)
            self._settings.setValue("remote/baud_rate", str(baud_rate))
        except serial.SerialException as exc:
            self._remote_serial = None
            QMessageBox.critical(self, APP_NAME, f"Unable to open {port_name}:\n{exc}")
            return

        self._update_remote_ui()
        self.statusBar().showMessage(f"Connected to Flipper on {port_name}", 3000)

    def _disconnect_remote(self) -> None:
        if self._remote_serial is not None:
            try:
                self._remote_serial.close()
            except serial.SerialException:
                pass
            self._remote_serial = None
        self._update_remote_ui()

    def _on_remote_port_changed(self, port_name: str) -> None:
        if port_name.strip():
            self._settings.setValue("remote/last_port", port_name.strip())

    def _on_remote_baud_changed(self, baud_rate: str) -> None:
        if baud_rate.strip():
            self._settings.setValue("remote/baud_rate", baud_rate.strip())

    def _validate_replayable_bursts(self, bursts: Iterable[tuple[int, BurstData]]) -> str | None:
        for index, burst in bursts:
            if burst.truncated or burst.timing_count != len(burst.timings):
                return f"Burst {index + 1} is incomplete and cannot be replayed faithfully."
            if not burst.timings:
                return f"Burst {index + 1} has no stored timings to replay."
            if len(burst.timings) > MAX_AUDIO_TIMINGS:
                return f"Burst {index + 1} exceeds the Flipper replay buffer ({len(burst.timings)} timings)."
            if burst.frequency_hz <= 0:
                return f"Burst {index + 1} is missing a valid frequency."
        return None

    def _selection_replay_batches(self) -> list[tuple[list[str], float]]:
        selected = self._selected_bursts()
        validation_error = self._validate_replayable_bursts(selected)
        if validation_error is not None:
            raise ValueError(validation_error)

        batches: list[tuple[list[str], float]] = []
        for position, (_index, burst) in enumerate(selected):
            commands = list(
                build_replay_commands(
                    burst.frequency_hz,
                    burst.first_level,
                    burst.timings,
                )
            )
            delay_s = max(0.15, burst.total_duration_us / 1_000_000.0)
            if position + 1 < len(selected):
                next_burst = selected[position + 1][1]
                if burst.timestamp is not None and next_burst.timestamp is not None:
                    gap_ms = max(
                        0,
                        int(next_burst.timestamp - burst.timestamp - round(burst.total_duration_us / 1000.0)),
                    )
                    delay_s += gap_ms / 1000.0
            batches.append((commands, delay_s))
        return batches

    def _replay_selected_bursts(self) -> None:
        if self._remote_serial is None:
            QMessageBox.warning(self, APP_NAME, "Connect to the Flipper before replaying a selection.")
            return

        selected = self._selected_bursts()
        if not selected:
            QMessageBox.information(self, APP_NAME, "Select one or more bursts first.")
            return

        try:
            batches = self._selection_replay_batches()
            QApplication.setOverrideCursor(Qt.WaitCursor)
            for batch_index, (commands, delay_s) in enumerate(batches):
                for command in commands:
                    self._remote_serial.write(f"{command}\n".encode("utf-8"))
                    QApplication.processEvents()
                    time.sleep(0.015)
                self._remote_serial.flush()
                if batch_index + 1 < len(batches):
                    QApplication.processEvents()
                    time.sleep(delay_s)
        except (ValueError, serial.SerialException, serial.SerialTimeoutException) as exc:
            QMessageBox.critical(self, APP_NAME, f"Unable to replay selection:\n{exc}")
            if isinstance(exc, serial.SerialException):
                self._disconnect_remote()
            return
        finally:
            QApplication.restoreOverrideCursor()

        start, end = self._selection_range()
        self.statusBar().showMessage(
            f"Sent burst selection {start + 1}-{end + 1} to the Flipper for replay",
            4000,
        )

    def _advance_playback(self) -> None:
        if not self._bursts:
            self._stop_playback()
            return
        if self._current_index >= len(self._bursts) - 1:
            self._finish_playback()
            return

        self._current_index += 1
        self._show_current_burst()
        self._play_current_audio_if_needed()
        self._schedule_next_playback_step()

    def _play_current_audio_if_needed(self) -> None:
        if not self.audio_checkbox.isChecked():
            return
        if self._current_index == self._last_audio_index:
            return
        burst = self._current_burst()
        if burst is None:
            return
        self._last_audio_index = self._current_index
        self._play_burst_audio(burst)

    def _play_burst_audio(self, burst: BurstData) -> None:
        if winsound is None or not burst.timings:
            return
        if burst.total_duration_us > MAX_AUDIO_BURST_US or burst.timing_count > MAX_AUDIO_TIMINGS:
            return

        wave_bytes = self._burst_to_wave_bytes(burst)
        if not wave_bytes:
            return

        with self._audio_lock:
            self._audio_playback_index += 1
            audio_path = self._audio_temp_dir / f"burst_{self._audio_playback_index:08d}.wav"
            audio_path.write_bytes(wave_bytes)

        winsound.PlaySound(str(audio_path), winsound.SND_FILENAME | winsound.SND_ASYNC)

    def _stop_audio_playback(self) -> None:
        if winsound is None:
            return
        winsound.PlaySound(None, 0)

    def _cleanup_audio_temp_files(self) -> None:
        if not self._audio_temp_dir.exists():
            return
        for path in self._audio_temp_dir.glob("*.wav"):
            try:
                path.unlink()
            except OSError:
                continue

    def _burst_to_wave_bytes(self, burst: BurstData) -> bytes:
        sample_rate = 22050
        segments: list[np.ndarray] = []
        level = 0.75 if burst.first_level else -0.75
        for duration_us in burst.timings:
            sample_count = max(1, int(round((duration_us / 1_000_000.0) * sample_rate)))
            segments.append(np.full(sample_count, level, dtype=np.float32))
            level = -level

        if not segments:
            return b""

        waveform = np.concatenate(segments)
        waveform = np.clip(waveform * 32767.0, -32767, 32767).astype(np.int16)

        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(waveform.tobytes())
        return buffer.getvalue()

    def _on_speed_changed(self, text: str) -> None:
        parsed = text.strip().rstrip("x")
        self._playback_speed = float(parsed) if parsed else DEFAULT_PLAYBACK_SPEED

    def _sync_view_mode(self, mode: str) -> None:
        if mode == "Waterfall":
            self.waveform_plot.hide()
            self.waterfall_plot.show()
        else:
            self.waterfall_plot.hide()
            self.waveform_plot.show()

    def _load_ui_settings(self) -> None:
        saved_speed = float(self._settings.value("playback/speed", DEFAULT_PLAYBACK_SPEED, type=float))
        if saved_speed in PLAYBACK_SPEED_OPTIONS:
            self.speed_combo.setCurrentText(f"{saved_speed:g}x")
            self._playback_speed = saved_speed
        else:
            self.speed_combo.setCurrentText(f"{DEFAULT_PLAYBACK_SPEED:g}x")

        saved_audio = bool(self._settings.value("audio/enabled", False, type=bool))
        self.audio_checkbox.setChecked(saved_audio)
        saved_view = str(self._settings.value("view/mode", "Waveform", type=str))
        self.view_combo.setCurrentText(saved_view if saved_view in {"Waveform", "Waterfall"} else "Waveform")
        saved_baud = str(
            self._settings.value("remote/baud_rate", str(DEFAULT_REMOTE_BAUD_RATE), type=str)
        ).strip()
        if saved_baud:
            self.remote_baud_combo.setCurrentText(saved_baud)

    def _save_ui_settings(self) -> None:
        self._settings.setValue("playback/speed", self._playback_speed)
        self._settings.setValue("audio/enabled", self.audio_checkbox.isChecked())
        self._settings.setValue("view/mode", self.view_combo.currentText())
        self._settings.setValue("remote/last_port", self.port_combo.currentText().strip())
        self._settings.setValue("remote/baud_rate", self.remote_baud_combo.currentText().strip())


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    window = AnalyzerMainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
