/*
 * Copyright (C) 2026 LunarisOS
 * Reverse-Engineered OPlus A2DP Sharing Native Interface
 */

package com.oplus.bluetooth.feature.a2dpshare;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.util.Log;

public class OplusA2dpSharingNativeInterface {
    private static final String TAG = "OplusA2dpSharingNativeInterface";
    private static final Object INSTANCE_LOCK = new Object();
    private static volatile OplusA2dpSharingNativeInterface sInstance;
    private static volatile boolean sNativeLoaded = false;

    private Callback mCallback;

    public interface Callback {
        void onSharingDeviceVolumeChanged(BluetoothDevice device, int volume);
        void onSharingStateChanged(int state, BluetoothDevice primary, BluetoothDevice secondary);
    }

    private static native void classInitNative();
    private native boolean initNative();
    private native void cleanupNative();
    private native boolean enterSharingNative(byte[] address);
    private native boolean exitSharingNative();

    private volatile boolean mInitDone = false;

    static {
        ensureNativeLoaded();
    }

    public static synchronized boolean ensureNativeLoaded() {
        if (sNativeLoaded) return true;
        try {
            System.loadLibrary("bluetooth_jni");
        } catch (Throwable t) {
            Log.w(TAG, "System.loadLibrary(bluetooth_jni) deferred or failed: " + t.getMessage());
        }
        try {
            classInitNative();
            sNativeLoaded = true;
            Log.i(TAG, "Native library bluetooth_jni and classInitNative initialized successfully");
            return true;
        } catch (Throwable t) {
            sNativeLoaded = false;
            Log.w(TAG, "classInitNative failed (will retry): " + t.getMessage());
            return false;
        }
    }

    private OplusA2dpSharingNativeInterface() {}

    public static OplusA2dpSharingNativeInterface getInstance() {
        if (sInstance == null) {
            synchronized (INSTANCE_LOCK) {
                if (sInstance == null) {
                    sInstance = new OplusA2dpSharingNativeInterface();
                }
            }
        }
        return sInstance;
    }

    public synchronized boolean ensureInitialized(Callback callback) {
        if (callback != null) {
            mCallback = callback;
        }
        if (mInitDone) {
            return true;
        }
        if (!ensureNativeLoaded()) {
            Log.w(TAG, "ensureInitialized: native library not ready yet");
            return false;
        }
        try {
            mInitDone = initNative();
            if (mInitDone) {
                Log.i(TAG, "initNative completed successfully");
            } else {
                Log.w(TAG, "initNative returned false (Fluoride stack or sharing interface not ready yet)");
            }
        } catch (Throwable t) {
            Log.e(TAG, "initNative threw exception: " + t.getMessage(), t);
            mInitDone = false;
        }
        return mInitDone;
    }

    public void init(Callback callback) {
        ensureInitialized(callback);
    }

    public synchronized void cleanup() {
        if (sNativeLoaded && mInitDone) {
            try {
                cleanupNative();
            } catch (Throwable t) {
                Log.w(TAG, "cleanupNative failed: " + t.getMessage(), t);
            }
        }
        mInitDone = false;
        mCallback = null;
    }

    public boolean enterSharing(BluetoothDevice device) {
        if (device == null) {
            Log.e(TAG, "enterSharing: device is null");
            return false;
        }
        if (!ensureInitialized(mCallback)) {
            Log.e(TAG, "enterSharing: native interface not initialized");
            return false;
        }
        byte[] addressBytes = getByteAddress(device.getAddress());
        Log.i(TAG, "enterSharing: device=" + device.getAddress());
        try {
            return enterSharingNative(addressBytes);
        } catch (Throwable t) {
            Log.e(TAG, "enterSharingNative failed", t);
            return false;
        }
    }

    public boolean exitSharing() {
        Log.i(TAG, "exitSharing");
        if (!sNativeLoaded || !mInitDone) {
            Log.w(TAG, "exitSharing: native interface not active or not loaded");
            return false;
        }
        try {
            return exitSharingNative();
        } catch (Throwable t) {
            Log.e(TAG, "exitSharingNative failed", t);
            return false;
        }
    }

    private static byte[] getByteAddress(String address) {
        if (address == null) {
            return new byte[6];
        }
        String[] parts = address.split(":");
        byte[] bytes = new byte[parts.length];
        for (int i = 0; i < parts.length; i++) {
            bytes[i] = (byte) Integer.parseInt(parts[i], 16);
        }
        return bytes;
    }

    // Called by JNI
    public void onSharingDeviceVolumeChanged(String address, int volume) {
        Log.d(TAG, "onSharingDeviceVolumeChanged: " + address + ", volume: " + volume);
        if (mCallback != null && address != null) {
            try {
                BluetoothDevice device = BluetoothAdapter.getDefaultAdapter().getRemoteDevice(address.toUpperCase(java.util.Locale.ROOT));
                mCallback.onSharingDeviceVolumeChanged(device, volume);
            } catch (Exception e) {
                Log.e(TAG, "Error resolving remote device for volume change", e);
            }
        }
    }

    // Called by JNI
    public void onSharingStateChanged(int state, String primaryAddress, String secondaryAddress) {
        Log.d(TAG, "onSharingStateChanged: state=" + state + " primary=" + primaryAddress + " secondary=" + secondaryAddress);
        if (mCallback != null) {
            BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
            BluetoothDevice primary = null;
            BluetoothDevice secondary = null;
            if (adapter != null) {
                if (primaryAddress != null && !primaryAddress.isEmpty() && !primaryAddress.equals("00:00:00:00:00:00")) {
                    try {
                        primary = adapter.getRemoteDevice(primaryAddress.toUpperCase(java.util.Locale.ROOT));
                    } catch (Exception e) {
                        Log.w(TAG, "Failed to resolve primary device: " + primaryAddress);
                    }
                }
                if (secondaryAddress != null && !secondaryAddress.isEmpty() && !secondaryAddress.equals("00:00:00:00:00:00")) {
                    try {
                        secondary = adapter.getRemoteDevice(secondaryAddress.toUpperCase(java.util.Locale.ROOT));
                    } catch (Exception e) {
                        Log.w(TAG, "Failed to resolve secondary device: " + secondaryAddress);
                    }
                }
            }
            try {
                mCallback.onSharingStateChanged(state, primary, secondary);
            } catch (Exception e) {
                Log.e(TAG, "Error forwarding sharing state change to callback", e);
            }
        }
    }
}
