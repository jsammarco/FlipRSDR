from __future__ import annotations

import io
import json
import math
import sys
import tempfile
import threading
import time
import wave
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import numpy as np
import pyqtgraph as pg
import serial
import serial.tools.list_ports
from PySide6.QtCore import QEvent, QSettings, QThread, QTimer, Qt, Signal
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

from fliprsdr_protocol import (
    BinaryStreamDecoder,
    PROTOCOL_FORMAT_FLIPRSDR,
    PROTOCOL_FORMAT_JSON,
    encode_recording_burst,
    format_message_log,
)

try:
    import winsound
except ImportError:  # pragma: no cover - only used on Windows builds
    winsound = None


APP_NAME = "FlipRSDR Receiver"
DEFAULT_BAUD_RATE = 9600
PORT_REFRESH_INTERVAL_MS = 5000
WATERFALL_BINS = 512
DEFAULT_WATERFALL_WINDOW_S = 30
WATERFALL_WINDOW_OPTIONS_S = [10, 20, 30, 60, 120]
WATERFALL_MAX_RENDER_ROWS = 512
WATERFALL_MIN_US = 16.0
WATERFALL_MAX_US = 65536.0
WAVEFORM_UPDATE_INTERVAL_S = 0.05
WATERFALL_REFRESH_INTERVAL_MS = 250
WATERFALL_X_TICKS_US = [16, 32, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000, 32000, 65536]
PROTOCOL_OPTIONS = [PROTOCOL_FORMAT_FLIPRSDR, PROTOCOL_FORMAT_JSON]
REMOTE_FREQUENCY_OPTIONS = ["300.000", "315.000", "390.000", "433.920", "868.350", "915.000"]
REMOTE_RSSI_THRESHOLD_OPTIONS = ["Off", "-110", "-100", "-90", "-80", "-70", "-60", "-50"]


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
    complete: bool = False

    def key(self) -> tuple[int, int]:
        return (self.session, self.burst)

    def clone(self) -> "BurstData":
        return BurstData(
            session=self.session,
            burst=self.burst,
            frequency_hz=self.frequency_hz,
            timestamp=self.timestamp,
            first_level=self.first_level,
            timings=list(self.timings),
            count=self.count,
            rssi=self.rssi,
            truncated=self.truncated,
            complete=self.complete,
        )

    @property
    def timing_count(self) -> int:
        return self.count if self.count else len(self.timings)

    @property
    def total_duration_us(self) -> int:
        return int(sum(self.timings))

    def to_capture_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = {
            "type": "burst_capture",
            "session": self.session,
            "burst": self.burst,
            "freq": self.frequency_hz,
            "first_level": 1 if self.first_level else 0,
            "timings": self.timings,
            "count": self.timing_count,
            "truncated": self.truncated,
        }
        if self.timestamp is not None:
            data["timestamp"] = self.timestamp
        if self.rssi is not None:
            data["rssi"] = self.rssi
        return data


class SerialReceiverThread(QThread):
    status_changed = Signal(str)
    raw_line_received = Signal(str)
    burst_started = Signal(object)
    burst_chunk = Signal(object)
    burst_completed = Signal(object)
    parse_warning = Signal(str)

    def __init__(
        self,
        port_name: str,
        baud_rate: int,
        protocol_format: str,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._port_name = port_name
        self._baud_rate = baud_rate
        self._protocol_format = protocol_format
        self._stop_event = threading.Event()
        self._bursts: dict[tuple[int, int], BurstData] = {}
        self._binary_decoder = BinaryStreamDecoder()
        self._command_lock = threading.Lock()
        self._pending_commands: deque[str] = deque()
        self._connected = False

    def stop(self) -> None:
        self._stop_event.set()

    def send_command(self, command: str) -> bool:
        command = command.strip()
        if not command:
            return False
        with self._command_lock:
            if not self._connected:
                return False
            self._pending_commands.append(command)
        return True

    def _flush_pending_commands(self, serial_port: serial.Serial) -> bool:
        while True:
            with self._command_lock:
                if not self._pending_commands:
                    return True
                command = self._pending_commands.popleft()

            payload = f"{command}\n".encode("utf-8")
            try:
                serial_port.write(payload)
            except serial.SerialTimeoutException as exc:
                self.parse_warning.emit(f"Serial write timeout: {exc}")
                with self._command_lock:
                    self._pending_commands.appendleft(command)
                return True
            except serial.SerialException as exc:
                self.status_changed.emit(f"Serial write failed: {exc}")
                with self._command_lock:
                    self._connected = False
                return False
            self.raw_line_received.emit(f"[cmd] {command}")

    def run(self) -> None:
        try:
            with serial.Serial(
                self._port_name,
                self._baud_rate,
                timeout=0.2,
                write_timeout=1.0,
            ) as serial_port:
                try:
                    serial_port.dtr = True
                    serial_port.rts = True
                except serial.SerialException:
                    pass

                serial_port.reset_input_buffer()
                with self._command_lock:
                    self._connected = True
                self.status_changed.emit(f"Connected to {self._port_name}")

                while not self._stop_event.is_set():
                    if not self._flush_pending_commands(serial_port):
                        break

                    try:
                        if self._protocol_format == PROTOCOL_FORMAT_FLIPRSDR:
                            raw = serial_port.read(max(1, serial_port.in_waiting or 0))
                        else:
                            raw = serial_port.readline()
                    except serial.SerialException as exc:
                        self.status_changed.emit(f"Serial read failed: {exc}")
                        break

                    if not raw:
                        continue

                    if self._protocol_format == PROTOCOL_FORMAT_FLIPRSDR:
                        messages, warnings = self._binary_decoder.feed(raw)
                        for warning in warnings:
                            self.parse_warning.emit(f"fliprsdr parse warning: {warning}")
                        for message in messages:
                            self.raw_line_received.emit(format_message_log(message))
                            self._handle_message(message)
                        continue

                    text = raw.decode("utf-8", errors="replace").strip()
                    if not text:
                        continue

                    self.raw_line_received.emit(text)
                    payload = self._extract_json_payload(text)
                    if payload is None:
                        continue

                    try:
                        message = json.loads(payload)
                    except json.JSONDecodeError as exc:
                        self.parse_warning.emit(f"JSON parse warning: {exc}")
                        continue

                    self._handle_message(message)
        except serial.SerialException as exc:
            self.status_changed.emit(f"Unable to open {self._port_name}: {exc}")
        finally:
            with self._command_lock:
                self._connected = False
            self.status_changed.emit("Disconnected")

    @staticmethod
    def _extract_json_payload(text: str) -> str | None:
        start = text.find("{")
        end = text.rfind("}")
        if start < 0 or end <= start:
            return None
        return text[start : end + 1]

    @staticmethod
    def _optional_int(value: object) -> int | None:
        return int(value) if value is not None else None

    @staticmethod
    def _optional_float(value: object) -> float | None:
        return float(value) if value is not None else None

    def _get_or_create_burst(self, message: dict[str, Any]) -> BurstData:
        session = int(message.get("session", 0))
        burst = int(message.get("burst", 0))
        key = (session, burst)
        burst_data = self._bursts.get(key)
        if burst_data is None:
            burst_data = BurstData(session=session, burst=burst)
            self._bursts[key] = burst_data
        return burst_data

    def _handle_message(self, message: dict[str, Any]) -> None:
        message_type = message.get("type")
        if message_type == "burst_start":
            burst = self._get_or_create_burst(message)
            burst.frequency_hz = int(message.get("freq", burst.frequency_hz or 0))
            timestamp = message.get("timestamp")
            burst.timestamp = int(timestamp) if timestamp is not None else burst.timestamp
            burst.first_level = bool(int(message.get("first_level", 1)))
            burst.timings.clear()
            burst.count = 0
            burst.rssi = None
            burst.truncated = False
            burst.complete = False
            self.burst_started.emit(
                {
                    "session": burst.session,
                    "burst": burst.burst,
                    "freq": burst.frequency_hz,
                    "timestamp": burst.timestamp,
                    "first_level": burst.first_level,
                }
            )
        elif message_type == "timing_chunk":
            burst = self._get_or_create_burst(message)
            timings = [int(value) for value in message.get("timings", [])]
            burst.timings.extend(timings)
            burst.count = len(burst.timings)
            self.burst_chunk.emit(
                {
                    "session": burst.session,
                    "burst": burst.burst,
                    "timings": timings,
                    "current_count": burst.count,
                    "freq": burst.frequency_hz,
                    "first_level": burst.first_level,
                }
            )
        elif message_type == "burst_end":
            burst = self._get_or_create_burst(message)
            burst.count = int(message.get("count", len(burst.timings)))
            if "rssi" in message:
                burst.rssi = float(message["rssi"])
            burst.truncated = bool(message.get("truncated", False))
            burst.complete = True
            self.burst_completed.emit(burst.clone())
            self._bursts.pop(burst.key(), None)
        elif message_type == "burst_capture":
            burst = BurstData(
                session=int(message.get("session", 0)),
                burst=int(message.get("burst", 0)),
                frequency_hz=int(message.get("freq", 0)),
                timestamp=self._optional_int(message.get("timestamp")),
                first_level=bool(int(message.get("first_level", 1))),
                timings=[int(value) for value in message.get("timings", [])],
                count=int(message.get("count", len(message.get("timings", [])))),
                rssi=self._optional_float(message.get("rssi")),
                truncated=bool(message.get("truncated", False)),
                complete=True,
            )
            self.burst_completed.emit(burst)


class ReceiverMainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.resize(1440, 920)

        self._settings = QSettings("FlipRSDR", "Receiver")
        self._receiver_thread: SerialReceiverThread | None = None
        self._current_burst: BurstData | None = None
        self._last_waveform_update = 0.0
        self._record_file_handle: io.TextIOWrapper | io.BufferedWriter | None = None
        self._recording_path: Path | None = None
        self._waterfall_window_s = DEFAULT_WATERFALL_WINDOW_S
        self._waterfall_entries: deque[tuple[float, np.ndarray]] = deque()
        self._audio_temp_dir = Path(tempfile.gettempdir()) / "FlipRSDRReceiverAudio"
        self._audio_temp_dir.mkdir(parents=True, exist_ok=True)
        self._audio_playback_index = 0
        self._current_audio_path: Path | None = None
        self._audio_lock = threading.Lock()
        self._port_refresh_timer = QTimer(self)
        self._port_refresh_timer.setInterval(PORT_REFRESH_INTERVAL_MS)
        self._port_refresh_timer.timeout.connect(self._refresh_ports_if_disconnected)
        self._waterfall_timer = QTimer(self)
        self._waterfall_timer.setInterval(WATERFALL_REFRESH_INTERVAL_MS)
        self._waterfall_timer.timeout.connect(self._refresh_waterfall_window)

        pg.setConfigOptions(antialias=True, background="#05070a", foreground="#d8e1eb")

        self._build_ui()
        self._load_ui_settings()
        self._refresh_ports()
        self._update_connection_ui(False)
        self._port_refresh_timer.start()
        self._waterfall_timer.start()

    def _build_ui(self) -> None:
        central = QWidget(self)
        self.setCentralWidget(central)

        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(12, 12, 12, 12)
        main_layout.setSpacing(10)

        control_row = QHBoxLayout()
        control_row.setSpacing(8)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(180)
        self.refresh_ports_button = QPushButton("Refresh Ports")
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["9600", "57600", "115200", "230400", "460800", "921600"])
        self.baud_combo.setCurrentText(str(DEFAULT_BAUD_RATE))
        self.protocol_combo = QComboBox()
        self.protocol_combo.addItems(PROTOCOL_OPTIONS)
        self.connect_button = QPushButton("Connect")
        self.view_combo = QComboBox()
        self.view_combo.addItems(["Waveform", "Waterfall"])
        self.view_combo.currentTextChanged.connect(self._sync_view_mode)
        self.waterfall_window_combo = QComboBox()
        self.waterfall_window_combo.addItems([f"{seconds}s" for seconds in WATERFALL_WINDOW_OPTIONS_S])
        self.audio_checkbox = QCheckBox("Audio")
        self.record_button = QPushButton("Start Recording")
        self.record_button.setCheckable(True)
        self.choose_recording_button = QPushButton("Recording Folder")
        self.clear_waterfall_button = QPushButton("Clear Waterfall")
        self.start_scan_button = QPushButton("Start Scan")
        self.stop_scan_button = QPushButton("Stop Scan")
        self.remote_frequency_combo = QComboBox()
        self.remote_frequency_combo.setEditable(True)
        self.remote_frequency_combo.addItems(REMOTE_FREQUENCY_OPTIONS)
        self.send_frequency_button = QPushButton("Send Frequency")
        self.remote_rssi_threshold_combo = QComboBox()
        self.remote_rssi_threshold_combo.setEditable(True)
        self.remote_rssi_threshold_combo.addItems(REMOTE_RSSI_THRESHOLD_OPTIONS)
        self.send_rssi_threshold_button = QPushButton("Send RSSI")
        self.command_status_value = QLabel("Remote control idle")
        self.command_status_value.setStyleSheet("color: #9fb3c8;")

        self.refresh_ports_button.clicked.connect(self._refresh_ports)
        self.connect_button.clicked.connect(self._toggle_connection)
        self.audio_checkbox.toggled.connect(self._on_audio_toggled)
        self.waterfall_window_combo.currentTextChanged.connect(self._on_waterfall_window_changed)
        self.record_button.toggled.connect(self._toggle_recording)
        self.choose_recording_button.clicked.connect(self._choose_recording_folder)
        self.clear_waterfall_button.clicked.connect(self._clear_waterfall)
        self.port_combo.currentTextChanged.connect(self._on_port_selection_changed)
        self.baud_combo.currentTextChanged.connect(self._on_baud_selection_changed)
        self.protocol_combo.currentTextChanged.connect(self._on_protocol_selection_changed)
        self.start_scan_button.clicked.connect(lambda: self._send_receiver_command("start_scan"))
        self.stop_scan_button.clicked.connect(lambda: self._send_receiver_command("stop_scan"))
        self.send_frequency_button.clicked.connect(self._send_frequency_command)
        self.send_rssi_threshold_button.clicked.connect(self._send_rssi_threshold_command)
        self.remote_frequency_combo.currentTextChanged.connect(self._on_remote_frequency_changed)
        self.remote_rssi_threshold_combo.currentTextChanged.connect(
            self._on_remote_rssi_threshold_changed
        )

        control_row.addWidget(QLabel("Port"))
        control_row.addWidget(self.port_combo)
        control_row.addWidget(self.refresh_ports_button)
        control_row.addSpacing(8)
        control_row.addWidget(QLabel("Baud"))
        control_row.addWidget(self.baud_combo)
        control_row.addSpacing(8)
        control_row.addWidget(QLabel("Protocol"))
        control_row.addWidget(self.protocol_combo)
        control_row.addSpacing(8)
        control_row.addWidget(self.connect_button)
        control_row.addSpacing(16)
        control_row.addWidget(QLabel("View"))
        control_row.addWidget(self.view_combo)
        control_row.addWidget(QLabel("Window"))
        control_row.addWidget(self.waterfall_window_combo)
        control_row.addWidget(self.audio_checkbox)
        control_row.addWidget(self.record_button)
        control_row.addWidget(self.choose_recording_button)
        control_row.addWidget(self.clear_waterfall_button)
        control_row.addStretch(1)

        main_layout.addLayout(control_row)

        command_row = QHBoxLayout()
        command_row.setSpacing(8)
        command_row.addWidget(QLabel("Remote"))
        command_row.addWidget(self.start_scan_button)
        command_row.addWidget(self.stop_scan_button)
        command_row.addSpacing(10)
        command_row.addWidget(QLabel("Frequency"))
        command_row.addWidget(self.remote_frequency_combo)
        command_row.addWidget(self.send_frequency_button)
        command_row.addSpacing(10)
        command_row.addWidget(QLabel("RSSI Min"))
        command_row.addWidget(self.remote_rssi_threshold_combo)
        command_row.addWidget(self.send_rssi_threshold_button)
        command_row.addStretch(1)
        command_row.addWidget(self.command_status_value)
        main_layout.addLayout(command_row)

        stats_layout = QFormLayout()
        stats_layout.setHorizontalSpacing(18)
        stats_layout.setVerticalSpacing(6)

        self.connection_value = QLabel("Disconnected")
        self.connection_value.setStyleSheet("color: #8fbc8f;")
        self.session_value = QLabel("-")
        self.burst_value = QLabel("-")
        self.freq_value = QLabel("-")
        self.count_value = QLabel("0")
        self.duration_value = QLabel("0 ms")
        self.rssi_value = QLabel("-")
        self.truncated_value = QLabel("No")
        self.recording_value = QLabel("Off")

        stats_layout.addRow("Connection", self.connection_value)
        stats_layout.addRow("Session", self.session_value)
        stats_layout.addRow("Burst", self.burst_value)
        stats_layout.addRow("Frequency", self.freq_value)
        stats_layout.addRow("Timings", self.count_value)
        stats_layout.addRow("Duration", self.duration_value)
        stats_layout.addRow("RSSI", self.rssi_value)
        stats_layout.addRow("Truncated", self.truncated_value)
        stats_layout.addRow("Recording", self.recording_value)

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
        self.waterfall_plot.setLabel("left", "Recent bursts")
        self.waterfall_plot.showGrid(x=False, y=False)
        self.waterfall_image = pg.ImageItem(axisOrder="row-major")
        self.waterfall_plot.addItem(self.waterfall_image)
        self.waterfall_plot.setMouseEnabled(x=False, y=False)
        self.waterfall_plot.setMenuEnabled(False)
        self.waterfall_image.setLookupTable(self._build_sdr_lut())
        self.waterfall_image.setLevels((0.0, 1.0))
        self._configure_waterfall_axes()

        self.plot_stack = QWidget()
        stack_layout = QVBoxLayout(self.plot_stack)
        stack_layout.setContentsMargins(0, 0, 0, 0)
        stack_layout.addWidget(self.waveform_plot)
        stack_layout.addWidget(self.waterfall_plot)
        self.waterfall_plot.hide()

        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(600)
        self.log_view.setStyleSheet(
            "QPlainTextEdit { background: #090d13; color: #d6dee8; font-family: Consolas, monospace; }"
        )

        splitter = QSplitter(Qt.Vertical)
        splitter.addWidget(self.plot_stack)
        splitter.addWidget(self.log_view)
        splitter.setSizes([700, 220])
        main_layout.addWidget(splitter, 1)

        self.statusBar().showMessage("Ready")

        refresh_action = QAction("Refresh Ports", self)
        refresh_action.triggered.connect(self._refresh_ports)
        self.addAction(refresh_action)

    def closeEvent(self, event) -> None:  # type: ignore[override]
        self._save_ui_settings()
        self._disconnect_receiver()
        self._stop_audio_playback()
        self._stop_recording()
        self._cleanup_audio_temp_files()
        super().closeEvent(event)

    def focusInEvent(self, event) -> None:  # type: ignore[override]
        self._refresh_ports_if_disconnected()
        super().focusInEvent(event)

    def changeEvent(self, event) -> None:  # type: ignore[override]
        if event.type() == QEvent.Type.ActivationChange and self.isActiveWindow():
            self._refresh_ports_if_disconnected()
        super().changeEvent(event)

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
            if duration_us >= 1000:
                label = f"{duration_us / 1000:.0f} ms"
            else:
                label = f"{duration_us:g} us"
            ticks.append((position, label))

        self.waterfall_plot.getAxis("bottom").setTicks([ticks])

    def _refresh_ports(self) -> None:
        preferred_port = self.port_combo.currentText().strip()
        if not preferred_port:
            preferred_port = str(self._settings.value("serial/last_port", "", type=str)).strip()

        current_port = self.port_combo.currentText()
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if preferred_port and preferred_port in ports:
            self.port_combo.setCurrentText(preferred_port)
        elif current_port and current_port in ports:
            self.port_combo.setCurrentText(current_port)
        elif ports:
            self.port_combo.setCurrentIndex(0)
        self.statusBar().showMessage(f"Found {len(ports)} serial port(s)")
        if self.port_combo.currentText().strip():
            self._settings.setValue("serial/last_port", self.port_combo.currentText().strip())

    def _refresh_ports_if_disconnected(self) -> None:
        if self._receiver_thread and self._receiver_thread.isRunning():
            return
        self._refresh_ports()

    def _toggle_connection(self) -> None:
        if self._receiver_thread and self._receiver_thread.isRunning():
            self._disconnect_receiver()
        else:
            self._connect_receiver()

    def _connect_receiver(self) -> None:
        port_name = self.port_combo.currentText().strip()
        if not port_name:
            QMessageBox.warning(self, APP_NAME, "Select a serial port first.")
            return

        baud_rate = int(self.baud_combo.currentText())
        protocol_format = self.protocol_combo.currentText().strip() or PROTOCOL_FORMAT_FLIPRSDR
        self._settings.setValue("serial/last_port", port_name)
        self._settings.setValue("serial/baud_rate", baud_rate)
        self._settings.setValue("serial/protocol", protocol_format)
        self._receiver_thread = SerialReceiverThread(port_name, baud_rate, protocol_format, self)
        self._receiver_thread.status_changed.connect(self._on_status_changed)
        self._receiver_thread.raw_line_received.connect(self._append_log_line)
        self._receiver_thread.burst_started.connect(self._on_burst_started)
        self._receiver_thread.burst_chunk.connect(self._on_burst_chunk)
        self._receiver_thread.burst_completed.connect(self._on_burst_completed)
        self._receiver_thread.parse_warning.connect(self._append_warning)
        self._receiver_thread.finished.connect(lambda: self._update_connection_ui(False))
        self._receiver_thread.start()
        self._update_connection_ui(True)

    def _disconnect_receiver(self) -> None:
        if not self._receiver_thread:
            return
        self._receiver_thread.stop()
        self._receiver_thread.wait(1500)
        self._receiver_thread = None
        self._stop_audio_playback()
        self._update_connection_ui(False)
        self._set_command_status("Remote control disconnected", ok=False)

    def _update_connection_ui(self, connected: bool) -> None:
        self.connect_button.setText("Disconnect" if connected else "Connect")
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        self.protocol_combo.setEnabled(not connected)
        self.refresh_ports_button.setEnabled(not connected)
        self.start_scan_button.setEnabled(connected)
        self.stop_scan_button.setEnabled(connected)
        self.remote_frequency_combo.setEnabled(connected)
        self.send_frequency_button.setEnabled(connected)
        self.remote_rssi_threshold_combo.setEnabled(connected)
        self.send_rssi_threshold_button.setEnabled(connected)

    def _on_port_selection_changed(self, port_name: str) -> None:
        if port_name.strip():
            self._settings.setValue("serial/last_port", port_name.strip())

    def _on_baud_selection_changed(self, baud_rate: str) -> None:
        if baud_rate.strip():
            self._settings.setValue("serial/baud_rate", baud_rate.strip())

    def _on_protocol_selection_changed(self, protocol_format: str) -> None:
        if protocol_format.strip():
            self._settings.setValue("serial/protocol", protocol_format.strip())

    def _on_remote_frequency_changed(self, text: str) -> None:
        if text.strip():
            self._settings.setValue("remote/frequency", text.strip())

    def _on_remote_rssi_threshold_changed(self, text: str) -> None:
        if text.strip():
            self._settings.setValue("remote/rssi_threshold", text.strip())

    def _on_audio_toggled(self, enabled: bool) -> None:
        if not enabled:
            self._stop_audio_playback()

    def _on_waterfall_window_changed(self, text: str) -> None:
        window_text = text.strip().rstrip("s")
        if not window_text:
            return
        self._waterfall_window_s = max(1, int(window_text))
        self._settings.setValue("waterfall/window_s", self._waterfall_window_s)
        self._refresh_waterfall_window()

    def _on_status_changed(self, text: str) -> None:
        self.connection_value.setText(text)
        if "Connected" in text:
            self.connection_value.setStyleSheet("color: #7CFC98;")
        elif "Disconnected" in text:
            self.connection_value.setStyleSheet("color: #ff9c74;")
        else:
            self.connection_value.setStyleSheet("color: #ffd166;")
        self.statusBar().showMessage(text, 3000)

    def _set_command_status(self, text: str, *, ok: bool | None = None) -> None:
        if ok is True:
            color = "#7CFC98"
        elif ok is False:
            color = "#ff9c74"
        else:
            color = "#9fb3c8"
        self.command_status_value.setStyleSheet(f"color: {color};")
        self.command_status_value.setText(text)

    def _send_receiver_command(self, command: str) -> bool:
        if not self._receiver_thread or not self._receiver_thread.isRunning():
            QMessageBox.warning(self, APP_NAME, "Connect to the Flipper before sending commands.")
            self._set_command_status("Remote control disconnected", ok=False)
            return False

        sent = self._receiver_thread.send_command(command)
        if sent:
            self._append_log_line(f"[cmd] {command}")
            self._set_command_status(f"Sent: {command}", ok=True)
        else:
            self._set_command_status(f"Failed: {command}", ok=False)
        return sent

    def _send_frequency_command(self) -> None:
        frequency_text = self.remote_frequency_combo.currentText().strip()
        if not frequency_text:
            QMessageBox.warning(self, APP_NAME, "Enter a frequency first.")
            return
        self._send_receiver_command(f"set_frequency {frequency_text}")

    def _send_rssi_threshold_command(self) -> None:
        threshold_text = self.remote_rssi_threshold_combo.currentText().strip()
        if not threshold_text:
            QMessageBox.warning(self, APP_NAME, "Select an RSSI threshold first.")
            return
        self._send_receiver_command(f"set_rssi_threshold {threshold_text}")

    def _append_log_line(self, text: str) -> None:
        self.log_view.appendPlainText(text)

    def _append_warning(self, text: str) -> None:
        self.log_view.appendPlainText(f"[warn] {text}")

    def _on_burst_started(self, payload: dict[str, Any]) -> None:
        self._current_burst = BurstData(
            session=int(payload["session"]),
            burst=int(payload["burst"]),
            frequency_hz=int(payload.get("freq", 0)),
            timestamp=payload.get("timestamp"),
            first_level=bool(payload.get("first_level", True)),
        )
        self._last_waveform_update = 0.0
        self._update_stats(self._current_burst)

    def _on_burst_chunk(self, payload: dict[str, Any]) -> None:
        session = int(payload["session"])
        burst = int(payload["burst"])
        if self._current_burst is None or self._current_burst.key() != (session, burst):
            self._current_burst = BurstData(
                session=session,
                burst=burst,
                frequency_hz=int(payload.get("freq", 0)),
                first_level=bool(payload.get("first_level", True)),
            )

        self._current_burst.timings.extend(int(value) for value in payload.get("timings", []))
        self._current_burst.count = int(payload.get("current_count", len(self._current_burst.timings)))
        now = time.monotonic()
        if now - self._last_waveform_update >= WAVEFORM_UPDATE_INTERVAL_S:
            self._update_stats(self._current_burst)
            self._render_waveform(self._current_burst)
            self._last_waveform_update = now

    def _on_burst_completed(self, burst: BurstData) -> None:
        self._current_burst = burst
        self._update_stats(burst)
        self._render_waveform(burst)
        self._append_waterfall_row(burst)
        if self.record_button.isChecked():
            self._write_recording_line(burst)
        if self.audio_checkbox.isChecked():
            self._play_burst_audio(burst)

    def _update_stats(self, burst: BurstData | None) -> None:
        if burst is None:
            self.session_value.setText("-")
            self.burst_value.setText("-")
            self.freq_value.setText("-")
            self.count_value.setText("0")
            self.duration_value.setText("0 ms")
            self.rssi_value.setText("-")
            self.truncated_value.setText("No")
            return

        self.session_value.setText(str(burst.session))
        self.burst_value.setText(str(burst.burst))
        self.freq_value.setText(f"{burst.frequency_hz / 1_000_000:.3f} MHz" if burst.frequency_hz else "-")
        self.count_value.setText(str(burst.timing_count))
        self.duration_value.setText(f"{burst.total_duration_us / 1000.0:.3f} ms")
        self.rssi_value.setText(f"{burst.rssi:.1f} dBm" if burst.rssi is not None else "-")
        self.truncated_value.setText("Yes" if burst.truncated else "No")

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

        self.waveform_curve.setData(np.array(x_values), np.array(y_values))
        self.waveform_plot.setXRange(0.0, max(elapsed_ms, 1.0), padding=0.02)
        self.waveform_plot.setYRange(-0.15, 1.15, padding=0.0)

    def _append_waterfall_row(self, burst: BurstData) -> None:
        if not burst.timings:
            return

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
        row = np.sqrt(row)

        self._waterfall_entries.append((time.monotonic(), row))
        while len(self._waterfall_entries) > WATERFALL_MAX_RENDER_ROWS:
            self._waterfall_entries.popleft()
        self._refresh_waterfall_window()

    def _refresh_waterfall_window(self) -> None:
        cutoff = time.monotonic() - float(self._waterfall_window_s)
        while self._waterfall_entries and self._waterfall_entries[0][0] < cutoff:
            self._waterfall_entries.popleft()

        if not self._waterfall_entries:
            canvas = np.zeros((1, WATERFALL_BINS), dtype=np.float32)
            self.waterfall_image.setImage(canvas, autoLevels=False)
            self.waterfall_image.setRect(0.0, 0.0, float(WATERFALL_BINS), 1.0)
            self.waterfall_plot.setLimits(xMin=0, xMax=WATERFALL_BINS, yMin=0, yMax=1)
            self.waterfall_plot.setRange(xRange=(0, WATERFALL_BINS), yRange=(0, 1), padding=0.0)
            return

        recent_entries = list(self._waterfall_entries)[-WATERFALL_MAX_RENDER_ROWS:]
        canvas = np.vstack([entry[1] for entry in recent_entries])
        self.waterfall_image.setImage(canvas, autoLevels=False)
        self.waterfall_image.setRect(0.0, 0.0, float(WATERFALL_BINS), float(canvas.shape[0]))
        self.waterfall_plot.setLimits(xMin=0, xMax=WATERFALL_BINS, yMin=0, yMax=canvas.shape[0])
        self.waterfall_plot.setRange(
            xRange=(0, WATERFALL_BINS), yRange=(0, canvas.shape[0]), padding=0.0
        )

    def _clear_waterfall(self) -> None:
        self._waterfall_entries.clear()
        self._refresh_waterfall_window()

    def _sync_view_mode(self, mode: str) -> None:
        if mode == "Waterfall":
            self.waveform_plot.hide()
            self.waterfall_plot.show()
        else:
            self.waterfall_plot.hide()
            self.waveform_plot.show()

    def _default_recordings_dir(self) -> Path:
        return Path(__file__).resolve().parent.parent / "recordings"

    def _choose_recording_folder(self) -> None:
        base_dir = self._default_recordings_dir()
        selected = QFileDialog.getExistingDirectory(
            self,
            "Select recording folder",
            str(base_dir),
        )
        if selected:
            self.statusBar().showMessage(f"Recording folder set to {selected}", 4000)
            self._recording_path = Path(selected)
            if self.record_button.isChecked():
                self._stop_recording()
                self.record_button.setChecked(True)

    def _toggle_recording(self, enabled: bool) -> None:
        if enabled:
            self._start_recording()
        else:
            self._stop_recording()

    def _start_recording(self) -> None:
        target_dir = self._recording_path or self._default_recordings_dir()
        target_dir.mkdir(parents=True, exist_ok=True)
        protocol_format = self.protocol_combo.currentText().strip() or PROTOCOL_FORMAT_FLIPRSDR
        extension = ".fliprsdr" if protocol_format == PROTOCOL_FORMAT_FLIPRSDR else ".jsonl"
        filename = time.strftime(f"fliprsdr_receiver_%Y%m%d_%H%M%S{extension}")
        path = target_dir / filename
        self._record_file_handle = (
            path.open("wb")
            if protocol_format == PROTOCOL_FORMAT_FLIPRSDR
            else path.open("w", encoding="utf-8")
        )
        self.recording_value.setText(str(path))
        self.record_button.setText("Stop Recording")
        self.statusBar().showMessage(f"Recording to {path}", 4000)

    def _stop_recording(self) -> None:
        if self._record_file_handle:
            self._record_file_handle.close()
            self._record_file_handle = None
        self.record_button.blockSignals(True)
        self.record_button.setChecked(False)
        self.record_button.blockSignals(False)
        self.record_button.setText("Start Recording")
        self.recording_value.setText("Off")

    def _write_recording_line(self, burst: BurstData) -> None:
        if not self._record_file_handle:
            return
        protocol_format = self.protocol_combo.currentText().strip() or PROTOCOL_FORMAT_FLIPRSDR
        if protocol_format == PROTOCOL_FORMAT_FLIPRSDR:
            assert isinstance(self._record_file_handle, io.BufferedWriter)
            self._record_file_handle.write(encode_recording_burst(burst.to_capture_dict()))
        else:
            assert isinstance(self._record_file_handle, io.TextIOWrapper)
            self._record_file_handle.write(json.dumps(burst.to_capture_dict(), separators=(",", ":")))
            self._record_file_handle.write("\n")
        self._record_file_handle.flush()

    def _play_burst_audio(self, burst: BurstData) -> None:
        if winsound is None or not burst.timings:
            return

        wave_bytes = self._burst_to_wave_bytes(burst)
        if wave_bytes:
            self._play_wave_bytes(wave_bytes)

    def _play_wave_bytes(self, wave_bytes: bytes) -> None:
        with self._audio_lock:
            self._audio_playback_index += 1
            audio_path = self._audio_temp_dir / f"burst_{self._audio_playback_index:08d}.wav"
            audio_path.write_bytes(wave_bytes)
            self._current_audio_path = audio_path

        winsound.PlaySound(
            str(audio_path),
            winsound.SND_FILENAME | winsound.SND_ASYNC,
        )

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

    def _load_ui_settings(self) -> None:
        saved_baud = str(self._settings.value("serial/baud_rate", str(DEFAULT_BAUD_RATE), type=str))
        baud_index = self.baud_combo.findText(saved_baud)
        if baud_index >= 0:
            self.baud_combo.setCurrentIndex(baud_index)

        saved_protocol = str(
            self._settings.value("serial/protocol", PROTOCOL_FORMAT_FLIPRSDR, type=str)
        ).strip()
        protocol_index = self.protocol_combo.findText(saved_protocol)
        if protocol_index >= 0:
            self.protocol_combo.setCurrentIndex(protocol_index)
        else:
            self.protocol_combo.setCurrentText(PROTOCOL_FORMAT_FLIPRSDR)

        saved_waterfall_window = int(
            self._settings.value("waterfall/window_s", DEFAULT_WATERFALL_WINDOW_S, type=int)
        )
        if saved_waterfall_window in WATERFALL_WINDOW_OPTIONS_S:
            self._waterfall_window_s = saved_waterfall_window
            self.waterfall_window_combo.setCurrentText(f"{saved_waterfall_window}s")
        else:
            self.waterfall_window_combo.setCurrentText(f"{DEFAULT_WATERFALL_WINDOW_S}s")

        saved_view = str(self._settings.value("view/mode", "Waterfall", type=str)).strip()
        self.view_combo.setCurrentText(saved_view if saved_view in {"Waveform", "Waterfall"} else "Waterfall")

        saved_recording_path = str(self._settings.value("recording/path", "", type=str)).strip()
        if saved_recording_path:
            self._recording_path = Path(saved_recording_path)

        saved_remote_frequency = str(
            self._settings.value("remote/frequency", "433.920", type=str)
        ).strip()
        if saved_remote_frequency:
            self.remote_frequency_combo.setCurrentText(saved_remote_frequency)

        saved_remote_rssi_threshold = str(
            self._settings.value("remote/rssi_threshold", "Off", type=str)
        ).strip()
        if saved_remote_rssi_threshold:
            self.remote_rssi_threshold_combo.setCurrentText(saved_remote_rssi_threshold)

    def _save_ui_settings(self) -> None:
        self._settings.setValue("serial/last_port", self.port_combo.currentText().strip())
        self._settings.setValue("serial/baud_rate", self.baud_combo.currentText().strip())
        self._settings.setValue("serial/protocol", self.protocol_combo.currentText().strip())
        self._settings.setValue("view/mode", self.view_combo.currentText())
        self._settings.setValue("waterfall/window_s", self._waterfall_window_s)
        if self._recording_path is not None:
            self._settings.setValue("recording/path", str(self._recording_path))


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    window = ReceiverMainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
