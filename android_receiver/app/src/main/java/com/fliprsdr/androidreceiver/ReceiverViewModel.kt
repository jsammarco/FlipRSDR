package com.fliprsdr.androidreceiver

import android.app.Application
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Environment
import android.os.SystemClock
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.BufferedWriter
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale
import kotlin.math.max

data class ReceiverUiState(
    val transportMode: TransportMode = TransportMode.USB,
    val devices: List<TransportDevice> = emptyList(),
    val selectedDeviceId: String? = null,
    val baudRate: Int = 9_600,
    val protocolFormat: ProtocolFormat = ProtocolFormat.FLIPRSDR,
    val connected: Boolean = false,
    val connectionStatus: String = "Disconnected",
    val commandStatus: String = "Remote control idle",
    val viewMode: ReceiverViewMode = ReceiverViewMode.WATERFALL,
    val waterfallWindowSeconds: Int = 30,
    val audioEnabled: Boolean = false,
    val recordingEnabled: Boolean = false,
    val recordingPath: String = "Off",
    val currentBurst: BurstData? = null,
    val waterfallRows: List<FloatArray> = emptyList(),
    val logs: List<String> = emptyList(),
    val remoteFrequency: String = "433.920",
    val remoteRssiThreshold: String = "Off",
)

private data class WaterfallEntry(
    val timestampMs: Long,
    val row: FloatArray,
)

class ReceiverViewModel(application: Application) : AndroidViewModel(application) {
    private val usbTransport = UsbSerialTransport(application.applicationContext)
    private val bleTransport = BleSerialTransport(application.applicationContext)
    private var binaryDecoder = BinaryStreamDecoder()
    private val jsonBuffer = ByteArrayOutputStream()
    private val activeBursts = linkedMapOf<Pair<Int, Int>, BurstData>()
    private val waterfallEntries = ArrayDeque<WaterfallEntry>()
    private val stateFlow = MutableStateFlow(ReceiverUiState())
    private var recordBinaryStream: OutputStream? = null
    private var recordTextWriter: BufferedWriter? = null
    private var activeAudioTrack: AudioTrack? = null

    val uiState: StateFlow<ReceiverUiState> = stateFlow.asStateFlow()

    init {
        observeTransport(usbTransport)
        observeTransport(bleTransport)
        refreshDevices()
    }

    fun setTransportMode(mode: TransportMode) {
        viewModelScope.launch {
            if (mode == stateFlow.value.transportMode) {
                return@launch
            }
            activeTransport().disconnect()
            val snapshot = transportFor(mode).snapshot.value
            stateFlow.update {
                it.copy(
                    transportMode = mode,
                    devices = snapshot.devices,
                    selectedDeviceId = snapshot.connectedDeviceId ?: snapshot.devices.firstOrNull()?.id,
                    connected = snapshot.connected,
                    connectionStatus = snapshot.statusText,
                )
            }
            refreshDevices()
        }
    }

    fun setSelectedDevice(deviceId: String) {
        stateFlow.update { it.copy(selectedDeviceId = deviceId) }
    }

    fun setBaudRate(value: String) {
        value.toIntOrNull()?.let { baud ->
            stateFlow.update { it.copy(baudRate = baud) }
        }
    }

    fun setProtocolFormat(format: ProtocolFormat) {
        stateFlow.update { it.copy(protocolFormat = format) }
        resetDecoders()
        if (stateFlow.value.recordingEnabled) {
            stopRecording()
            startRecording()
        }
    }

    fun setViewMode(mode: ReceiverViewMode) {
        stateFlow.update { it.copy(viewMode = mode) }
    }

    fun setWaterfallWindowSeconds(seconds: Int) {
        stateFlow.update { it.copy(waterfallWindowSeconds = seconds) }
        trimWaterfall()
    }

    fun setRemoteFrequency(value: String) {
        stateFlow.update { it.copy(remoteFrequency = value) }
    }

    fun setRemoteRssiThreshold(value: String) {
        stateFlow.update { it.copy(remoteRssiThreshold = value) }
    }

    fun setAudioEnabled(enabled: Boolean) {
        stateFlow.update { it.copy(audioEnabled = enabled) }
        if (!enabled) {
            stopAudioPlayback()
        }
    }

    fun toggleRecording() {
        if (stateFlow.value.recordingEnabled) {
            stopRecording()
        } else {
            startRecording()
        }
    }

    fun clearWaterfall() {
        waterfallEntries.clear()
        stateFlow.update { it.copy(waterfallRows = emptyList()) }
    }

    fun refreshDevices() {
        viewModelScope.launch {
            activeTransport().refreshDevices()
        }
    }

    fun toggleConnection() {
        viewModelScope.launch {
            val transport = activeTransport()
            if (stateFlow.value.connected) {
                transport.disconnect()
                stopAudioPlayback()
                return@launch
            }
            val deviceId = stateFlow.value.selectedDeviceId
                ?: transport.snapshot.value.devices.firstOrNull()?.id
            if (deviceId.isNullOrBlank()) {
                appendLog("[warn] No device selected")
                return@launch
            }
            val connected = transport.connect(deviceId, stateFlow.value.baudRate)
            if (connected) {
                resetDecoders()
                stateFlow.update { it.copy(selectedDeviceId = deviceId) }
            }
        }
    }

    fun sendStartScan() = sendCommand("start_scan")

    fun sendStopScan() = sendCommand("stop_scan")

    fun sendFrequency() = sendCommand("set_frequency ${stateFlow.value.remoteFrequency.trim()}")

    fun sendRssiThreshold() = sendCommand("set_rssi_threshold ${stateFlow.value.remoteRssiThreshold.trim()}")

    private fun sendCommand(command: String) {
        viewModelScope.launch {
            if (!stateFlow.value.connected) {
                stateFlow.update { it.copy(commandStatus = "Remote control disconnected") }
                appendLog("[warn] Connect before sending commands")
                return@launch
            }
            val payload = (command.trim() + "\n").toByteArray(Charsets.UTF_8)
            val sent = activeTransport().send(payload)
            if (sent) {
                appendLog("[cmd] ${command.trim()}")
                stateFlow.update { it.copy(commandStatus = "Sent: ${command.trim()}") }
            } else {
                stateFlow.update { it.copy(commandStatus = "Failed: ${command.trim()}") }
            }
        }
    }

    private fun observeTransport(transport: ReceiverTransport) {
        viewModelScope.launch {
            transport.snapshot.collectLatest { snapshot ->
                if (snapshot.mode != stateFlow.value.transportMode) {
                    return@collectLatest
                }
                stateFlow.update {
                    it.copy(
                        devices = snapshot.devices,
                        selectedDeviceId = it.selectedDeviceId ?: snapshot.connectedDeviceId ?: snapshot.devices.firstOrNull()?.id,
                        connected = snapshot.connected,
                        connectionStatus = snapshot.statusText,
                    )
                }
            }
        }

        viewModelScope.launch {
            transport.events.collectLatest { event ->
                if (event.mode != stateFlow.value.transportMode) {
                    return@collectLatest
                }
                when (event) {
                    is TransportEvent.Status -> stateFlow.update {
                        it.copy(
                            connected = transport.snapshot.value.connected,
                            connectionStatus = event.text,
                            commandStatus = if (transport.snapshot.value.connected) it.commandStatus else "Remote control disconnected",
                        )
                    }
                    is TransportEvent.Warning -> appendLog("[warn] ${event.text}")
                    is TransportEvent.Bytes -> handleIncomingBytes(event.payload)
                }
            }
        }
    }

    private fun handleIncomingBytes(payload: ByteArray) {
        if (stateFlow.value.protocolFormat == ProtocolFormat.FLIPRSDR) {
            val (messages, warnings) = binaryDecoder.feed(payload)
            warnings.forEach { appendLog("[warn] fliprsdr parse warning: $it") }
            messages.forEach { message ->
                appendLog(formatMessageLog(message))
                handleMessage(message)
            }
            return
        }

        jsonBuffer.write(payload, 0, payload.size)
        val text = jsonBuffer.toString(Charsets.UTF_8.name())
        val parts = text.split('\n')
        jsonBuffer.reset()
        val trailing = parts.lastOrNull().orEmpty()
        parts.dropLast(1).forEach { rawLine ->
            val line = rawLine.trim()
            if (line.isEmpty()) {
                return@forEach
            }
            appendLog(line)
            runCatching { parseJsonMessage(line) }
                .onSuccess { message ->
                    if (message != null) {
                        handleMessage(message)
                    }
                }
                .onFailure { appendLog("[warn] JSON parse warning: ${it.message}") }
        }
        if (trailing.isNotEmpty()) {
            jsonBuffer.write(trailing.toByteArray(Charsets.UTF_8))
        }
    }

    private fun handleMessage(message: FlipRSDRMessage) {
        when (message) {
            is BurstStartMessage -> {
                val burst = BurstData(
                    session = message.session,
                    burst = message.burst,
                    frequencyHz = message.frequencyHz,
                    timestampMs = message.timestampMs,
                    firstLevel = message.firstLevel,
                )
                activeBursts[message.session to message.burst] = burst
                stateFlow.update { it.copy(currentBurst = burst.copyMutable()) }
            }
            is TimingChunkMessage -> {
                val burst = activeBursts.getOrPut(message.session to message.burst) {
                    BurstData(session = message.session, burst = message.burst)
                }
                burst.timings += message.timings
                burst.count = burst.timings.size
                stateFlow.update { it.copy(currentBurst = burst.copyMutable()) }
            }
            is BurstEndMessage -> {
                val burst = activeBursts.remove(message.session to message.burst)
                    ?: BurstData(session = message.session, burst = message.burst)
                burst.count = message.count
                burst.rssi = message.rssi
                burst.truncated = message.truncated
                burst.overflow = message.overflow
                burst.complete = true
                onBurstCompleted(burst)
            }
            is BurstCaptureMessage -> {
                onBurstCompleted(
                    BurstData(
                        session = message.session,
                        burst = message.burst,
                        frequencyHz = message.frequencyHz,
                        timestampMs = message.timestampMs,
                        firstLevel = message.firstLevel,
                        timings = message.timings.toMutableList(),
                        count = message.count,
                        rssi = message.rssi,
                        truncated = message.truncated,
                        overflow = message.overflow,
                        complete = true,
                    ),
                )
            }
        }
    }

    private fun onBurstCompleted(burst: BurstData) {
        stateFlow.update { it.copy(currentBurst = burst.copyMutable()) }
        appendWaterfallRow(burst)
        if (stateFlow.value.recordingEnabled) {
            writeRecordingLine(burst)
        }
        if (stateFlow.value.audioEnabled) {
            playBurstAudio(burst)
        }
    }

    private fun appendWaterfallRow(burst: BurstData) {
        waterfallEntries += WaterfallEntry(SystemClock.elapsedRealtime(), buildWaterfallRow(burst))
        trimWaterfall()
    }

    private fun trimWaterfall() {
        val cutoff = SystemClock.elapsedRealtime() - (stateFlow.value.waterfallWindowSeconds * 1_000L)
        while (waterfallEntries.isNotEmpty() && waterfallEntries.first().timestampMs < cutoff) {
            waterfallEntries.removeFirst()
        }
        while (waterfallEntries.size > WATERFALL_MAX_RENDER_ROWS) {
            waterfallEntries.removeFirst()
        }
        stateFlow.update { it.copy(waterfallRows = waterfallEntries.map { entry -> entry.row.copyOf() }) }
    }

    private fun startRecording() {
        stopRecording()
        val directory = recordingDirectory().apply { mkdirs() }
        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val extension = if (stateFlow.value.protocolFormat == ProtocolFormat.FLIPRSDR) ".fliprsdr" else ".jsonl"
        val file = File(directory, "fliprsdr_receiver_$timestamp$extension")
        if (stateFlow.value.protocolFormat == ProtocolFormat.FLIPRSDR) {
            recordBinaryStream = BufferedOutputStream(FileOutputStream(file))
        } else {
            recordTextWriter = BufferedWriter(OutputStreamWriter(FileOutputStream(file), Charsets.UTF_8))
        }
        stateFlow.update {
            it.copy(
                recordingEnabled = true,
                recordingPath = file.absolutePath,
            )
        }
    }

    private fun stopRecording() {
        runCatching { recordBinaryStream?.close() }
        runCatching { recordTextWriter?.close() }
        recordBinaryStream = null
        recordTextWriter = null
        stateFlow.update { it.copy(recordingEnabled = false, recordingPath = "Off") }
    }

    private fun writeRecordingLine(burst: BurstData) {
        runCatching {
            if (stateFlow.value.protocolFormat == ProtocolFormat.FLIPRSDR) {
                recordBinaryStream?.write(encodeRecordingBurst(burst))
                recordBinaryStream?.flush()
            } else {
                val json = JSONObject().apply {
                    put("type", "burst_capture")
                    put("session", burst.session)
                    put("burst", burst.burst)
                    put("freq", burst.frequencyHz)
                    put("first_level", if (burst.firstLevel) 1 else 0)
                    put("count", burst.timingCount)
                    put("truncated", burst.truncated)
                    put("overflow", burst.overflow)
                    burst.timestampMs?.let { put("timestamp", it) }
                    burst.rssi?.let { put("rssi", it) }
                    put("timings", JSONArray(burst.timings))
                }
                recordTextWriter?.write(json.toString())
                recordTextWriter?.newLine()
                recordTextWriter?.flush()
            }
        }.onFailure {
            appendLog("[warn] Recording write failed: ${it.message}")
        }
    }

    private fun playBurstAudio(burst: BurstData) {
        val pcm = burstToPcm16(burst)
        if (pcm.isEmpty()) {
            return
        }

        stopAudioPlayback()
        val bufferSize = max(
            AudioTrack.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            ),
            pcm.size,
        )
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(bufferSize)
            .build()
        activeAudioTrack = track

        viewModelScope.launch(Dispatchers.IO) {
            track.play()
            track.write(pcm, 0, pcm.size)
            val durationMs = ((pcm.size / 2.0) / SAMPLE_RATE.toDouble() * 1_000.0).toLong()
            kotlinx.coroutines.delay(durationMs + 100L)
            if (activeAudioTrack === track) {
                stopAudioPlayback()
            } else {
                runCatching { track.stop() }
                track.release()
            }
        }
    }

    private fun stopAudioPlayback() {
        activeAudioTrack?.let { track ->
            runCatching { track.pause() }
            runCatching { track.flush() }
            runCatching { track.stop() }
            track.release()
        }
        activeAudioTrack = null
    }

    private fun appendLog(text: String) {
        stateFlow.update { current ->
            current.copy(logs = (current.logs + text).takeLast(LOG_LIMIT))
        }
    }

    private fun activeTransport(): ReceiverTransport = transportFor(stateFlow.value.transportMode)

    private fun transportFor(mode: TransportMode): ReceiverTransport =
        if (mode == TransportMode.USB) usbTransport else bleTransport

    private fun recordingDirectory(): File {
        val externalDocs = getApplication<Application>().getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
        return File(externalDocs ?: getApplication<Application>().filesDir, "recordings")
    }

    private fun resetDecoders() {
        binaryDecoder = BinaryStreamDecoder()
        activeBursts.clear()
        jsonBuffer.reset()
        stateFlow.update { it.copy(currentBurst = null) }
    }

    override fun onCleared() {
        runBlocking {
            usbTransport.disconnect()
            bleTransport.disconnect()
        }
        stopAudioPlayback()
        stopRecording()
        super.onCleared()
    }

    companion object {
        private const val LOG_LIMIT = 600
        private const val SAMPLE_RATE = 22_050
    }
}
