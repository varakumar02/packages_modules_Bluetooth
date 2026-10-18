/*
 * Copyright (C) 2026 LunarisOS
 * JNI Implementation for OPlus A2DP Audio Sharing
 */

#define LOG_TAG "BluetoothA2dpSharingJni"

#include <mutex>
#include <shared_mutex>

#include "btif/include/btif_a2dp_sharing.h"
#include "com_android_bluetooth.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_av.h"

namespace android {

static jmethodID method_onSharingDeviceVolumeChanged;
static jmethodID method_onSharingStateChanged;
static const btav_sharing_interface_t* sBluetoothA2dpSharingInterface = nullptr;
static jobject mCallbacksObj = nullptr;
static std::shared_timed_mutex interface_mutex;
static std::shared_timed_mutex callbacks_mutex;

static void sharing_volume_changed_callback(const RawAddress& bd_addr, int volume) {
  std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) return;
  if (method_onSharingDeviceVolumeChanged == nullptr) {
    log::error("method_onSharingDeviceVolumeChanged is null, skipping callback");
    return;
  }

  ScopedLocalRef<jstring> addr(
      sCallbackEnv.get(), sCallbackEnv->NewStringUTF(bd_addr.ToString().c_str()));
  sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onSharingDeviceVolumeChanged,
                                addr.get(), (jint)volume);
}

static void sharing_state_changed_callback(int state, const RawAddress& primary,
                                          const RawAddress& secondary) {
  log::info("sharing_state_changed_callback: state={}, primary={}, secondary={}",
            state, primary, secondary);
  std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
  CallbackEnv sCallbackEnv(__func__);
  if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) return;
  if (method_onSharingStateChanged == nullptr) {
    log::error("method_onSharingStateChanged is null, skipping callback");
    return;
  }

  ScopedLocalRef<jstring> prim(
      sCallbackEnv.get(), sCallbackEnv->NewStringUTF(primary.ToString().c_str()));
  ScopedLocalRef<jstring> sec(
      sCallbackEnv.get(), sCallbackEnv->NewStringUTF(secondary.ToString().c_str()));
  sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onSharingStateChanged,
                               (jint)state, prim.get(), sec.get());
}

static btav_sharing_callbacks_t sBluetoothA2dpSharingCallbacks = {
    .size = sizeof(btav_sharing_callbacks_t),
    .on_volume_changed = sharing_volume_changed_callback,
    .on_sharing_state_changed = sharing_state_changed_callback,
};

static void classInitNative(JNIEnv* env, jclass clazz) {
  method_onSharingDeviceVolumeChanged =
      env->GetMethodID(clazz, "onSharingDeviceVolumeChanged", "(Ljava/lang/String;I)V");
  if (method_onSharingDeviceVolumeChanged == nullptr) {
    log::error("Failed to find onSharingDeviceVolumeChanged");
    if (!env->ExceptionCheck()) {
      jclass exClazz = env->FindClass("java/lang/NoSuchMethodError");
      if (exClazz) env->ThrowNew(exClazz, "onSharingDeviceVolumeChanged not found");
    }
    return;
  }
  method_onSharingStateChanged =
      env->GetMethodID(clazz, "onSharingStateChanged", "(ILjava/lang/String;Ljava/lang/String;)V");
  if (method_onSharingStateChanged == nullptr) {
    log::error("Failed to find onSharingStateChanged");
    if (!env->ExceptionCheck()) {
      jclass exClazz = env->FindClass("java/lang/NoSuchMethodError");
      if (exClazz) env->ThrowNew(exClazz, "onSharingStateChanged not found");
    }
    return;
  }
  log::info("classInitNative completed");
}

static jboolean initNative(JNIEnv* env, jobject object) {
  std::unique_lock<std::shared_timed_mutex> lock(interface_mutex);
  std::unique_lock<std::shared_timed_mutex> cb_lock(callbacks_mutex);

  const bt_interface_t* btInf = getBluetoothInterface();
  if (btInf == nullptr) {
    log::error("Bluetooth module is not loaded");
    return JNI_FALSE;
  }

  if (sBluetoothA2dpSharingInterface != nullptr) {
    log::info("Cleaning up Bluetooth A2DP Sharing Interface before initializing...");
    sBluetoothA2dpSharingInterface->cleanup();
    sBluetoothA2dpSharingInterface = nullptr;
  }

  if (mCallbacksObj != nullptr) {
    env->DeleteGlobalRef(mCallbacksObj);
    mCallbacksObj = nullptr;
  }

  sBluetoothA2dpSharingInterface =
      (const btav_sharing_interface_t*)btInf->get_profile_interface(
          BT_PROFILE_A2DP_SHARING_ID);
  if (sBluetoothA2dpSharingInterface == nullptr) {
    log::error("Failed to get Bluetooth A2DP Sharing Interface");
    return JNI_FALSE;
  }

  mCallbacksObj = env->NewGlobalRef(object);
  bt_status_t status = sBluetoothA2dpSharingInterface->init(&sBluetoothA2dpSharingCallbacks);
  if (status != BT_STATUS_SUCCESS) {
    log::error("Failed to initialize Bluetooth A2DP Sharing, status: {}", bt_status_text(status));
    sBluetoothA2dpSharingInterface = nullptr;
    return JNI_FALSE;
  }

  log::info("initNative completed successfully");
  return JNI_TRUE;
}

static void cleanupNative(JNIEnv* env, jobject /* object */) {
  std::unique_lock<std::shared_timed_mutex> lock(interface_mutex);
  std::unique_lock<std::shared_timed_mutex> cb_lock(callbacks_mutex);

  if (sBluetoothA2dpSharingInterface != nullptr) {
    sBluetoothA2dpSharingInterface->cleanup();
    sBluetoothA2dpSharingInterface = nullptr;
  }
  if (mCallbacksObj != nullptr) {
    env->DeleteGlobalRef(mCallbacksObj);
    mCallbacksObj = nullptr;
  }
  log::info("Bluetooth A2DP Sharing cleaned up");
}

static jboolean enterSharingNative(JNIEnv* env, jobject /* object */, jbyteArray address) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sBluetoothA2dpSharingInterface) return JNI_FALSE;

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) return JNI_FALSE;

  RawAddress bd_addr = RawAddress::FromOctets(reinterpret_cast<const uint8_t*>(addr));
  env->ReleaseByteArrayElements(address, addr, 0);

  log::info("enterSharingNative: address={}", bd_addr);
  return sBluetoothA2dpSharingInterface->enter_sharing(bd_addr) ? JNI_TRUE : JNI_FALSE;
}

static jboolean exitSharingNative(JNIEnv* /* env */, jobject /* object */) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sBluetoothA2dpSharingInterface) return JNI_FALSE;

  log::info("exitSharingNative");
  return sBluetoothA2dpSharingInterface->exit_sharing() ? JNI_TRUE : JNI_FALSE;
}

int register_com_android_bluetooth_a2dp_sharing(JNIEnv* env) {
  jclass clazz = env->FindClass("com/oplus/bluetooth/feature/a2dpshare/OplusA2dpSharingNativeInterface");
  if (clazz == nullptr) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    log::warn("OplusA2dpSharingNativeInterface class not found, skipping JNI registration");
    return 0;
  }

  const JNINativeMethod methods[] = {
      {"classInitNative", "()V", (void*)classInitNative},
      {"initNative", "()Z", (void*)initNative},
      {"cleanupNative", "()V", (void*)cleanupNative},
      {"enterSharingNative", "([B)Z", (void*)enterSharingNative},
      {"exitSharingNative", "()Z", (void*)exitSharingNative},
  };
  int res = env->RegisterNatives(clazz, methods, NELEM(methods));
  if (res < 0) {
    log::error("RegisterNatives failed for OplusA2dpSharingNativeInterface");
    return -1;
  }
  return 0;
}

}  // namespace android
