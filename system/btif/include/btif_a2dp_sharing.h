/*
 * Copyright (C) 2026 LunarisOS
 * OPlus A2DP Sharing Interface Definition
 */

#pragma once

#include <hardware/bluetooth.h>
#include <bluetooth/types/address.h>

#define BT_PROFILE_A2DP_SHARING_ID "a2dp_sharing"

typedef enum {
    A2DP_SHARING_STATE_STOPPED = 0,
    A2DP_SHARING_STATE_STARTED = 1,
} btav_sharing_state_t;

typedef struct {
    size_t size;
    void (*on_volume_changed)(const RawAddress& bd_addr, int volume);
    void (*on_sharing_state_changed)(int state, const RawAddress& primary, const RawAddress& secondary);
} btav_sharing_callbacks_t;

typedef struct {
    size_t size;
    bt_status_t (*init)(btav_sharing_callbacks_t* callbacks);
    void (*cleanup)(void);
    bool (*enter_sharing)(const RawAddress& bd_addr);
    bool (*exit_sharing)(void);
    bool (*set_sharing_volume)(const RawAddress& bd_addr, int volume);
} btav_sharing_interface_t;

const btav_sharing_interface_t* btif_a2dp_sharing_get_interface(void);

/* Helper functions for Fluoride AV state machine and offload management */
bool btif_a2dp_sharing_is_active(void);
bool btif_a2dp_sharing_is_sharing_peer(const RawAddress& bd_addr);
RawAddress btif_a2dp_sharing_get_sharing_peer(void);
// persist.bluetooth.a2dp_sharing.offload_probe (aosp, true or qti): share with A2DP offload kept on (test)
bool btif_a2dp_sharing_offload_probe(void);
void btif_a2dp_sharing_on_peer_disconnected(const RawAddress& bd_addr);
// Lip sync hold in ms, -500..500: > 0 holds the sharing sink, < 0 the active sink (see
// btif_a2dp_sharing.cc)
int btif_a2dp_sharing_hold_ms(void);
// While sharing: the A/V sync delay (1/10 ms), the later sink's latency after the hold
int btif_a2dp_sharing_audio_delay(void);
// Recomputes the hold; true when it changed (the reported audio delay changes with it)
bool btif_a2dp_sharing_update_hold(void);
