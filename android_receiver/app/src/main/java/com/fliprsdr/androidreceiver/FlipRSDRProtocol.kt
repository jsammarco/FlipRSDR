package com.fliprsdr.androidreceiver

import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.sqrt

enum class ProtocolFormat(val wireName: String) {
    FLIPRSDR("fliprsdr"),
    JSON("json"),
}

enum class TransportMode {
    USB,
    BLE,
}

enum class ReceiverViewMode {
    WAVEFORM,
    WATERFALL,
}

data class TransportDevice(
    val id: String,
    val title: String,
    val subtitle: String = "",
)

data class BurstData(
    val session: Int,
    val burst: Int,
    var frequencyHz: Int = 0,
    var timestampMs: Long? = null,
    var firstLevel: Boolean = true,
    val timings: MutableList<Int> = mutableListOf(),
    var count: Int = 0,
    var rssi: Float? = null,
    var truncated: Boolean = false,
    var overflow: Boolean = false,
    var complete: Boolean = false,
) {
    val timingCount: Int
        get() = if (count > 0) count else timings.size

    val totalDurationUs: Int
        get() = timings.sum()

    fun copyMutable(): BurstData = copy(timings = timings.toMutableList())
}

sealed interface FlipRSDRMessage {
    val session: Int
    val burst: Int
}

data class BurstStartMessage(
    override val session: Int,
    override val burst: Int,
    val frequencyHz: Int,
    val timestampMs: Long?,
    val firstLevel: Boolean,
) : FlipRSDRMessage

data class TimingChunkMessage(
    override val session: Int,
    override val burst: Int,
    val timings: List<Int>,
) : FlipRSDRMessage

data class BurstEndMessage(
    override val session: Int,
    override val burst: Int,
    val count: Int,
    val rssi: Float?,
    val truncated: Boolean,
    val overflow: Boolean,
) : FlipRSDRMessage

data class BurstCaptureMessage(
    override val session: Int,
    override val burst: Int,
    val frequencyHz: Int,
    val timestampMs: Long?,
    val firstLevel: Boolean,
    val timings: List<Int>,
    val count: Int,
    val rssi: Float?,
    val truncated: Boolean,
    val overflow: Boolean,
) : FlipRSDRMessage

private const val PROTOCOL_VERSION = 0x01
private const val HEADER_SIZE = 8
private const val PACKET_BURST_START = 0x01
private const val PACKET_TIMING_CHUNK = 0x02
private const val PACKET_BURST_END = 0x03
private const val PACKET_BURST_CAPTURE = 0x04

private const val FLAG_TIMESTAMP = 1 shl 0
private const val FLAG_RSSI = 1 shl 1
private const val FLAG_TRUNCATED = 1 shl 2
private const val FLAG_OVERFLOW = 1 shl 3

private const val WATERFALL_BINS = 512
private const val WATERFALL_MIN_US = 16.0
private const val WATERFALL_MAX_US = 65536.0
const val WATERFALL_MAX_RENDER_ROWS = 512

fun formatMessageLog(message: FlipRSDRMessage): String = when (message) {
    is TimingChunkMessage -> "[fliprsdr] timing_chunk s=${message.session} b=${message.burst} timings=${message.timings.size}"
    is BurstEndMessage -> "[fliprsdr] burst_end s=${message.session} b=${message.burst} count=${message.count}"
    is BurstCaptureMessage -> "[fliprsdr] burst_capture s=${message.session} b=${message.burst} count=${message.count}"
    is BurstStartMessage -> "[fliprsdr] burst_start s=${message.session} b=${message.burst}"
}

class BinaryStreamDecoder {
    private val buffer = ByteArrayOutputStream()

    fun feed(data: ByteArray): Pair<List<FlipRSDRMessage>, List<String>> {
        buffer.write(data)
        val source = buffer.toByteArray()
        val messages = mutableListOf<FlipRSDRMessage>()
        val warnings = mutableListOf<String>()
        var frameStart = 0

        for (index in source.indices) {
            if (source[index] != 0.toByte()) {
                continue
            }
            if (index == frameStart) {
                frameStart = index + 1
                continue
            }
            val frame = source.copyOfRange(frameStart, index)
            try {
                val decoded = cobsDecode(frame)
                messages += parsePacket(decoded)
            } catch (error: IllegalArgumentException) {
                warnings += error.message.orEmpty()
            }
            frameStart = index + 1
        }

        buffer.reset()
        if (frameStart < source.size) {
            buffer.write(source, frameStart, source.size - frameStart)
        }

        return messages to warnings
    }
}

fun parseJsonMessage(line: String): FlipRSDRMessage? {
    val start = line.indexOf('{')
    val end = line.lastIndexOf('}')
    if (start < 0 || end <= start) {
        return null
    }

    val json = JSONObject(line.substring(start, end + 1))
    val type = json.optString("type")
    return when (type) {
        "burst_start" -> BurstStartMessage(
            session = json.optInt("session"),
            burst = json.optInt("burst"),
            frequencyHz = json.optInt("freq"),
            timestampMs = json.optLong("timestamp").takeIf { json.has("timestamp") },
            firstLevel = json.optInt("first_level", 1) != 0,
        )
        "timing_chunk" -> TimingChunkMessage(
            session = json.optInt("session"),
            burst = json.optInt("burst"),
            timings = json.optJSONArray("timings").toIntList(),
        )
        "burst_end" -> BurstEndMessage(
            session = json.optInt("session"),
            burst = json.optInt("burst"),
            count = json.optInt("count"),
            rssi = json.optDouble("rssi").toFloat().takeIf { json.has("rssi") },
            truncated = json.optBoolean("truncated"),
            overflow = json.optBoolean("overflow"),
        )
        "burst_capture" -> BurstCaptureMessage(
            session = json.optInt("session"),
            burst = json.optInt("burst"),
            frequencyHz = json.optInt("freq"),
            timestampMs = json.optLong("timestamp").takeIf { json.has("timestamp") },
            firstLevel = json.optInt("first_level", 1) != 0,
            timings = json.optJSONArray("timings").toIntList(),
            count = json.optInt("count"),
            rssi = json.optDouble("rssi").toFloat().takeIf { json.has("rssi") },
            truncated = json.optBoolean("truncated"),
            overflow = json.optBoolean("overflow"),
        )
        else -> null
    }
}

fun encodeRecordingBurst(burst: BurstData): ByteArray {
    val fullCapture = !burst.truncated && !burst.overflow && burst.timings.size == burst.timingCount
    return if (fullCapture) {
        encodeBurstCapturePacket(burst, 0)
    } else {
        val output = ByteArrayOutputStream()
        output.write(encodeBurstStartPacket(burst, 0))
        var sequence = 1
        burst.timings.chunked(128).forEach { chunk ->
            output.write(encodeTimingChunkPacket(burst.session, burst.burst, chunk, sequence))
            sequence += 1
        }
        output.write(encodeBurstEndPacket(burst, sequence))
        output.toByteArray()
    }
}

fun buildWaterfallRow(burst: BurstData): FloatArray {
    if (burst.timings.isEmpty()) {
        return FloatArray(WATERFALL_BINS)
    }

    val row = FloatArray(WATERFALL_BINS)
    val logMin = log2(WATERFALL_MIN_US)
    val logMax = log2(WATERFALL_MAX_US)
    val range = max(0.0001, logMax - logMin)

    burst.timings.forEach { duration ->
        val clipped = duration.toDouble().coerceIn(WATERFALL_MIN_US, WATERFALL_MAX_US)
        val ratio = (log2(clipped) - logMin) / range
        val index = ((ratio * (WATERFALL_BINS - 1)).toInt()).coerceIn(0, WATERFALL_BINS - 1)
        row[index] += 1f
    }

    val peak = row.maxOrNull() ?: 0f
    if (peak > 0f) {
        row.indices.forEach { index ->
            row[index] = sqrt(row[index] / peak)
        }
    }
    return row
}

fun burstToPcm16(burst: BurstData, sampleRate: Int = 22_050): ByteArray {
    if (burst.timings.isEmpty()) {
        return ByteArray(0)
    }

    val output = ByteArrayOutputStream()
    var level = if (burst.firstLevel) 0.75 else -0.75
    burst.timings.forEach { durationUs ->
        val sampleCount = max(1, ((durationUs / 1_000_000.0) * sampleRate).toInt())
        repeat(sampleCount) {
            val sample = (level * 32767.0).toInt().coerceIn(-32767, 32767).toShort()
            output.write(sample.toInt() and 0xFF)
            output.write((sample.toInt() shr 8) and 0xFF)
        }
        level = -level
    }
    return output.toByteArray()
}

private fun encodeBurstStartPacket(burst: BurstData, sequence: Int): ByteArray {
    var flags = 0
    val payload = ByteArrayOutputStream()
    payload.write(leShort(burst.session))
    payload.write(leShort(burst.burst))
    payload.write(leInt(burst.frequencyHz))
    payload.write(if (burst.firstLevel) 1 else 0)
    payload.write(0)
    burst.timestampMs?.let {
        flags = flags or FLAG_TIMESTAMP
        payload.write(leInt((it * 1000L).toInt()))
    }
    return encodePacket(PACKET_BURST_START, flags, payload.toByteArray(), sequence)
}

private fun encodeTimingChunkPacket(session: Int, burst: Int, timings: List<Int>, sequence: Int): ByteArray {
    val payload = ByteArrayOutputStream()
    payload.write(leShort(session))
    payload.write(leShort(burst))
    payload.write(leShort(timings.size))
    timings.forEach { payload.write(encodeUVarInt(it)) }
    return encodePacket(PACKET_TIMING_CHUNK, 0, payload.toByteArray(), sequence)
}

private fun encodeBurstEndPacket(burst: BurstData, sequence: Int): ByteArray {
    var flags = 0
    if (burst.truncated) flags = flags or FLAG_TRUNCATED
    if (burst.overflow) flags = flags or FLAG_OVERFLOW
    val payload = ByteArrayOutputStream()
    payload.write(leShort(burst.session))
    payload.write(leShort(burst.burst))
    payload.write(leShort(burst.timingCount))
    burst.rssi?.let {
        flags = flags or FLAG_RSSI
        payload.write(leShort((it * 100f).toInt()))
    }
    return encodePacket(PACKET_BURST_END, flags, payload.toByteArray(), sequence)
}

private fun encodeBurstCapturePacket(burst: BurstData, sequence: Int): ByteArray {
    var flags = 0
    if (burst.truncated) flags = flags or FLAG_TRUNCATED
    if (burst.overflow) flags = flags or FLAG_OVERFLOW
    val payload = ByteArrayOutputStream()
    payload.write(leShort(burst.session))
    payload.write(leShort(burst.burst))
    payload.write(leInt(burst.frequencyHz))
    payload.write(if (burst.firstLevel) 1 else 0)
    payload.write(0)
    payload.write(leShort(burst.timingCount))
    burst.timestampMs?.let {
        flags = flags or FLAG_TIMESTAMP
        payload.write(leInt((it * 1000L).toInt()))
    }
    burst.rssi?.let {
        flags = flags or FLAG_RSSI
        payload.write(leShort((it * 100f).toInt()))
    }
    burst.timings.forEach { payload.write(encodeUVarInt(it)) }
    return encodePacket(PACKET_BURST_CAPTURE, flags, payload.toByteArray(), sequence)
}

private fun encodePacket(packetType: Int, flags: Int, payload: ByteArray, sequence: Int): ByteArray {
    val header = ByteBuffer.allocate(HEADER_SIZE)
        .order(ByteOrder.LITTLE_ENDIAN)
        .put(PROTOCOL_VERSION.toByte())
        .put(packetType.toByte())
        .put(flags.toByte())
        .put(HEADER_SIZE.toByte())
        .putShort(payload.size.toShort())
        .putShort(sequence.toShort())
        .array()
    val body = header + payload
    val crc = crc16Xmodem(body)
    val packet = body + leShort(crc)
    return cobsEncode(packet) + byteArrayOf(0)
}

private fun parsePacket(packet: ByteArray): FlipRSDRMessage {
    require(packet.size >= HEADER_SIZE + 2) { "Packet too short" }
    val header = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
    val version = header.get().toInt() and 0xFF
    require(version == PROTOCOL_VERSION) { "Unsupported protocol version: $version" }
    val packetType = header.get().toInt() and 0xFF
    val flags = header.get().toInt() and 0xFF
    val headerSize = header.get().toInt() and 0xFF
    require(headerSize == HEADER_SIZE) { "Unsupported header size: $headerSize" }
    val payloadLength = header.short.toInt() and 0xFFFF
    header.short
    require(packet.size == HEADER_SIZE + payloadLength + 2) { "Packet length does not match header" }
    val crcOffset = packet.size - 2
    val expectedCrc = ByteBuffer.wrap(packet, crcOffset, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() and 0xFFFF
    val actualCrc = crc16Xmodem(packet.copyOfRange(0, crcOffset))
    require(actualCrc == expectedCrc) { "CRC check failed" }
    val payload = packet.copyOfRange(HEADER_SIZE, crcOffset)

    return when (packetType) {
        PACKET_BURST_START -> {
            require(payload.size == 10 || payload.size == 14) { "Invalid BURST_START payload length" }
            val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val session = buffer.short.toInt() and 0xFFFF
            val burst = buffer.short.toInt() and 0xFFFF
            val frequency = buffer.int
            val firstLevel = buffer.get().toInt() != 0
            buffer.get()
            val timestamp = if ((flags and FLAG_TIMESTAMP) != 0) {
                require(buffer.remaining() >= 4) { "Missing BURST_START timestamp" }
                buffer.int.toLong() / 1000L
            } else {
                null
            }
            BurstStartMessage(session, burst, frequency, timestamp, firstLevel)
        }
        PACKET_TIMING_CHUNK -> {
            require(payload.size >= 6) { "Invalid TIMING_CHUNK payload length" }
            val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val session = buffer.short.toInt() and 0xFFFF
            val burst = buffer.short.toInt() and 0xFFFF
            val count = buffer.short.toInt() and 0xFFFF
            var offset = 6
            val timings = ArrayList<Int>(count)
            repeat(count) {
                val decoded = decodeUVarInt(payload, offset)
                timings += decoded.first
                offset = decoded.second
            }
            require(offset == payload.size) { "Unexpected trailing bytes in TIMING_CHUNK" }
            TimingChunkMessage(session, burst, timings)
        }
        PACKET_BURST_END -> {
            require(payload.size == 6 || payload.size == 8) { "Invalid BURST_END payload length" }
            val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val session = buffer.short.toInt() and 0xFFFF
            val burst = buffer.short.toInt() and 0xFFFF
            val count = buffer.short.toInt() and 0xFFFF
            val rssi = if ((flags and FLAG_RSSI) != 0) {
                require(buffer.remaining() >= 2) { "Missing BURST_END RSSI" }
                buffer.short.toInt() / 100f
            } else {
                null
            }
            BurstEndMessage(
                session = session,
                burst = burst,
                count = count,
                rssi = rssi,
                truncated = (flags and FLAG_TRUNCATED) != 0,
                overflow = (flags and FLAG_OVERFLOW) != 0,
            )
        }
        PACKET_BURST_CAPTURE -> {
            require(payload.size >= 12) { "Invalid BURST_CAPTURE payload length" }
            val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val session = buffer.short.toInt() and 0xFFFF
            val burst = buffer.short.toInt() and 0xFFFF
            val frequency = buffer.int
            val firstLevel = buffer.get().toInt() != 0
            buffer.get()
            val count = buffer.short.toInt() and 0xFFFF
            var offset = 12
            val timestamp = if ((flags and FLAG_TIMESTAMP) != 0) {
                require(offset + 4 <= payload.size) { "Missing BURST_CAPTURE timestamp" }
                val value = ByteBuffer.wrap(payload, offset, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() / 1000L
                offset += 4
                value
            } else {
                null
            }
            val rssi = if ((flags and FLAG_RSSI) != 0) {
                require(offset + 2 <= payload.size) { "Missing BURST_CAPTURE RSSI" }
                val value = ByteBuffer.wrap(payload, offset, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() / 100f
                offset += 2
                value
            } else {
                null
            }
            val timings = mutableListOf<Int>()
            while (offset < payload.size) {
                val decoded = decodeUVarInt(payload, offset)
                timings += decoded.first
                offset = decoded.second
            }
            BurstCaptureMessage(
                session = session,
                burst = burst,
                frequencyHz = frequency,
                timestampMs = timestamp,
                firstLevel = firstLevel,
                timings = timings,
                count = count,
                rssi = rssi,
                truncated = (flags and FLAG_TRUNCATED) != 0,
                overflow = (flags and FLAG_OVERFLOW) != 0,
            )
        }
        else -> error("Unsupported packet type: $packetType")
    }
}

private fun cobsEncode(data: ByteArray): ByteArray {
    val output = ByteArrayOutputStream()
    val current = ArrayList<Byte>(data.size + 8)
    var codeIndex = 0
    var code = 1
    current += 0.toByte()
    data.forEach { byte ->
        val value = byte.toInt() and 0xFF
        if (value == 0) {
            current[codeIndex] = code.toByte()
            codeIndex = current.size
            current += 0.toByte()
            code = 1
        } else {
            current += byte
            code += 1
            if (code == 0xFF) {
                current[codeIndex] = code.toByte()
                codeIndex = current.size
                current += 0.toByte()
                code = 1
            }
        }
    }
    current[codeIndex] = code.toByte()
    current.forEach { output.write(it.toInt()) }
    return output.toByteArray()
}

private fun cobsDecode(data: ByteArray): ByteArray {
    val output = ByteArrayOutputStream()
    var index = 0
    while (index < data.size) {
        val code = data[index].toInt() and 0xFF
        require(code != 0) { "COBS decode failed: zero byte inside frame" }
        index += 1
        val end = index + code - 1
        require(end <= data.size) { "COBS decode failed: truncated code block" }
        output.write(data, index, code - 1)
        index = end
        if (code != 0xFF && index < data.size) {
            output.write(0)
        }
    }
    return output.toByteArray()
}

private fun encodeUVarInt(value: Int): ByteArray {
    require(value >= 0) { "Varint value must be non-negative" }
    val output = ByteArrayOutputStream()
    var current = value
    while (current >= 0x80) {
        output.write((current and 0x7F) or 0x80)
        current = current ushr 7
    }
    output.write(current and 0x7F)
    return output.toByteArray()
}

private fun decodeUVarInt(data: ByteArray, offset: Int): Pair<Int, Int> {
    var value = 0
    var shift = 0
    var index = offset
    while (index < data.size && shift < 35) {
        val current = data[index].toInt() and 0xFF
        index += 1
        value = value or ((current and 0x7F) shl shift)
        if ((current and 0x80) == 0) {
            return value to index
        }
        shift += 7
    }
    throw IllegalArgumentException("Invalid varint in packet payload")
}

private fun crc16Xmodem(data: ByteArray): Int {
    var crc = 0
    data.forEach { byte ->
        crc = crc xor ((byte.toInt() and 0xFF) shl 8)
        repeat(8) {
            crc = if ((crc and 0x8000) != 0) {
                ((crc shl 1) xor 0x1021) and 0xFFFF
            } else {
                (crc shl 1) and 0xFFFF
            }
        }
    }
    return crc
}

private fun leShort(value: Int): ByteArray =
    ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN).putShort(value.toShort()).array()

private fun leInt(value: Int): ByteArray =
    ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()

private fun JSONArray?.toIntList(): List<Int> {
    if (this == null) {
        return emptyList()
    }
    return List(length()) { index -> optInt(index) }
}

private fun log2(value: Double): Double = ln(value) / ln(2.0)
