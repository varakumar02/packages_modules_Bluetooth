/*
 * Copyright (C) 2026 LunarisOS
 * OPlus A2DP Sharing Implementation for Fluoride Stack
 */

#define LOG_TAG "btif_a2dp_sharing"

#include "btif_a2dp_sharing.h"
#include "btif_av.h"
#include "btif_a2dp_source.h"
#include "btif_common.h"

#include <bluetooth/log.h>
#include <mutex>
#include <future>
#include <thread>
#include <chrono>
#include <base/functional/bind.h>
#include "stack/include/main_thread.h"

using namespace bluetooth;

static btav_sharing_callbacks_t* s_callbacks = nullptr;
static std::mutex s_sharing_mutex;
static RawAddress s_primary_address = RawAddress::kEmpty;
static RawAddress s_secondary_address = RawAddress::kEmpty;
static bool s_in_sharing_mode = false;

static void btif_a2dp_sharing_exit_main_thread(const RawAddress& disconnected_peer);

bool btif_a2dp_sharing_is_active(void) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    return s_in_sharing_mode && !s_secondary_address.IsEmpty();
}

bool btif_a2dp_sharing_is_sharing_peer(const RawAddress& bd_addr) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    return s_in_sharing_mode && (bd_addr == s_secondary_address);
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
    return BT_STATUS_SUCCESS;
}

static void cleanup(void) {
    std::lock_guard<std::mutex> lock(s_sharing_mutex);
    log::info("btif_a2dp_sharing: cleanup called");
    s_callbacks = nullptr;
    s_primary_address = RawAddress::kEmpty;
    s_secondary_address = RawAddress::kEmpty;
    s_in_sharing_mode = false;
}

static void btif_a2dp_sharing_enter_main_thread(const RawAddress& bd_addr) {
    RawAddress primary = btif_av_source_active_peer();
    log::info("btif_a2dp_sharing_enter_main_thread: primary={}, secondary={}", primary, bd_addr);

    bool offload_was_running = btif_av_is_a2dp_offload_running();

    btav_sharing_callbacks_t* callbacks = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_sharing_mutex);
        s_secondary_address = bd_addr;
        s_in_sharing_mode = true;
        s_primary_address = primary;
        callbacks = s_callbacks;
    }

    if (offload_was_running && !primary.IsEmpty()) {
        log::info("btif_a2dp_sharing: offload active, restarting source session for software SBC transition");
        std::promise<void> promise;
        std::future<void> future = promise.get_future();
        btif_a2dp_source_restart_session(primary, primary, std::move(promise));

        // Await the session restart completion signal (promise fulfillment) on a worker thread
        // and then chain secondary stream start back to the BTIF main thread without deadlock.
        std::thread([future = std::move(future), bd_addr, primary, callbacks]() mutable {
            if (future.valid()) {
                auto status = future.wait_for(std::chrono::milliseconds(1000));
                if (status == std::future_status::timeout) {
                    log::warn("btif_a2dp_sharing: restart_session future wait timed out after 1000ms");
                }
            }
            do_in_main_thread(base::BindOnce([](const RawAddress& secondary, const RawAddress& prim, btav_sharing_callbacks_t* cb) {
                if (!btif_a2dp_sharing_is_active()) {
                    log::warn("btif_a2dp_sharing: sharing aborted before secondary stream start");
                    return;
                }

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
            }, bd_addr, primary, callbacks));
        }).detach();
    } else {
        log::info("btif_a2dp_sharing: software encoding already active, starting secondary stream directly for {}", bd_addr);
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
        if (!s_in_sharing_mode && s_secondary_address.IsEmpty()) {
            log::info("btif_a2dp_sharing_exit_main_thread: already exited, ignoring");
            return;
        }
        s_in_sharing_mode = false;
        old_secondary = s_secondary_address;
        primary = s_primary_address;
        s_secondary_address = RawAddress::kEmpty;
        s_primary_address = RawAddress::kEmpty;
        callbacks = s_callbacks;
    }

    if (!old_secondary.IsEmpty() && old_secondary != disconnected_peer) {
        log::info("btif_a2dp_sharing: stopping secondary peer stream {}", old_secondary);
        btif_av_source_stop_sharing_stream(old_secondary);
    }

    // Do NOT restart source session for current_active!
    // Restarting session mid-playback destroys the Audio HAL stream and freezes playback.
    // The active device continues streaming SBC uninterrupted without needing a session reset.
    RawAddress current_active = btif_av_source_active_peer();
    log::info("btif_a2dp_sharing: exit completed, surviving active device {} maintains continuous playback",
              current_active);

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
