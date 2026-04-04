package com.fliprsdr.androidreceiver

import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect

class MainActivity : ComponentActivity() {
    private val viewModel by viewModels<ReceiverViewModel>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val launcher = rememberLauncherForActivityResult(
                contract = ActivityResultContracts.RequestMultiplePermissions(),
            ) { }

            LaunchedEffect(Unit) {
                val permissions = buildList {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        add(Manifest.permission.BLUETOOTH_SCAN)
                        add(Manifest.permission.BLUETOOTH_CONNECT)
                    } else {
                        add(Manifest.permission.ACCESS_FINE_LOCATION)
                    }
                }
                if (permissions.isNotEmpty()) {
                    launcher.launch(permissions.toTypedArray())
                }
            }

            AndroidReceiverScreen(viewModel = viewModel)
        }
    }
}
