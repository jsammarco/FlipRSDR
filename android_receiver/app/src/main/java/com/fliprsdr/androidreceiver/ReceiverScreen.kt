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

@Composable
fun AndroidReceiverScreen(viewModel: ReceiverViewModel) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()

    MaterialTheme(
        colorScheme = androidReceiverColors(),
    ) {
        Scaffold { padding ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.background)
                    .verticalScroll(rememberScrollState())
                    .padding(padding)
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                HeaderCard()
                TransportCard(
                    state = state,
                    onTransportMode = viewModel::setTransportMode,
                    onRefresh = viewModel::refreshDevices,
                    onSelectDevice = viewModel::setSelectedDevice,
                    onBaudRateChanged = viewModel::setBaudRate,
                    onProtocolChanged = viewModel::setProtocolFormat,
                    onToggleConnection = viewModel::toggleConnection,
                )
                RemoteCard(
                    state = state,
                    onStartScan = viewModel::sendStartScan,
                    onStopScan = viewModel::sendStopScan,
                    onFrequencyChanged = viewModel::setRemoteFrequency,
                    onSendFrequency = viewModel::sendFrequency,
                    onRssiChanged = viewModel::setRemoteRssiThreshold,
                    onSendRssi = viewModel::sendRssiThreshold,
                )
                ViewOptionsCard(
                    state = state,
                    onViewMode = viewModel::setViewMode,
                    onWindowChanged = viewModel::setWaterfallWindowSeconds,
                    onAudioChanged = viewModel::setAudioEnabled,
                    onToggleRecording = viewModel::toggleRecording,
                    onClearWaterfall = viewModel::clearWaterfall,
                )
                StatsCard(state)
                PlotCard(state)
                LogCard(state.logs)
            }
        }
    }
}

@Composable
private fun HeaderCard() {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text("Android Receiver", style = MaterialTheme.typography.headlineSmall)
            Text(
                "USB serial and BLE serial companion for FlipRSDR with live waveform, waterfall, commands, recording, and audio playback.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun TransportCard(
    state: ReceiverUiState,
    onTransportMode: (TransportMode) -> Unit,
    onRefresh: () -> Unit,
    onSelectDevice: (String) -> Unit,
    onBaudRateChanged: (String) -> Unit,
    onProtocolChanged: (ProtocolFormat) -> Unit,
    onToggleConnection: () -> Unit,
) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Connection", style = MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ToggleChoice(
                    label = "USB",
                    active = state.transportMode == TransportMode.USB,
                    onClick = { onTransportMode(TransportMode.USB) },
                )
                ToggleChoice(
                    label = "BLE",
                    active = state.transportMode == TransportMode.BLE,
                    onClick = { onTransportMode(TransportMode.BLE) },
                )
                Spacer(Modifier.weight(1f))
                OutlinedButton(onClick = onRefresh) {
                    Text(if (state.transportMode == TransportMode.USB) "Refresh" else "Scan")
                }
                Button(onClick = onToggleConnection) {
                    Text(if (state.connected) "Disconnect" else "Connect")
                }
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
            Text(
                state.connectionStatus,
                color = if (state.connected) Color(0xFF7CFC98) else Color(0xFFFFA46A),
            )
        }
    }
}

@Composable
private fun RemoteCard(
    state: ReceiverUiState,
    onStartScan: () -> Unit,
    onStopScan: () -> Unit,
    onFrequencyChanged: (String) -> Unit,
    onSendFrequency: () -> Unit,
    onRssiChanged: (String) -> Unit,
    onSendRssi: () -> Unit,
) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Remote Commands", style = MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onStartScan, enabled = state.connected) {
                    Text("Start Scan")
                }
                OutlinedButton(onClick = onStopScan, enabled = state.connected) {
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
                Button(onClick = onSendFrequency, enabled = state.connected, modifier = Modifier.align(Alignment.CenterVertically)) {
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
                Button(onClick = onSendRssi, enabled = state.connected, modifier = Modifier.align(Alignment.CenterVertically)) {
                    Text("Send")
                }
            }
            Text(state.commandStatus, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun ViewOptionsCard(
    state: ReceiverUiState,
    onViewMode: (ReceiverViewMode) -> Unit,
    onWindowChanged: (Int) -> Unit,
    onAudioChanged: (Boolean) -> Unit,
    onToggleRecording: () -> Unit,
    onClearWaterfall: () -> Unit,
) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("View + Capture", style = MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ToggleChoice("Waveform", state.viewMode == ReceiverViewMode.WAVEFORM) { onViewMode(ReceiverViewMode.WAVEFORM) }
                ToggleChoice("Waterfall", state.viewMode == ReceiverViewMode.WATERFALL) { onViewMode(ReceiverViewMode.WATERFALL) }
                Spacer(Modifier.width(8.dp))
                SelectionField(
                    modifier = Modifier.width(110.dp),
                    label = "Window",
                    value = "${state.waterfallWindowSeconds}s",
                    options = listOf("10s", "20s", "30s", "60s", "120s"),
                    onSelect = { selected -> selected.removeSuffix("s").toIntOrNull()?.let(onWindowChanged) },
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Audio")
                Switch(checked = state.audioEnabled, onCheckedChange = onAudioChanged)
                Spacer(Modifier.width(8.dp))
                Button(onClick = onToggleRecording) {
                    Text(if (state.recordingEnabled) "Stop Recording" else "Start Recording")
                }
                OutlinedButton(onClick = onClearWaterfall) {
                    Text("Clear Waterfall")
                }
            }
            Text(
                if (state.recordingEnabled) state.recordingPath else "Recording Off",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun StatsCard(state: ReceiverUiState) {
    val burst = state.currentBurst
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Stats", style = MaterialTheme.typography.titleMedium)
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
            Text(if (state.viewMode == ReceiverViewMode.WAVEFORM) "Waveform" else "Waterfall", style = MaterialTheme.typography.titleMedium)
            if (state.viewMode == ReceiverViewMode.WAVEFORM) {
                WaveformChart(state.currentBurst, Modifier.fillMaxWidth().height(260.dp))
            } else {
                WaterfallChart(state.waterfallRows, Modifier.fillMaxWidth().height(260.dp))
            }
        }
    }
}

@Composable
private fun LogCard(logs: List<String>) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("Logs", style = MaterialTheme.typography.titleMedium)
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(220.dp),
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
    primary = Color(0xFF66D5FF),
    onPrimary = Color(0xFF00141B),
    secondary = Color(0xFF8CF0C3),
    onSecondary = Color(0xFF032014),
    background = Color(0xFF071018),
    surface = Color(0xFF0D1822),
    surfaceVariant = Color(0xFF122130),
    onSurface = Color(0xFFECF4FA),
    onSurfaceVariant = Color(0xFF9FB3C8),
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
