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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

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

    // Marks a codec config set by sharing (also how a snapshot recognizes it)
    private static final int SHARING_CODEC_PRIORITY = 1000000;
    // auto (default), ldac, aptx_hd, aptx, aac or sbc; written by DeviceSettings
    private static final String PROP_SHARING_CODEC = "persist.bluetooth.a2dp_sharing.codec";
    // Best first. Both sinks decode the same packets, so only codecs with a software
    // encoder can be shared (aptX Adaptive is offload-only).
    private static final int[] SHARING_CODEC_ORDER = {
        BluetoothCodecConfig.SOURCE_CODEC_TYPE_LDAC,
        BluetoothCodecConfig.SOURCE_CODEC_TYPE_APTX_HD,
        BluetoothCodecConfig.SOURCE_CODEC_TYPE_APTX,
        BluetoothCodecConfig.SOURCE_CODEC_TYPE_AAC,
        BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC,
    };
    // LDAC adaptive bit rate per sink, kept at 660 kbps or below by the encoder while sharing
    // (two 990 kbps streams don't fit the air time); steps down on congestion
    private static final long LDAC_SHARING_QUALITY = 1003L;
    private static final ExecutorService sAsyncExecutor = Executors.newSingleThreadExecutor();

    private final RemoteCallbackList<IOplusA2dpSharingStateCallback> mStateCallbacks =
            new RemoteCallbackList<>();
    private final RemoteCallbackList<IOplusA2dpSharingVolumeCallback> mVolumeCallbacks =
            new RemoteCallbackList<>();

    private final Object mLock = new Object();
    private BluetoothDevice mPrimaryDevice;
    private BluetoothDevice mSecondaryDevice;
    // The codec config both peers were put on (sharingCodecCandidates)
    private BluetoothCodecConfig mSharingCodec;
    private boolean mInSharingMode = false;
    private boolean mStarting = false;
    private boolean mMasterSwitchEnabled = false;
    private int mSharingType = 0;
    private boolean mIsPrimaryAbsTurnedOffAgainstWill = false;
    private boolean mIsSecondaryAbsTurnedOffAgainstWill = false;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private Runnable mTeardownFallbackRunnable = null;
    // Rolls back a start whose native STARTED never came; cancelled once it came or sharing stopped
    private Runnable mEnterTimeoutRunnable = null;
    // No active A2DP device while sharing (phone speaker or another output picked): stop sharing
    // unless a headset comes back as the active device within this time
    private static final long NO_ACTIVE_DEVICE_TEARDOWN_MS = 1500;
    private final Runnable mNoActiveDeviceRunnable = this::onNoActiveDevice;

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
                    if (state == BluetoothProfile.STATE_DISCONNECTED && device != null) {
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
                    BluetoothDevice newSecondary = null;
                    mHandler.removeCallbacks(mNoActiveDeviceRunnable);
                    if (activeDevice == null) {
                        mHandler.postDelayed(mNoActiveDeviceRunnable, NO_ACTIVE_DEVICE_TEARDOWN_MS);
                    }
                    synchronized (mLock) {
                        if (mInSharingMode && activeDevice != null && activeDevice.equals(mSecondaryDevice)) {
                            // The sharing peer became the active device (output switcher, role swap):
                            // the old primary is now the sharing peer, else the stack stops it for good
                            newSecondary = mPrimaryDevice;
                            mPrimaryDevice = activeDevice;
                            mSecondaryDevice = newSecondary;
                            boolean primaryAbsOff = mIsPrimaryAbsTurnedOffAgainstWill;
                            mIsPrimaryAbsTurnedOffAgainstWill = mIsSecondaryAbsTurnedOffAgainstWill;
                            mIsSecondaryAbsTurnedOffAgainstWill = primaryAbsOff;
                            Log.i(TAG, "Active A2DP device switched to the sharing peer " + activeDevice
                                    + ", sharing peer is now " + newSecondary);
                        } else if (mInSharingMode || mStarting) {
                            // A null activeDevice is handled by onNoActiveDevice; tear down at once only
                            // when another A2DP device became active.
                            if (activeDevice != null && !activeDevice.equals(mPrimaryDevice) && !activeDevice.equals(mSecondaryDevice)) {
                                shouldTeardown = true;
                                Log.i(TAG, "Active A2DP device switched to 3rd party device " + activeDevice
                                        + " (primary=" + mPrimaryDevice + ", sec=" + mSecondaryDevice + "); triggering teardown");
                            }
                        } else {
                            if (activeDevice != null) {
                                mPrimaryDevice = activeDevice;
                                Log.i(TAG, "Updated prospective primary device to: " + activeDevice);
                            }
                        }
                    }
                    if (shouldTeardown) {
                        teardown("active_device_changed", true);
                    } else if (newSecondary != null && mNativeInterface != null
                            && !mNativeInterface.enterSharing(newSecondary)) {
                        teardown("role_swap_failed", true);
                    }
                }
                case BluetoothA2dp.ACTION_CODEC_CONFIG_CHANGED -> {
                    BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                    BluetoothCodecStatus status =
                            intent.getParcelableExtra(BluetoothCodecStatus.EXTRA_CODEC_STATUS);
                    boolean shouldTeardown;
                    synchronized (mLock) {
                        // A peer reconfigured to another codec (developer options, the headset)
                        // can't decode the other one's packets any more
                        shouldTeardown = mInSharingMode && mSharingCodec != null && status != null
                                && (device != null && (device.equals(mPrimaryDevice)
                                        || device.equals(mSecondaryDevice)))
                                && !matchesSharingCodec(status.getCodecConfig(), mSharingCodec);
                    }
                    if (shouldTeardown) {
                        Log.i(TAG, device + " left the sharing codec: " + status.getCodecConfig());
                        teardown("codec_changed", true);
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

            if (!enabled) {
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

            if (!ensureNativeInitialized() || mNativeInterface == null) {
                Log.e(TAG, "startSharing: native interface not available");
                synchronized (mLock) { mStarting = false; }
                return false;
            }

            AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
            if (adapterService == null) {
                Log.e(TAG, "startSharing: AdapterService unavailable");
                synchronized (mLock) { mStarting = false; }
                return false;
            }

            A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
            if (a2dpService == null) {
                Log.e(TAG, "startSharing: A2dpService unavailable");
                synchronized (mLock) { mStarting = false; }
                return false;
            }

            if (a2dpService.getConnectionState(device) != BluetoothProfile.STATE_CONNECTED) {
                Log.e(TAG, "startSharing: secondary device A2DP not connected");
                synchronized (mLock) { mStarting = false; }
                return false;
            }

            BluetoothDevice primary = a2dpService.getActiveDevice();
            if (primary == null || primary.equals(device)) {
                Log.e(TAG, "startSharing: invalid primary=" + primary + " secondary=" + device);
                synchronized (mLock) { mStarting = false; }
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

            // Execute configuration, SBC verification wait, and native enter asynchronously
            // so the Binder thread is not blocked (eliminates Tile / SystemUI ANR hazard)
            sAsyncExecutor.execute(() -> {
                try {
                    // Snapshot existing codecs and optional codec preferences before modification
                    snapshotCodec(a2dpService, primary);
                    snapshotCodec(a2dpService, device);

                    // Put both peers on the same codec config, best common codec first
                    boolean configured = false;
                    for (BluetoothCodecConfig cfg : sharingCodecCandidates(a2dpService, primary, device)) {
                        Log.i(TAG, "startSharing: trying " + cfg);
                        if (applySharingCodec(a2dpService, primary, cfg)
                                && applySharingCodec(a2dpService, device, cfg)
                                && waitForSharingCodec(a2dpService, primary, device, cfg, 1500)) {
                            configured = true;
                            synchronized (mLock) { mSharingCodec = cfg; }
                            break;
                        }
                    }
                    if (!configured) {
                        Log.e(TAG, "startSharing: no codec config both peers accept, rolling back");
                        rollbackSharing(a2dpService, primary, device);
                        return;
                    }

                    synchronized (mLock) {
                        if (!mStarting) {
                            Log.w(TAG, "startSharing: aborted during setup wait (teardown requested)");
                            rollbackSharing(a2dpService, primary, device);
                            return;
                        }
                    }

                    if (mNativeInterface == null) {
                        Log.e(TAG, "startSharing: mNativeInterface is null");
                        rollbackSharing(a2dpService, primary, device);
                        return;
                    }

                    // Trigger Fluoride native sharing stream
                    boolean success = mNativeInterface.enterSharing(device);
                    Log.i(TAG, "Native enterSharing returned: " + success);

                    if (!success) {
                        Log.e(TAG, "Native enterSharing failed, rolling back");
                        rollbackSharing(a2dpService, primary, device);
                        return;
                    }

                    // Safety timeout guard: abort if native STARTED callback does not arrive within 3000ms
                    Runnable enterTimeout = () -> {
                        synchronized (mLock) {
                            mEnterTimeoutRunnable = null;
                            if (mStarting && !mInSharingMode) {
                                Log.w(TAG, "startSharing: timeout waiting for native STARTED callback, rolling back");
                                teardown("native_enter_timeout", true);
                            }
                        }
                    };
                    synchronized (mLock) {
                        cancelEnterTimeout();
                        mEnterTimeoutRunnable = enterTimeout;
                    }
                    mHandler.postDelayed(enterTimeout, 3000);

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
                } catch (Throwable t) {
                    Log.e(TAG, "startSharing async setup encountered error, rolling back", t);
                    rollbackSharing(a2dpService, primary, device);
                }
            });

            return true;
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
            filter.addAction(BluetoothA2dp.ACTION_CODEC_CONFIG_CHANGED);
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
                    cancelEnterTimeout();
                    mInSharingMode = true;
                    if (primary != null) mPrimaryDevice = primary;
                    if (secondary != null) mSecondaryDevice = secondary;
                }
                notifyStateChanged(true, primary, secondary);
            } else {
                // Native-initiated exit (e.g. BTA_AV_CLOSE_EVT) routes through teardown
                teardown("native_stopped_callback", false);
            }
            announceCodecForRouting();
        }
    };

    /**
     * Sharing moved the stream between the offload and the software session without a codec
     * change: report the active device's codec again, so AudioService re-reads it
     * (bluetooth.a2dp_sharing.active) and audio policy moves the sink to the matching module.
     */
    private static void announceCodecForRouting() {
        AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
        if (adapterService == null) return;
        A2dpService a2dpService = (A2dpService) adapterService.getA2dpService().orElse(null);
        if (a2dpService == null) return;
        BluetoothDevice active = a2dpService.getActiveDevice();
        if (active == null) return;
        BluetoothCodecStatus status = a2dpService.getCodecStatus(active);
        if (status == null || status.getCodecConfig() == null) return;
        Log.i(TAG, "announceCodecForRouting: " + active);
        a2dpService.codecConfigUpdated(active, status, false);
    }

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

            // Never record a config sharing itself set as the one to restore
            if (cfg != null && cfg.getCodecPriority() == SHARING_CODEC_PRIORITY) {
                Log.i(TAG, "snapshotCodec for " + device + ": current config was set by sharing, skipping");
                return;
            }

            mCodecSnapshots.putIfAbsent(device, new DeviceCodecSnapshot(opt, cfg));
            Log.i(TAG, "snapshotCodec for " + device + ": opt=" + opt + ", cfg=" + cfg);
        } catch (Exception e) {
            Log.w(TAG, "Failed to snapshot codec for " + device, e);
        }
    }

    private static String getSharingCodecProperty() {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method get = sp.getMethod("get", String.class, String.class);
            return (String) get.invoke(null, PROP_SHARING_CODEC, "auto");
        } catch (Exception e) {
            return "auto";
        }
    }

    private static int codecTypeFromName(String name) {
        return switch (name) {
            case "ldac" -> BluetoothCodecConfig.SOURCE_CODEC_TYPE_LDAC;
            case "aptx_hd" -> BluetoothCodecConfig.SOURCE_CODEC_TYPE_APTX_HD;
            case "aptx" -> BluetoothCodecConfig.SOURCE_CODEC_TYPE_APTX;
            case "aac" -> BluetoothCodecConfig.SOURCE_CODEC_TYPE_AAC;
            case "sbc" -> BluetoothCodecConfig.SOURCE_CODEC_TYPE_SBC;
            default -> -1;
        };
    }

    private static BluetoothCodecConfig findCapability(BluetoothCodecStatus status, int type) {
        if (status == null) return null;
        for (BluetoothCodecConfig cap : status.getCodecsSelectableCapabilities()) {
            if (cap.getCodecType() == type) return cap;
        }
        return null;
    }

    /**
     * Codec configs both peers can take, in the order to try them: the codec chosen in
     * PROP_SHARING_CODEC first (if both support it), then SHARING_CODEC_ORDER.
     */
    private static List<BluetoothCodecConfig> sharingCodecCandidates(A2dpService a2dpService,
            BluetoothDevice primary, BluetoothDevice secondary) {
        BluetoothCodecStatus primStatus = a2dpService.getCodecStatus(primary);
        BluetoothCodecStatus secStatus = a2dpService.getCodecStatus(secondary);
        List<Integer> order = new ArrayList<>();
        int preferred = codecTypeFromName(getSharingCodecProperty());
        if (preferred != -1) order.add(preferred);
        for (int type : SHARING_CODEC_ORDER) {
            if (type != preferred) order.add(type);
        }

        List<BluetoothCodecConfig> candidates = new ArrayList<>();
        for (int type : order) {
            BluetoothCodecConfig a = findCapability(primStatus, type);
            BluetoothCodecConfig b = findCapability(secStatus, type);
            if (a == null || b == null) continue;
            int rates = a.getSampleRate() & b.getSampleRate();
            int sampleRate = (rates & BluetoothCodecConfig.SAMPLE_RATE_44100) != 0
                    ? BluetoothCodecConfig.SAMPLE_RATE_44100
                    : (rates & BluetoothCodecConfig.SAMPLE_RATE_48000) != 0
                    ? BluetoothCodecConfig.SAMPLE_RATE_48000 : 0;
            int bits = a.getBitsPerSample() & b.getBitsPerSample();
            int bitsPerSample = (bits & BluetoothCodecConfig.BITS_PER_SAMPLE_24) != 0
                    ? BluetoothCodecConfig.BITS_PER_SAMPLE_24
                    : (bits & BluetoothCodecConfig.BITS_PER_SAMPLE_16) != 0
                    ? BluetoothCodecConfig.BITS_PER_SAMPLE_16 : 0;
            boolean stereo = (a.getChannelMode() & b.getChannelMode()
                    & BluetoothCodecConfig.CHANNEL_MODE_STEREO) != 0;
            if (sampleRate == 0 || bitsPerSample == 0 || !stereo) continue;
            candidates.add(new BluetoothCodecConfig.Builder()
                    .setCodecType(type)
                    .setCodecPriority(SHARING_CODEC_PRIORITY)
                    .setSampleRate(sampleRate)
                    .setBitsPerSample(bitsPerSample)
                    .setChannelMode(BluetoothCodecConfig.CHANNEL_MODE_STEREO)
                    .setCodecSpecific1(type == BluetoothCodecConfig.SOURCE_CODEC_TYPE_LDAC
                            ? LDAC_SHARING_QUALITY : 0L)
                    .build());
        }
        return candidates;
    }

    private static boolean matchesSharingCodec(BluetoothCodecConfig cur, BluetoothCodecConfig want) {
        return cur != null
                && cur.getCodecType() == want.getCodecType()
                && cur.getSampleRate() == want.getSampleRate()
                && cur.getBitsPerSample() == want.getBitsPerSample()
                && cur.getChannelMode() == want.getChannelMode()
                && (want.getCodecType() != BluetoothCodecConfig.SOURCE_CODEC_TYPE_LDAC
                        || cur.getCodecSpecific1() == want.getCodecSpecific1());
    }

    private static BluetoothCodecConfig currentCodec(A2dpService a2dpService, BluetoothDevice device) {
        BluetoothCodecStatus status = a2dpService.getCodecStatus(device);
        return status != null ? status.getCodecConfig() : null;
    }

    private static boolean applySharingCodec(A2dpService a2dpService, BluetoothDevice device,
            BluetoothCodecConfig cfg) {
        try {
            if (matchesSharingCodec(currentCodec(a2dpService, device), cfg)) {
                Log.i(TAG, "applySharingCodec: " + device + " already on the sharing codec");
                return true;
            }
            a2dpService.setCodecConfigPreference(device, cfg);
            Log.i(TAG, "applySharingCodec: requested sharing codec on " + device);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to apply sharing codec on " + device, e);
            return false;
        }
    }

    private static boolean waitForSharingCodec(A2dpService a2dpService, BluetoothDevice primary,
            BluetoothDevice secondary, BluetoothCodecConfig cfg, long timeoutMs) {
        long start = System.currentTimeMillis();
        while (System.currentTimeMillis() - start < timeoutMs) {
            try {
                if (matchesSharingCodec(currentCodec(a2dpService, primary), cfg)
                        && matchesSharingCodec(currentCodec(a2dpService, secondary), cfg)) {
                    Log.i(TAG, "waitForSharingCodec: both peers on the sharing codec in "
                            + (System.currentTimeMillis() - start) + "ms");
                    return true;
                }
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            } catch (Exception e) {
                Log.w(TAG, "waitForSharingCodec check failed", e);
                return false;
            }
        }
        Log.w(TAG, "waitForSharingCodec: timed out");
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

    private void onNoActiveDevice() {
        AdapterService adapterService = AdapterService.deprecatedGetAdapterService();
        A2dpService a2dpService = adapterService == null ? null
                : (A2dpService) adapterService.getA2dpService().orElse(null);
        if (a2dpService != null && a2dpService.getActiveDevice() != null) return;
        boolean sharing;
        synchronized (mLock) {
            sharing = mInSharingMode || mStarting;
        }
        if (sharing) {
            Log.i(TAG, "No active A2DP device for " + NO_ACTIVE_DEVICE_TEARDOWN_MS
                    + " ms (audio left the headsets), triggering teardown");
            teardown("no_active_device", true);
        }
    }

    private void cancelEnterTimeout() {
        if (mEnterTimeoutRunnable != null) {
            mHandler.removeCallbacks(mEnterTimeoutRunnable);
            mEnterTimeoutRunnable = null;
        }
    }

    private void rollbackSharing(A2dpService a2dpService, BluetoothDevice primary, BluetoothDevice secondary) {
        synchronized (mLock) {
            cancelEnterTimeout();
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
        notifyStateChanged(false, primary, secondary);
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
            cancelEnterTimeout();
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
                if (primary != null) {
                    restoreDeviceCodec(a2dpService, primary);
                    mCodecSnapshots.remove(primary);
                }
                for (BluetoothDevice dev : new ArrayList<>(mCodecSnapshots.keySet())) {
                    restoreDeviceCodec(a2dpService, dev);
                    mCodecSnapshots.remove(dev);
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
