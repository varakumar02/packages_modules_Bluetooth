/*
 * Copyright (C) 2026 LunarisOS
 */

package com.oplus.bluetooth;

import android.bluetooth.BluetoothDevice;

oneway interface IOplusA2dpSharingStateCallback {
    void onSharingStarted(int role, int type, int reason, in BluetoothDevice primary, in BluetoothDevice secondary);
    void onSharingStopped(int role, int type, int reason, in BluetoothDevice primary, in BluetoothDevice secondary);
}
