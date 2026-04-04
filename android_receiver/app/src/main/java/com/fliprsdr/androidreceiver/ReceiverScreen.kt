package com.fliprsdr.androidreceiver

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlin.math.max

private enum class ReceiverTab(
    val title: String,
    val glyph: String,
) {
    CONNECT("Connect", "USB"),
    CONTROL("Control", "CMD"),
    MONITOR("Monitor", "SIG"),
    LOGS("Logs", "LOG"),
}

@Composable
fun AndroidReceiverScreen(viewModel: ReceiverViewModel) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    var currentTab by rememberSaveable { mutableStateOf(ReceiverTab.CONNECT) }

    MaterialTheme(colorScheme = androidReceiverColors()) {
        Scaffold(
            containerColor = MaterialTheme.colorScheme.background,
            bottomBar = {
                NavigationBar(containerColor = Color(0xFF0B1620)) {
                    ReceiverTab.entries.forEach { tab ->
                        NavigationBarItem(
                            selected = currentTab == tab,
                            onClick = { currentTab = tab },
                            icon = { Text(tab.glyph, fontSize = 11.sp) },
                            label = { Text(tab.title) },
                        )
                    }
                }
            },
        ) { padding ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.background)
                    .padding(padding)
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                StatusBanner(state)
                when (currentTab) {
                    ReceiverTab.CONNECT -> ConnectPage(
                        state = state,
                        onTransportMode = viewModel::setTransportMode,
                        onRefresh = viewModel::refreshDevices,
                        onSelectDevice = viewModel::setSelectedDevice,
                        onBaudRateChanged = viewModel::setBaudRate,
                        onProtocolChanged = viewModel::setProtocolFormat,
                        onToggleConnection = viewModel::toggleConnection,
                    )
                    ReceiverTab.CONTROL -> ControlPage(
                        state = state,
                        onStartScan = viewModel::sendStartScan,
                        onStopScan = viewModel::sendStopScan,
                        onFrequencyChanged = viewModel::setRemoteFrequency,
                        onSendFrequency = viewModel::sendFrequency,
                        onRssiChanged = viewModel::setRemoteRssiThreshold,
                        onSendRssi = viewModel::sendRssiThreshold,
                    )
                    ReceiverTab.MONITOR -> MonitorPage(
                        state = state,
                        onViewMode = viewModel::setViewMode,
                        onWindowChanged = viewModel::setWaterfallWindowSeconds,
                        onAudioChanged = viewModel::setAudioEnabled,
                        onToggleRecording = viewModel::toggleRecording,
                        onClearWaterfall = viewModel::clearWaterfall,
                    )
                    ReceiverTab.LOGS -> LogsPage(state.logs)
                }
            }
        }
    }
}

@Composable
private fun StatusBanner(state: ReceiverUiState) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Text("Android Receiver", style = MaterialTheme.typography.headlineSmall)
            Text(
                state.connectionStatus,
                color = if (state.connected) Color(0xFF8CF0C3) else Color(0xFFFFAE77),
                style = MaterialTheme.typography.bodyLarge,
            )
            Text(
                "USB and BLE serial receiver for FlipRSDR with remote commands, waveform/waterfall monitoring, recording, and audio playback.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun ConnectPage(
    state: ReceiverUiState,
    onTransportMode: (TransportMode) -> Unit,
    onRefresh: () -> Unit,
    onSelectDevice: (String) -> Unit,
    onBaudRateChanged: (String) -> Unit,
    onProtocolChanged: (ProtocolFormat) -> Unit,
    onToggleConnection: () -> Unit,
) {
    Column(
        modifier = Modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                Text("Transport", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    ToggleChoice("USB", state.transportMode == TransportMode.USB) { onTransportMode(TransportMode.USB) }
                    ToggleChoice("BLE", state.transportMode == TransportMode.BLE) { onTransportMode(TransportMode.BLE) }
                }
                DropdownField(
                    label = if (state.transportMode == TransportMode.USB) "USB Device" else "BLE Device",
                    options = state.devices,
                    selectedId = state.selectedDeviceId,
                    onSelect = onSelectDevice,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    SelectionField(
                        modifier = Modifier.weight(1f),
                        label = "Baud",
                        value = state.baudRate.toString(),
                        options = listOf("9600", "57600", "115200", "230400", "460800", "921600"),
                        onSelect = onBaudRateChanged,
                    )
                    SelectionField(
                        modifier = Modifier.weight(1f),
                        label = "Protocol",
                        value = state.protocolFormat.wireName,
                        options = ProtocolFormat.entries.map { it.wireName },
                        onSelect = { selected ->
                            ProtocolFormat.entries.firstOrNull { it.wireName == selected }?.let(onProtocolChanged)
                        },
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedButton(
                        onClick = onRefresh,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(if (state.transportMode == TransportMode.USB) "Refresh Devices" else "Scan Devices")
                    }
                    Button(
                        onClick = onToggleConnection,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(if (state.connected) "Disconnect" else "Connect")
                    }
                }
            }
        }

        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Tips", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (state.transportMode == TransportMode.USB) {
                        "FlipRSDR uses the Flipper's user CDC channel. The app now prefers the later CDC port automatically when multiple USB serial ports are exposed."
                    } else {
                        "BLE scanning needs Bluetooth permissions, and the app will first try Nordic UART-style serial characteristics before falling back to generic write/notify pairs."
                    },
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun ControlPage(
    state: ReceiverUiState,
    onStartScan: () -> Unit,
    onStopScan: () -> Unit,
    onFrequencyChanged: (String) -> Unit,
    onSendFrequency: () -> Unit,
    onRssiChanged: (String) -> Unit,
    onSendRssi: () -> Unit,
) {
    Column(
        modifier = Modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Remote Commands", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(onClick = onStartScan, enabled = state.connected, modifier = Modifier.weight(1f)) {
                        Text("Start Scan")
                    }
                    OutlinedButton(onClick = onStopScan, enabled = state.connected, modifier = Modifier.weight(1f)) {
                        Text("Stop Scan")
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        modifier = Modifier.weight(1f),
                        value = state.remoteFrequency,
                        onValueChange = onFrequencyChanged,
                        label = { Text("Frequency MHz") },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    )
                    Button(
                        onClick = onSendFrequency,
                        enabled = state.connected,
                        modifier = Modifier.align(Alignment.CenterVertically),
                    ) {
                        Text("Send")
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        modifier = Modifier.weight(1f),
                        value = state.remoteRssiThreshold,
                        onValueChange = onRssiChanged,
                        label = { Text("RSSI Min / Off") },
                        singleLine = true,
                    )
                    Button(
                        onClick = onSendRssi,
                        enabled = state.connected,
                        modifier = Modifier.align(Alignment.CenterVertically),
                    ) {
                        Text("Send")
                    }
                }
                Text(state.commandStatus, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }

        StatsCard(state)
    }
}

@Composable
private fun MonitorPage(
    state: ReceiverUiState,
    onViewMode: (ReceiverViewMode) -> Unit,
    onWindowChanged: (Int) -> Unit,
    onAudioChanged: (Boolean) -> Unit,
    onToggleRecording: () -> Unit,
    onClearWaterfall: () -> Unit,
) {
    Column(
        modifier = Modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Monitor", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    ToggleChoice("Waveform", state.viewMode == ReceiverViewMode.WAVEFORM) { onViewMode(ReceiverViewMode.WAVEFORM) }
                    ToggleChoice("Waterfall", state.viewMode == ReceiverViewMode.WATERFALL) { onViewMode(ReceiverViewMode.WATERFALL) }
                }
                SelectionField(
                    modifier = Modifier.fillMaxWidth(),
                    label = "Waterfall Window",
                    value = "${state.waterfallWindowSeconds}s",
                    options = listOf("10s", "20s", "30s", "60s", "120s"),
                    onSelect = { selected -> selected.removeSuffix("s").toIntOrNull()?.let(onWindowChanged) },
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text("Audio")
                        Switch(checked = state.audioEnabled, onCheckedChange = onAudioChanged)
                    }
                    Text(
                        if (state.recordingEnabled) "Recording On" else "Recording Off",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Button(onClick = onToggleRecording, modifier = Modifier.weight(1f)) {
                        Text(if (state.recordingEnabled) "Stop Recording" else "Start Recording")
                    }
                    OutlinedButton(onClick = onClearWaterfall, modifier = Modifier.weight(1f)) {
                        Text("Clear Waterfall")
                    }
                }
                Text(state.recordingPath, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }

        PlotCard(state)
        StatsCard(state)
    }
}

@Composable
private fun LogsPage(logs: List<String>) {
    Column(
        modifier = Modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        LogCard(logs)
    }
}

@Composable
private fun StatsCard(state: ReceiverUiState) {
    val burst = state.currentBurst
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Burst Stats", style = MaterialTheme.typography.titleMedium)
            StatRow("Session", burst?.session?.toString() ?: "-")
            StatRow("Burst", burst?.burst?.toString() ?: "-")
            StatRow("Frequency", burst?.frequencyHz?.takeIf { it > 0 }?.let { "%.3f MHz".format(it / 1_000_000.0) } ?: "-")
            StatRow("Timings", burst?.timingCount?.toString() ?: "0")
            StatRow("Duration", burst?.let { "%.3f ms".format(it.totalDurationUs / 1000.0) } ?: "0 ms")
            StatRow("RSSI", burst?.rssi?.let { "%.1f dBm".format(it) } ?: "-")
            StatRow("Truncated", if (burst?.truncated == true) "Yes" else "No")
        }
    }
}

@Composable
private fun PlotCard(state: ReceiverUiState) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(if (state.viewMode == ReceiverViewMode.WAVEFORM) "Live Waveform" else "Live Waterfall", style = MaterialTheme.typography.titleMedium)
            if (state.viewMode == ReceiverViewMode.WAVEFORM) {
                WaveformChart(state.currentBurst, Modifier.fillMaxWidth().height(280.dp))
            } else {
                WaterfallChart(state.waterfallRows, Modifier.fillMaxWidth().height(280.dp))
            }
        }
    }
}

@Composable
private fun LogCard(logs: List<String>) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("Receiver Log", style = MaterialTheme.typography.titleMedium)
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(420.dp),
                color = Color(0xFF091017),
                shape = RoundedCornerShape(12.dp),
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    if (logs.isEmpty()) {
                        Text("No receiver traffic yet.", color = Color(0xFF9FB3C8))
                    } else {
                        logs.forEach { line ->
                            Text(
                                text = line,
                                color = Color(0xFFD6DEE8),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 12.sp,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun WaveformChart(burst: BurstData?, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        color = Color(0xFF05070A),
        shape = RoundedCornerShape(16.dp),
    ) {
        if (burst == null || burst.timings.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Waiting for burst data", color = Color(0xFF8CA2BB))
            }
            return@Surface
        }

        Canvas(modifier = Modifier.fillMaxSize().padding(12.dp)) {
            val path = Path()
            val totalUs = max(1, burst.totalDurationUs)
            var elapsedUs = 0f
            var level = if (burst.firstLevel) 1f else 0f
            val width = size.width
            val height = size.height
            val highY = height * 0.15f
            val lowY = height * 0.85f

            repeat(5) { index ->
                val y = height * (index / 4f)
                drawLine(Color(0xFF172433), Offset(0f, y), Offset(width, y), 1f)
            }

            path.moveTo(0f, if (level > 0.5f) highY else lowY)
            burst.timings.forEach { timing ->
                elapsedUs += timing.toFloat()
                val x = (elapsedUs / totalUs) * width
                path.lineTo(x, if (level > 0.5f) highY else lowY)
                level = if (level > 0.5f) 0f else 1f
                path.lineTo(x, if (level > 0.5f) highY else lowY)
            }
            drawPath(path = path, color = Color(0xFF5DD4FF), style = Stroke(width = 3f))
        }
    }
}

@Composable
private fun WaterfallChart(rows: List<FloatArray>, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        color = Color(0xFF05070A),
        shape = RoundedCornerShape(16.dp),
    ) {
        if (rows.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Waiting for waterfall data", color = Color(0xFF8CA2BB))
            }
            return@Surface
        }

        Canvas(modifier = Modifier.fillMaxSize().padding(8.dp)) {
            val rowCount = rows.size
            val columnCount = rows.firstOrNull()?.size ?: 0
            if (columnCount == 0) {
                return@Canvas
            }
            val rowHeight = size.height / rowCount.toFloat()
            val cellWidth = size.width / columnCount.toFloat()
            rows.forEachIndexed { rowIndex, row ->
                row.forEachIndexed { columnIndex, intensity ->
                    if (intensity <= 0f) {
                        return@forEachIndexed
                    }
                    drawRect(
                        color = waterfallColor(intensity),
                        topLeft = Offset(columnIndex * cellWidth, rowIndex * rowHeight),
                        size = Size(cellWidth + 0.5f, rowHeight + 0.5f),
                    )
                }
            }
        }
    }
}

@Composable
private fun ToggleChoice(label: String, active: Boolean, onClick: () -> Unit) {
    if (active) {
        Button(onClick = onClick) { Text(label) }
    } else {
        OutlinedButton(onClick = onClick) { Text(label) }
    }
}

@Composable
private fun DropdownField(
    label: String,
    options: List<TransportDevice>,
    selectedId: String?,
    onSelect: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    val selected = options.firstOrNull { it.id == selectedId }
    Box {
        OutlinedButton(onClick = { expanded = true }, modifier = Modifier.fillMaxWidth()) {
            Column(horizontalAlignment = Alignment.Start, modifier = Modifier.fillMaxWidth()) {
                Text(label)
                Text(selected?.title ?: "Choose device", color = MaterialTheme.colorScheme.onSurface)
                if (!selected?.subtitle.isNullOrBlank()) {
                    Text(selected?.subtitle.orEmpty(), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { option ->
                DropdownMenuItem(
                    text = {
                        Column {
                            Text(option.title)
                            if (option.subtitle.isNotBlank()) {
                                Text(option.subtitle, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    },
                    onClick = {
                        expanded = false
                        onSelect(option.id)
                    },
                )
            }
        }
    }
}

@Composable
private fun SelectionField(
    modifier: Modifier = Modifier,
    label: String,
    value: String,
    options: List<String>,
    onSelect: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Box(modifier = modifier) {
        OutlinedButton(onClick = { expanded = true }, modifier = Modifier.fillMaxWidth()) {
            Column(horizontalAlignment = Alignment.Start, modifier = Modifier.fillMaxWidth()) {
                Text(label)
                Text(value)
            }
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { option ->
                DropdownMenuItem(
                    text = { Text(option) },
                    onClick = {
                        expanded = false
                        onSelect(option)
                    },
                )
            }
        }
    }
}

@Composable
private fun StatRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value)
    }
}

@Composable
private fun androidReceiverColors() = androidx.compose.material3.darkColorScheme(
    primary = Color(0xFF59D8FF),
    onPrimary = Color(0xFF031018),
    secondary = Color(0xFF80F0BB),
    onSecondary = Color(0xFF05180F),
    background = Color(0xFF071018),
    surface = Color(0xFF0C1822),
    surfaceVariant = Color(0xFF122432),
    onSurface = Color(0xFFEAF4FC),
    onSurfaceVariant = Color(0xFF9FB7C8),
)

private fun waterfallColor(intensity: Float): Color {
    val value = intensity.coerceIn(0f, 1f)
    return when {
        value < 0.12f -> lerp(Color(0xFF00000A), Color(0xFF002056), value / 0.12f)
        value < 0.26f -> lerp(Color(0xFF002056), Color(0xFF0070B9), (value - 0.12f) / 0.14f)
        value < 0.48f -> lerp(Color(0xFF0070B9), Color(0xFF00C796), (value - 0.26f) / 0.22f)
        value < 0.68f -> lerp(Color(0xFF00C796), Color(0xFFE8DE3F), (value - 0.48f) / 0.20f)
        value < 0.82f -> lerp(Color(0xFFE8DE3F), Color(0xFFFF621E), (value - 0.68f) / 0.14f)
        else -> lerp(Color(0xFFFF621E), Color.White, (value - 0.82f) / 0.18f)
    }
}
