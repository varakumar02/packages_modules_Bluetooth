/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * aptX-adaptive Codec Information Element handling and the R2.1 capability
 * negotiation follow Qualcomm's stack: Copyright (c) 2018, The Linux
 * Foundation. All rights reserved. Redistribution and use permitted under the
 * BSD-3-Clause-Clear terms in a2dp_vendor_aptx_adaptive_constants.h.
 */

/******************************************************************************
 *
 *  Utility functions to help build and parse the aptX-adaptive Codec
 *  Information Element. The codec is offload only: the audio DSP encodes it
 *  behind the Qualcomm Bluetooth audio HAL, so the encoder interface below is a
 *  stub that keeps btif_a2dp_source_setup_codec() going.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include "a2dp_vendor_aptx_adaptive.h"

#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

#include "a2dp_api.h"
#include "a2dp_codec_api.h"
#include "a2dp_constants.h"
#include "a2dp_vendor.h"
#include "a2dp_vendor_aptx_adaptive_constants.h"
#include "avdt_api.h"
#include "hardware/bt_av.h"
#include "internal_include/bt_trace.h"
#include "osi/include/properties.h"
#include "stack/include/bt_hdr.h"

using namespace bluetooth;

#define APTX_ADAPTIVE_VENDOR_DATA(features)                                                  \
  {A2DP_APTX_ADAPTIVE_TTP_LL_0,          A2DP_APTX_ADAPTIVE_TTP_LL_1,                       \
   A2DP_APTX_ADAPTIVE_TTP_HQ_0,          A2DP_APTX_ADAPTIVE_TTP_HQ_1,                       \
   A2DP_APTX_ADAPTIVE_TTP_TWS_0,         A2DP_APTX_ADAPTIVE_TTP_TWS_1,                      \
   A2DP_APTX_ADAPTIVE_RESERVED_15THBYTE, A2DP_APTX_ADAPTIVE_CAP_EXT_VER_NUM,                \
   (features),                           A2DP_APTX_ADAPTIVE_FIRST_SETUP_PREF,               \
   A2DP_APTX_ADAPTIVE_SECOND_SETUP_PREF, A2DP_APTX_ADAPTIVE_THIRD_SETUP_PREF,               \
   A2DP_APTX_ADAPTIVE_FOURTH_SETUP_PREF, A2DP_APTX_ADAPTIVE_EOC0,                           \
   A2DP_APTX_ADAPTIVE_EOC1}

#define APTX_ADAPTIVE_CHANNELS                                                       \
  (A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_STEREO |  \
   A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO | \
   A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS)

/* aptX-adaptive R2.1 offload capabilities (Qualcomm's r2_1_offload_caps) */
static const tA2DP_APTX_ADAPTIVE_CIE a2dp_aptx_adaptive_source_caps = {
        A2DP_APTX_ADAPTIVE_VENDOR_ID,          /* vendorId */
        A2DP_APTX_ADAPTIVE_CODEC_ID_BLUETOOTH, /* codecId */
        (A2DP_APTX_ADAPTIVE_SAMPLERATE_48000 | A2DP_APTX_ADAPTIVE_SAMPLERATE_96000), /* sampleRate */
        A2DP_APTX_ADAPTIVE_SOURCE_TYPE_1,                                           /* sourceType */
        APTX_ADAPTIVE_CHANNELS,                                                     /* channelMode */
        APTX_ADAPTIVE_VENDOR_DATA(A2DP_APTX_ADAPTIVE_R2_1_SUPPORTED_FEATURES),
        BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24 /* bits_per_sample */
};

/* Default aptX-adaptive codec configuration */
static const tA2DP_APTX_ADAPTIVE_CIE a2dp_aptx_adaptive_default_config = {
        A2DP_APTX_ADAPTIVE_VENDOR_ID,          /* vendorId */
        A2DP_APTX_ADAPTIVE_CODEC_ID_BLUETOOTH, /* codecId */
        A2DP_APTX_ADAPTIVE_SAMPLERATE_48000,   /* sampleRate */
        A2DP_APTX_ADAPTIVE_SOURCE_TYPE_1,      /* sourceType */
        APTX_ADAPTIVE_CHANNELS,                /* channelMode */
        APTX_ADAPTIVE_VENDOR_DATA(A2DP_APTX_ADAPTIVE_R2_1_SUPPORTED_FEATURES),
        BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24 /* bits_per_sample */
};

// Offload only: nothing is encoded on the host
static void a2dp_vendor_aptx_adaptive_encoder_init(const tA2DP_ENCODER_INIT_PEER_PARAMS*,
                                                   A2dpCodecConfig*, a2dp_source_read_callback_t,
                                                   a2dp_source_enqueue_callback_t) {}
static void a2dp_vendor_aptx_adaptive_noop(void) {}
static uint64_t a2dp_vendor_aptx_adaptive_get_encoder_interval_ms(void) { return 20; }
static int a2dp_vendor_aptx_adaptive_get_effective_frame_size(void) { return 0; }
static void a2dp_vendor_aptx_adaptive_send_frames(uint64_t) {}

static const tA2DP_ENCODER_INTERFACE a2dp_encoder_interface_aptx_adaptive = {
        a2dp_vendor_aptx_adaptive_encoder_init,
        a2dp_vendor_aptx_adaptive_noop,  // encoder_cleanup
        a2dp_vendor_aptx_adaptive_noop,  // feeding_reset
        a2dp_vendor_aptx_adaptive_noop,  // feeding_flush
        a2dp_vendor_aptx_adaptive_get_encoder_interval_ms,
        a2dp_vendor_aptx_adaptive_get_effective_frame_size,
        a2dp_vendor_aptx_adaptive_send_frames,
        nullptr  // set_transmit_queue_length
};

// Builds the aptX-adaptive Media Codec Capabilities byte sequence beginning
// from the LOSC octet. |media_type| is the media type |AVDT_MEDIA_TYPE_*|.
// The result is stored in |p_result|, A2DP_APTX_ADAPTIVE_CODEC_LEN + 1 bytes.
static bool A2DP_BuildInfoAptxAdaptive(uint8_t media_type, const tA2DP_APTX_ADAPTIVE_CIE* p_ie,
                                       uint8_t* p_result) {
  if (p_ie == NULL || p_result == NULL) {
    return false;
  }

  uint8_t* p_end = p_result + 1 + A2DP_APTX_ADAPTIVE_CODEC_LEN;
  *p_result++ = A2DP_APTX_ADAPTIVE_CODEC_LEN;
  *p_result++ = (media_type << 4);
  *p_result++ = A2DP_MEDIA_CT_NON_A2DP;
  *p_result++ = (uint8_t)(p_ie->vendorId & 0x000000FF);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0x0000FF00) >> 8);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0x00FF0000) >> 16);
  *p_result++ = (uint8_t)((p_ie->vendorId & 0xFF000000) >> 24);
  *p_result++ = (uint8_t)(p_ie->codecId & 0x00FF);
  *p_result++ = (uint8_t)((p_ie->codecId & 0xFF00) >> 8);
  *p_result++ = p_ie->sampleRate | p_ie->sourceType;
  *p_result++ = p_ie->channelMode;
  memcpy(p_result, &p_ie->aptx_data, A2DP_APTX_ADAPTIVE_VENDOR_DATA_LEN);
  p_result += A2DP_APTX_ADAPTIVE_VENDOR_DATA_LEN;
  memset(p_result, 0, p_end - p_result);  // reserved for future extension

  return true;
}

// Parses the aptX-adaptive Media Codec Capabilities byte sequence beginning
// from the LOSC octet. The result is stored in |p_ie|. If |is_capability| is
// true, the byte sequence is codec capabilities, otherwise codec configuration.
static tA2DP_STATUS A2DP_ParseInfoAptxAdaptive(tA2DP_APTX_ADAPTIVE_CIE* p_ie,
                                               const uint8_t* p_codec_info, bool is_capability) {
  if (p_ie == NULL || p_codec_info == NULL) {
    return AVDTP_UNSUPPORTED_CONFIGURATION;
  }

  // Check the codec capability length
  uint8_t losc = *p_codec_info++;
  if (losc != A2DP_APTX_ADAPTIVE_CODEC_LEN) {
    return AVDTP_UNSUPPORTED_CONFIGURATION;
  }

  uint8_t media_type = (*p_codec_info++) >> 4;
  tA2DP_CODEC_TYPE codec_type = static_cast<tA2DP_CODEC_TYPE>(*p_codec_info++);
  /* Check the Media Type and Media Codec Type */
  if (media_type != AVDT_MEDIA_TYPE_AUDIO || codec_type != A2DP_MEDIA_CT_NON_A2DP) {
    return AVDTP_UNSUPPORTED_CONFIGURATION;
  }

  // Check the Vendor ID and Codec ID
  p_ie->vendorId = (*p_codec_info & 0x000000FF) | (*(p_codec_info + 1) << 8 & 0x0000FF00) |
                   (*(p_codec_info + 2) << 16 & 0x00FF0000) |
                   (*(p_codec_info + 3) << 24 & 0xFF000000);
  p_codec_info += 4;
  p_ie->codecId = (*p_codec_info & 0x00FF) | (*(p_codec_info + 1) << 8 & 0xFF00);
  p_codec_info += 2;
  if (p_ie->vendorId != A2DP_APTX_ADAPTIVE_VENDOR_ID ||
      p_ie->codecId != A2DP_APTX_ADAPTIVE_CODEC_ID_BLUETOOTH) {
    return AVDTP_UNSUPPORTED_CONFIGURATION;
  }

  p_ie->sourceType = *p_codec_info & 0x07;
  p_ie->sampleRate = *p_codec_info & 0xF8;
  p_codec_info++;
  p_ie->channelMode = *p_codec_info & 0x3F;
  p_codec_info++;
  memset(&p_ie->aptx_data, 0, sizeof(p_ie->aptx_data));
  memcpy(&p_ie->aptx_data, p_codec_info, A2DP_APTX_ADAPTIVE_VENDOR_DATA_LEN);
  p_ie->bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24;

  if (is_capability) {
    if (A2DP_BitsSet(p_ie->sampleRate) == A2DP_SET_ZERO_BIT) {
      return A2DP_INVALID_SAMPLING_FREQUENCY;
    }
    if (A2DP_BitsSet(p_ie->channelMode) == A2DP_SET_ZERO_BIT) {
      return A2DP_INVALID_CHANNEL_MODE;
    }
    return A2DP_SUCCESS;
  }

  if (A2DP_BitsSet(p_ie->channelMode) != A2DP_SET_ONE_BIT) {
    return A2DP_INVALID_CHANNEL_MODE;
  }

  return A2DP_SUCCESS;
}

bool A2DP_GetAptxAdaptiveCIE(const uint8_t* p_codec_info, tA2DP_APTX_ADAPTIVE_CIE* p_cie) {
  return A2DP_ParseInfoAptxAdaptive(p_cie, p_codec_info, false) == A2DP_SUCCESS;
}

bool A2DP_IsCodecValidAptxAdaptive(const uint8_t* p_codec_info) {
  tA2DP_APTX_ADAPTIVE_CIE cfg_cie;

  /* Use a liberal check when parsing the codec info */
  return (A2DP_ParseInfoAptxAdaptive(&cfg_cie, p_codec_info, false) == A2DP_SUCCESS) ||
         (A2DP_ParseInfoAptxAdaptive(&cfg_cie, p_codec_info, true) == A2DP_SUCCESS);
}

bool A2DP_VendorUsesRtpHeaderAptxAdaptive(bool /* content_protection_enabled */,
                                          const uint8_t* /* p_codec_info */) {
  return true;
}

const char* A2DP_VendorCodecNameAptxAdaptive(const uint8_t* /* p_codec_info */) {
  return "aptX-adaptive";
}

bool A2DP_VendorCodecTypeEqualsAptxAdaptive(const uint8_t* p_codec_info_a,
                                            const uint8_t* p_codec_info_b) {
  tA2DP_APTX_ADAPTIVE_CIE cie_a;
  tA2DP_APTX_ADAPTIVE_CIE cie_b;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie_a, p_codec_info_a, true);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return false;
  }
  a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie_b, p_codec_info_b, true);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return false;
  }

  return true;
}

bool A2DP_VendorCodecEqualsAptxAdaptive(const uint8_t* p_codec_info_a,
                                        const uint8_t* p_codec_info_b) {
  tA2DP_APTX_ADAPTIVE_CIE cie_a;
  tA2DP_APTX_ADAPTIVE_CIE cie_b;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie_a, p_codec_info_a, true);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return false;
  }
  a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie_b, p_codec_info_b, true);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return false;
  }

  return (cie_a.sampleRate == cie_b.sampleRate) && (cie_a.channelMode == cie_b.channelMode);
}

int A2DP_VendorGetBitRateAptxAdaptive(const uint8_t* /* p_codec_info */) {
  // Adaptive bit rate, chosen by the encoder in the audio DSP
  return 0;
}

int A2DP_VendorGetTrackSampleRateAptxAdaptive(const uint8_t* p_codec_info) {
  tA2DP_APTX_ADAPTIVE_CIE cie;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie, p_codec_info, false);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return -1;
  }

  if (cie.sampleRate == A2DP_APTX_ADAPTIVE_SAMPLERATE_44100) {
    return 44100;
  }
  if (cie.sampleRate == A2DP_APTX_ADAPTIVE_SAMPLERATE_48000) {
    return 48000;
  }
  if (cie.sampleRate == A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) {
    return 96000;
  }

  return -1;
}

int A2DP_VendorGetTrackBitsPerSampleAptxAdaptive(const uint8_t* p_codec_info) {
  tA2DP_APTX_ADAPTIVE_CIE cie;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie, p_codec_info, false);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return -1;
  }

  return 24;
}

int A2DP_VendorGetTrackChannelCountAptxAdaptive(const uint8_t* p_codec_info) {
  tA2DP_APTX_ADAPTIVE_CIE cie;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie, p_codec_info, false);
  if (a2dp_status != A2DP_SUCCESS) {
    log::error("cannot decode codec information: {}", a2dp_status);
    return -1;
  }

  switch (cie.channelMode) {
    case A2DP_APTX_ADAPTIVE_CHANNELS_MONO:
    case A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO:
      return 1;
    case A2DP_APTX_ADAPTIVE_CHANNELS_STEREO:
    case A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO:
    case A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO:
    case A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS:
      return 2;
  }

  return -1;
}

bool A2DP_VendorGetPacketTimestampAptxAdaptive(const uint8_t* /* p_codec_info */,
                                               const uint8_t* p_data, uint32_t* p_timestamp) {
  *p_timestamp = *(const uint32_t*)p_data;
  return true;
}

bool A2DP_VendorBuildCodecHeaderAptxAdaptive(const uint8_t* /* p_codec_info */,
                                             BT_HDR* /* p_buf */,
                                             uint16_t /* frames_per_packet */) {
  // Nothing to do
  return true;
}

std::string A2DP_VendorCodecInfoStringAptxAdaptive(const uint8_t* p_codec_info) {
  std::stringstream res;
  std::string field;
  tA2DP_APTX_ADAPTIVE_CIE cie;

  tA2DP_STATUS a2dp_status = A2DP_ParseInfoAptxAdaptive(&cie, p_codec_info, true);
  if (a2dp_status != A2DP_SUCCESS) {
    res << "A2DP_ParseInfoAptxAdaptive fail: " << loghex(static_cast<uint8_t>(a2dp_status));
    return res.str();
  }

  res << "\tname: aptX-adaptive\n";

  field.clear();
  AppendField(&field, (cie.sampleRate == 0), "NONE");
  AppendField(&field, (cie.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_44100), "44100");
  AppendField(&field, (cie.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_48000), "48000");
  AppendField(&field, (cie.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_96000), "96000");
  res << "\tsamp_freq: " << field << " (" << loghex(cie.sampleRate) << ")\n";

  field.clear();
  AppendField(&field, (cie.channelMode == 0), "NONE");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_MONO), "Mono");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_STEREO), "Stereo");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO), "TWS Stereo");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO),
              "Joint Stereo");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO), "TWS Mono");
  AppendField(&field, (cie.channelMode & A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS), "TWS+");
  res << "\tch_mode: " << field << " (" << loghex(cie.channelMode) << ")\n";

  res << "\tcap_ext_ver: " << loghex(cie.aptx_data.cap_ext_ver_num)
      << " features: " << loghex(cie.aptx_data.aptx_adaptive_sup_features) << "\n";

  return res.str();
}

const tA2DP_ENCODER_INTERFACE* A2DP_VendorGetEncoderInterfaceAptxAdaptive(
        const uint8_t* p_codec_info) {
  if (!A2DP_IsCodecValidAptxAdaptive(p_codec_info)) {
    return NULL;
  }

  return &a2dp_encoder_interface_aptx_adaptive;
}

bool A2DP_VendorAdjustCodecAptxAdaptive(uint8_t* p_codec_info) {
  tA2DP_APTX_ADAPTIVE_CIE cfg_cie;

  // Nothing to do: just verify the codec info is valid
  return A2DP_ParseInfoAptxAdaptive(&cfg_cie, p_codec_info, true) == A2DP_SUCCESS;
}

btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexAptxAdaptive(
        const uint8_t* /* p_codec_info */) {
  return BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_ADAPTIVE;
}

const char* A2DP_VendorCodecIndexStrAptxAdaptive(void) { return "aptX-adaptive"; }

bool A2DP_VendorInitCodecConfigAptxAdaptive(AvdtpSepConfig* p_cfg) {
  return A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &a2dp_aptx_adaptive_source_caps,
                                    p_cfg->codec_info);
}

A2dpCodecConfigAptxAdaptive::A2dpCodecConfigAptxAdaptive(btav_a2dp_codec_priority_t codec_priority)
    : A2dpCodecConfig(BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_ADAPTIVE,
                      bluetooth::a2dp::CodecId::APTX_ADAPTIVE,
                      A2DP_VendorCodecIndexStrAptxAdaptive(), codec_priority) {
  // Compute the local capability
  if (a2dp_aptx_adaptive_source_caps.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_44100) {
    codec_local_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
  }
  if (a2dp_aptx_adaptive_source_caps.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_48000) {
    codec_local_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
  }
  if (a2dp_aptx_adaptive_source_caps.sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) {
    codec_local_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
  }
  codec_local_capability_.bits_per_sample = a2dp_aptx_adaptive_source_caps.bits_per_sample;
  if (a2dp_aptx_adaptive_source_caps.channelMode &
      (A2DP_APTX_ADAPTIVE_CHANNELS_MONO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO)) {
    codec_local_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_MONO;
  }
  if (a2dp_aptx_adaptive_source_caps.channelMode &
      (A2DP_APTX_ADAPTIVE_CHANNELS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO |
       A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS)) {
    codec_local_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;
  }
}

A2dpCodecConfigAptxAdaptive::~A2dpCodecConfigAptxAdaptive() {}

bool A2dpCodecConfigAptxAdaptive::init() {
  // There is no host encoder: the codec is usable only through A2DP offload
  if (!osi_property_get_bool("ro.bluetooth.a2dp_offload.supported", false) ||
      osi_property_get_bool("persist.bluetooth.a2dp_offload.disabled", false)) {
    log::info("aptX-adaptive needs A2DP offload, disabled");
    return false;
  }
  return true;
}

bool A2dpCodecConfigAptxAdaptive::useRtpHeaderMarkerBit() const { return false; }

//
// Selects the best sample rate from |sampleRate|.
// The result is stored in |p_result| and |p_codec_config|.
//
static bool select_best_sample_rate(uint8_t sampleRate, tA2DP_APTX_ADAPTIVE_CIE* p_result,
                                    btav_a2dp_codec_config_t* p_codec_config) {
  // 96 kHz is two bits (0xA0): both must be set
  if ((sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) == A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) {
    p_result->sampleRate = A2DP_APTX_ADAPTIVE_SAMPLERATE_96000;
    p_codec_config->sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
    return true;
  }
  if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_48000) {
    p_result->sampleRate = A2DP_APTX_ADAPTIVE_SAMPLERATE_48000;
    p_codec_config->sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
    return true;
  }
  if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_44100) {
    p_result->sampleRate = A2DP_APTX_ADAPTIVE_SAMPLERATE_44100;
    p_codec_config->sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
    return true;
  }
  return false;
}

//
// Selects the sample rate of |requested| if |sampleRate| allows it.
//
static bool select_sample_rate(btav_a2dp_codec_sample_rate_t requested, uint8_t sampleRate,
                               tA2DP_APTX_ADAPTIVE_CIE* p_result,
                               btav_a2dp_codec_config_t* p_codec_config) {
  uint8_t ie_rate;
  switch (requested) {
    case BTAV_A2DP_CODEC_SAMPLE_RATE_44100:
      ie_rate = A2DP_APTX_ADAPTIVE_SAMPLERATE_44100;
      break;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_48000:
      ie_rate = A2DP_APTX_ADAPTIVE_SAMPLERATE_48000;
      break;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_96000:
      ie_rate = A2DP_APTX_ADAPTIVE_SAMPLERATE_96000;
      break;
    default:
      return false;
  }
  if ((sampleRate & ie_rate) != ie_rate) {
    return false;
  }
  p_result->sampleRate = ie_rate;
  p_codec_config->sample_rate = requested;
  return true;
}

//
// Selects the best channel mode from |channelMode|, in Qualcomm's order.
//
static bool select_best_channel_mode(uint8_t channelMode, tA2DP_APTX_ADAPTIVE_CIE* p_result,
                                     btav_a2dp_codec_config_t* p_codec_config) {
  static const struct {
    uint8_t ie_mode;
    btav_a2dp_codec_channel_mode_t mode;
  } kOrder[] = {
          {A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS, BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO},
          {A2DP_APTX_ADAPTIVE_CHANNELS_STEREO, BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO},
          {A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO, BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO},
          {A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO, BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO},
          {A2DP_APTX_ADAPTIVE_CHANNELS_MONO, BTAV_A2DP_CODEC_CHANNEL_MODE_MONO},
          {A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO, BTAV_A2DP_CODEC_CHANNEL_MODE_MONO},
  };
  for (const auto& entry : kOrder) {
    if (channelMode & entry.ie_mode) {
      p_result->channelMode = entry.ie_mode;
      p_codec_config->channel_mode = entry.mode;
      return true;
    }
  }
  return false;
}

//
// Selects a channel mode of kind |requested| (mono / stereo) from |channelMode|.
//
static bool select_channel_mode(btav_a2dp_codec_channel_mode_t requested, uint8_t channelMode,
                                tA2DP_APTX_ADAPTIVE_CIE* p_result,
                                btav_a2dp_codec_config_t* p_codec_config) {
  uint8_t allowed;
  switch (requested) {
    case BTAV_A2DP_CODEC_CHANNEL_MODE_MONO:
      allowed = A2DP_APTX_ADAPTIVE_CHANNELS_MONO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO;
      break;
    case BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO:
      allowed = A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS | A2DP_APTX_ADAPTIVE_CHANNELS_STEREO |
                A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO;
      break;
    default:
      return false;
  }
  return select_best_channel_mode(channelMode & allowed, p_result, p_codec_config);
}

// Fills the R2 vendor data of the result configuration from the sink's
// capability, as Qualcomm's stack does for R2.1: byte 17 (low byte of the
// supported features) keeps the OR of both upper nibbles and the AND of both
// lower nibbles; a sink without capability extension keeps its own data.
static void negotiate_vendor_data(const tA2DP_APTX_ADAPTIVE_CIE& sink,
                                  tA2DP_APTX_ADAPTIVE_CIE* p_result) {
  if (sink.aptx_data.cap_ext_ver_num == 0) {
    p_result->aptx_data = sink.aptx_data;
    return;
  }
  uint8_t sink_byte = sink.aptx_data.aptx_adaptive_sup_features & 0xFF;
  uint8_t src_byte = a2dp_aptx_adaptive_source_caps.aptx_data.aptx_adaptive_sup_features & 0xFF;
  uint8_t negotiated =
          (((sink_byte >> 4) | (src_byte >> 4)) << 4) | ((sink_byte & src_byte) & 0x0F);
  p_result->aptx_data = a2dp_aptx_adaptive_source_caps.aptx_data;
  p_result->aptx_data.aptx_adaptive_sup_features =
          (sink.aptx_data.aptx_adaptive_sup_features & 0xFFFFFF00) | negotiated;
}

tA2DP_STATUS A2dpCodecConfigAptxAdaptive::setCodecConfig(const uint8_t* p_peer_codec_info,
                                                         bool is_capability,
                                                         uint8_t* p_result_codec_config) {
  std::lock_guard<std::recursive_mutex> lock(codec_mutex_);
  tA2DP_APTX_ADAPTIVE_CIE peer_info_cie;
  tA2DP_APTX_ADAPTIVE_CIE result_config_cie;
  uint8_t sampleRate;
  uint8_t channelMode;

  // Save the internal state
  btav_a2dp_codec_config_t saved_codec_config = codec_config_;
  btav_a2dp_codec_config_t saved_codec_selectable_capability = codec_selectable_capability_;
  btav_a2dp_codec_config_t saved_codec_user_config = codec_user_config_;
  btav_a2dp_codec_config_t saved_codec_audio_config = codec_audio_config_;
  uint8_t saved_ota_codec_config[AVDT_CODEC_SIZE];
  uint8_t saved_ota_codec_peer_capability[AVDT_CODEC_SIZE];
  uint8_t saved_ota_codec_peer_config[AVDT_CODEC_SIZE];
  memcpy(saved_ota_codec_config, ota_codec_config_, sizeof(ota_codec_config_));
  memcpy(saved_ota_codec_peer_capability, ota_codec_peer_capability_,
         sizeof(ota_codec_peer_capability_));
  memcpy(saved_ota_codec_peer_config, ota_codec_peer_config_, sizeof(ota_codec_peer_config_));

  tA2DP_STATUS status =
          A2DP_ParseInfoAptxAdaptive(&peer_info_cie, p_peer_codec_info, is_capability);
  if (status != A2DP_SUCCESS) {
    log::error("can't parse peer's capabilities: error = {}", status);
    goto fail;
  }

  //
  // Build the preferred configuration
  //
  memset(&result_config_cie, 0, sizeof(result_config_cie));
  result_config_cie.vendorId = a2dp_aptx_adaptive_source_caps.vendorId;
  result_config_cie.codecId = a2dp_aptx_adaptive_source_caps.codecId;
  result_config_cie.sourceType = a2dp_aptx_adaptive_source_caps.sourceType;
  result_config_cie.bits_per_sample = a2dp_aptx_adaptive_source_caps.bits_per_sample;
  negotiate_vendor_data(peer_info_cie, &result_config_cie);

  //
  // Select the sample frequency
  //
  sampleRate = a2dp_aptx_adaptive_source_caps.sampleRate & peer_info_cie.sampleRate;
  codec_config_.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_NONE;
  select_sample_rate(codec_user_config_.sample_rate, sampleRate, &result_config_cie,
                     &codec_config_);

  // Select the sample frequency if there is no user preference
  do {
    // Compute the selectable capability
    if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_44100) {
      codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
    }
    if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_48000) {
      codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
    }
    if ((sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) == A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) {
      codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
    }

    if (codec_config_.sample_rate != BTAV_A2DP_CODEC_SAMPLE_RATE_NONE) {
      break;
    }

    // No user preference - try the codec audio config
    if (select_sample_rate(codec_audio_config_.sample_rate, sampleRate, &result_config_cie,
                           &codec_config_)) {
      break;
    }

    // No user preference - try the default config
    if (select_best_sample_rate(a2dp_aptx_adaptive_default_config.sampleRate & sampleRate,
                                &result_config_cie, &codec_config_)) {
      break;
    }

    // No user preference - use the best match
    if (select_best_sample_rate(sampleRate, &result_config_cie, &codec_config_)) {
      break;
    }
  } while (false);
  if (codec_config_.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_NONE) {
    log::error("cannot match sample frequency: local caps = 0x{:x} peer info = 0x{:x}",
               a2dp_aptx_adaptive_source_caps.sampleRate, peer_info_cie.sampleRate);
    status = A2DP_NOT_SUPPORTED_SAMPLING_FREQUENCY;
    goto fail;
  }

  //
  // Select the bits per sample: always 24 bit for aptX-adaptive offload, not
  // included in the codec description sent OTA.
  //
  codec_selectable_capability_.bits_per_sample = a2dp_aptx_adaptive_source_caps.bits_per_sample;
  codec_config_.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24;

  //
  // Select the channel mode
  //
  channelMode = a2dp_aptx_adaptive_source_caps.channelMode & peer_info_cie.channelMode;
  codec_config_.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_NONE;
  select_channel_mode(codec_user_config_.channel_mode, channelMode, &result_config_cie,
                      &codec_config_);

  // Select the channel mode if there is no user preference
  do {
    // Compute the selectable capability
    if (channelMode & (A2DP_APTX_ADAPTIVE_CHANNELS_MONO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO)) {
      codec_selectable_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_MONO;
    }
    if (channelMode &
        (A2DP_APTX_ADAPTIVE_CHANNELS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO |
         A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS)) {
      codec_selectable_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;
    }

    if (codec_config_.channel_mode != BTAV_A2DP_CODEC_CHANNEL_MODE_NONE) {
      break;
    }

    // No user preference - try the codec audio config
    if (select_channel_mode(codec_audio_config_.channel_mode, channelMode, &result_config_cie,
                            &codec_config_)) {
      break;
    }

    // No user preference - try the default config
    if (select_best_channel_mode(a2dp_aptx_adaptive_default_config.channelMode & channelMode,
                                 &result_config_cie, &codec_config_)) {
      break;
    }

    // No user preference - use the best match
    if (select_best_channel_mode(channelMode, &result_config_cie, &codec_config_)) {
      break;
    }
  } while (false);
  if (codec_config_.channel_mode == BTAV_A2DP_CODEC_CHANNEL_MODE_NONE) {
    log::error("cannot match channel mode: local caps = 0x{:x} peer info = 0x{:x}",
               a2dp_aptx_adaptive_source_caps.channelMode, peer_info_cie.channelMode);
    status = A2DP_NOT_SUPPORTED_CHANNEL_MODE;
    goto fail;
  }

  if (!A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &result_config_cie,
                                  p_result_codec_config)) {
    status = AVDTP_UNSUPPORTED_CONFIGURATION;
    goto fail;
  }

  //
  // Copy the codec-specific fields if they are not zero
  //
  if (codec_user_config_.codec_specific_1 != 0) {
    codec_config_.codec_specific_1 = codec_user_config_.codec_specific_1;
  }
  if (codec_user_config_.codec_specific_2 != 0) {
    codec_config_.codec_specific_2 = codec_user_config_.codec_specific_2;
  }
  if (codec_user_config_.codec_specific_3 != 0) {
    codec_config_.codec_specific_3 = codec_user_config_.codec_specific_3;
  }
  if (codec_user_config_.codec_specific_4 != 0) {
    codec_config_.codec_specific_4 = codec_user_config_.codec_specific_4;
  }

  // Create a local copy of the peer codec capability/config, and the
  // result codec config.
  if (is_capability) {
    log::assert_that(A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &peer_info_cie,
                                                ota_codec_peer_capability_),
                     "Failed to build media codec capabilities");
  } else {
    log::assert_that(A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &peer_info_cie,
                                                ota_codec_peer_config_),
                     "Failed to build media codec capabilities");
  }
  log::assert_that(
          A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &result_config_cie, ota_codec_config_),
          "Failed to build media codec capabilities");
  return A2DP_SUCCESS;

fail:
  // Restore the internal state
  codec_config_ = saved_codec_config;
  codec_selectable_capability_ = saved_codec_selectable_capability;
  codec_user_config_ = saved_codec_user_config;
  codec_audio_config_ = saved_codec_audio_config;
  memcpy(ota_codec_config_, saved_ota_codec_config, sizeof(ota_codec_config_));
  memcpy(ota_codec_peer_capability_, saved_ota_codec_peer_capability,
         sizeof(ota_codec_peer_capability_));
  memcpy(ota_codec_peer_config_, saved_ota_codec_peer_config, sizeof(ota_codec_peer_config_));
  return status;
}

bool A2dpCodecConfigAptxAdaptive::setPeerCodecCapabilities(
        const uint8_t* p_peer_codec_capabilities) {
  std::lock_guard<std::recursive_mutex> lock(codec_mutex_);
  tA2DP_APTX_ADAPTIVE_CIE peer_info_cie;
  uint8_t sampleRate;
  uint8_t channelMode;

  // Save the internal state
  btav_a2dp_codec_config_t saved_codec_selectable_capability = codec_selectable_capability_;
  uint8_t saved_ota_codec_peer_capability[AVDT_CODEC_SIZE];
  memcpy(saved_ota_codec_peer_capability, ota_codec_peer_capability_,
         sizeof(ota_codec_peer_capability_));

  tA2DP_STATUS status =
          A2DP_ParseInfoAptxAdaptive(&peer_info_cie, p_peer_codec_capabilities, true);
  if (status != A2DP_SUCCESS) {
    log::error("can't parse peer's capabilities: error = {}", status);
    goto fail;
  }

  // Compute the selectable capability - sample rate
  sampleRate = a2dp_aptx_adaptive_source_caps.sampleRate & peer_info_cie.sampleRate;
  if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_44100) {
    codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
  }
  if (sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_48000) {
    codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
  }
  if ((sampleRate & A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) == A2DP_APTX_ADAPTIVE_SAMPLERATE_96000) {
    codec_selectable_capability_.sample_rate |= BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
  }

  // Compute the selectable capability - bits per sample
  codec_selectable_capability_.bits_per_sample = a2dp_aptx_adaptive_source_caps.bits_per_sample;

  // Compute the selectable capability - channel mode
  channelMode = a2dp_aptx_adaptive_source_caps.channelMode & peer_info_cie.channelMode;
  if (channelMode & (A2DP_APTX_ADAPTIVE_CHANNELS_MONO | A2DP_APTX_ADAPTIVE_CHANNELS_TWS_MONO)) {
    codec_selectable_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_MONO;
  }
  if (channelMode & (A2DP_APTX_ADAPTIVE_CHANNELS_STEREO | A2DP_APTX_ADAPTIVE_CHANNELS_JOINT_STEREO |
                     A2DP_APTX_ADAPTIVE_CHANNELS_TWS_STEREO |
                     A2DP_APTX_ADAPTIVE_CHANNELS_TWS_PLUS)) {
    codec_selectable_capability_.channel_mode |= BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;
  }

  log::assert_that(A2DP_BuildInfoAptxAdaptive(AVDT_MEDIA_TYPE_AUDIO, &peer_info_cie,
                                              ota_codec_peer_capability_),
                   "Failed to build media codec capabilities");
  return true;

fail:
  // Restore the internal state
  codec_selectable_capability_ = saved_codec_selectable_capability;
  memcpy(ota_codec_peer_capability_, saved_ota_codec_peer_capability,
         sizeof(ota_codec_peer_capability_));
  return false;
}
