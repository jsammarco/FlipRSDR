package com.fliprsdr.androidreceiver

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

class UsbSerialTransport(
    private val appContext: Context,
) : ReceiverTransport {
    private val usbManager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
    private val eventFlow = MutableSharedFlow<TransportEvent>(extraBufferCapacity = 128)
    private val stateFlow = MutableStateFlow(TransportSnapshot(mode = TransportMode.USB))
    private var activeSession: UsbSession? = null

    override val snapshot: StateFlow<TransportSnapshot> = stateFlow.asStateFlow()
    override val events: SharedFlow<TransportEvent> = eventFlow.asSharedFlow()

    override suspend fun refreshDevices() {
        val devices = enumeratePorts()
        stateFlow.value = stateFlow.value.copy(
            devices = devices.map { it.device },
            statusText = if (stateFlow.value.connected) stateFlow.value.statusText else "Found ${devices.size} USB serial port(s)",
        )
    }

    override suspend fun connect(deviceId: String, baudRate: Int): Boolean {
        disconnect()
        val target = enumeratePorts().firstOrNull { it.device.id == deviceId }
            ?: run {
                eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB serial port not found"))
                return false
            }

        if (!usbManager.hasPermission(target.driver.device) && !requestPermission(target.driver.device)) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB permission denied"))
            return false
        }

        val connection = usbManager.openDevice(target.driver.device)
        if (connection == null) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "Unable to open USB device"))
            return false
        }

        return try {
            val port = target.driver.ports[target.portIndex]
            port.open(connection)
            port.setParameters(baudRate, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            runCatching { port.dtr = true }
            runCatching { port.rts = true }

            val session = UsbSession(
                connection = connection,
                port = port,
                portLabel = target.device.title,
            )
            session.start()
            activeSession = session
            stateFlow.value = stateFlow.value.copy(
                connectedDeviceId = target.device.id,
                connected = true,
                statusText = "Connected to ${target.device.title}",
            )
            eventFlow.emit(TransportEvent.Status(TransportMode.USB, stateFlow.value.statusText))
            true
        } catch (error: Exception) {
            runCatching { connection.close() }
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB open failed: ${error.message}"))
            false
        }
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

    override suspend fun send(data: ByteArray): Boolean {
        val session = activeSession ?: return false
        return try {
            session.port.write(data, WRITE_TIMEOUT_MS)
            true
        } catch (error: Exception) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.USB, "USB write failed: ${error.message}"))
            false
        }
    }

    private fun enumeratePorts(): List<DetectedUsbPort> {
        val prober = UsbSerialProber.getDefaultProber()
        return prober.findAllDrivers(usbManager)
            .sortedBy { it.device.deviceName }
            .flatMap { driver ->
                driver.ports.mapIndexed { index, _ ->
                    val device = driver.device
                    val multiplePorts = driver.ports.size > 1
                    val title = buildString {
                        append(device.productName ?: "USB ${device.deviceId}")
                        if (multiplePorts) {
                            append(" Port ")
                            append(index + 1)
                        }
                    }
                    val subtitle = listOfNotNull(
                        device.manufacturerName,
                        "VID ${device.vendorId}:PID ${device.productId}",
                        "Driver ${driver.javaClass.simpleName}",
                    ).joinToString(" | ")
                    DetectedUsbPort(
                        driver = driver,
                        portIndex = index,
                        device = TransportDevice(
                            id = "${device.deviceId}:$index",
                            title = title,
                            subtitle = subtitle,
                        ),
                    )
                }
            }
    }

    private suspend fun requestPermission(device: android.hardware.usb.UsbDevice): Boolean {
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

    private inner class UsbSession(
        val connection: android.hardware.usb.UsbDeviceConnection,
        val port: UsbSerialPort,
        val portLabel: String,
    ) : SerialInputOutputManager.Listener {
        private val ioManager = SerialInputOutputManager(port, this)

        fun start() {
            ioManager.start()
        }

        fun close() {
            ioManager.stop()
            runCatching { port.dtr = false }
            runCatching { port.rts = false }
            runCatching { port.close() }
            runCatching { connection.close() }
        }

        override fun onNewData(data: ByteArray) {
            eventFlow.tryEmit(TransportEvent.Bytes(TransportMode.USB, data))
        }

        override fun onRunError(error: Exception) {
            if (activeSession === this) {
                eventFlow.tryEmit(
                    TransportEvent.Warning(
                        TransportMode.USB,
                        "USB serial error on $portLabel: ${error.message ?: error.javaClass.simpleName}",
                    ),
                )
            }
        }
    }

    private data class DetectedUsbPort(
        val driver: UsbSerialDriver,
        val portIndex: Int,
        val device: TransportDevice,
    )

    companion object {
        private const val WRITE_TIMEOUT_MS = 1_000
    }
}
