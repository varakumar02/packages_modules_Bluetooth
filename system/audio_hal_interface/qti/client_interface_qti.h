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

// Client for the Qualcomm Bluetooth audio HAL (vendor.qti.hardware.bluetooth_audio@2.1), the
// provider the QTI audio HAL's A2DP offload shim (btaudio_offload_if) is driven by on stock
// Qualcomm builds. Adapted from hidl/client_interface_hidl.h, A2DP sink side only.

#pragma once

#include <bluetooth/log.h>
#include <fmq/MessageQueue.h>
#include <hardware/audio.h>
#include <time.h>
#include <vendor/qti/hardware/bluetooth_audio/2.1/IBluetoothAudioProvider.h>
#include <vendor/qti/hardware/bluetooth_audio/2.1/IBluetoothAudioProvidersFactory.h>
#include <vendor/qti/hardware/bluetooth_audio/2.1/types.h>

#include <mutex>
#include <vector>

#include "common/message_loop_thread.h"

namespace bluetooth {
namespace audio {
namespace qti {

using AudioCapabilities = ::vendor::qti::hardware::bluetooth_audio::V2_1::AudioCapabilities;
using AudioConfiguration = ::vendor::qti::hardware::bluetooth_audio::V2_1::AudioConfiguration;
using ::vendor::qti::hardware::bluetooth_audio::V2_0::BitsPerSample;
using ::vendor::qti::hardware::bluetooth_audio::V2_0::ChannelMode;
using IBluetoothAudioProvider = ::vendor::qti::hardware::bluetooth_audio::V2_1::IBluetoothAudioProvider;
using IBluetoothAudioProvidersFactory =
        ::vendor::qti::hardware::bluetooth_audio::V2_1::IBluetoothAudioProvidersFactory;
using PcmParameters = ::vendor::qti::hardware::bluetooth_audio::V2_0::PcmParameters;
using SampleRate = ::vendor::qti::hardware::bluetooth_audio::V2_0::SampleRate;
using SessionType = ::vendor::qti::hardware::bluetooth_audio::V2_0::SessionType;
using ::vendor::qti::hardware::bluetooth_audio::V2_0::TimeSpec;
using BluetoothAudioStatus = ::vendor::qti::hardware::bluetooth_audio::V2_0::Status;

enum class BluetoothAudioCtrlAck : uint8_t {
  SUCCESS_FINISHED = 0,
  PENDING,
  FAILURE_UNSUPPORTED,
  FAILURE_BUSY,
  FAILURE_DISCONNECTING,
  FAILURE
};

std::ostream& operator<<(std::ostream& os, const BluetoothAudioCtrlAck& ack);

inline BluetoothAudioStatus BluetoothAudioCtrlAckToHalStatus(const BluetoothAudioCtrlAck& ack) {
  switch (ack) {
    case BluetoothAudioCtrlAck::SUCCESS_FINISHED:
      return BluetoothAudioStatus::SUCCESS;
    case BluetoothAudioCtrlAck::FAILURE_UNSUPPORTED:
      return BluetoothAudioStatus::UNSUPPORTED_CODEC_CONFIGURATION;
    case BluetoothAudioCtrlAck::FAILURE_DISCONNECTING:
      return BluetoothAudioStatus::FAILURE_DISC_IN_PROGRESS;
    default:
      return BluetoothAudioStatus::FAILURE;
  }
}

// Implemented by the A2DP transport to handle callbacks from the audio HAL.
class IBluetoothSinkTransportInstance {
public:
  IBluetoothSinkTransportInstance(SessionType sessionType, AudioConfiguration audioConfig)
      : session_type_(sessionType), audio_config_(std::move(audioConfig)) {}
  virtual ~IBluetoothSinkTransportInstance() = default;

  SessionType GetSessionType() const { return session_type_; }
  AudioConfiguration GetAudioConfiguration() const { return audio_config_; }
  void UpdateAudioConfiguration(const AudioConfiguration& audio_config) {
    audio_config_ = audio_config;
  }

  virtual BluetoothAudioCtrlAck StartRequest() = 0;
  virtual BluetoothAudioCtrlAck SuspendRequest() = 0;
  virtual void StopRequest() = 0;
  virtual bool GetPresentationPosition(uint64_t* remote_delay_report_ns,
                                       uint64_t* total_bytes_readed, timespec* data_position) = 0;
  // aptX Adaptive mode change requested by the audio HAL (IBluetoothAudioPort::updateAptxMode)
  virtual void AptxModeChanged(uint16_t aptx_mode) = 0;
  virtual void ResetPresentationPosition() = 0;
  virtual void LogBytesRead(size_t bytes_readed) = 0;

private:
  const SessionType session_type_;
  AudioConfiguration audio_config_;
};

class BluetoothAudioDeathRecipient;

// Connects an IBluetoothSinkTransportInstance to the QTI IBluetoothAudioProvider 2.1.
class BluetoothAudioSinkClientInterface {
public:
  BluetoothAudioSinkClientInterface(IBluetoothSinkTransportInstance* sink,
                                    bluetooth::common::MessageLoopThread* message_loop);
  ~BluetoothAudioSinkClientInterface();

  // True when the QTI 2.1 providers factory is registered on this device.
  static bool IsFactoryAvailable();
  static std::vector<AudioCapabilities> GetAudioCapabilities(SessionType session_type);

  bool IsValid() const { return provider_ != nullptr; }
  IBluetoothSinkTransportInstance* GetTransportInstance() const { return sink_; }

  void StreamStarted(const BluetoothAudioCtrlAck& ack);
  void StreamSuspended(const BluetoothAudioCtrlAck& ack);
  int StartSession();
  int EndSession();
  bool UpdateAudioConfig(const AudioConfiguration& audio_config);
  // Read data from the audio HAL through the FMQ (software encoding session only)
  size_t ReadAudioData(uint8_t* p_buf, uint32_t len);
  // Renew the connection, used when the audio HAL restarted
  void RenewAudioProviderAndSession();

private:
  void FetchAudioProvider();

  mutable std::mutex internal_mutex_;
  android::sp<IBluetoothAudioProvider> provider_;
  bool session_started_;
  std::unique_ptr<
          ::android::hardware::MessageQueue<uint8_t, ::android::hardware::kSynchronizedReadWrite>>
          mDataMQ;
  android::sp<BluetoothAudioDeathRecipient> death_recipient_;
  IBluetoothSinkTransportInstance* sink_;
  std::vector<AudioCapabilities> capabilities_;
};

}  // namespace qti
}  // namespace audio
}  // namespace bluetooth

namespace std {
template <>
struct formatter<bluetooth::audio::qti::BluetoothAudioCtrlAck>
    : enum_formatter<bluetooth::audio::qti::BluetoothAudioCtrlAck> {};
}  // namespace std
