/*
 * Copyright (C) 2026 LunarisOS
 */

package com.oplus.bluetooth;

import android.bluetooth.BluetoothDevice;
import android.content.AttributionSource;
import com.oplus.bluetooth.IOplusA2dpSharingStateCallback;
import com.oplus.bluetooth.IOplusA2dpSharingVolumeCallback;
import java.util.List;

interface IOplusA2dpSharing {
    boolean isA2dpSharingEnabled(in AttributionSource attributionSource);
    boolean setA2dpSharingEnabled(boolean enabled, in AttributionSource attributionSource);
    boolean isA2dpSharingAvailable(in AttributionSource attributionSource);
    boolean inSharingMode(in AttributionSource attributionSource);
    int getSharingType(in AttributionSource attributionSource);
    int getDeviceSharingRole(in BluetoothDevice device, in AttributionSource attributionSource);
    List<BluetoothDevice> getSharingDevices(in AttributionSource attributionSource);
    boolean startSharing(in BluetoothDevice device, int sharingType, in AttributionSource attributionSource);
    boolean stopSharing(in AttributionSource attributionSource);
    boolean setSharingDeviceAbsoluteVolume(in BluetoothDevice device, int volume, in AttributionSource attributionSource);
    boolean registerStateCallback(in IOplusA2dpSharingStateCallback callback, in AttributionSource attributionSource);
    boolean unregisterStateCallback(in IOplusA2dpSharingStateCallback callback, in AttributionSource attributionSource);
    boolean registerVolumeCallback(in IOplusA2dpSharingVolumeCallback callback, in AttributionSource attributionSource);
    boolean unregisterVolumeCallback(in IOplusA2dpSharingVolumeCallback callback, in AttributionSource attributionSource);
}
