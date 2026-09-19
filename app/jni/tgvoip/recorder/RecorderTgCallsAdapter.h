#ifndef TGX_RECORDER_TGCALLS_ADAPTER_H
#define TGX_RECORDER_TGCALLS_ADAPTER_H

#include "../CallRecorder.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace tgcalls {
struct Descriptor;
} // namespace tgcalls

namespace tgx {
namespace call_recording {

inline constexpr uint32_t kRecorderHookApiVersion = 1;

struct RecorderHookCapabilities {
  bool localHookAvailable = false;
  bool remoteHookAvailable = false;

  constexpr bool fullyAvailable() const noexcept {
    return localHookAvailable && remoteHookAvailable;
  }
};

struct RecorderIntegrationDescriptor {
  uint32_t integrationVersion = 1;
  uint32_t tgcallsHookApiVersion = kRecorderHookApiVersion;
  uint32_t localHookVersion = 1;
  uint32_t remoteHookVersion = 1;
  RecorderHookCapabilities capabilities = {true, true};
};

inline constexpr RecorderIntegrationDescriptor
    kRecorderIntegrationDescriptor {};

enum class RecorderIntegrationSupport : uint8_t {
  Supported,
  DisabledByConfiguration,
  LocalHookMissing,
  RemoteHookMissing,
  BothHooksMissing,
  TargetDestroyed
};

constexpr RecorderIntegrationSupport EvaluateRecorderIntegrationSupport(
    bool enabled,
    RecorderHookCapabilities capabilities,
    bool targetAlive = true) noexcept {
  if (!enabled) {
    return RecorderIntegrationSupport::DisabledByConfiguration;
  }
  if (!targetAlive) {
    return RecorderIntegrationSupport::TargetDestroyed;
  }
  if (!capabilities.localHookAvailable &&
      !capabilities.remoteHookAvailable) {
    return RecorderIntegrationSupport::BothHooksMissing;
  }
  if (!capabilities.localHookAvailable) {
    return RecorderIntegrationSupport::LocalHookMissing;
  }
  if (!capabilities.remoteHookAvailable) {
    return RecorderIntegrationSupport::RemoteHookMissing;
  }
  return RecorderIntegrationSupport::Supported;
}

// The current hooks deliver interleaved signed PCM16. Mono and stereo are
// explicitly supported because the recorder worker has a tested stereo-to-mono
// normalization path. The worker resampler consumes complete 10 ms chunks and
// the bounded realtime queue accepts at most 20 ms per callback.
constexpr bool IsSupportedRecorderPcmFormat(
    size_t framesPerChannel,
    size_t bytesPerFrame,
    size_t channels,
    uint32_t sampleRate) noexcept {
  const bool supportedRate = sampleRate == 8000 || sampleRate == 16000 ||
      sampleRate == 32000 || sampleRate == 44100 || sampleRate == 48000;
  if (channels == 0 || channels > 2 ||
      bytesPerFrame != channels * sizeof(int16_t) ||
      !supportedRate || framesPerChannel == 0) {
    return false;
  }
  const size_t framesPerTenMs = sampleRate / 100;
  return framesPerChannel % framesPerTenMs == 0 &&
      framesPerChannel <= 2 * framesPerTenMs;
}

RecorderHookCapabilities CurrentRecorderHookCapabilities() noexcept;

void ConfigureRecorderTgCallsHooks(
    tgcalls::Descriptor &descriptor,
    std::shared_ptr<CallRecordingController> controller) noexcept;

} // namespace call_recording
} // namespace tgx

#endif // TGX_RECORDER_TGCALLS_ADAPTER_H
