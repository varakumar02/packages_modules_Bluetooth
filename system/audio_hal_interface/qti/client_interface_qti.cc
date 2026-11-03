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

#define LOG_TAG "BTAudioClientQTI"

#include "client_interface_qti.h"

#include <bluetooth/log.h>
#include <hidl/MQDescriptor.h>
#include <vendor/qti/hardware/bluetooth_audio/2.1/IBluetoothAudioPort.h>

#include <future>
#include <vector>

#include "common/stop_watch_legacy.h"

namespace bluetooth {
namespace audio {
namespace qti {

using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::bluetooth::common::StopWatchLegacy;
using IBluetoothAudioPort = ::vendor::qti::hardware::bluetooth_audio::V2_1::IBluetoothAudioPort;
using ::vendor::qti::hardware::bluetooth_audio::V2_0::SessionParams;
using ::vendor::qti::hardware::bluetooth_audio::V2_0::SessionParamType;

using DataMQ =
        ::android::hardware::MessageQueue<uint8_t, ::android::hardware::kSynchronizedReadWrite>;

static constexpr int kDefaultDataReadTimeoutMs = 10;      // 10 ms
static constexpr int kDefaultDataReadPollIntervalMs = 1;  // non-blocking poll

std::ostream& operator<<(std::ostream& os, const BluetoothAudioCtrlAck& ack) {
  switch (ack) {
    case BluetoothAudioCtrlAck::SUCCESS_FINISHED:
      return os << "SUCCESS_FINISHED";
    case BluetoothAudioCtrlAck::PENDING:
      return os << "PENDING";
    case BluetoothAudioCtrlAck::FAILURE_UNSUPPORTED:
      return os << "FAILURE_UNSUPPORTED";
    case BluetoothAudioCtrlAck::FAILURE_BUSY:
      return os << "FAILURE_BUSY";
    case BluetoothAudioCtrlAck::FAILURE_DISCONNECTING:
      return os << "FAILURE_DISCONNECTING";
    case BluetoothAudioCtrlAck::FAILURE:
      return os << "FAILURE";
    default:
      return os << "UNDEFINED " << static_cast<int8_t>(ack);
  }
}

class BluetoothAudioPortImpl : public IBluetoothAudioPort {
public:
  BluetoothAudioPortImpl(IBluetoothSinkTransportInstance* transport_instance,
                         const android::sp<IBluetoothAudioProvider>& provider)
      : transport_instance_(transport_instance), provider_(provider) {}

  Return<void> startStream() override {
    StopWatchLegacy stop_watch(__func__);
    BluetoothAudioCtrlAck ack = transport_instance_->StartRequest();
    if (ack != BluetoothAudioCtrlAck::PENDING) {
      auto hidl_retval = provider_->streamStarted(BluetoothAudioCtrlAckToHalStatus(ack));
      if (!hidl_retval.isOk()) {
        log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
      }
    }
    return Void();
  }

  Return<void> suspendStream() override {
    StopWatchLegacy stop_watch(__func__);
    BluetoothAudioCtrlAck ack = transport_instance_->SuspendRequest();
    if (ack != BluetoothAudioCtrlAck::PENDING) {
      auto hidl_retval = provider_->streamSuspended(BluetoothAudioCtrlAckToHalStatus(ack));
      if (!hidl_retval.isOk()) {
        log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
      }
    }
    return Void();
  }

  Return<void> stopStream() override {
    StopWatchLegacy stop_watch(__func__);
    transport_instance_->StopRequest();
    return Void();
  }

  Return<void> getPresentationPosition(getPresentationPosition_cb _hidl_cb) override {
    StopWatchLegacy stop_watch(__func__);
    uint64_t remote_delay_report_ns;
    uint64_t total_bytes_read;
    timespec data_position;
    bool retval = transport_instance_->GetPresentationPosition(&remote_delay_report_ns,
                                                               &total_bytes_read, &data_position);

    TimeSpec transmittedOctetsTimeStamp;
    if (retval) {
      transmittedOctetsTimeStamp = {.tvSec = static_cast<uint64_t>(data_position.tv_sec),
                                    .tvNSec = static_cast<uint64_t>(data_position.tv_nsec)};
    } else {
      remote_delay_report_ns = 0;
      total_bytes_read = 0;
      transmittedOctetsTimeStamp = {};
    }
    log::verbose("result={}, delay={}, data={} byte(s), timestamp={}", retval,
                 remote_delay_report_ns, total_bytes_read, toString(transmittedOctetsTimeStamp));
    _hidl_cb((retval ? BluetoothAudioStatus::SUCCESS : BluetoothAudioStatus::FAILURE),
             remote_delay_report_ns, total_bytes_read, transmittedOctetsTimeStamp);
    return Void();
  }

  Return<void> updateAptxMode(uint16_t aptxMode) override {
    StopWatchLegacy stop_watch(__func__);
    transport_instance_->AptxModeChanged(aptxMode);
    return Void();
  }

private:
  IBluetoothSinkTransportInstance* transport_instance_;
  const android::sp<IBluetoothAudioProvider> provider_;
};

class BluetoothAudioDeathRecipient : public ::android::hardware::hidl_death_recipient {
public:
  BluetoothAudioDeathRecipient(BluetoothAudioSinkClientInterface* clientif,
                               bluetooth::common::MessageLoopThread* message_loop)
      : bluetooth_audio_clientif_(clientif), message_loop_(message_loop) {}
  void serviceDied(uint64_t /*cookie*/,
                   const ::android::wp<::android::hidl::base::V1_0::IBase>& /*who*/) override {
    log::warn("restarting connection with new Audio Hal");
    if (bluetooth_audio_clientif_ != nullptr && message_loop_ != nullptr) {
      // restart the session on the correct thread
      message_loop_->DoInThread(
              base::BindOnce(&BluetoothAudioSinkClientInterface::RenewAudioProviderAndSession,
                             base::Unretained(bluetooth_audio_clientif_)));
    } else {
      log::error("BluetoothAudioSinkClientInterface corrupted");
    }
  }

private:
  BluetoothAudioSinkClientInterface* bluetooth_audio_clientif_;
  bluetooth::common::MessageLoopThread* message_loop_;
};

bool BluetoothAudioSinkClientInterface::IsFactoryAvailable() {
  return IBluetoothAudioProvidersFactory::getService() != nullptr;
}

std::vector<AudioCapabilities> BluetoothAudioSinkClientInterface::GetAudioCapabilities(
        SessionType session_type) {
  std::vector<AudioCapabilities> capabilities(0);
  android::sp<IBluetoothAudioProvidersFactory> providersFactory =
          IBluetoothAudioProvidersFactory::getService();
  if (providersFactory == nullptr) {
    log::error("IBluetoothAudioProvidersFactory::getService() failed");
    return capabilities;
  }

  auto hidl_retval = providersFactory->getProviderCapabilities_2_1(
          session_type, [&capabilities](const hidl_vec<AudioCapabilities>& audioCapabilities) {
            for (auto capability : audioCapabilities) {
              capabilities.push_back(capability);
            }
          });
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal::getProviderCapabilities_2_1 failure: {}",
               hidl_retval.description());
  }
  return capabilities;
}

void BluetoothAudioSinkClientInterface::FetchAudioProvider() {
  if (provider_ != nullptr) {
    log::warn("reflash");
    provider_ = nullptr;
  }

  android::sp<IBluetoothAudioProvidersFactory> providersFactory =
          IBluetoothAudioProvidersFactory::getService();
  if (providersFactory == nullptr) {
    log::error("IBluetoothAudioProvidersFactory::getService() failed");
    return;
  }

  capabilities_ = GetAudioCapabilities(sink_->GetSessionType());
  if (capabilities_.empty()) {
    log::warn("SessionType={} Not supported by BluetoothAudioHal",
              toString(sink_->GetSessionType()));
    return;
  }
  log::info("BluetoothAudioHal SessionType={} has {} AudioCapabilities",
            toString(sink_->GetSessionType()), capabilities_.size());

  std::promise<void> openProvider_promise;
  auto openProvider_future = openProvider_promise.get_future();
  auto openProvider_cb = [&provider_ = this->provider_, &openProvider_promise](
                                 BluetoothAudioStatus status,
                                 const android::sp<IBluetoothAudioProvider>& provider) {
    log::info("openProvider_2_1_cb({})", toString(status));
    if (status == BluetoothAudioStatus::SUCCESS) {
      provider_ = provider;
    }
    openProvider_promise.set_value();
  };
  auto hidl_retval = providersFactory->openProvider_2_1(sink_->GetSessionType(), openProvider_cb);
  openProvider_future.get();
  if (!hidl_retval.isOk() || provider_ == nullptr) {
    log::error("BluetoothAudioHal::openProvider_2_1 failure: {}", hidl_retval.description());
    provider_ = nullptr;
    return;
  }

  if (!provider_->linkToDeath(death_recipient_, 0).isOk()) {
    log::error("BluetoothAudioDeathRecipient link failure");
  }

  log::info("IBluetoothAudioProvidersFactory::openProvider_2_1() returned {}{}",
            std::format_ptr(provider_.get()), (provider_->isRemote() ? " (remote)" : " (local)"));
}

BluetoothAudioSinkClientInterface::BluetoothAudioSinkClientInterface(
        IBluetoothSinkTransportInstance* sink, bluetooth::common::MessageLoopThread* message_loop)
    : provider_(nullptr),
      session_started_(false),
      mDataMQ(nullptr),
      death_recipient_(new BluetoothAudioDeathRecipient(this, message_loop)),
      sink_(sink) {
  FetchAudioProvider();
}

BluetoothAudioSinkClientInterface::~BluetoothAudioSinkClientInterface() {
  if (provider_ != nullptr) {
    auto hidl_retval = provider_->unlinkToDeath(death_recipient_);
    if (!hidl_retval.isOk()) {
      log::error("BluetoothAudioDeathRecipient unlink failure: {}", hidl_retval.description());
    }
  }
}

bool BluetoothAudioSinkClientInterface::UpdateAudioConfig(const AudioConfiguration& audio_config) {
  // QTI AudioConfiguration is a raw HIDL union: the session type tells which member is set
  if (sink_->GetSessionType() != SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH &&
      sink_->GetSessionType() != SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH) {
    return false;
  }
  sink_->UpdateAudioConfiguration(audio_config);
  return true;
}

int BluetoothAudioSinkClientInterface::StartSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    session_started_ = false;
    return -EINVAL;
  }
  if (session_started_) {
    log::error("session started already");
    return -EBUSY;
  }

  android::sp<IBluetoothAudioPort> stack_if = new BluetoothAudioPortImpl(sink_, provider_);

  std::unique_ptr<DataMQ> tempDataMQ;
  BluetoothAudioStatus session_status = BluetoothAudioStatus::FAILURE;

  std::promise<void> hidl_startSession_promise;
  auto hidl_startSession_future = hidl_startSession_promise.get_future();
  auto hidl_cb = [&session_status, &tempDataMQ, &hidl_startSession_promise](
                         BluetoothAudioStatus status, const DataMQ::Descriptor& dataMQ) {
    log::info("startSession_2_1_cb({})", toString(status));
    session_status = status;
    if (status == BluetoothAudioStatus::SUCCESS && dataMQ.isHandleValid()) {
      tempDataMQ.reset(new DataMQ(dataMQ));
    }
    hidl_startSession_promise.set_value();
  };
  auto hidl_retval = provider_->startSession_2_1(stack_if, sink_->GetAudioConfiguration(), hidl_cb);
  hidl_startSession_future.get();
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
    return -EPROTO;
  }

  if (tempDataMQ && tempDataMQ->isValid()) {
    mDataMQ = std::move(tempDataMQ);
  } else if (sink_->GetSessionType() == SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH &&
             session_status == BluetoothAudioStatus::SUCCESS) {
    sink_->ResetPresentationPosition();
    session_started_ = true;
    SendSinkLatency();
    return 0;
  }
  if (mDataMQ && mDataMQ->isValid()) {
    sink_->ResetPresentationPosition();
    session_started_ = true;
    SendSinkLatency();
    return 0;
  }
  log::error("Failed to obtain a valid audio data path, status={}", toString(session_status));
  session_started_ = false;
  return -EIO;
}

void BluetoothAudioSinkClientInterface::UpdateSinkLatency(uint16_t delay_report) {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  sink_latency_ = delay_report;
  if (session_started_) {
    SendSinkLatency();
  }
}

void BluetoothAudioSinkClientInterface::SendSinkLatency() {
  if (provider_ == nullptr || sink_latency_ == 0) {
    return;
  }
  SessionParams session_params{};
  session_params.paramType = SessionParamType::SINK_LATENCY;
  session_params.param.sinkLatency = {sink_latency_, 0, {}};
  auto hidl_retval = provider_->updateSessionParams(session_params);
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal updateSessionParams failure: {}", hidl_retval.description());
  }
}

void BluetoothAudioSinkClientInterface::StreamStarted(const BluetoothAudioCtrlAck& ack) {
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    log::info("{} ignored", ack);
    return;
  }
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  auto hidl_retval = provider_->streamStarted(BluetoothAudioCtrlAckToHalStatus(ack));
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
  }
}

void BluetoothAudioSinkClientInterface::StreamSuspended(const BluetoothAudioCtrlAck& ack) {
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    log::info("{} ignored", ack);
    return;
  }
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  auto hidl_retval = provider_->streamSuspended(BluetoothAudioCtrlAckToHalStatus(ack));
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
  }
}

int BluetoothAudioSinkClientInterface::EndSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  if (!session_started_) {
    log::info("session ended already");
    return 0;
  }

  session_started_ = false;
  mDataMQ = nullptr;

  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return -EINVAL;
  }
  auto hidl_retval = provider_->endSession();
  if (!hidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", hidl_retval.description());
    return -EPROTO;
  }
  return 0;
}

size_t BluetoothAudioSinkClientInterface::ReadAudioData(uint8_t* p_buf, uint32_t len) {
  if (!IsValid()) {
    log::error("BluetoothAudioHal is not valid");
    return 0;
  }
  if (p_buf == nullptr || len == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> guard(internal_mutex_);

  size_t total_read = 0;
  int timeout_ms = kDefaultDataReadTimeoutMs;
  do {
    if (mDataMQ == nullptr || !mDataMQ->isValid()) {
      break;
    }

    size_t avail_to_read = mDataMQ->availableToRead();
    if (avail_to_read) {
      if (avail_to_read > len - total_read) {
        avail_to_read = len - total_read;
      }
      if (mDataMQ->read(p_buf + total_read, avail_to_read) == 0) {
        log::warn("len={} total_read={} failed", len, total_read);
        break;
      }
      total_read += avail_to_read;
    } else if (timeout_ms >= kDefaultDataReadPollIntervalMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultDataReadPollIntervalMs));
      timeout_ms -= kDefaultDataReadPollIntervalMs;
      continue;
    } else {
      log::warn("{}/{} no data {} ms", len - total_read, len,
                kDefaultDataReadTimeoutMs - timeout_ms);
      break;
    }
  } while (total_read < len);

  sink_->LogBytesRead(total_read);
  return total_read;
}

void BluetoothAudioSinkClientInterface::RenewAudioProviderAndSession() {
  // NOTE: must be invoked on the same thread where this interface is running
  FetchAudioProvider();
  if (session_started_) {
    log::info("Restart the session while audio HAL recovering");
    session_started_ = false;
    StartSession();
  }
}

}  // namespace qti
}  // namespace audio
}  // namespace bluetooth
