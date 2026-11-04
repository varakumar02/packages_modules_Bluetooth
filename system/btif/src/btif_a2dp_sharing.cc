/*
 * Copyright (C) 2026 LunarisOS
 * OPlus A2DP Sharing Implementation for Fluoride Stack
 */

#define LOG_TAG "btif_a2dp_sharing"

#include "btif_a2dp_sharing.h"
#include "audio_hal_interface/a2dp_encoding.h"
#include "btif_av.h"
#include "btif_a2dp_source.h"
#include "btif_common.h"

#include <bluetooth/log.h>
#include <mutex>
#include <future>
#include <thread>
#include <chrono>
#include <cstring>
#include <base/functional/bind.h>
#include "osi/include/properties.h"
#include "stack/include/main_thread.h"

using namespace bluetooth;

static btav_sharing_callbacks_t* s_callbacks = nullptr;
static std::mutex s_sharing_mutex;
static RawAddress s_primary_address = RawAddress::kEmpty;
static RawAddress s_secondary_address = RawAddress::kEmpty;
static bool s_in_sharing_mode = false;
static uint32_t s_sharing_generation = 0;

static void btif_a2dp_sharing_exit_main_thread(const RawAddress& disconnected_peer);

bool btif_a2dp_sharing_is_active(void) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    return s_in_sharing_mode && !s_secondary_address.IsEmpty();
}

bool btif_a2dp_sharing_is_sharing_peer(const RawAddress& bd_addr) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    return s_in_sharing_mode && (bd_addr == s_secondary_address);
}

// Read by the audio HAL interface (setup_codec: software session) and by AudioService
// (BtHelper.getCodec: route to the software Bluetooth module) while sharing is on.
static void set_sharing_active_prop(bool active) {
    osi_property_set("bluetooth.a2dp_sharing.active", active ? "1" : "0");
}

bool btif_a2dp_sharing_offload_probe(void) {
    char mode[PROPERTY_VALUE_MAX] = {};
    osi_property_get("persist.bluetooth.a2dp_sharing.offload_probe", mode, "");
    return mode[0] != '\0' && strcmp(mode, "false") != 0 && strcmp(mode, "0") != 0;
}

RawAddress btif_a2dp_sharing_get_sharing_peer(void) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    if (!s_in_sharing_mode) {
        return RawAddress::kEmpty;
    }
    return s_secondary_address;
}

void btif_a2dp_sharing_on_peer_disconnected(const RawAddress& bd_addr) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    if (!s_in_sharing_mode) return;
    if (bd_addr == s_secondary_address || bd_addr == s_primary_address) {
        log::info("btif_a2dp_sharing: active peer {} disconnected, triggering exit", bd_addr);
        do_in_main_thread(base::BindOnce(&btif_a2dp_sharing_exit_main_thread, bd_addr));
    }
}

static bt_status_t init(btav_sharing_callbacks_t* callbacks) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    log::info("btif_a2dp_sharing: init called");
    s_callbacks = callbacks;
    set_sharing_active_prop(false);
    return BT_STATUS_SUCCESS;
}

static void cleanup(void) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    log::info("btif_a2dp_sharing: cleanup called");
    s_callbacks = nullptr;
    s_primary_address = RawAddress::kEmpty;
    s_secondary_address = RawAddress::kEmpty;
    s_in_sharing_mode = false;
    ++s_sharing_generation;
    set_sharing_active_prop(false);
}

static void btif_a2dp_sharing_enter_main_thread(const RawAddress& bd_addr) {
    RawAddress primary = btif_av_source_active_peer();
    log::info("btif_a2dp_sharing_enter_main_thread: primary={}, secondary={}", primary, bd_addr);

    btav_sharing_callbacks_t* callbacks = nullptr;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(s_sharing_mutex);
        generation = ++s_sharing_generation;
        s_secondary_address = bd_addr;
        s_in_sharing_mode = true;
        s_primary_address = primary;
        callbacks = s_callbacks;
    }

    if (!primary.IsEmpty()) {
        std::promise<void> promise;
        std::future<void> future = promise.get_future();
        if (btif_a2dp_sharing_offload_probe()) {
            log::info("btif_a2dp_sharing: offload probe, session left as it is (gen={})", generation);
            promise.set_value();
        } else if (bluetooth::audio::a2dp::is_hal_offloading()) {
            // The restart's setup_codec moves the stream to the software session
            set_sharing_active_prop(true);
            log::info("btif_a2dp_sharing: restarting source session for dual sharing setup (gen={})", generation);
            btif_a2dp_source_restart_session(primary, primary, std::move(promise));
        } else {
            set_sharing_active_prop(true);
            // A session restart leaves the audio HAL output stream DISABLED until it is reopened,
            // so a software session only gets its encoder re-initialized (SBC sharing caps).
            log::info("btif_a2dp_sharing: re-initializing the encoder for dual sharing setup (gen={})", generation);
            btif_a2dp_source_reinit_encoder(primary, std::move(promise));
        }

        // Await the session restart completion signal (promise fulfillment) on a worker thread
        // and then chain secondary stream start back to the BTIF main thread without deadlock.
        std::thread([future = std::move(future), bd_addr, primary, callbacks, generation]() mutable {
            if (future.valid()) {
                auto status = future.wait_for(std::chrono::milliseconds(1000));
                if (status == std::future_status::timeout) {
                    log::warn("btif_a2dp_sharing: restart_session future wait timed out after 1000ms");
                }
            }
            do_in_main_thread(base::BindOnce([](uint32_t gen, const RawAddress& secondary, const RawAddress& prim, btav_sharing_callbacks_t* cb) {
                {
                    std::lock_guard<std::mutex> lock(s_sharing_mutex);
                    if (gen != s_sharing_generation || !s_in_sharing_mode) {
                        log::warn("btif_a2dp_sharing: stale enter continuation dropped (gen={}, current_gen={})",
                                  gen, s_sharing_generation);
                        return;
                    }
                }

                // Audio/video sync now waits for the later of the two sinks
                bluetooth::audio::a2dp::set_remote_delay(btif_av_get_audio_delay(A2dpType::kSource));

                // Handle active streaming (kStateStarted)
                if (btif_av_stream_started_ready(A2dpType::kSource)) {
                    log::info("btif_a2dp_sharing: primary stream active, starting stream for secondary peer {}", secondary);
                    btif_av_source_start_sharing_stream(secondary);
                } else {
                    log::info("btif_a2dp_sharing: primary stream idle; secondary stream start deferred to playback dispatch");
                }

                // Fire STARTED callback on JNI callback thread
                if (cb && cb->on_sharing_state_changed) {
                    do_in_jni_thread(base::BindOnce(cb->on_sharing_state_changed, A2DP_SHARING_STATE_STARTED, prim, secondary));
                }
            }, generation, bd_addr, primary, callbacks));
        }).detach();
    } else {
        log::info("btif_a2dp_sharing: primary empty, starting secondary stream directly for {}", bd_addr);
        btif_av_source_start_sharing_stream(bd_addr);

        if (callbacks && callbacks->on_sharing_state_changed) {
            do_in_jni_thread(base::BindOnce(callbacks->on_sharing_state_changed, A2DP_SHARING_STATE_STARTED, primary, bd_addr));
        }
    }
}

static void btif_a2dp_sharing_exit_main_thread(const RawAddress& disconnected_peer) {
    log::info("btif_a2dp_sharing_exit_main_thread: disconnected_peer={}", disconnected_peer);
    RawAddress old_secondary = RawAddress::kEmpty;
    RawAddress primary = RawAddress::kEmpty;
    btav_sharing_callbacks_t* callbacks = nullptr;

    {
        std::lock_guard<std::mutex> lock(s_sharing_mutex);
        ++s_sharing_generation; // Invalidate any in-flight enter continuation
        callbacks = s_callbacks;

        if (!s_in_sharing_mode && s_secondary_address.IsEmpty()) {
            log::info("btif_a2dp_sharing_exit_main_thread: already exited, acknowledging STOPPED");
            if (callbacks && callbacks->on_sharing_state_changed) {
                do_in_jni_thread(base::BindOnce(callbacks->on_sharing_state_changed,
                                                 A2DP_SHARING_STATE_STOPPED,
                                                 RawAddress::kEmpty, RawAddress::kEmpty));
            }
            return;
        }
        s_in_sharing_mode = false;
        set_sharing_active_prop(false);
        old_secondary = s_secondary_address;
        primary = s_primary_address;
        s_secondary_address = RawAddress::kEmpty;
        s_primary_address = RawAddress::kEmpty;
    }

    if (!old_secondary.IsEmpty() && old_secondary != disconnected_peer) {
        log::info("btif_a2dp_sharing: stopping secondary peer stream {}", old_secondary);
        btif_av_source_stop_sharing_stream(old_secondary);
    }

    // Upshift: Restart source session for surviving active device to restore Hardware Offload or uncap SBC bitpool
    RawAddress current_active = btif_av_source_active_peer();
    if (!current_active.IsEmpty()) {
        std::promise<void> promise;
        if (btif_a2dp_sharing_offload_probe()) {
            log::info("btif_a2dp_sharing: offload probe, session left as it is");
        } else if (btif_av_is_a2dp_offload_enabled()) {
            log::info("btif_a2dp_sharing: restarting session for surviving active device {} to restore full capabilities",
                      current_active);
            btif_a2dp_source_restart_session(current_active, current_active, std::move(promise));
        } else {
            log::info("btif_a2dp_sharing: re-initializing the encoder for surviving active device {}", current_active);
            btif_a2dp_source_reinit_encoder(current_active, std::move(promise));
        }
    }

    // Back to the active sink's own delay
    bluetooth::audio::a2dp::set_remote_delay(btif_av_get_audio_delay(A2dpType::kSource));

    // Invoke state callback on JNI thread without holding s_sharing_mutex
    if (callbacks && callbacks->on_sharing_state_changed) {
        do_in_jni_thread(base::BindOnce(callbacks->on_sharing_state_changed, A2DP_SHARING_STATE_STOPPED, primary, old_secondary));
    }
}

static bool enter_sharing(const RawAddress& bd_addr) {
    log::info("btif_a2dp_sharing: enter_sharing dispatched to main thread for {}", bd_addr);
    if (do_in_main_thread(base::BindOnce(&btif_a2dp_sharing_enter_main_thread, bd_addr)) != BT_STATUS_SUCCESS) {
        log::error("btif_a2dp_sharing: failed to dispatch enter_sharing to main thread");
        return false;
    }
    return true;
}

static bool exit_sharing(void) {
    log::info("btif_a2dp_sharing: exit_sharing dispatched to main thread");
    if (do_in_main_thread(base::BindOnce(&btif_a2dp_sharing_exit_main_thread, RawAddress::kEmpty)) != BT_STATUS_SUCCESS) {
        log::error("btif_a2dp_sharing: failed to dispatch exit_sharing to main thread");
        return false;
    }
    return true;
}

static bool set_sharing_volume(const RawAddress& bd_addr, int volume) {
    btav_sharing_callbacks_t* callbacks = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_sharing_mutex);
        callbacks = s_callbacks;
    }
    log::info("btif_a2dp_sharing: set_sharing_volume for {} to {}", bd_addr, volume);
    if (callbacks && callbacks->on_volume_changed) {
        do_in_jni_thread(base::BindOnce(callbacks->on_volume_changed, bd_addr, volume));
    }
    return true;
}

static const btav_sharing_interface_t s_a2dp_sharing_interface = {
    .size = sizeof(btav_sharing_interface_t),
    .init = init,
    .cleanup = cleanup,
    .enter_sharing = enter_sharing,
    .exit_sharing = exit_sharing,
    .set_sharing_volume = set_sharing_volume,
};

const btav_sharing_interface_t* btif_a2dp_sharing_get_interface(void) {
    return &s_a2dp_sharing_interface;
}
