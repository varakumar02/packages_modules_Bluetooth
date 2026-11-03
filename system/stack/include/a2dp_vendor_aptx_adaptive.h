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
 * The aptX-adaptive Codec Information Element layout below is taken from
 * Qualcomm's stack: Copyright (c) 2018, The Linux Foundation. All rights
 * reserved. Redistribution and use permitted under the BSD-3-Clause-Clear
 * terms in a2dp_vendor_aptx_adaptive_constants.h.
 */

//
// A2DP Codec API for aptX-adaptive (offload only: the encoder runs in the
// audio DSP behind the Qualcomm Bluetooth audio HAL)
//

#ifndef A2DP_VENDOR_APTX_ADAPTIVE_H
#define A2DP_VENDOR_APTX_ADAPTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "a2dp_codec_api.h"
#include "a2dp_vendor_aptx_adaptive_constants.h"
#include "avdt_api.h"
#include "internal_include/bt_target.h"
#include "stack/include/bt_hdr.h"

// aptX-adaptive vendor data: bytes 12..29 of the Codec Information Element
typedef struct {
  uint8_t ttp_ll_0;
  uint8_t ttp_ll_1;
  uint8_t ttp_hq_0;
  uint8_t ttp_hq_1;
  uint8_t ttp_tws_0;
  uint8_t ttp_tws_1;
  uint8_t reserved_15thbyte;
  // R2 capability extension
  uint8_t cap_ext_ver_num;
  uint32_t aptx_adaptive_sup_features;
  uint8_t first_setup_pref;
  uint8_t second_setup_pref;
  uint8_t third_setup_pref;
  uint8_t fourth_setup_pref;
  uint8_t eoc0;
  uint8_t eoc1;
} tA2DP_APTX_ADAPTIVE_VENDOR_DATA;
// 18 bytes on the air; sizeof() is 20 because of the uint32_t alignment padding at the end
#define A2DP_APTX_ADAPTIVE_VENDOR_DATA_LEN 18
static_assert(offsetof(tA2DP_APTX_ADAPTIVE_VENDOR_DATA, eoc1) + 1 ==
                      A2DP_APTX_ADAPTIVE_VENDOR_DATA_LEN,
              "aptX-adaptive vendor data layout");

// aptX-adaptive Codec Information Element
typedef struct {
  uint32_t vendorId;
  uint16_t codecId;
  uint8_t sampleRate;
  uint8_t sourceType;
  uint8_t channelMode;
  tA2DP_APTX_ADAPTIVE_VENDOR_DATA aptx_data;
  btav_a2dp_codec_bits_per_sample_t bits_per_sample;
} tA2DP_APTX_ADAPTIVE_CIE;

class A2dpCodecConfigAptxAdaptive : public A2dpCodecConfig {
public:
  A2dpCodecConfigAptxAdaptive(btav_a2dp_codec_priority_t codec_priority);
  virtual ~A2dpCodecConfigAptxAdaptive();

  bool init() override;
  tA2DP_STATUS setCodecConfig(const uint8_t* p_peer_codec_info, bool is_capability,
                              uint8_t* p_result_codec_config) override;
  bool setPeerCodecCapabilities(const uint8_t* p_peer_codec_capabilities) override;

private:
  bool useRtpHeaderMarkerBit() const override;
};

// Parses |p_codec_info| (a codec configuration) into |p_cie|.
bool A2DP_GetAptxAdaptiveCIE(const uint8_t* p_codec_info, tA2DP_APTX_ADAPTIVE_CIE* p_cie);

bool A2DP_IsCodecValidAptxAdaptive(const uint8_t* p_codec_info);

bool A2DP_VendorUsesRtpHeaderAptxAdaptive(bool content_protection_enabled,
                                          const uint8_t* p_codec_info);

const char* A2DP_VendorCodecNameAptxAdaptive(const uint8_t* p_codec_info);

bool A2DP_VendorCodecTypeEqualsAptxAdaptive(const uint8_t* p_codec_info_a,
                                            const uint8_t* p_codec_info_b);

bool A2DP_VendorCodecEqualsAptxAdaptive(const uint8_t* p_codec_info_a,
                                        const uint8_t* p_codec_info_b);

int A2DP_VendorGetTrackSampleRateAptxAdaptive(const uint8_t* p_codec_info);

int A2DP_VendorGetTrackBitsPerSampleAptxAdaptive(const uint8_t* p_codec_info);

int A2DP_VendorGetBitRateAptxAdaptive(const uint8_t* p_codec_info);

int A2DP_VendorGetTrackChannelCountAptxAdaptive(const uint8_t* p_codec_info);

bool A2DP_VendorGetPacketTimestampAptxAdaptive(const uint8_t* p_codec_info, const uint8_t* p_data,
                                               uint32_t* p_timestamp);

bool A2DP_VendorBuildCodecHeaderAptxAdaptive(const uint8_t* p_codec_info, BT_HDR* p_buf,
                                             uint16_t frames_per_packet);

std::string A2DP_VendorCodecInfoStringAptxAdaptive(const uint8_t* p_codec_info);

const tA2DP_ENCODER_INTERFACE* A2DP_VendorGetEncoderInterfaceAptxAdaptive(
        const uint8_t* p_codec_info);

bool A2DP_VendorAdjustCodecAptxAdaptive(uint8_t* p_codec_info);

btav_a2dp_codec_index_t A2DP_VendorSourceCodecIndexAptxAdaptive(const uint8_t* p_codec_info);

const char* A2DP_VendorCodecIndexStrAptxAdaptive(void);

bool A2DP_VendorInitCodecConfigAptxAdaptive(AvdtpSepConfig* p_cfg);

#endif  // A2DP_VENDOR_APTX_ADAPTIVE_H
