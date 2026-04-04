package com.fliprsdr.androidreceiver

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.coroutines.resume
import kotlin.math.max
import kotlin.math.min

private const val USB_RECIP_INTERFACE = 0x01

class UsbSerialTransport(
    private val appContext: Context,
) : ReceiverTransport {
    private val usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val eventFlow = MutableSharedFlow<TransportEvent>(extraBufferCapacity = 64)
    private val stateFlow = MutableStateFlow(TransportSnapshot(mode = TransportMode.USB))
    private var activeSession: UsbSession? = null

    override val snapshot: StateFlow<TransportSnapshot> = stateFlow.asStateFlow()
    override val events: SharedFlow<TransportEvent> = eventFlow.asSharedFlow()

    override suspend fun refreshDevices() {
        val devices = usbManager.deviceList.values
            .sortedBy { it.deviceName }
            .filter { hasDataEndpoints(it) }
            .map { device ->
                TransportDevice(
                    id = device.deviceId.toString(),
                    title = device.productName ?: "USB ${device.deviceId}",
                    subtitle = listOfNotNull(
                        device.manufacturerName,
                        "VID ${device.vendorId}:PID ${device.productId}",
                    ).joinToString(" | "),
                )
            }
        stateFlow.value = stateFlow.value.copy(
            devices = devices,
            statusText = if (stateFlow.value.connected) stateFlow.value.statusText else "Found ${devices.size} USB serial device(s)",
        )
    }

    override suspend fun connect(deviceId: String, baudRate: Int): Boolean {
        disconnect()
        val device = usbManager.deviceList.values.firstOrNull { it.deviceId.toString() == deviceId }
            ?: run {
                eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB device not found"))
                return false
            }

        if (!usbManager.hasPermission(device) && !requestPermission(device)) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB permission denied"))
            return false
        }

        val connection = usbManager.openDevice(device)
        if (connection == null) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "Unable to open USB device"))
            return false
        }

        val session = UsbSession(device, connection, baudRate)
        if (!session.open()) {
            connection.close()
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "Unable to configure USB CDC serial"))
            return false
        }

        activeSession = session
        stateFlow.value = stateFlow.value.copy(
            connectedDeviceId = deviceId,
            connected = true,
            statusText = "Connected to ${device.productName ?: device.deviceName}",
        )
        eventFlow.emit(TransportEvent.Status(TransportMode.USB, stateFlow.value.statusText))
        session.startReader(scope, eventFlow)
        return true
    }

    override suspend fun disconnect() {
        activeSession?.close()
        activeSession = null
        stateFlow.value = stateFlow.value.copy(
            connected = false,
            connectedDeviceId = null,
            statusText = "Disconnected",
        )
        eventFlow.emit(TransportEvent.Status(TransportMode.USB, "Disconnected"))
    }

    override suspend fun send(data: ByteArray): Boolean = activeSession?.write(data) ?: false

    private suspend fun requestPermission(device: UsbDevice): Boolean {
        val action = "${appContext.packageName}.USB_PERMISSION"
        val intent = PendingIntent.getBroadcast(
            appContext,
            device.deviceId,
            Intent(action),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

        return suspendCancellableCoroutine { continuation ->
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intentData: Intent?) {
                    if (intentData?.action != action) {
                        return
                    }
                    runCatching { appContext.unregisterReceiver(this) }
                    val granted = intentData.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    if (continuation.isActive) {
                        continuation.resume(granted)
                    }
                }
            }

            val filter = IntentFilter(action)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                appContext.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                appContext.registerReceiver(receiver, filter)
            }
            continuation.invokeOnCancellation {
                runCatching { appContext.unregisterReceiver(receiver) }
            }
            usbManager.requestPermission(device, intent)
        }
    }

    private fun hasDataEndpoints(device: UsbDevice): Boolean {
        repeat(device.interfaceCount) { interfaceIndex ->
            val iface = device.getInterface(interfaceIndex)
            var hasIn = false
            var hasOut = false
            repeat(iface.endpointCount) { endpointIndex ->
                val endpoint = iface.getEndpoint(endpointIndex)
                if (endpoint.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                    if (endpoint.direction == UsbConstants.USB_DIR_IN) hasIn = true
                    if (endpoint.direction == UsbConstants.USB_DIR_OUT) hasOut = true
                }
            }
            if (hasIn && hasOut) {
                return true
            }
        }
        return false
    }

    private inner class UsbSession(
        private val device: UsbDevice,
        private val connection: UsbDeviceConnection,
        private val baudRate: Int,
    ) {
        private var controlInterface: UsbInterface? = null
        private var dataInterface: UsbInterface? = null
        private var inputEndpoint: UsbEndpoint? = null
        private var outputEndpoint: UsbEndpoint? = null
        private var readerJob: Job? = null
        @Volatile
        private var open = false

        fun open(): Boolean {
            controlInterface = findControlInterface(device)
            dataInterface = findDataInterface(device)
            val data = dataInterface ?: return false
            inputEndpoint = findEndpoint(data, UsbConstants.USB_DIR_IN)
            outputEndpoint = findEndpoint(data, UsbConstants.USB_DIR_OUT)
            if (inputEndpoint == null || outputEndpoint == null) {
                return false
            }

            controlInterface?.let {
                if (!connection.claimInterface(it, true)) return false
            }
            if (!connection.claimInterface(data, true)) return false

            val lineCoding = ByteBuffer.allocate(7)
                .order(ByteOrder.LITTLE_ENDIAN)
                .putInt(baudRate)
                .put(0)
                .put(0)
                .put(8)
                .array()

            val controlId = controlInterface?.id ?: 0
            connection.controlTransfer(
                UsbConstants.USB_DIR_OUT or UsbConstants.USB_TYPE_CLASS or USB_RECIP_INTERFACE,
                0x20,
                0,
                controlId,
                lineCoding,
                lineCoding.size,
                1_000,
            )
            connection.controlTransfer(
                UsbConstants.USB_DIR_OUT or UsbConstants.USB_TYPE_CLASS or USB_RECIP_INTERFACE,
                0x22,
                0x03,
                controlId,
                null,
                0,
                1_000,
            )
            open = true
            return true
        }

        fun startReader(scope: CoroutineScope, flow: MutableSharedFlow<TransportEvent>) {
            val endpoint = inputEndpoint ?: return
            readerJob = scope.launch {
                val buffer = ByteArray(max(64, endpoint.maxPacketSize))
                while (open) {
                    val count = connection.bulkTransfer(endpoint, buffer, buffer.size, 250)
                    if (count > 0) {
                        flow.emit(TransportEvent.Bytes(TransportMode.USB, buffer.copyOf(count)))
                    } else if (count < 0 && open) {
                        flow.emit(TransportEvent.Warning(TransportMode.USB, "USB read stalled"))
                        delay(50)
                    }
                }
            }
        }

        fun write(data: ByteArray): Boolean {
            val endpoint = outputEndpoint ?: return false
            var offset = 0
            while (offset < data.size && open) {
                val chunkSize = min(data.size - offset, max(16, endpoint.maxPacketSize))
                val chunk = data.copyOfRange(offset, offset + chunkSize)
                val count = connection.bulkTransfer(endpoint, chunk, chunk.size, 1_000)
                if (count <= 0) {
                    return false
                }
                offset += count
            }
            return offset == data.size
        }

        fun close() {
            open = false
            readerJob?.cancel()
            dataInterface?.let { runCatching { connection.releaseInterface(it) } }
            controlInterface?.let { runCatching { connection.releaseInterface(it) } }
            connection.close()
        }
    }

    private fun findControlInterface(device: UsbDevice): UsbInterface? =
        (0 until device.interfaceCount)
            .map { device.getInterface(it) }
            .firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_COMM }

    private fun findDataInterface(device: UsbDevice): UsbInterface? =
        (0 until device.interfaceCount)
            .map { device.getInterface(it) }
            .firstOrNull { iface ->
                var hasIn = false
                var hasOut = false
                repeat(iface.endpointCount) { endpointIndex ->
                    val endpoint = iface.getEndpoint(endpointIndex)
                    if (endpoint.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                        if (endpoint.direction == UsbConstants.USB_DIR_IN) hasIn = true
                        if (endpoint.direction == UsbConstants.USB_DIR_OUT) hasOut = true
                    }
                }
                hasIn && hasOut
            }

    private fun findEndpoint(iface: UsbInterface, direction: Int): UsbEndpoint? =
        (0 until iface.endpointCount)
            .map { iface.getEndpoint(it) }
            .firstOrNull { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK && it.direction == direction }
}
