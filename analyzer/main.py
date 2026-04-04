from __future__ import annotations

import io
import json
import math
import sys
import tempfile
import threading
import wave
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import numpy as np
import pyqtgraph as pg
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

from fliprsdr_protocol import decode_binary_recording

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
                    timestamp=int(message["timestamp"]) if "timestamp" in message else None,
                    first_level=bool(int(message.get("first_level", 1))),
                    timings=[int(value) for value in message.get("timings", [])],
                    count=int(message.get("count", len(message.get("timings", [])))),
                    rssi=float(message["rssi"]) if "rssi" in message else None,
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
                timestamp=int(message["timestamp"]) if "timestamp" in message else None,
                first_level=bool(int(message.get("first_level", 1))),
                timings=[int(value) for value in message.get("timings", [])],
                count=int(message.get("count", len(message.get("timings", [])))),
                rssi=float(message["rssi"]) if "rssi" in message else None,
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


def try_decode_burst(burst: BurstData) -> str:
    if len(burst.timings) < 8:
        return "Too short to decode confidently."

    timings = [value for value in burst.timings if value > 0]
    if len(timings) < 8:
        return "Not enough non-zero timings."

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
        self.prev_button.clicked.connect(self._select_previous_burst)
        self.play_button.clicked.connect(self._toggle_playback)
        self.next_button.clicked.connect(self._select_next_burst)
        self.speed_combo.currentTextChanged.connect(self._on_speed_changed)
        self.view_combo.currentTextChanged.connect(self._sync_view_mode)

        controls.addWidget(self.open_button)
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
        self.decode_value = QLabel("-")
        self.decode_value.setWordWrap(True)

        stats_layout.addRow("File", self.file_value)
        stats_layout.addRow("Playback", self.playback_value)
        stats_layout.addRow("Session", self.session_value)
        stats_layout.addRow("Burst", self.burst_value)
        stats_layout.addRow("Frequency", self.freq_value)
        stats_layout.addRow("Timings", self.count_value)
        stats_layout.addRow("Duration", self.duration_value)
        stats_layout.addRow("RSSI", self.rssi_value)
        stats_layout.addRow("Decode Guess", self.decode_value)

        stats_container = QWidget()
        stats_container.setLayout(stats_layout)
        main_layout.addWidget(stats_container)

        self.waveform_plot = pg.PlotWidget()
        self.waveform_plot.setLabel("bottom", "Elapsed time", units="ms")
        self.waveform_plot.setLabel("left", "Signal state")
        self.waveform_plot.showGrid(x=True, y=True, alpha=0.28)
        self.waveform_curve = self.waveform_plot.plot(pen=pg.mkPen("#5dd4ff", width=1.6))

        self.waterfall_plot = pg.PlotWidget()
        self.waterfall_plot.setLabel("bottom", "Pulse / gap duration", units="us")
        self.waterfall_plot.setLabel("left", "Bursts over time")
        self.waterfall_plot.showGrid(x=False, y=False)
        self.waterfall_plot.setMouseEnabled(x=False, y=False)
        self.waterfall_plot.setMenuEnabled(False)
        self.waterfall_image = pg.ImageItem(axisOrder="row-major")
        self.waterfall_plot.addItem(self.waterfall_image)
        self.waterfall_cursor = pg.InfiniteLine(angle=0, movable=False, pen=pg.mkPen("#ffffff", width=1))
        self.waterfall_plot.addItem(self.waterfall_cursor)
        self.waterfall_image.setLookupTable(self._build_sdr_lut())
        self.waterfall_image.setLevels((0.0, 1.0))
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

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self._save_ui_settings()
        self._stop_playback()
        self._stop_audio_playback()
        self._cleanup_audio_temp_files()
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
        self._bursts = bursts
        self._waterfall_rows = [burst_histogram_row(burst) for burst in bursts]
        self._decode_cache.clear()
        self._current_index = 0 if bursts else -1
        self._last_audio_index = -1
        self.file_value.setText(str(path))
        self._render_timeline()
        self._render_waterfall()
        self._render_analysis()
        self._show_current_burst()
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
            return

        canvas = np.vstack(self._waterfall_rows)
        self.waterfall_image.setImage(canvas, autoLevels=False)
        self.waterfall_image.setRect(0.0, 0.0, float(WATERFALL_BINS), float(canvas.shape[0]))
        self.waterfall_plot.setLimits(xMin=0, xMax=WATERFALL_BINS, yMin=0, yMax=canvas.shape[0])
        self.waterfall_plot.setRange(xRange=(0, WATERFALL_BINS), yRange=(0, canvas.shape[0]), padding=0.0)
        self.waterfall_cursor.setValue(float(self._current_index if self._current_index >= 0 else 0))

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
        decode_text = self._decode_cache.get(self._current_index)
        if decode_text is None:
            decode_text = try_decode_burst(burst)
            self._decode_cache[self._current_index] = decode_text
        self.decode_value.setText(decode_text)
        self._render_waveform(burst)
        self._update_timeline_cursor()
        self.waterfall_cursor.setValue(float(self._current_index))
        if self.follow_checkbox.isChecked() and self.view_combo.currentText() == "Waterfall":
            row_min = max(0, self._current_index - 24)
            row_max = min(len(self._bursts), self._current_index + 24)
            self.waterfall_plot.setYRange(float(row_min), float(max(row_max, row_min + 1)), padding=0.0)

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
        self._show_current_burst()
        self._play_current_audio_if_needed()

    def _select_next_burst(self) -> None:
        if not self._bursts:
            return
        self._current_index = min(len(self._bursts) - 1, self._current_index + 1)
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

    def _save_ui_settings(self) -> None:
        self._settings.setValue("playback/speed", self._playback_speed)
        self._settings.setValue("audio/enabled", self.audio_checkbox.isChecked())
        self._settings.setValue("view/mode", self.view_combo.currentText())


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    window = AnalyzerMainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
