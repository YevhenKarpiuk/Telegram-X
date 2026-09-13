#ifndef TGX_CALL_RECORDER_H
#define TGX_CALL_RECORDER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>

#include <api/audio/audio_frame_processor.h>
#include <modules/audio_device/include/audio_device_data_observer.h>

namespace tgx {
namespace call_recording {

enum class RecordingState : int32_t {
  Idle = 0,
  Recording = 1,
  Paused = 2,
  Inactive = 3,
  Finalized = 4,
  Failed = 5,
  Unsupported = 6
};

enum class OutputMode : int32_t {
  MixedAndSeparate = 0,
  MixedOnly = 1,
  SeparateOnly = 2
};

struct CallMetadata {
  int64_t callId = 0;
  int64_t userId = 0;
  std::string displayName;
  bool isOutgoing = false;
};

class RecordingSession final {
public:
  static std::shared_ptr<RecordingSession> Create(
      std::string basePath,
      CallMetadata callMetadata,
      OutputMode outputMode,
      int64_t sessionStartMonotonicNs,
      int64_t recordingStartWallTimeMs,
      bool autoStarted,
      std::function<void()> fatalFailureCallback);

  ~RecordingSession();

  RecordingSession(const RecordingSession &) = delete;
  RecordingSession &operator=(const RecordingSession &) = delete;

  void start();
  void pause(int64_t boundaryMonotonicNs);
  void resume(int64_t boundaryMonotonicNs);
  void stop(int64_t boundaryMonotonicNs);
  void restart(int64_t boundaryMonotonicNs);
  void beginFinishCall(
      int64_t teardownMonotonicNs,
      int64_t callEndWallTimeMs);
  void finish();

  void enqueueLocal(
      const int16_t *samples,
      size_t framesPerChannel,
      size_t channels,
      uint32_t sampleRate) noexcept;
  void enqueueRemote(
      const void *samples,
      size_t framesPerChannel,
      size_t bytesPerFrame,
      size_t channels,
      uint32_t sampleRate) noexcept;

private:
  class Impl;

  RecordingSession(
      std::string basePath,
      CallMetadata callMetadata,
      OutputMode outputMode,
      int64_t sessionStartMonotonicNs,
      int64_t recordingStartWallTimeMs,
      bool autoStarted,
      std::function<void()> fatalFailureCallback);

  std::unique_ptr<Impl> impl_;
};

class CallRecordingController final
    : public std::enable_shared_from_this<CallRecordingController> {
public:
  using StateCallback = std::function<void(
      RecordingState state,
      int64_t elapsedSamples,
      bool autoRecordingEnabled)>;

  static std::shared_ptr<CallRecordingController> Create(
      std::string basePath,
      CallMetadata callMetadata,
      bool supported,
      bool autoRecordingEnabled,
      OutputMode initialOutputMode,
      StateCallback stateCallback);

  ~CallRecordingController();

  CallRecordingController(const CallRecordingController &) = delete;
  CallRecordingController &operator=(const CallRecordingController &) = delete;

  void notifyInitialState();
  void onEstablished();
  void start(OutputMode outputMode);
  void pause();
  void resume();
  void stop();
  void beginFinishCall();
  void finishCall();

  RecordingState state() const noexcept;
  int64_t elapsedSamples() const noexcept;
  bool autoRecordingEnabled() const noexcept;

  void enqueueLocal(
      const int16_t *samples,
      size_t framesPerChannel,
      size_t channels,
      uint32_t sampleRate) noexcept;
  void enqueueRemote(
      const void *samples,
      size_t framesPerChannel,
      size_t bytesPerFrame,
      size_t channels,
      uint32_t sampleRate) noexcept;

private:
  class Impl;

  CallRecordingController(
      std::string basePath,
      CallMetadata callMetadata,
      bool supported,
      bool autoRecordingEnabled,
      OutputMode initialOutputMode,
      StateCallback stateCallback);

  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<webrtc::AudioDeviceDataObserver> CreateAudioDeviceObserver(
    std::shared_ptr<CallRecordingController> controller);

std::unique_ptr<webrtc::AudioFrameProcessor> CreateAudioFrameProcessor(
    std::shared_ptr<CallRecordingController> controller,
    std::unique_ptr<webrtc::AudioFrameProcessor> existingProcessor);

} // namespace call_recording
} // namespace tgx

#endif // TGX_CALL_RECORDER_H
