/*
 * Copyright 2022 The Android Open Source Project
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
/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.

    * Redistribution and use in source and binary forms, with or without
      modification, are permitted (subject to the limitations in the
      disclaimer below) provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

#pragma once

#include <fmq/AidlMessageQueue.h>
#include <hardware/audio.h>

#include <ctime>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "audio_aidl_interfaces.h"
#include "audio_ctrl_ack.h"
#include "bluetooth_audio_port_impl.h"
#include "osi/include/thread.h"
#include "transport_instance.h"

#define BLUETOOTH_AUDIO_HAL_PROP_DISABLED \
  "persist.bluetooth.bluetooth_audio_hal.disabled"

namespace bluetooth {
namespace audio {
namespace aidl {

using ::aidl::android::hardware::bluetooth::audio::AudioCapabilities;
using ::aidl::android::hardware::bluetooth::audio::AudioConfiguration;
using ::aidl::android::hardware::bluetooth::audio::BluetoothAudioStatus;
using ::aidl::android::hardware::bluetooth::audio::IBluetoothAudioPort;
using ::aidl::android::hardware::bluetooth::audio::IBluetoothAudioProvider;
using ::aidl::android::hardware::bluetooth::audio::
    IBluetoothAudioProviderFactory;
using ::aidl::android::hardware::bluetooth::audio::LeAudioBroadcastConfiguration;
using ::aidl::android::hardware::bluetooth::audio::LeAudioCodecConfiguration;
using ::aidl::android::hardware::bluetooth::audio::PcmConfiguration;

using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::android::AidlMessageQueue;

using MqDataType = int8_t;
using MqDataMode = SynchronizedReadWrite;
using DataMQ = AidlMessageQueue<MqDataType, MqDataMode>;
using DataMQDesc = MQDescriptor<MqDataType, MqDataMode>;

/***
 * The client interface connects an IBluetoothTransportInstance to
 * IBluetoothAudioProvider and helps to route callbacks to
 * IBluetoothTransportInstance
 ***/
class BluetoothAudioClientInterface {
 public:
  BluetoothAudioClientInterface(IBluetoothTransportInstance* instance);
  virtual ~BluetoothAudioClientInterface();

  bool IsValid() const { return provider_ != nullptr; }

  std::vector<AudioCapabilities> GetAudioCapabilities() const;
  static std::vector<AudioCapabilities> GetAudioCapabilities(
      SessionType session_type);
  void StreamStarted(const BluetoothAudioCtrlAck& ack);

  void StreamSuspended(const BluetoothAudioCtrlAck& ack);

  int StartSession();

  /***
   * Renew the connection and usually is used when aidl restarted
   ***/
  void RenewAudioProviderAndSession();

  int EndSession();

  bool UpdateAudioConfig(const AudioConfiguration& audioConfig);

  void FlushAudioData();

  void set_low_latency_allowed(bool is_low_latency_allowed);

  static constexpr PcmConfiguration kInvalidPcmConfiguration = {};

  static bool is_aidl_available();

 protected:
  mutable std::mutex internal_mutex_;
  /***
   * Helper function to connect to an IBluetoothAudioProvider
   ***/
  void FetchAudioProvider();

  /***
   * Invoked when binder died
   ***/
  static void binderDiedCallbackAidl(void* cookie_ptr);

  static bool isDestroy(BluetoothAudioClientInterface *ptr) { return objs_address_.find(ptr) == objs_address_.end(); }

  std::shared_ptr<IBluetoothAudioProvider> provider_;

  std::shared_ptr<IBluetoothAudioProviderFactory> provider_factory_;

  bool session_started_;
  std::unique_ptr<DataMQ> data_mq_;

  ::ndk::ScopedAIBinder_DeathRecipient death_recipient_;
  // static constexpr const char* kDefaultAudioProviderFactoryInterface =
  //     "android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default";
  static inline const std::string kDefaultAudioProviderFactoryInterface =
      std::string(IBluetoothAudioProviderFactory::descriptor) + "/default";
      //std::string() + IBluetoothAudioProviderFactory::descriptor + "/default";


 private:
  static inline bool aidl_available = true;
  bool is_low_latency_allowed_ = false;
  IBluetoothTransportInstance* transport_;
  std::vector<AudioCapabilities> capabilities_;
  static std::unordered_set<BluetoothAudioClientInterface *> objs_address_;
};

/***
 * The client interface connects an IBluetoothTransportInstance to
 * IBluetoothAudioProvider and helps to route callbacks to
 * IBluetoothTransportInstance
 ***/
class BluetoothAudioSinkClientInterface : public BluetoothAudioClientInterface {
 public:
  /***
   * Constructs an BluetoothAudioSinkClientInterface to communicate to
   * BluetoothAudio HAL. |sink| is the implementation for the transport, and
   * |message_loop| is the thread where callbacks are invoked.
   ***/
  BluetoothAudioSinkClientInterface(
      IBluetoothSinkTransportInstance* sink,
     thread_t* message_loop);
  virtual ~BluetoothAudioSinkClientInterface();

  IBluetoothSinkTransportInstance* GetTransportInstance() const {
    return sink_;
  }

  /***
   * Read data from audio HAL through fmq
   ***/
  size_t ReadAudioData(uint8_t* p_buf, uint32_t len);

 private:
  IBluetoothSinkTransportInstance* sink_;

  static constexpr int kDefaultDataReadTimeoutMs = 10;
  static constexpr int kDefaultDataReadPollIntervalMs = 1;
};

class BluetoothAudioSourceClientInterface
    : public BluetoothAudioClientInterface {
 public:
  /***
   * Constructs an BluetoothAudioSourceClientInterface to communicate to
   * BluetoothAudio HAL. |source| is the implementation for the transport, and
   * |message_loop| is the thread where callbacks are invoked.
   ***/
  BluetoothAudioSourceClientInterface(
      IBluetoothSourceTransportInstance* source,
      thread_t* message_loop);
  virtual ~BluetoothAudioSourceClientInterface();

  IBluetoothSourceTransportInstance* GetTransportInstance() const {
    return source_;
  }

  /***
   * Write data to audio HAL through fmq
   ***/
  size_t WriteAudioData(const uint8_t* p_buf, uint32_t len);

 private:
  IBluetoothSourceTransportInstance* source_;

  static constexpr int kDefaultDataWriteTimeoutMs = 10;
  static constexpr int kDefaultDataWritePollIntervalMs = 1;
};

}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
