package com.fliprsdr.androidreceiver

import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

data class TransportSnapshot(
    val mode: TransportMode,
    val devices: List<TransportDevice> = emptyList(),
    val connectedDeviceId: String? = null,
    val connected: Boolean = false,
    val statusText: String = "Disconnected",
)

sealed interface TransportEvent {
    val mode: TransportMode

    data class Status(override val mode: TransportMode, val text: String) : TransportEvent
    data class Warning(override val mode: TransportMode, val text: String) : TransportEvent
    data class Bytes(override val mode: TransportMode, val payload: ByteArray) : TransportEvent
}

interface ReceiverTransport {
    val snapshot: StateFlow<TransportSnapshot>
    val events: SharedFlow<TransportEvent>
    suspend fun refreshDevices()
    suspend fun connect(deviceId: String, baudRate: Int = 9_600): Boolean
    suspend fun disconnect()
    suspend fun send(data: ByteArray): Boolean
}
