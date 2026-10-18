/*
 * Copyright (C) 2026 LunarisOS
 */

package com.oplus.bluetooth;

import android.bluetooth.BluetoothDevice;

oneway interface IOplusA2dpSharingVolumeCallback {
    void onDeviceVolumeChanged(in BluetoothDevice device, int volume);
}
