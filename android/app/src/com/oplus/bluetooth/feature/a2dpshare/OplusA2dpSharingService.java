/*
 * Copyright (C) 2026 LunarisOS
 * OPlus A2DP Audio Sharing Service inside Bluetooth Process
 */

package com.oplus.bluetooth.feature.a2dpshare;

import android.app.Service;
import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothCodecConfig;
import android.bluetooth.BluetoothCodecStatus;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.AttributionSource;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioDeviceAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.provider.Settings;
import android.util.Log;

import com.android.bluetooth.a2dp.A2dpService;
import com.android.bluetooth.avrcp.AvrcpTargetService;
import com.android.bluetooth.btservice.AdapterService;
import com.oplus.bluetooth.IOplusA2dpSharing;
import com.oplus.bluetooth.IOplusA2dpSharingStateCallback;
import com.oplus.bluetooth.IOplusA2dpSharingVolumeCallback;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

@SuppressWarnings("AndroidFrameworkRequiresPermission")
public class OplusA2dpSharingService extends Service {
    private static final String TAG = "OplusA2dpSharingService";

    public static final String ACTION_A2DP_SHARING_SERVICE =
            "com.oplus.bluetooth.action.A2DP_SHARING_SERVICE";

    // OPlus Sharing Constants
    public static final int SHARING_ROLE_NONE = 0;
    public static final int SHARING_ROLE_PRIMARY = 1;
    public static final int SHARING_ROLE_SECONDARY = 2;

    public static final int SHARING_TYPE_FORMAL = 1;
    public static final int SHARING_TYPE_PROVISIONAL = 2;

    private static final long SBC_SHARING_CODEC_SPECIFIC_2 = 22L;

    private final RemoteCallbackList<IOplusA2dpSharingStateCallback> mStateCallbacks =
            new RemoteCallbackList<>();
    private final RemoteCallbackList<IOplusA2dpSharingVolumeCallback> mVolumeCallbacks =
            new RemoteCallbackList<>();

    private final Object mLock = new Object();
    private BluetoothDevice mPrimaryDevice;
    private BluetoothDevice mSecondaryDevice;
    private boolean mInSharingMode = false;
    private boolean mStarting = false;
    private boolean mMasterSwitchEnabled = false;
    private int mSharingType = 0;
    private boolean mIsPrimaryAbsTurnedOffAgainstWill = false;
    private boolean mIsSecondaryAbsTurnedOffAgainstWill = false;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private Runnable mTeardownFallbackRunnable = null;

    private static class DeviceCodecSnapshot {
        final int optionalCodecsPreference;
        final BluetoothCodecConfig codecConfig;

        DeviceCodecSnapshot(int opt, BluetoothCodecConfig cfg) {
            this.optionalCodecsPreference = opt;
            this.codecConfig = cfg;
        }
    }

    private final Map<BluetoothDevice, DeviceCodecSnapshot> mCodecSnapshots =
            new ConcurrentHashMap<>();

    private OplusA2dpSharingNativeInterface mNativeInterface;
    private boolean mReceiverRegistered = false;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (action == null) return;

            switch (action) {
                case BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    int state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
                    Log.i(TAG, "BluetoothAdapter state changed to " + state);
                    if (state == BluetoothAdapter.STATE_ON) {
                        ensureNativeInitialized();
                    } else if (state == BluetoothAdapter.STATE_TURNING_OFF) {
                        teardown("adapter_turning_off", true);
                    } else if (state == BluetoothAdapter.STATE_OFF) {
                        teardown("adapter_off", false);
                        if (mNativeInterface != null) {
                            mNativeInterface.cleanup();
                        }
                    }
                }
                case BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED -> {
                    BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                    int state = intent.getIntExtra(BluetoothProfile.EXTRA_STATE, BluetoothProfile.STATE_DISCONNECTED);
                    if (state == BluetoothProfile.STATE_CONNECTED && device != null) {
                        boolean masterOn;
                        synchronized (mLock) {
                            masterOn = mMasterSwitchEnabled;
                        }
                        if (masterOn) {
                            AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
                            if (adapterService != null) {
                                A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
                                if (a2dpService != null) {
                                    snapshotCodec(a2dpService, device);
                                    applySharingCodec(a2dpService, device);
                                }
                            }
                        }
                    } else if (state == BluetoothProfile.STATE_DISCONNECTED && device != null) {
                        boolean shouldTeardown;
                        synchronized (mLock) {
                            shouldTeardown = (mInSharingMode || mStarting)
                                    && (device.equals(mPrimaryDevice) || device.equals(mSecondaryDevice));
                            if (shouldTeardown) {
                                Log.i(TAG, "Sharing peer " + device + " disconnected, triggering teardown");
                            }
                        }
                        if (shouldTeardown) {
                            teardown("peer_disconnected", true);
                        } else {
                            mCodecSnapshots.remove(device);
                        }
                    }
                }
                case BluetoothA2dp.ACTION_ACTIVE_DEVICE_CHANGED -> {
                    BluetoothDevice activeDevice = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                    boolean shouldTeardown = false;
                    synchronized (mLock) {
                        if (mInSharingMode || mStarting) {
                            // Do not tear down if activeDevice is null (e.g. playback paused or app switched)
                            // or if activeDevice is still one of our sharing peers. Only tear down if switched
                            // to a completely different 3rd party device.
                            if (activeDevice != null && !activeDevice.equals(mPrimaryDevice) && !activeDevice.equals(mSecondaryDevice)) {
                                shouldTeardown = true;
                                Log.i(TAG, "Active A2DP device switched to 3rd party device " + activeDevice
                                        + " (primary=" + mPrimaryDevice + ", sec=" + mSecondaryDevice + "); triggering teardown");
                            }
                        } else {
                            if (activeDevice != null) {
                                mPrimaryDevice = activeDevice;
                                Log.i(TAG, "Updated prospective primary device to: " + activeDevice);
                                boolean masterOn;
                                synchronized (mLock) {
                                    masterOn = mMasterSwitchEnabled;
                                }
                                if (masterOn) {
                                    AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
                                    if (adapterService != null) {
                                        A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
                                        if (a2dpService != null) {
                                            snapshotCodec(a2dpService, activeDevice);
                                            applySharingCodec(a2dpService, activeDevice);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (shouldTeardown) {
                        teardown("active_device_changed", true);
                    }
                }
                default -> { /* ignore unhandled broadcast */ }
            }
        }
    };

    private final IOplusA2dpSharing.Stub mBinder = new IOplusA2dpSharing.Stub() {
        @Override
        public boolean isA2dpSharingEnabled(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            synchronized (mLock) {
                return mMasterSwitchEnabled;
            }
        }

        @Override
        public boolean setA2dpSharingEnabled(boolean enabled, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            Log.i(TAG, "setA2dpSharingEnabled: " + enabled);
            synchronized (mLock) {
                mMasterSwitchEnabled = enabled;
            }
            try {
                Settings.System.putInt(getContentResolver(), "oplus_bluetooth_a2dp_share", enabled ? 1 : 0);
            } catch (Throwable th) {
                Log.w(TAG, "Failed to persist oplus_bluetooth_a2dp_share setting", th);
            }
            AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
            if (adapterService == null) return true;
            A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
            if (a2dpService == null) return true;

            if (enabled) {
                List<BluetoothDevice> connectedDevices = a2dpService.getConnectedDevices();
                if (connectedDevices != null) {
                    for (BluetoothDevice dev : connectedDevices) {
                        snapshotCodec(a2dpService, dev);
                        applySharingCodec(a2dpService, dev);
                    }
                }
            } else {
                boolean inSharing;
                synchronized (mLock) {
                    inSharing = mInSharingMode || mStarting;
                }
                if (inSharing) {
                    teardown("master_switch_disabled", true);
                } else {
                    for (BluetoothDevice dev : new ArrayList<>(mCodecSnapshots.keySet())) {
                        restoreDeviceCodec(a2dpService, dev);
                        mCodecSnapshots.remove(dev);
                    }
                }
            }
            return true;
        }

        @Override
        public boolean isA2dpSharingAvailable(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (!isFeaturePropertyEnabled()) return false;
            BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
            return adapter != null && adapter.isEnabled();
        }

        @Override
        public boolean inSharingMode(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            synchronized (mLock) {
                return mInSharingMode;
            }
        }

        @Override
        public int getSharingType(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            synchronized (mLock) {
                return mSharingType;
            }
        }

        @Override
        public int getDeviceSharingRole(BluetoothDevice device, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (device == null) return SHARING_ROLE_NONE;
            synchronized (mLock) {
                if (!mInSharingMode) return SHARING_ROLE_NONE;
                if (device.equals(mPrimaryDevice)) return SHARING_ROLE_PRIMARY;
                if (device.equals(mSecondaryDevice)) return SHARING_ROLE_SECONDARY;
            }
            return SHARING_ROLE_NONE;
        }

        @Override
        public List<BluetoothDevice> getSharingDevices(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            List<BluetoothDevice> list = new ArrayList<>();
            synchronized (mLock) {
                if (!mInSharingMode) return list;
                if (mPrimaryDevice != null) list.add(mPrimaryDevice);
                if (mSecondaryDevice != null) list.add(mSecondaryDevice);
            }
            return list;
        }

        @Override
        public boolean startSharing(BluetoothDevice device, int sharingType, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            Log.i(TAG, "startSharing requested with secondary device=" + device + " type=" + sharingType);
            if (device == null) {
                Log.e(TAG, "startSharing: secondary device is null");
                return false;
            }

            // Atomic re-entrancy & transition guard
            synchronized (mLock) {
                if (mInSharingMode || mStarting) {
                    Log.w(TAG, "startSharing: sharing already active or transition in progress, rejecting");
                    return false;
                }
                mStarting = true;
            }

            try {
                if (!ensureNativeInitialized() || mNativeInterface == null) {
                    Log.e(TAG, "startSharing: native interface not available");
                    return false;
                }

                AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
                if (adapterService == null) {
                    Log.e(TAG, "startSharing: AdapterService unavailable");
                    return false;
                }

                A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
                if (a2dpService == null) {
                    Log.e(TAG, "startSharing: A2dpService unavailable");
                    return false;
                }

                if (a2dpService.getConnectionState(device) != BluetoothProfile.STATE_CONNECTED) {
                    Log.e(TAG, "startSharing: secondary device A2DP not connected");
                    return false;
                }

                BluetoothDevice primary = a2dpService.getActiveDevice();
                if (primary == null || primary.equals(device)) {
                    Log.e(TAG, "startSharing: invalid primary=" + primary + " secondary=" + device);
                    return false;
                }

                // Bind primary device immediately under lock so active device changes during
                // setup are accurately compared against this primary instead of a stale/null field
                synchronized (mLock) {
                    if (!mStarting) {
                        Log.w(TAG, "startSharing: aborted before setup began");
                        return false;
                    }
                    mPrimaryDevice = primary;
                }

                // Promote service to started state so it outlives DeviceSettings process
                try {
                    startService(new Intent(OplusA2dpSharingService.this, OplusA2dpSharingService.class));
                } catch (Exception e) {
                    Log.w(TAG, "Could not startService self for lifetime persistence", e);
                }

                // Snapshot existing codecs and optional codec preferences before modification
                snapshotCodec(a2dpService, primary);
                snapshotCodec(a2dpService, device);

                // Apply dual-A2DP compatible SBC sharing configuration to both peers
                boolean primaryApplied = applySharingCodec(a2dpService, primary);
                boolean secondaryApplied = applySharingCodec(a2dpService, device);
                if (!primaryApplied || !secondaryApplied) {
                    Log.e(TAG, "startSharing: failed to dispatch codec preferences, rolling back");
                    rollbackSharing(a2dpService, primary, device);
                    return false;
                }

                // Synchronously wait for both devices to confirm compatible SBC configuration (up to 1500ms)
                if (!waitForSbcCodec(a2dpService, primary, device, 1500)) {
                    Log.e(TAG, "startSharing: codec transition to compatible SBC timed out, rolling back");
                    rollbackSharing(a2dpService, primary, device);
                    return false;
                }

                // Re-check mStarting after 1.5s wait to ensure teardown wasn't requested concurrently
                synchronized (mLock) {
                    if (!mStarting) {
                        Log.w(TAG, "startSharing: aborted during setup wait (teardown requested)");
                        rollbackSharing(a2dpService, primary, device);
                        return false;
                    }
                }

                if (mNativeInterface == null) {
                    Log.e(TAG, "startSharing: mNativeInterface is null");
                    rollbackSharing(a2dpService, primary, device);
                    return false;
                }

                // Trigger Fluoride native sharing stream
                boolean success = mNativeInterface.enterSharing(device);
                Log.i(TAG, "Native enterSharing returned: " + success);

                if (!success) {
                    Log.e(TAG, "Native enterSharing failed, rolling back");
                    rollbackSharing(a2dpService, primary, device);
                    return false;
                }

                // Single atomic check-and-commit under the lock
                synchronized (mLock) {
                    if (!mStarting) {
                        Log.w(TAG, "startSharing: teardown occurred while enterSharing was executing");
                        rollbackSharing(a2dpService, primary, device);
                        return false;
                    }
                    mPrimaryDevice = primary;
                    mSecondaryDevice = device;
                    mSharingType = sharingType;
                    mInSharingMode = true;
                    mStarting = false;
                }

                // Dynamic Absolute Volume fallback:
                // If one device lacks Absolute Volume, force Absolute Volume OFF on the other device
                // (drops it to DEVICE_VOLUME_BEHAVIOR_VARIABLE software attenuation), preventing
                // non-AbsVol secondary devices (smartwatches, speakers) from blasting at 100% max volume.
                handleDynamicAbsVolumeStateChange(primary, device);

                // Query and broadcast initial secondary volume to UI sliders
                try {
                    AvrcpTargetService avrcp = (AvrcpTargetService) adapterService.getAvrcpTargetService().orElse(null);
                    if (avrcp != null) {
                        int curVol = avrcp.getRememberedVolumeForDevice(device);
                        int avrcpVol = (curVol >= 0) ? Math.round((float) curVol * 127.0f / 30.0f) : 100;
                        notifyVolumeChanged(device, avrcpVol);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "Failed to broadcast initial secondary volume", t);
                }

                return true;
            } finally {
                synchronized (mLock) {
                    if (!mInSharingMode) {
                        mStarting = false;
                    }
                }
            }
        }

        @Override
        public boolean stopSharing(AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            Log.i(TAG, "stopSharing called");
            teardown("user_stop", true);
            return true;
        }

        @Override
        public boolean setSharingDeviceAbsoluteVolume(BluetoothDevice device, int volume, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (device == null) return false;
            int clampedVolume = Math.max(0, Math.min(127, volume));
            Log.i(TAG, "setSharingDeviceAbsoluteVolume: dev=" + device + " vol=" + clampedVolume);

            try {
                AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
                if (adapterService != null) {
                    AvrcpTargetService avrcp = (AvrcpTargetService) adapterService.getAvrcpTargetService().orElse(null);
                    if (avrcp != null) {
                        avrcp.sendAbsVolumeChanged(device, clampedVolume);
                    }
                }
            } catch (Throwable t) {
                Log.w(TAG, "Failed to dispatch AVRCP absolute volume to " + device, t);
            }

            notifyVolumeChanged(device, clampedVolume);
            return true;
        }

        @Override
        public boolean registerStateCallback(IOplusA2dpSharingStateCallback callback, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (callback != null) {
                boolean registered = mStateCallbacks.register(callback);
                if (registered) {
                    // Send current state immediately upon registration so cold-binding main thread gets true state
                    synchronized (mLock) {
                        try {
                            if (mInSharingMode) {
                                callback.onSharingStarted(
                                        SHARING_ROLE_PRIMARY, mSharingType, 0, mPrimaryDevice, mSecondaryDevice);
                            } else {
                                callback.onSharingStopped(
                                        SHARING_ROLE_PRIMARY, mSharingType, 0, mPrimaryDevice, mSecondaryDevice);
                            }
                        } catch (RemoteException e) {
                            Log.w(TAG, "Failed to send initial state to registered callback", e);
                        }
                    }
                }
                return registered;
            }
            return false;
        }

        @Override
        public boolean unregisterStateCallback(IOplusA2dpSharingStateCallback callback, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (callback != null) {
                return mStateCallbacks.unregister(callback);
            }
            return false;
        }

        @Override
        public boolean registerVolumeCallback(IOplusA2dpSharingVolumeCallback callback, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (callback != null) {
                boolean registered = mVolumeCallbacks.register(callback);
                if (registered) {
                    synchronized (mLock) {
                        if (mInSharingMode && mSecondaryDevice != null) {
                            try {
                                AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
                                if (adapterService != null) {
                                    AvrcpTargetService avrcp = (AvrcpTargetService) adapterService.getAvrcpTargetService().orElse(null);
                                    if (avrcp != null) {
                                        int curVol = avrcp.getRememberedVolumeForDevice(mSecondaryDevice);
                                        int avrcpVol = (curVol >= 0) ? Math.round((float) curVol * 127.0f / 30.0f) : 100;
                                        callback.onDeviceVolumeChanged(mSecondaryDevice, avrcpVol);
                                    }
                                }
                            } catch (Throwable t) {
                                Log.w(TAG, "Failed to send initial volume to registered callback", t);
                            }
                        }
                    }
                }
                return registered;
            }
            return false;
        }

        @Override
        public boolean unregisterVolumeCallback(IOplusA2dpSharingVolumeCallback callback, AttributionSource attributionSource) {
            enforceBluetoothPrivileged();
            if (callback != null) {
                return mVolumeCallbacks.unregister(callback);
            }
            return false;
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            mMasterSwitchEnabled = Settings.System.getInt(getContentResolver(), "oplus_bluetooth_a2dp_share", 0) != 0;
        } catch (Throwable unused) {
            mMasterSwitchEnabled = false;
        }
        try {
            mNativeInterface = OplusA2dpSharingNativeInterface.getInstance();
            ensureNativeInitialized();
        } catch (Throwable t) {
            Log.e(TAG, "Failed initial native initialization in onCreate, will retry on STATE_ON: " + t.getMessage(), t);
        }

        try {
            IntentFilter filter = new IntentFilter();
            filter.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
            filter.addAction(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED);
            filter.addAction(BluetoothA2dp.ACTION_ACTIVE_DEVICE_CHANGED);
            registerReceiver(mReceiver, filter);
            mReceiverRegistered = true;
        } catch (Throwable t) {
            Log.e(TAG, "Failed to register broadcast receiver in onCreate", t);
        }

        Log.i(TAG, "OplusA2dpSharingService created inside Bluetooth process");
    }

    private final OplusA2dpSharingNativeInterface.Callback mNativeCallback =
            new OplusA2dpSharingNativeInterface.Callback() {
        @Override
        public void onSharingDeviceVolumeChanged(BluetoothDevice device, int volume) {
            notifyVolumeChanged(device, volume);
        }

        @Override
        public void onSharingStateChanged(int state, BluetoothDevice primary, BluetoothDevice secondary) {
            boolean active = (state == 1);
            if (active) {
                synchronized (mLock) {
                    mInSharingMode = true;
                    if (primary != null) mPrimaryDevice = primary;
                    if (secondary != null) mSecondaryDevice = secondary;
                }
                notifyStateChanged(true, primary, secondary);
            } else {
                // Native-initiated exit (e.g. BTA_AV_CLOSE_EVT) routes through teardown
                teardown("native_stopped_callback", false);
            }
        }
    };

    private synchronized boolean ensureNativeInitialized() {
        try {
            if (mNativeInterface == null) {
                mNativeInterface = OplusA2dpSharingNativeInterface.getInstance();
            }
            if (mNativeInterface != null) {
                return mNativeInterface.ensureInitialized(mNativeCallback);
            }
        } catch (Throwable t) {
            Log.e(TAG, "ensureNativeInitialized failed: " + t.getMessage(), t);
        }
        return false;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        enforceBluetoothPrivileged();
        return mBinder;
    }

    @Override
    public void onDestroy() {
        if (mReceiverRegistered) {
            unregisterReceiver(mReceiver);
            mReceiverRegistered = false;
        }
        teardown("service_destroyed", true);

        // Only cleanup native if Bluetooth adapter is off/disabled
        BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            if (mNativeInterface != null) {
                mNativeInterface.cleanup();
            }
        }
        super.onDestroy();
        Log.i(TAG, "OplusA2dpSharingService destroyed");
    }

    @SuppressWarnings("AndroidFrameworkRequiresPermission")
    @android.annotation.SuppressLint("AndroidFrameworkRequiresPermission")
    private void enforceBluetoothPrivileged() {
        enforceCallingOrSelfPermission(
                android.Manifest.permission.BLUETOOTH_PRIVILEGED,
                "Need BLUETOOTH_PRIVILEGED permission to access OplusA2dpSharingService");
    }

    private static boolean isFeaturePropertyEnabled() {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method getBoolean = sp.getMethod("getBoolean", String.class, boolean.class);
            return (Boolean) getBoolean.invoke(null, "persist.bluetooth.a2dpshare", true);
        } catch (Exception e) {
            return true;
        }
    }

    private void snapshotCodec(A2dpService a2dpService, BluetoothDevice device) {
        if (a2dpService == null || device == null) return;
        try {
            int opt = a2dpService.getOptionalCodecsEnabled(device);
            BluetoothCodecStatus status = a2dpService.getCodecStatus(device);
            BluetoothCodecConfig cfg = (status != null) ? status.getCodecConfig() : null;
            mCodecSnapshots.put(device, new DeviceCodecSnapshot(opt, cfg));
            Log.i(TAG, "snapshotCodec for " + device + ": opt=" + opt + ", cfg=" + cfg);
        } catch (Exception e) {
            Log.w(TAG, "Failed to snapshot codec for " + device, e);
        }
    }

    private boolean applySharingCodec(A2dpService a2dpService, BluetoothDevice device) {
        try {
            // Check if device is already configured with SBC sharing parameters to avoid redundant re-negotiation
            BluetoothCodecStatus status = a2dpService.getCodecStatus(device);
            if (status != null && status.getCodecConfig() != null) {
                BluetoothCodecConfig cur = status.getCodecConfig();
                if (cur.getCodecType() == BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC
                        && cur.getSampleRate() == BluetoothCodecConfig.SAMPLE_RATE_44100
                        && cur.getBitsPerSample() == BluetoothCodecConfig.BITS_PER_SAMPLE_16
                        && cur.getChannelMode() == BluetoothCodecConfig.CHANNEL_MODE_STEREO) {
                    Log.i(TAG, "applySharingCodec: " + device + " already configured with SBC sharing codec, skipping re-negotiation");
                    return true;
                }
            }

            // Disable optional codecs if previously enabled (0 = disabled)
            DeviceCodecSnapshot snapshot = mCodecSnapshots.get(device);
            if (snapshot != null && snapshot.optionalCodecsPreference == 1) {
                a2dpService.setOptionalCodecsEnabled(device, 0);
            }

            // Explicitly configure SBC parameters matching dual-A2DP sharing requirements
            BluetoothCodecConfig sbcConfig = new BluetoothCodecConfig.Builder()
                    .setCodecType(BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC)
                    .setCodecPriority(1000000) // Highest priority
                    .setSampleRate(BluetoothCodecConfig.SAMPLE_RATE_44100)
                    .setBitsPerSample(BluetoothCodecConfig.BITS_PER_SAMPLE_16)
                    .setChannelMode(BluetoothCodecConfig.CHANNEL_MODE_STEREO)
                    .setCodecSpecific2(SBC_SHARING_CODEC_SPECIFIC_2)
                    .build();
            a2dpService.setCodecConfigPreference(device, sbcConfig);
            Log.i(TAG, "applySharingCodec: enforced SBC sharing codec on " + device);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to apply sharing codec on " + device, e);
            return false;
        }
    }

    private static boolean areSbcConfigsCompatible(A2dpService a2dpService, BluetoothDevice primary, BluetoothDevice secondary) {
        if (a2dpService == null || primary == null || secondary == null) return false;
        try {
            BluetoothCodecStatus primStatus = a2dpService.getCodecStatus(primary);
            BluetoothCodecStatus secStatus = a2dpService.getCodecStatus(secondary);
            if (primStatus == null || secStatus == null) return false;
            BluetoothCodecConfig primCfg = primStatus.getCodecConfig();
            BluetoothCodecConfig secCfg = secStatus.getCodecConfig();
            if (primCfg == null || secCfg == null) return false;

            if (primCfg.getCodecType() != BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC ||
                secCfg.getCodecType() != BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC) {
                return false;
            }

            if (primCfg.getSampleRate() != secCfg.getSampleRate()) {
                Log.w(TAG, "SBC sample rate mismatch: prim=" + primCfg.getSampleRate()
                        + ", sec=" + secCfg.getSampleRate());
                return false;
            }

            if (primCfg.getChannelMode() != secCfg.getChannelMode()) {
                Log.w(TAG, "SBC channel mode mismatch: prim=" + primCfg.getChannelMode()
                        + ", sec=" + secCfg.getChannelMode());
                return false;
            }

            if (primCfg.getBitsPerSample() != secCfg.getBitsPerSample()) {
                Log.w(TAG, "SBC bits per sample mismatch: prim=" + primCfg.getBitsPerSample()
                        + ", sec=" + secCfg.getBitsPerSample());
                return false;
            }

            return true;
        } catch (Exception e) {
            Log.w(TAG, "areSbcConfigsCompatible check failed", e);
            return false;
        }
    }

    private static boolean waitForSbcCodec(A2dpService a2dpService, BluetoothDevice primary,
                                    BluetoothDevice secondary, long timeoutMs) {
        long start = System.currentTimeMillis();
        while (System.currentTimeMillis() - start < timeoutMs) {
            if (areSbcConfigsCompatible(a2dpService, primary, secondary)) {
                Log.i(TAG, "waitForSbcCodec: both primary and secondary verified compatible on SBC in "
                        + (System.currentTimeMillis() - start) + "ms");
                return true;
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        Log.w(TAG, "waitForSbcCodec: timed out waiting for compatible SBC configs");
        return false;
    }

    private void restoreDeviceCodec(A2dpService a2dpService, BluetoothDevice device) {
        if (a2dpService == null || device == null) return;
        DeviceCodecSnapshot snapshot = mCodecSnapshots.remove(device);
        if (snapshot == null) return;

        try {
            // Restore original optional codecs preference in DatabaseManager
            if (snapshot.optionalCodecsPreference != -1) {
                a2dpService.setOptionalCodecsEnabled(device, snapshot.optionalCodecsPreference);
                if (snapshot.optionalCodecsPreference == BluetoothA2dp.OPTIONAL_CODECS_PREF_ENABLED) {
                    a2dpService.enableOptionalCodecs(device);
                }
            }
            // Restore exact previous codec config
            if (snapshot.codecConfig != null) {
                a2dpService.setCodecConfigPreference(device, snapshot.codecConfig);
            }
            Log.i(TAG, "restoreDeviceCodec: restored saved codec for " + device);
        } catch (Exception e) {
            Log.e(TAG, "Failed to restore codec on " + device, e);
        }
    }

    private void rollbackSharing(A2dpService a2dpService, BluetoothDevice primary, BluetoothDevice secondary) {
        synchronized (mLock) {
            mInSharingMode = false;
            mStarting = false;
            mPrimaryDevice = null;
            mSecondaryDevice = null;
        }
        if (a2dpService != null) {
            if (primary != null) restoreDeviceCodec(a2dpService, primary);
            if (secondary != null) restoreDeviceCodec(a2dpService, secondary);
            for (BluetoothDevice dev : new ArrayList<>(mCodecSnapshots.keySet())) {
                restoreDeviceCodec(a2dpService, dev);
            }
        }
        restoreDynamicAbsVolume(primary, secondary);
        try {
            stopSelf();
        } catch (Exception e) {
            Log.d(TAG, "rollbackSharing: stopSelf skipped", e);
        }
    }

    /**
     * Unified single-owner teardown function.
     * Invoked by user stop, peer disconnects, active device transitions, and native STOPPED callbacks.
     * Restores all modified device codecs, notifies state callbacks, and avoids duplicate execution.
     */
    private void teardown(String reason, boolean callNativeExit) {
        BluetoothDevice primary;
        BluetoothDevice secondary;

        synchronized (mLock) {
            boolean wasSharing = mInSharingMode || mStarting || !mCodecSnapshots.isEmpty() || mSecondaryDevice != null;
            if (!wasSharing) {
                Log.d(TAG, "teardown: already idle (" + reason + ")");
                return;
            }
            primary = mPrimaryDevice;
            secondary = mSecondaryDevice;
            mInSharingMode = false;
            mStarting = false;
            mSecondaryDevice = null;
        }

        Log.i(TAG, "Executing teardown (reason: " + reason + ", callNativeExit=" + callNativeExit
                + ", primary=" + primary + ", secondary=" + secondary + ")");

        if (callNativeExit && mNativeInterface != null) {
            // Cancel any previously scheduled fallback
            if (mTeardownFallbackRunnable != null) {
                mHandler.removeCallbacks(mTeardownFallbackRunnable);
                mTeardownFallbackRunnable = null;
            }

            // Post a fallback watchdog (1000ms) in case native callback does not return
            final BluetoothDevice finalPrim = primary;
            final BluetoothDevice finalSec = secondary;
            mTeardownFallbackRunnable = () -> {
                Log.w(TAG, "Teardown native STOPPED callback timed out after 1000ms, forcing completion");
                synchronized (mLock) {
                    mTeardownFallbackRunnable = null;
                }
                finishTeardown(finalPrim, finalSec);
            };
            mHandler.postDelayed(mTeardownFallbackRunnable, 1000);

            // Request Fluoride main thread to stop secondary stream and restart primary session
            mNativeInterface.exitSharing();
            return;
        }

        // Native exit completed or fallback triggered: perform codec restore and service shutdown
        synchronized (mLock) {
            if (mTeardownFallbackRunnable != null) {
                mHandler.removeCallbacks(mTeardownFallbackRunnable);
                mTeardownFallbackRunnable = null;
            }
        }
        finishTeardown(primary, secondary);
    }

    private void finishTeardown(BluetoothDevice primary, BluetoothDevice secondary) {
        AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
        if (adapterService != null) {
            A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
            if (a2dpService != null) {
                if (secondary != null) {
                    restoreDeviceCodec(a2dpService, secondary);
                    mCodecSnapshots.remove(secondary);
                }
                boolean masterOn;
                synchronized (mLock) {
                    masterOn = mMasterSwitchEnabled;
                }
                if (!masterOn) {
                    if (primary != null) {
                        restoreDeviceCodec(a2dpService, primary);
                        mCodecSnapshots.remove(primary);
                    }
                    for (BluetoothDevice dev : new ArrayList<>(mCodecSnapshots.keySet())) {
                        restoreDeviceCodec(a2dpService, dev);
                        mCodecSnapshots.remove(dev);
                    }
                } else {
                    for (BluetoothDevice dev : new ArrayList<>(mCodecSnapshots.keySet())) {
                        if (primary == null || !dev.equals(primary)) {
                            restoreDeviceCodec(a2dpService, dev);
                            mCodecSnapshots.remove(dev);
                        }
                    }
                }
            }
        }

        restoreDynamicAbsVolume(primary, secondary);
        notifyStateChanged(false, primary, secondary);

        try {
            stopSelf();
        } catch (Exception e) {
            Log.d(TAG, "finishTeardown: stopSelf skipped", e);
        }
    }

    private void handleDynamicAbsVolumeStateChange(BluetoothDevice primary, BluetoothDevice secondary) {
        if (primary == null || secondary == null) return;
        try {
            AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
            if (adapterService == null) return;
            AvrcpTargetService avrcp = (AvrcpTargetService) adapterService.getAvrcpTargetService().orElse(null);
            AudioManager audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);

            boolean isPrimaryAbs = (avrcp != null) && avrcp.getAbsoluteVolumeSupported(primary);
            boolean isSecondaryAbs = (avrcp != null) && avrcp.getAbsoluteVolumeSupported(secondary);
            Log.i(TAG, "handleDynamicAbsVolumeStateChange: primary=" + primary + " (abs=" + isPrimaryAbs
                    + "), secondary=" + secondary + " (abs=" + isSecondaryAbs + ")");

            if (isPrimaryAbs && !isSecondaryAbs) {
                Log.i(TAG, "Dynamic AbsVol: secondary lacks AbsVol, forcing primary to DEVICE_VOLUME_BEHAVIOR_VARIABLE");
                if (avrcp != null) {
                    avrcp.setAbsoluteVolumeSupported(primary, false);
                }
                if (audioManager != null) {
                    AudioDeviceAttributes primAttr = new AudioDeviceAttributes(
                            AudioDeviceAttributes.ROLE_OUTPUT,
                            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                            primary.getAddress());
                    audioManager.setDeviceVolumeBehavior(primAttr, AudioManager.DEVICE_VOLUME_BEHAVIOR_VARIABLE);
                }
                mIsPrimaryAbsTurnedOffAgainstWill = true;
            } else if (!isPrimaryAbs && isSecondaryAbs) {
                Log.i(TAG, "Dynamic AbsVol: primary lacks AbsVol, forcing secondary to DEVICE_VOLUME_BEHAVIOR_VARIABLE");
                if (avrcp != null) {
                    avrcp.setAbsoluteVolumeSupported(secondary, false);
                }
                if (audioManager != null) {
                    AudioDeviceAttributes secAttr = new AudioDeviceAttributes(
                            AudioDeviceAttributes.ROLE_OUTPUT,
                            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                            secondary.getAddress());
                    audioManager.setDeviceVolumeBehavior(secAttr, AudioManager.DEVICE_VOLUME_BEHAVIOR_VARIABLE);
                }
                mIsSecondaryAbsTurnedOffAgainstWill = true;
            }
        } catch (Throwable t) {
            Log.w(TAG, "handleDynamicAbsVolumeStateChange failed", t);
        }
    }

    private void restoreDynamicAbsVolume(BluetoothDevice primary, BluetoothDevice secondary) {
        try {
            AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
            AvrcpTargetService avrcp = (adapterService != null)
                    ? (AvrcpTargetService) adapterService.getAvrcpTargetService().orElse(null)
                    : null;
            AudioManager audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);

            if (mIsPrimaryAbsTurnedOffAgainstWill && primary != null) {
                Log.i(TAG, "Dynamic AbsVol: restoring primary to DEVICE_VOLUME_BEHAVIOR_ABSOLUTE");
                if (avrcp != null) {
                    avrcp.setAbsoluteVolumeSupported(primary, true);
                }
                if (audioManager != null) {
                    AudioDeviceAttributes primAttr = new AudioDeviceAttributes(
                            AudioDeviceAttributes.ROLE_OUTPUT,
                            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                            primary.getAddress());
                    audioManager.setDeviceVolumeBehavior(primAttr, AudioManager.DEVICE_VOLUME_BEHAVIOR_ABSOLUTE);
                }
                mIsPrimaryAbsTurnedOffAgainstWill = false;
            }
            if (mIsSecondaryAbsTurnedOffAgainstWill && secondary != null) {
                Log.i(TAG, "Dynamic AbsVol: restoring secondary to DEVICE_VOLUME_BEHAVIOR_ABSOLUTE");
                if (avrcp != null) {
                    avrcp.setAbsoluteVolumeSupported(secondary, true);
                }
                if (audioManager != null) {
                    AudioDeviceAttributes secAttr = new AudioDeviceAttributes(
                            AudioDeviceAttributes.ROLE_OUTPUT,
                            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                            secondary.getAddress());
                    audioManager.setDeviceVolumeBehavior(secAttr, AudioManager.DEVICE_VOLUME_BEHAVIOR_ABSOLUTE);
                }
                mIsSecondaryAbsTurnedOffAgainstWill = false;
            }
        } catch (Throwable t) {
            Log.w(TAG, "restoreDynamicAbsVolume failed", t);
        }
    }

    private void notifyStateChanged(boolean active, BluetoothDevice primary, BluetoothDevice secondary) {
        synchronized (mStateCallbacks) {
            int n = mStateCallbacks.beginBroadcast();
            for (int i = 0; i < n; i++) {
                try {
                    if (active) {
                        mStateCallbacks.getBroadcastItem(i).onSharingStarted(
                                SHARING_ROLE_PRIMARY, mSharingType, 0, primary, secondary);
                    } else {
                        mStateCallbacks.getBroadcastItem(i).onSharingStopped(
                                SHARING_ROLE_PRIMARY, mSharingType, 0, primary, secondary);
                    }
                } catch (RemoteException e) {
                    Log.e(TAG, "RemoteException in notifyStateChanged", e);
                }
            }
            mStateCallbacks.finishBroadcast();
        }
    }

    private void notifyVolumeChanged(BluetoothDevice device, int volume) {
        synchronized (mVolumeCallbacks) {
            int n = mVolumeCallbacks.beginBroadcast();
            for (int i = 0; i < n; i++) {
                try {
                    mVolumeCallbacks.getBroadcastItem(i).onDeviceVolumeChanged(device, volume);
                } catch (RemoteException e) {
                    Log.e(TAG, "RemoteException in notifyVolumeChanged", e);
                }
            }
            mVolumeCallbacks.finishBroadcast();
        }
    }
}
