package com.fliprsdr.androidreceiver

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.util.UUID
import kotlin.math.max
import kotlin.math.min

class BleSerialTransport(
    private val appContext: Context,
) : ReceiverTransport {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val bluetoothManager = appContext.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter: BluetoothAdapter? = bluetoothManager.adapter
    private val eventFlow = MutableSharedFlow<TransportEvent>(extraBufferCapacity = 64)
    private val stateFlow = MutableStateFlow(TransportSnapshot(mode = TransportMode.BLE))
    private val scannedDevices = linkedMapOf<String, TransportDevice>()
    private var gatt: BluetoothGatt? = null
    private var writeCharacteristic: BluetoothGattCharacteristic? = null
    private var notifyCharacteristic: BluetoothGattCharacteristic? = null
    private var mtu = 20
    private var connectDeferred: CompletableDeferred<Boolean>? = null

    override val snapshot: StateFlow<TransportSnapshot> = stateFlow.asStateFlow()
    override val events: SharedFlow<TransportEvent> = eventFlow.asSharedFlow()

    @SuppressLint("MissingPermission")
    override suspend fun refreshDevices() {
        if (!hasBlePermissions()) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.BLE, "BLE permissions are missing"))
            return
        }
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.BLE, "Bluetooth LE scanner unavailable"))
            return
        }

        scannedDevices.clear()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device ?: return
                val title = device.name ?: "BLE ${device.address}"
                scannedDevices[device.address] = TransportDevice(
                    id = device.address,
                    title = title,
                    subtitle = result.scanRecord?.deviceName ?: device.address,
                )
                stateFlow.value = stateFlow.value.copy(
                    devices = scannedDevices.values.toList(),
                    statusText = "Scanning BLE devices...",
                )
            }
        }

        scanner.startScan(callback)
        stateFlow.value = stateFlow.value.copy(statusText = "Scanning BLE devices...")
        delay(4_000)
        scanner.stopScan(callback)
        val devices = scannedDevices.values.sortedBy { it.title.lowercase() }
        stateFlow.value = stateFlow.value.copy(
            devices = devices,
            statusText = if (stateFlow.value.connected) stateFlow.value.statusText else "Found ${devices.size} BLE device(s)",
        )
    }

    @SuppressLint("MissingPermission")
    override suspend fun connect(deviceId: String, baudRate: Int): Boolean {
        disconnect()
        if (!hasBlePermissions()) {
            eventFlow.emit(TransportEvent.Warning(TransportMode.BLE, "BLE permissions are missing"))
            return false
        }
        val device = adapter?.getRemoteDevice(deviceId)
            ?: run {
                eventFlow.emit(TransportEvent.Warning(TransportMode.BLE, "BLE device not found"))
                return false
            }
        val deferred = CompletableDeferred<Boolean>()
        connectDeferred = deferred
        gatt = device.connectGatt(appContext, false, callback, BluetoothDevice.TRANSPORT_LE)
        stateFlow.value = stateFlow.value.copy(statusText = "Connecting to ${device.name ?: device.address}...")
        return deferred.await()
    }

    @SuppressLint("MissingPermission")
    override suspend fun disconnect() {
        connectDeferred?.complete(false)
        connectDeferred = null
        writeCharacteristic = null
        notifyCharacteristic = null
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        stateFlow.value = stateFlow.value.copy(
            connected = false,
            connectedDeviceId = null,
            statusText = "Disconnected",
        )
        eventFlow.emit(TransportEvent.Status(TransportMode.BLE, "Disconnected"))
    }

    @SuppressLint("MissingPermission")
    override suspend fun send(data: ByteArray): Boolean {
        if (!hasBlePermissions()) {
            return false
        }
        val currentGatt = gatt ?: return false
        val characteristic = writeCharacteristic ?: return false
        val packetSize = max(20, mtu - 3)
        var offset = 0
        while (offset < data.size) {
            val end = min(offset + packetSize, data.size)
            val chunk = data.copyOfRange(offset, end)
            val success = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                currentGatt.writeCharacteristic(
                    characteristic,
                    chunk,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    characteristic.value = chunk
                    currentGatt.writeCharacteristic(characteristic)
                }
            }
            if (!success) {
                return false
            }
            offset = end
            delay(12)
        }
        return true
    }

    @SuppressLint("MissingPermission")
    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    this@BleSerialTransport.gatt = gatt
                    stateFlow.value = stateFlow.value.copy(
                        connectedDeviceId = gatt.device.address,
                        statusText = "Discovering services...",
                    )
                    gatt.requestMtu(247)
                    gatt.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    stateFlow.value = stateFlow.value.copy(
                        connected = false,
                        connectedDeviceId = null,
                        statusText = "Disconnected",
                    )
                    connectDeferred?.complete(false)
                    connectDeferred = null
                    scope.launch {
                        eventFlow.emit(TransportEvent.Status(TransportMode.BLE, "Disconnected"))
                    }
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                this@BleSerialTransport.mtu = mtu
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val pair = findSerialCharacteristics(gatt.services)
            if (pair == null) {
                connectDeferred?.complete(false)
                connectDeferred = null
                scope.launch {
                    eventFlow.emit(TransportEvent.Warning(TransportMode.BLE, "No serial write/notify characteristics found"))
                }
                return
            }

            writeCharacteristic = pair.first
            notifyCharacteristic = pair.second
            gatt.setCharacteristicNotification(pair.second, true)
            pair.second.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID)?.let { descriptor ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    run {
                        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        gatt.writeDescriptor(descriptor)
                    }
                }
            }
            stateFlow.value = stateFlow.value.copy(
                connected = true,
                connectedDeviceId = gatt.device.address,
                statusText = "Connected to ${gatt.device.name ?: gatt.device.address}",
            )
            connectDeferred?.complete(true)
            connectDeferred = null
            scope.launch {
                eventFlow.emit(TransportEvent.Status(TransportMode.BLE, stateFlow.value.statusText))
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            scope.launch {
                eventFlow.emit(TransportEvent.Bytes(TransportMode.BLE, value))
            }
        }

        @Deprecated("Deprecated on older APIs")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val value = characteristic.value ?: return
            scope.launch {
                eventFlow.emit(TransportEvent.Bytes(TransportMode.BLE, value))
            }
        }
    }

    private fun findSerialCharacteristics(services: List<BluetoothGattService>): Pair<BluetoothGattCharacteristic, BluetoothGattCharacteristic>? {
        services.firstOrNull { it.uuid == NUS_SERVICE_UUID }?.let { service ->
            val write = service.getCharacteristic(NUS_RX_UUID)
            val notify = service.getCharacteristic(NUS_TX_UUID)
            if (write != null && notify != null) {
                return write to notify
            }
        }

        services.forEach { service ->
            val writable = service.characteristics.firstOrNull { characteristic ->
                val props = characteristic.properties
                props and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ||
                    props and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0
            }
            val notify = service.characteristics.firstOrNull { characteristic ->
                val props = characteristic.properties
                props and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 ||
                    props and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0
            }
            if (writable != null && notify != null) {
                return writable to notify
            }
        }
        return null
    }

    private fun hasBlePermissions(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return true
        }
        return appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
            appContext.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }

    companion object {
        private val CLIENT_CHARACTERISTIC_CONFIG_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val NUS_SERVICE_UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        private val NUS_RX_UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        private val NUS_TX_UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
    }
}
