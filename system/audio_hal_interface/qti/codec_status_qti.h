/*
 * Copyright 2019 The Android Open Source Project
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
 */

#pragma once

#include <vendor/qti/hardware/bluetooth_audio/2.1/types.h>

#include <vector>

#include "a2dp_codec_api.h"
#include "audio_hal_interface/a2dp_encoding.h"

namespace bluetooth::audio::qti::codec {

using ::vendor::qti::hardware::bluetooth_audio::V2_0::PcmParameters;
using ::vendor::qti::hardware::bluetooth_audio::V2_1::CodecConfiguration;
using ::bluetooth::audio::a2dp::ahal_codec_configuration;

/// Configure the framework supported offload capabilities.
bool UpdateOffloadingCapabilities(
        const std::vector<btav_a2dp_codec_config_t>& framework_preference);

/// PCM configuration for the software encoding session; false if unsupported.
bool getHalPcmConfiguration(const ahal_codec_configuration& config,
                            PcmParameters* pcm_configuration);

/// Codec configuration for the offload encoding session; false if the negotiated
/// codec is not supported by the hardware offload encoding session.
bool getHalCodecConfiguration(const ahal_codec_configuration& config,
                              CodecConfiguration* codec_configuration);

}  // namespace bluetooth::audio::qti::codec
