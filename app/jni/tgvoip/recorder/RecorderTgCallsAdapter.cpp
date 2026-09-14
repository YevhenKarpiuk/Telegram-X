#include "RecorderTgCallsAdapter.h"

#include <android/log.h>

#include <api/audio/audio_frame.h>
#include <api/audio/audio_frame_processor.h>
#include <modules/audio_device/include/audio_device_data_observer.h>
#include <sdk/android/native_api/audio_device_module/audio_device_android.h>
#include <tgcalls/Instance.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace tgx {
namespace call_recording {
namespace {

constexpr char kLogTag[] = "TGX-CallRecorder";

using ExpectedAudioFrameProcessorFactory = std::function<
    std::unique_ptr<webrtc::AudioFrameProcessor>(
        std::unique_ptr<webrtc::AudioFrameProcessor>)>;
using ExpectedAudioDeviceFactory = std::function<
    webrtc::scoped_refptr<webrtc::AudioDeviceModule>(
        webrtc::TaskQueueFactory *)>;

static_assert(
    std::is_same_v<
        decltype(std::declval<tgcalls::Descriptor &>()
                     .createAudioFrameProcessor),
        ExpectedAudioFrameProcessorFactory>,
    "tgcalls Descriptor::createAudioFrameProcessor changed; review the local "
    "PCM hook before updating the production submodule");
static_assert(
    std::is_same_v<
        decltype(std::declval<tgcalls::Descriptor &>()
                     .createAudioDeviceModule),
        ExpectedAudioDeviceFactory>,
    "tgcalls Descriptor::createAudioDeviceModule changed; review the remote "
    "PCM hook before updating the production submodule");
static_assert(
    std::is_same_v<
        decltype(std::declval<const webrtc::AudioFrame &>().data()),
        const int16_t *>,
    "WebRTC AudioFrame sample type changed; do not reinterpret local PCM");

constexpr bool RunRecorderAdapterRegressionChecks() noexcept {
  constexpr RecorderHookCapabilities both {true, true};
  constexpr RecorderHookCapabilities bothMissing {false, false};
  constexpr RecorderHookCapabilities localMissing {false, true};
  constexpr RecorderHookCapabilities remoteMissing {true, false};
  if (EvaluateRecorderIntegrationSupport(true, both) !=
          RecorderIntegrationSupport::Supported ||
      EvaluateRecorderIntegrationSupport(true, localMissing) !=
          RecorderIntegrationSupport::LocalHookMissing ||
      EvaluateRecorderIntegrationSupport(true, remoteMissing) !=
          RecorderIntegrationSupport::RemoteHookMissing ||
      EvaluateRecorderIntegrationSupport(true, bothMissing) !=
          RecorderIntegrationSupport::BothHooksMissing ||
      EvaluateRecorderIntegrationSupport(false, both) !=
          RecorderIntegrationSupport::DisabledByConfiguration ||
      EvaluateRecorderIntegrationSupport(true, both, false) !=
          RecorderIntegrationSupport::TargetDestroyed) {
    return false;
  }
  if (!IsSupportedRecorderPcmFormat(480, 2, 1, 48000) ||
      !IsSupportedRecorderPcmFormat(960, 4, 2, 48000) ||
      !IsSupportedRecorderPcmFormat(160, 2, 1, 16000) ||
      IsSupportedRecorderPcmFormat(480, 4, 1, 48000) ||
      IsSupportedRecorderPcmFormat(480, 2, 3, 48000) ||
      !IsSupportedRecorderPcmFormat(441, 2, 1, 44100) ||
      IsSupportedRecorderPcmFormat(220, 2, 1, 22050)) {
    return false;
  }

  // Repeated adapter target create/destroy decisions must never resurrect a
  // destroyed callback target.
  for (size_t cycle = 0; cycle < 32; ++cycle) {
    if (EvaluateRecorderIntegrationSupport(true, both, true) !=
            RecorderIntegrationSupport::Supported ||
        EvaluateRecorderIntegrationSupport(true, both, false) !=
            RecorderIntegrationSupport::TargetDestroyed) {
      return false;
    }
  }
  return true;
}

static_assert(
    RunRecorderAdapterRegressionChecks(),
    "Recorder/tgcalls adapter capability regression checks failed");
static_assert(
    kRecorderIntegrationDescriptor.integrationVersion == 1 &&
        kRecorderIntegrationDescriptor.tgcallsHookApiVersion == 1 &&
        kRecorderIntegrationDescriptor.localHookVersion == 1 &&
        kRecorderIntegrationDescriptor.remoteHookVersion == 1 &&
        kRecorderIntegrationDescriptor.capabilities.fullyAvailable(),
    "Recorder integration descriptor must change explicitly with hook API");

class RecordingAudioDeviceObserver final
    : public webrtc::AudioDeviceDataObserver {
public:
  explicit RecordingAudioDeviceObserver(
      std::shared_ptr<CallRecordingController> controller)
      : controller_(std::move(controller)) {
  }

  void OnCaptureData(
      const void *, size_t, size_t, size_t, uint32_t) override {
    // ADM capture is raw microphone audio, not the selected post-APM signal.
  }

  void OnRenderData(
      const void *audioSamples,
      size_t numSamples,
      size_t bytesPerSample,
      size_t numChannels,
      uint32_t samplesPerSec) override {
    if (!IsSupportedRecorderPcmFormat(
            numSamples,
            bytesPerSample,
            numChannels,
            samplesPerSec)) {
      // The recorder queue turns this sentinel into a worker-side stream
      // failure. No logging, allocation, lock, JNI, or file I/O occurs here.
      controller_->enqueueRemote(nullptr, 0, 0, 0, 0);
      return;
    }
    controller_->enqueueRemote(
        audioSamples,
        numSamples,
        bytesPerSample,
        numChannels,
        samplesPerSec);
  }

private:
  const std::shared_ptr<CallRecordingController> controller_;
};

class RecordingAudioFrameProcessor final
    : public webrtc::AudioFrameProcessor {
public:
  RecordingAudioFrameProcessor(
      std::shared_ptr<CallRecordingController> controller,
      std::unique_ptr<webrtc::AudioFrameProcessor> existingProcessor)
      : controller_(std::move(controller)),
        existingProcessor_(std::move(existingProcessor)) {
  }

  void Process(std::unique_ptr<webrtc::AudioFrame> frame) override {
    if (existingProcessor_ != nullptr) {
      existingProcessor_->Process(std::move(frame));
      return;
    }

    record(frame.get());
    SinkHolder *sink = sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
      sink->callback(std::move(frame));
    }
  }

  void SetSink(OnAudioFrameCallback sinkCallback) override {
    if (existingProcessor_ != nullptr) {
      if (!sinkCallback) {
        existingProcessor_->SetSink(nullptr);
      } else {
        existingProcessor_->SetSink(
            [controller = controller_, sink = std::move(sinkCallback)](
                std::unique_ptr<webrtc::AudioFrame> frame) mutable {
              recordFrame(controller, frame.get());
              sink(std::move(frame));
            });
      }
      return;
    }
    if (!sinkCallback) {
      sink_.store(nullptr, std::memory_order_release);
      return;
    }

    auto holder = std::make_unique<SinkHolder>(std::move(sinkCallback));
    SinkHolder *holderPointer = holder.get();
    {
      std::lock_guard<std::mutex> lock(sinkHoldersMutex_);
      sinkHolders_.push_back(std::move(holder));
    }
    // Holders live until this processor is destroyed, so Process() needs only
    // an atomic load and never waits for SetSink().
    sink_.store(holderPointer, std::memory_order_release);
  }

private:
  static void recordFrame(
      const std::shared_ptr<CallRecordingController> &controller,
      const webrtc::AudioFrame *frame) noexcept {
    if (frame == nullptr || frame->sample_rate_hz() <= 0 ||
        !IsSupportedRecorderPcmFormat(
            frame->samples_per_channel(),
            frame->num_channels() * sizeof(int16_t),
            frame->num_channels(),
            static_cast<uint32_t>(frame->sample_rate_hz()))) {
      controller->enqueueLocal(nullptr, 0, 0, 0);
      return;
    }
    controller->enqueueLocal(
        frame->data(),
        frame->samples_per_channel(),
        frame->num_channels(),
        static_cast<uint32_t>(frame->sample_rate_hz()));
  }

  void record(const webrtc::AudioFrame *frame) noexcept {
    recordFrame(controller_, frame);
  }

  struct SinkHolder {
    explicit SinkHolder(OnAudioFrameCallback value)
        : callback(std::move(value)) {
    }
    OnAudioFrameCallback callback;
  };

  const std::shared_ptr<CallRecordingController> controller_;
  std::unique_ptr<webrtc::AudioFrameProcessor> existingProcessor_;
  std::atomic<SinkHolder *> sink_ = {nullptr};
  std::mutex sinkHoldersMutex_;
  std::vector<std::unique_ptr<SinkHolder>> sinkHolders_;
};

void ReportHookUnavailable(
    const std::shared_ptr<CallRecordingController> &controller,
    const char *hook) noexcept {
  __android_log_print(
      ANDROID_LOG_ERROR,
      kLogTag,
      "Recorder hook API: %u %s hook installation failed; recording "
      "unsupported",
      kRecorderHookApiVersion,
      hook);
  if (controller != nullptr) {
    controller->markIntegrationUnsupported();
  }
}

} // namespace

RecorderHookCapabilities CurrentRecorderHookCapabilities() noexcept {
  return kRecorderIntegrationDescriptor.capabilities;
}

void ConfigureRecorderTgCallsHooks(
    tgcalls::Descriptor &descriptor,
    std::shared_ptr<CallRecordingController> controller) noexcept {
  const RecorderHookCapabilities capabilities =
      CurrentRecorderHookCapabilities();
  __android_log_print(
      ANDROID_LOG_INFO,
      kLogTag,
      "Recorder integration: %u hook API: %u Local hook: v%u %s Remote hook: "
      "v%u %s",
      kRecorderIntegrationDescriptor.integrationVersion,
      kRecorderIntegrationDescriptor.tgcallsHookApiVersion,
      kRecorderIntegrationDescriptor.localHookVersion,
      capabilities.localHookAvailable ? "available" : "missing",
      kRecorderIntegrationDescriptor.remoteHookVersion,
      capabilities.remoteHookAvailable ? "available" : "missing");
  if (controller == nullptr || !capabilities.fullyAvailable()) {
    if (controller != nullptr) {
      controller->markIntegrationUnsupported();
    }
    return;
  }

  descriptor.createAudioDeviceModule = [controller](
      webrtc::TaskQueueFactory *) {
    auto audioDevice = webrtc::CreateAndroidAudioDeviceModule(
        webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    if (audioDevice == nullptr) {
      ReportHookUnavailable(controller, "remote");
      return audioDevice;
    }
    try {
      auto observer = std::make_unique<RecordingAudioDeviceObserver>(
          controller);
      auto observedAudioDevice = webrtc::CreateAudioDeviceWithDataObserver(
          audioDevice,
          std::move(observer));
      if (observedAudioDevice == nullptr) {
        ReportHookUnavailable(controller, "remote");
        return audioDevice;
      }
      __android_log_print(
          ANDROID_LOG_INFO,
          kLogTag,
          "Recorder hook API: %u Remote hook installed",
          kRecorderHookApiVersion);
      return observedAudioDevice;
    } catch (...) {
      ReportHookUnavailable(controller, "remote");
      return audioDevice;
    }
  };
  descriptor.createAudioFrameProcessor = [controller](
      std::unique_ptr<webrtc::AudioFrameProcessor> existingProcessor)
      -> std::unique_ptr<webrtc::AudioFrameProcessor> {
    try {
      auto processor = std::make_unique<RecordingAudioFrameProcessor>(
          controller,
          std::move(existingProcessor));
      __android_log_print(
          ANDROID_LOG_INFO,
          kLogTag,
          "Recorder hook API: %u Local hook installed",
          kRecorderHookApiVersion);
      return processor;
    } catch (...) {
      ReportHookUnavailable(controller, "local");
      return existingProcessor;
    }
  };
}

} // namespace call_recording
} // namespace tgx
