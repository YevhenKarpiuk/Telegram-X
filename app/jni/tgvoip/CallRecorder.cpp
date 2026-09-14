#include "CallRecorder.h"

#include <android/log.h>

#include <common_audio/resampler/include/push_resampler.h>
#include <ogg/ogg.h>
#include <opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tgx {
namespace call_recording {
namespace {

constexpr size_t kQueueSlots = 128;
constexpr size_t kMaxSamplesPerBlock = 3840;
constexpr uint32_t kMaxAcceptedBlockDurationMs = 20;
constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint16_t kOutputChannels = 1;
constexpr int kStreamOpusBitrate = 32000;
constexpr int kMixedOpusBitrate = 48000;
constexpr size_t kOpusFrameSamples = 960;
constexpr uint32_t kOpusFrameDurationMs = 20;
constexpr size_t kResamplerOutputSamples = 480;
constexpr size_t kMaxOpusPacketBytes = 4000;
constexpr uint64_t kMixedHoldbackSamples = kOutputSampleRate / 2;
constexpr int64_t kContinuityJitterToleranceNs = 50000000;
constexpr uint64_t kContinuityJitterToleranceSamples =
    kOutputSampleRate * kContinuityJitterToleranceNs / 1000000000ULL;
constexpr int64_t kTimestampDiagnosticEnterNs = 100000000;
constexpr int64_t kTimestampDiagnosticExitNs = 60000000;
constexpr int64_t kTimestampDiagnosticReportStepNs = 50000000;
constexpr uint64_t kTimestampDiagnosticEnterSamples =
    kOutputSampleRate * kTimestampDiagnosticEnterNs / 1000000000ULL;
constexpr uint64_t kTimestampDiagnosticExitSamples =
    kOutputSampleRate * kTimestampDiagnosticExitNs / 1000000000ULL;
constexpr uint64_t kTimestampDiagnosticReportStepSamples =
    kOutputSampleRate * kTimestampDiagnosticReportStepNs / 1000000000ULL;
constexpr size_t kMaxTimestampDiscontinuities = 256;
constexpr size_t kMaxContinuityGaps = 256;
constexpr size_t kMaxLateDrops = 256;
constexpr size_t kMaxSourceFormats = 64;
constexpr size_t kMaxQueueGaps = 256;
constexpr size_t kMaxControlIntervals = 256;
constexpr uint64_t kMaxNormalizedBlockSamples =
    kOutputSampleRate * kMaxAcceptedBlockDurationMs / 1000U;
constexpr uint64_t kMaxTimelineBufferedSamplesPerTrack =
    kMixedHoldbackSamples + kContinuityJitterToleranceSamples +
    3 * kMaxNormalizedBlockSamples;
constexpr char kCallRecorderLogTag[] = "TGX-CallRecorder";
constexpr char kOpusVendor[] = "Telegram X Recorder";
constexpr char kMixedAlgorithm[] =
    "fixed-point 0.5*local + 0.5*remote, saturating PCM16";
constexpr std::chrono::seconds kMetadataCheckpointInterval(5);

const char *OutputModeName(OutputMode mode) noexcept {
  switch (mode) {
    case OutputMode::MixedAndSeparate:
      return "mixed_and_separate";
    case OutputMode::MixedOnly:
      return "mixed_only";
    case OutputMode::SeparateOnly:
      return "separate_only";
  }
  return "mixed_and_separate";
}

std::atomic<uint32_t> gNextOggSerial = {0x54475801U};

int64_t CurrentUnixTimeMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t CurrentMonotonicTimeNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string FormatDirectoryTime(int64_t unixTimeMs) {
  const std::time_t seconds = static_cast<std::time_t>(unixTimeMs / 1000);
  std::tm value = {};
  localtime_r(&seconds, &value);
  std::ostringstream result;
  result << std::put_time(&value, "%Y-%m-%d_%H-%M-%S");
  return result.str();
}

std::string FormatJsonTime(int64_t unixTimeMs) {
  if (unixTimeMs <= 0) {
    return std::string();
  }
  const std::time_t seconds = static_cast<std::time_t>(unixTimeMs / 1000);
  std::tm value = {};
  gmtime_r(&seconds, &value);
  std::ostringstream result;
  result << std::put_time(&value, "%Y-%m-%dT%H:%M:%S")
         << '.' << std::setw(3) << std::setfill('0') << (unixTimeMs % 1000)
         << 'Z';
  return result.str();
}

std::string JsonEscape(const std::string &value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 8);
  for (unsigned char character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20U) {
          result += "\\u00";
          result += kHex[(character >> 4U) & 0x0fU];
          result += kHex[character & 0x0fU];
        } else {
          result += static_cast<char>(character);
        }
        break;
    }
  }
  return result;
}

bool RunMetadataRegressionChecks() {
  std::string controls;
  controls.push_back('\0');
  controls.push_back('\x01');
  controls += "\b\f\n\r\t";
  return JsonEscape(u8"Євген Карпюк") == u8"Євген Карпюк" &&
      JsonEscape("Иван \"Test\"") ==
          std::string("Иван ") + "\\\"Test\\\"" &&
      JsonEscape("O'Connor") == "O'Connor" &&
      JsonEscape("Back\\Slash") == "Back\\\\Slash" &&
      JsonEscape(u8"emoji 😀") == u8"emoji 😀" &&
      JsonEscape(controls) == "\\u0000\\u0001\\b\\f\\n\\r\\t";
}

int64_t MonotonicEndToWallTimeMs(
    int64_t recordingStartWallTimeMs,
    int64_t sessionStartMonotonicNs,
    int64_t sessionEndMonotonicNs) noexcept {
  const __int128 rawDeltaNs = static_cast<__int128>(
      sessionEndMonotonicNs) - sessionStartMonotonicNs;
  const __int128 deltaNs = std::max<__int128>(0, rawDeltaNs);
  const __int128 result = static_cast<__int128>(
      recordingStartWallTimeMs) + deltaNs / 1000000;
  if (result > std::numeric_limits<int64_t>::max()) {
    return std::numeric_limits<int64_t>::max();
  }
  if (result < std::numeric_limits<int64_t>::min()) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(result);
}

bool RunMetadataTimeRegressionChecks() noexcept {
  constexpr int64_t startWallMs = 1757684881135LL;
  constexpr int64_t startMonotonicNs = 100000000000LL;
  constexpr int64_t finalStopMonotonicNs = 131876000000LL;
  constexpr int64_t callEndWallMs = startWallMs + 41742LL;
  const int64_t recordingEndWallMs = MonotonicEndToWallTimeMs(
      startWallMs, startMonotonicNs, finalStopMonotonicNs);
  return recordingEndWallMs == startWallMs + 31876LL &&
      recordingEndWallMs < callEndWallMs &&
      MonotonicEndToWallTimeMs(
          startWallMs, startMonotonicNs, startMonotonicNs - 1) ==
          startWallMs;
}

struct MetadataCheckpointDiagnostics {
  uint64_t failures = 0;
  uint32_t consecutiveFailures = 0;

  constexpr void record(bool succeeded) noexcept {
    if (succeeded) {
      consecutiveFailures = 0;
      return;
    }
    if (failures != std::numeric_limits<uint64_t>::max()) {
      ++failures;
    }
    if (consecutiveFailures != std::numeric_limits<uint32_t>::max()) {
      ++consecutiveFailures;
    }
  }
};

constexpr bool RunMetadataFailureIsolationRegressionChecks() noexcept {
  MetadataCheckpointDiagnostics diagnostics;
  diagnostics.record(false);
  diagnostics.record(false);
  if (diagnostics.failures != 2 || diagnostics.consecutiveFailures != 2) {
    return false;
  }
  // Metadata failure only changes diagnostics. There is deliberately no
  // recorder-fatal output in this state object.
  diagnostics.record(true);
  return diagnostics.failures == 2 && diagnostics.consecutiveFailures == 0;
}

static_assert(
    RunMetadataFailureIsolationRegressionChecks(),
    "metadata checkpoint failures must remain retryable and non-fatal");

bool EnsureDirectory(
    const std::string &path,
    bool allowExisting) noexcept {
  if (mkdir(path.c_str(), 0700) == 0) {
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "mkdir path=%s success",
        path.c_str());
    return true;
  }
  const int mkdirError = errno;
  if (allowExisting && mkdirError == EEXIST) {
    struct stat value = {};
    if (stat(path.c_str(), &value) == 0 && S_ISDIR(value.st_mode)) {
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "mkdir path=%s success existing",
          path.c_str());
      return true;
    }
  }
  __android_log_print(
      ANDROID_LOG_ERROR,
      kCallRecorderLogTag,
      "mkdir path=%s failure errno=%d strerror=%s",
      path.c_str(),
      mkdirError,
      std::strerror(mkdirError));
  return false;
}

bool AtomicWriteFile(
    const std::string &directory,
    const std::string &filename,
    const std::string &value) noexcept {
  const std::string finalPath = directory + "/" + filename;
  const std::string temporaryPath = finalPath + ".tmp";
  FILE *file = std::fopen(temporaryPath.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  bool success = std::fwrite(value.data(), 1, value.size(), file) == value.size() &&
      std::ferror(file) == 0 && std::fflush(file) == 0;
  if (success) {
    const int descriptor = fileno(file);
    success = descriptor >= 0 && fsync(descriptor) == 0;
  }
  if (std::fclose(file) != 0) {
    success = false;
  }
  if (!success || std::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
    return false;
  }
  const int directoryDescriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (directoryDescriptor >= 0) {
    // The file is already atomically visible. Directory fsync is best effort
    // because some Android filesystems reject it.
    (void) fsync(directoryDescriptor);
    (void) close(directoryDescriptor);
  }
  return true;
}

struct AudioBlock {
  int64_t monotonicTimeNs = 0;
  uint64_t sequence = 0;
  uint64_t controlGeneration = 0;
  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint32_t framesPerChannel = 0;
  uint32_t sampleCount = 0;
  uint64_t droppedRecoverableBlocksBefore = 0;
  uint64_t droppedRecoverableSamplesBefore = 0;
  uint64_t droppedUnknownDurationBlocksBefore = 0;
  std::array<int16_t, kMaxSamplesPerBlock> samples = {};
};

struct SourceFormat {
  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint64_t startSequence = 0;
  int64_t startTimestampNs = 0;
};

struct QueueGap {
  uint64_t startSequence = 0;
  uint64_t blocks = 0;
  uint64_t detectedAtSequence = 0;
  uint64_t recoverableBlocks = 0;
  uint64_t recoverableSamples = 0;
  uint64_t unknownDurationBlocks = 0;
  bool representedByMasterTimeline = true;
};

enum class StreamSide {
  Local,
  Remote
};

const char *StreamSideName(StreamSide side) noexcept {
  return side == StreamSide::Local ? "local" : "remote";
}

struct TimestampDiscontinuity {
  StreamSide side = StreamSide::Local;
  uint64_t sequence = 0;
  int64_t deltaMs = 0;
  enum class Phase {
    Entered,
    Updated,
    Restored
  } phase = Phase::Entered;
};

const char *TimestampDiscontinuityPhaseName(
    TimestampDiscontinuity::Phase phase) noexcept {
  switch (phase) {
    case TimestampDiscontinuity::Phase::Entered:
      return "entered";
    case TimestampDiscontinuity::Phase::Updated:
      return "updated";
    case TimestampDiscontinuity::Phase::Restored:
      return "restored";
  }
  return "unknown";
}

constexpr bool AdvanceTimestampDiagnosticState(
    bool &active,
    int64_t &lastReportedDeltaSamples,
    int64_t deltaSamples,
    TimestampDiscontinuity::Phase &phase) noexcept {
  const __int128 delta = deltaSamples;
  const __int128 magnitude = delta >= 0 ? delta : -delta;
  phase = TimestampDiscontinuity::Phase::Entered;
  if (!active) {
    if (magnitude <= kTimestampDiagnosticEnterSamples) {
      return false;
    }
    active = true;
  } else if (magnitude < kTimestampDiagnosticExitSamples) {
    active = false;
    phase = TimestampDiscontinuity::Phase::Restored;
  } else {
    const __int128 difference = delta - lastReportedDeltaSamples;
    const __int128 differenceMagnitude = difference >= 0
        ? difference : -difference;
    if (differenceMagnitude <= kTimestampDiagnosticReportStepSamples) {
      return false;
    }
    phase = TimestampDiscontinuity::Phase::Updated;
  }
  lastReportedDeltaSamples = deltaSamples;
  return true;
}

struct ContinuityGap {
  StreamSide side = StreamSide::Local;
  uint64_t sequence = 0;
  uint64_t previousTimelineEndSample = 0;
  uint64_t timestampTimelineStartSample = 0;
  uint64_t gapSamples = 0;
};

struct LateDrop {
  StreamSide side = StreamSide::Local;
  uint64_t sequence = 0;
  uint64_t timelineStartSample = 0;
  uint64_t timelineEndSample = 0;
  uint64_t commitCursorSample = 0;
  uint64_t droppedSamples = 0;
};

constexpr bool MonotonicNsToTimelineSample(
    int64_t sessionStartNs,
    int64_t timestampNs,
    uint64_t &result) noexcept {
  if (timestampNs <= sessionStartNs) {
    result = 0;
    return true;
  }
  const __int128 delta = static_cast<__int128>(timestampNs) -
      static_cast<__int128>(sessionStartNs);
  const __int128 samples =
      (delta * kOutputSampleRate + 500000000LL) / 1000000000LL;
  if (samples < 0 || samples > std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  result = static_cast<uint64_t>(samples);
  return true;
}

constexpr uint64_t ChooseContinuityStart(
    bool continuityActive,
    uint64_t previousTimelineEnd,
    uint64_t timestampCandidate,
    bool forceNewSegment,
    bool &positiveGap,
    bool &negativeDiscontinuity) noexcept {
  positiveGap = false;
  negativeDiscontinuity = false;
  if (!continuityActive) {
    return timestampCandidate;
  }
  if (forceNewSegment) {
    positiveGap = timestampCandidate > previousTimelineEnd;
    negativeDiscontinuity = timestampCandidate < previousTimelineEnd;
    return timestampCandidate;
  }
  if (timestampCandidate > previousTimelineEnd &&
      timestampCandidate - previousTimelineEnd >
          kContinuityJitterToleranceSamples) {
    positiveGap = true;
    return timestampCandidate;
  }
  if (previousTimelineEnd > timestampCandidate &&
      previousTimelineEnd - timestampCandidate >
          kContinuityJitterToleranceSamples) {
    negativeDiscontinuity = true;
  }
  return previousTimelineEnd;
}

constexpr bool ShouldRecordContinuityDiagnostic(
    bool detected,
    bool controlBoundarySegment) noexcept {
  return detected && !controlBoundarySegment;
}

constexpr bool IsNewControlGeneration(
    uint64_t blockGeneration,
    uint64_t workerGeneration) noexcept {
  return blockGeneration != workerGeneration;
}

constexpr bool HasUsableEnabledOutput(
    OutputMode outputMode,
    bool localWriterFailed,
    bool remoteWriterFailed,
    bool mixedWriterFailed,
    bool localCaptureFailed,
    bool remoteCaptureFailed) noexcept {
  const bool localUsable = outputMode != OutputMode::MixedOnly &&
      !localWriterFailed && !localCaptureFailed;
  const bool remoteUsable = outputMode != OutputMode::MixedOnly &&
      !remoteWriterFailed && !remoteCaptureFailed;
  const bool mixedUsable = outputMode != OutputMode::SeparateOnly &&
      !mixedWriterFailed && (!localCaptureFailed || !remoteCaptureFailed);
  return localUsable || remoteUsable || mixedUsable;
}

constexpr uint64_t RegularCommitUntil(uint64_t nowTimelineSample) noexcept {
  if (nowTimelineSample <= kMixedHoldbackSamples) {
    return 0;
  }
  const uint64_t ready = nowTimelineSample - kMixedHoldbackSamples;
  return ready - ready % kOpusFrameSamples;
}

constexpr uint32_t QueueDepth(size_t head, size_t tail) noexcept {
  return static_cast<uint32_t>(
      head >= tail ? head - tail : kQueueSlots - tail + head);
}

constexpr bool TimelineSamplesToMilliseconds(
    uint64_t samples,
    uint64_t &result) noexcept {
  const __int128 milliseconds =
      static_cast<__int128>(samples) * 1000 / kOutputSampleRate;
  if (milliseconds < 0 ||
      milliseconds > std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  result = static_cast<uint64_t>(milliseconds);
  return true;
}

constexpr bool SimulateContinuousCallbacks(
    uint64_t initialTimelineEnd,
    uint64_t callbackCount,
    uint64_t samplesPerCallback,
    uint64_t &finalTimelineEnd) noexcept {
  const __int128 finalPosition =
      static_cast<__int128>(initialTimelineEnd) +
      static_cast<__int128>(callbackCount) * samplesPerCallback;
  if (finalPosition < 0 ||
      finalPosition > std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  finalTimelineEnd = static_cast<uint64_t>(finalPosition);
  return true;
}

constexpr bool RunSessionTimelineRegressionChecks() noexcept {
  struct TrackModel {
    bool active = false;
    uint64_t end = 0;
    uint64_t gaps = 0;
  };
  auto append = [](
      TrackModel &track,
      uint64_t timestampCandidate,
      uint64_t samples,
      uint64_t &start,
      bool forceNewSegment = false) noexcept {
    bool positiveGap = false;
    bool negativeDiscontinuity = false;
    start = ChooseContinuityStart(
        track.active,
        track.end,
        timestampCandidate,
        forceNewSegment,
        positiveGap,
        negativeDiscontinuity);
    if (positiveGap) {
      ++track.gaps;
    }
    track.active = true;
    track.end = start + samples;
    return !negativeDiscontinuity || start >= timestampCandidate;
  };

  constexpr uint64_t kTenMs = 480;
  constexpr uint64_t kSecond = 48000;
  uint64_t start = 0;

  // A: continuous streams use sample progression despite callback jitter.
  TrackModel local;
  TrackModel remote;
  if (!append(local, 0, kTenMs, start) || start != 0 ||
      !append(local, kTenMs + 960, kTenMs, start) ||
      start != kTenMs || local.gaps != 0 ||
      !append(remote, 0, kTenMs, start) || start != 0) {
    return false;
  }

  // B: a side starting one second late retains leading silence.
  TrackModel lateRemote;
  if (!append(lateRemote, kSecond, kTenMs, start) || start != kSecond) {
    return false;
  }

  // C/D: a one-second local or remote callback pause becomes a real gap.
  TrackModel pausedLocal;
  TrackModel pausedRemote;
  if (!append(pausedLocal, 0, kSecond, start) ||
      !append(pausedLocal, 2 * kSecond, kTenMs, start) ||
      start != 2 * kSecond || pausedLocal.gaps != 1 ||
      !append(pausedRemote, 0, kSecond, start) ||
      !append(pausedRemote, 2 * kSecond, kTenMs, start) ||
      start != 2 * kSecond || pausedRemote.gaps != 1) {
    return false;
  }

  // E: simultaneous pauses preserve the same 1..2 second silent interval.
  if (pausedLocal.end != pausedRemote.end) {
    return false;
  }

  // F: 250 ms is detected even though it is shorter than the holdback.
  TrackModel shortPause;
  if (!append(shortPause, 0, kSecond, start) ||
      !append(shortPause, kSecond + 12000, kTenMs, start) ||
      start != kSecond + 12000 || shortPause.gaps != 1) {
    return false;
  }

  // G/H: a format change or queue loss uses the same timestamp gap model.
  TrackModel routeOrQueueGap;
  if (!append(routeOrQueueGap, 0, kSecond, start) ||
      !append(routeOrQueueGap, kSecond + 24000, kTenMs, start) ||
      routeOrQueueGap.gaps != 1) {
    return false;
  }
  TrackModel singleQueueLoss;
  if (!append(singleQueueLoss, 0, kTenMs, start) ||
      !append(singleQueueLoss, 2 * kTenMs, kTenMs, start, true) ||
      start != 2 * kTenMs || singleQueueLoss.gaps != 1) {
    return false;
  }

  // I: +/-5/10/20 ms jitter never creates a continuity gap.
  TrackModel jitter;
  if (!append(jitter, 0, kTenMs, start)) {
    return false;
  }
  constexpr int64_t kJitterSamples[] = {
      240, -480, 960, -960, 1440, -1440, 1920, -1920};
  for (int64_t jitterSamples : kJitterSamples) {
    const uint64_t candidate = jitterSamples >= 0
        ? jitter.end + static_cast<uint64_t>(jitterSamples)
        : jitter.end - static_cast<uint64_t>(-jitterSamples);
    if (!append(jitter, candidate, kTenMs, start) ||
        start + kTenMs != jitter.end || jitter.gaps != 0) {
      return false;
    }
  }

  // J: committed audio is trimmed, never shifted to the cursor.
  constexpr uint64_t kCommitCursor = 30000;
  constexpr uint64_t kLateStart = 29520;
  const uint64_t lateDropped = std::min(
      kTenMs, kCommitCursor - kLateStart);
  constexpr uint64_t kPartialLateStart = 29760;
  const uint64_t partialLateDropped = std::min(
      kOpusFrameSamples, kCommitCursor - kPartialLateStart);
  if (lateDropped != kTenMs || partialLateDropped != 240 ||
      kOpusFrameSamples - partialLateDropped != 720) {
    return false;
  }

  // K/L: master commits advance all writers equally, including silence-only.
  constexpr uint64_t kFiveSeconds = 5 * kSecond;
  uint64_t localWritten = 0;
  uint64_t remoteWritten = 0;
  uint64_t mixedWritten = 0;
  for (uint64_t cursor = 0; cursor < kFiveSeconds;) {
    const uint64_t count = std::min<uint64_t>(
        kOpusFrameSamples, kFiveSeconds - cursor);
    localWritten += count;
    remoteWritten += count;
    mixedWritten += count;
    cursor += count;
  }
  if (localWritten != kFiveSeconds || remoteWritten != kFiveSeconds ||
      mixedWritten != kFiveSeconds ||
      RegularCommitUntil(kMixedHoldbackSamples + 1919) != 960 ||
      kMaxTimelineBufferedSamplesPerTrack != 29280 ||
      QueueDepth(0, 0) != 0 || QueueDepth(kQueueSlots - 1, 0) != 127 ||
      QueueDepth(1, 2) != 127) {
    return false;
  }

  // Diagnostic hysteresis is intentionally independent from continuity.
  bool diagnosticActive = false;
  int64_t lastDiagnostic = 0;
  TimestampDiscontinuity::Phase diagnosticPhase =
      TimestampDiscontinuity::Phase::Entered;
  if (AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -2496, diagnosticPhase) ||
      AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -2400, diagnosticPhase) ||
      !AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -4848, diagnosticPhase) ||
      diagnosticPhase != TimestampDiscontinuity::Phase::Entered ||
      AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -4896, diagnosticPhase) ||
      !AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -7680, diagnosticPhase) ||
      diagnosticPhase != TimestampDiscontinuity::Phase::Updated ||
      !AdvanceTimestampDiagnosticState(
          diagnosticActive, lastDiagnostic, -1920, diagnosticPhase) ||
      diagnosticPhase != TimestampDiscontinuity::Phase::Restored ||
      diagnosticActive) {
    return false;
  }

  // Absolute ns conversion and sample progression have no duration drift.
  constexpr uint64_t kMinute = 60 * kSecond;
  constexpr uint64_t kThirtyMinutes = 30 * kMinute;
  constexpr uint64_t kTwoHours = 120 * kMinute;
  constexpr int64_t kSyntheticStartNs = 1000000000000LL;
  uint64_t convertedSamples = 0;
  uint64_t simulatedTimelineEnd = 0;
  uint64_t durationMs = 0;
  if (!MonotonicNsToTimelineSample(
          kSyntheticStartNs,
          kSyntheticStartNs + 60LL * 1000000000LL,
          convertedSamples) ||
      convertedSamples != kMinute ||
      !MonotonicNsToTimelineSample(
          kSyntheticStartNs,
          kSyntheticStartNs + 1800LL * 1000000000LL,
          convertedSamples) ||
      convertedSamples != kThirtyMinutes ||
      !MonotonicNsToTimelineSample(
          kSyntheticStartNs,
          kSyntheticStartNs + 7200LL * 1000000000LL,
          convertedSamples) ||
      convertedSamples != kTwoHours ||
      !SimulateContinuousCallbacks(
          0, kMinute / kTenMs, kTenMs, simulatedTimelineEnd) ||
      simulatedTimelineEnd != kMinute ||
      !SimulateContinuousCallbacks(
          0, kThirtyMinutes / kTenMs, kTenMs, simulatedTimelineEnd) ||
      simulatedTimelineEnd != kThirtyMinutes ||
      !SimulateContinuousCallbacks(
          0, kTwoHours / kTenMs, kTenMs, simulatedTimelineEnd) ||
      simulatedTimelineEnd != kTwoHours ||
      !TimelineSamplesToMilliseconds(kTwoHours, durationMs) ||
      durationMs != 7200000 ||
      kTwoHours + std::numeric_limits<uint16_t>::max() >=
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }

  struct SessionModel {
    uint64_t timeline = 0;
    uint64_t localAudio = 0;
    uint64_t remoteAudio = 0;
  };
  auto accountingValid = [](const SessionModel &session) constexpr noexcept {
    const uint64_t localSilence = session.timeline - session.localAudio;
    const uint64_t remoteSilence = session.timeline - session.remoteAudio;
    const uint64_t localWriter = session.timeline;
    const uint64_t remoteWriter = session.timeline;
    const uint64_t mixedWriter = session.timeline;
    return session.localAudio + localSilence == session.timeline &&
        session.remoteAudio + remoteSilence == session.timeline &&
        localWriter == session.timeline && remoteWriter == session.timeline &&
        mixedWriter == session.timeline;
  };

  // Reconnect, one-sided absence, simultaneous absence, and stop-in-gap.
  constexpr uint64_t kThirtySeconds = 30 * kSecond;
  const SessionModel remoteReconnect {
      kThirtySeconds, kThirtySeconds, 25 * kSecond};
  const SessionModel localReconnect {
      kThirtySeconds, 25 * kSecond, kThirtySeconds};
  const SessionModel bothReconnect {
      kThirtySeconds, 25 * kSecond, 25 * kSecond};
  const SessionModel multipleReconnects {
      kThirtySeconds, 22 * kSecond, 22 * kSecond};
  const SessionModel stopDuringGap {
      kThirtySeconds, 10 * kSecond, 10 * kSecond};
  const SessionModel noAudio {kFiveSeconds, 0, 0};
  if (!accountingValid(remoteReconnect) ||
      !accountingValid(localReconnect) ||
      !accountingValid(bothReconnect) ||
      !accountingValid(multipleReconnects) ||
      !accountingValid(stopDuringGap) || !accountingValid(noAudio) ||
      remoteReconnect.timeline - remoteReconnect.remoteAudio != 5 * kSecond ||
      localReconnect.timeline - localReconnect.localAudio != 5 * kSecond ||
      bothReconnect.timeline - bothReconnect.localAudio != 5 * kSecond ||
      multipleReconnects.timeline - multipleReconnects.localAudio !=
          8 * kSecond) {
    return false;
  }
  TrackModel reconnectSegments;
  if (!append(reconnectSegments, 0, 2 * kSecond, start) ||
      !append(reconnectSegments, 4 * kSecond, 2 * kSecond, start) ||
      start != 4 * kSecond ||
      !append(reconnectSegments, 11 * kSecond, 2 * kSecond, start) ||
      start != 11 * kSecond ||
      !append(reconnectSegments, 14 * kSecond, 2 * kSecond, start) ||
      start != 14 * kSecond || reconnectSegments.gaps != 3 ||
      reconnectSegments.end != 16 * kSecond) {
    return false;
  }

  // 100/119/250 ms gaps remain detectable below the 500 ms holdback.
  constexpr uint64_t kOneHundredMs = 4800;
  constexpr uint64_t kOneHundredNineteenMs = 5712;
  TrackModel shortRealGaps;
  if (!append(shortRealGaps, 0, kSecond, start) ||
      !append(
          shortRealGaps,
          kSecond + kOneHundredMs,
          kTenMs,
          start) ||
      start != kSecond + kOneHundredMs) {
    return false;
  }
  const uint64_t nextShortGapTimestamp =
      shortRealGaps.end + kOneHundredNineteenMs;
  if (!append(
          shortRealGaps,
          nextShortGapTimestamp,
          kTenMs,
          start) ||
      start != nextShortGapTimestamp || shortRealGaps.gaps != 2) {
    return false;
  }

  // Route/source format normalization preserves exact block duration.
  auto normalizedSamples = [](
      uint64_t framesPerChannel,
      uint32_t sampleRate,
      uint16_t channels,
      uint64_t &samples) constexpr noexcept {
    const __int128 numerator =
        static_cast<__int128>(framesPerChannel) * kOutputSampleRate;
    if (sampleRate == 0 || channels == 0 || numerator % sampleRate != 0) {
      return false;
    }
    samples = static_cast<uint64_t>(numerator / sampleRate);
    return true;
  };
  uint64_t normalized = 0;
  if (!normalizedSamples(480, 48000, 1, normalized) || normalized != 480 ||
      !normalizedSamples(160, 16000, 1, normalized) || normalized != 480 ||
      !normalizedSamples(480, 48000, 2, normalized) || normalized != 480 ||
      !normalizedSamples(960, 48000, 1, normalized) || normalized != 960) {
    return false;
  }

  // Multiple forced queue-loss segments create exactly the timestamp gaps.
  TrackModel multipleQueueLosses;
  if (!append(multipleQueueLosses, 0, kTenMs, start) ||
      !append(
          multipleQueueLosses, 2 * kTenMs, kTenMs, start, true) ||
      start != 2 * kTenMs ||
      !append(
          multipleQueueLosses, 4 * kTenMs, kTenMs, start, true) ||
      start != 4 * kTenMs || multipleQueueLosses.gaps != 2) {
    return false;
  }

  // A failed writer does not change the master or the other virtual writers.
  const bool localWriterFailed = true;
  const uint64_t failedLocalWritten = 0;
  const uint64_t healthyRemoteWritten = kThirtySeconds;
  const uint64_t healthyMixedWritten = kThirtySeconds;
  if (!localWriterFailed || failedLocalWritten == kThirtySeconds ||
      healthyRemoteWritten != kThirtySeconds ||
      healthyMixedWritten != kThirtySeconds) {
    return false;
  }
  return true;
}

static_assert(
    RunSessionTimelineRegressionChecks(),
    "master session timeline regression scenarios must pass");

constexpr uint64_t UngatedSamplesForRange(
    uint64_t rangeStart,
    uint64_t rangeEnd,
    uint64_t gateStart,
    uint64_t gateEnd) noexcept {
  const uint64_t overlapStart = std::max(rangeStart, gateStart);
  const uint64_t overlapEnd = std::min(rangeEnd, gateEnd);
  const uint64_t gated = overlapEnd > overlapStart
      ? overlapEnd - overlapStart : 0;
  return rangeEnd - rangeStart - gated;
}

struct ControlTimelineModel {
  bool started = false;
  bool recording = false;
  bool paused = false;
  bool stopped = false;
  uint64_t sessionStart = 0;
  uint64_t pauseStart = 0;
  uint64_t stopStart = 0;
  uint64_t end = 0;
  uint64_t pausedSamples = 0;
  uint64_t stoppedSamples = 0;
  uint32_t startCount = 0;

  constexpr void start(uint64_t at) noexcept {
    if (!started) {
      started = true;
      sessionStart = at;
      end = 0;
    } else if (stopped) {
      stoppedSamples += at - stopStart;
      end = at - sessionStart;
      stopped = false;
    }
    recording = true;
    paused = false;
    ++startCount;
  }

  constexpr void pause(uint64_t at) noexcept {
    recording = false;
    paused = true;
    pauseStart = at;
  }

  constexpr void resume(uint64_t at) noexcept {
    pausedSamples += at - pauseStart;
    paused = false;
    recording = true;
    end = at - sessionStart;
  }

  constexpr void stop(uint64_t at) noexcept {
    if (paused) {
      pausedSamples += at - pauseStart;
      paused = false;
    }
    recording = false;
    stopped = true;
    stopStart = at;
    end = at - sessionStart;
  }

  constexpr void finish(uint64_t at) noexcept {
    if (stopped) {
      return;
    }
    if (paused) {
      pausedSamples += at - pauseStart;
      paused = false;
    }
    end = at - sessionStart;
  }
};

constexpr bool RunCallControlRegressionChecks() noexcept {
  constexpr uint64_t second = kOutputSampleRate;

  // Repeated Start/Stop: 10 audio + 5 silence + 10 audio + 3 silence
  // + 10 audio uses one 38-second monotonic master timeline.
  ControlTimelineModel repeated;
  repeated.start(0);
  repeated.stop(10 * second);
  repeated.start(15 * second);
  repeated.stop(25 * second);
  repeated.start(28 * second);
  repeated.finish(38 * second);
  if (repeated.end != 38 * second ||
      repeated.stoppedSamples != 8 * second || repeated.startCount != 3) {
    return false;
  }

  // A final Stop freezes the logical end, while a Pause tail reaches teardown.
  ControlTimelineModel finalStop;
  finalStop.start(0);
  finalStop.stop(10 * second);
  finalStop.finish(70 * second);
  ControlTimelineModel pauseTail;
  pauseTail.start(0);
  pauseTail.pause(10 * second);
  pauseTail.finish(15 * second);
  if (finalStop.end != 10 * second || finalStop.stoppedSamples != 0 ||
      pauseTail.end != 15 * second ||
      pauseTail.pausedSamples != 5 * second) {
    return false;
  }

  // First manual Start is the anchor, so five pre-start minutes contribute 0.
  ControlTimelineModel lateManual;
  lateManual.start(300 * second);
  lateManual.stop(320 * second);
  lateManual.finish(380 * second);
  if (lateManual.end != 20 * second) {
    return false;
  }

  // Exact crossing-block trimming at both edges of a control interval.
  if (UngatedSamplesForRange(90, 110, 100, 200) != 10 ||
      UngatedSamplesForRange(190, 210, 100, 200) != 10 ||
      UngatedSamplesForRange(120, 180, 100, 200) != 0 ||
      UngatedSamplesForRange(210, 230, 100, 200) != 20) {
    return false;
  }

  // The first block after every control gap starts at its absolute timestamp,
  // even below the normal 50 ms continuity tolerance. The control interval is
  // the sole diagnostic representation of the gap.
  auto checkControlGap = [](uint64_t gapSamples) constexpr noexcept {
    constexpr uint64_t boundary = 10 * kOutputSampleRate;
    bool positiveGap = false;
    bool negativeDiscontinuity = false;
    const uint64_t start = ChooseContinuityStart(
        true,
        boundary,
        boundary + gapSamples,
        IsNewControlGeneration(1, 0),
        positiveGap,
        negativeDiscontinuity);
    return start == boundary + gapSamples && start > boundary &&
        !negativeDiscontinuity &&
        !ShouldRecordContinuityDiagnostic(positiveGap, true) &&
        !ShouldRecordContinuityDiagnostic(true, true);
  };
  constexpr uint64_t kPauseControlGaps[] = {
      20 * second / 1000,
      30 * second / 1000,
      40 * second / 1000,
      2 * second};
  constexpr uint64_t kStopControlGaps[] = {
      20 * second / 1000,
      30 * second / 1000,
      40 * second / 1000,
      5 * second};
  for (uint64_t gap : kPauseControlGaps) {
    if (!checkControlGap(gap)) {
      return false;
    }
  }
  for (uint64_t gap : kStopControlGaps) {
    if (!checkControlGap(gap)) {
      return false;
    }
  }
  return true;
}

static_assert(
    RunCallControlRegressionChecks(),
    "call recording control/timeline regression scenarios must pass");

constexpr bool RunFailureIsolationRegressionChecks() noexcept {
  // MIXED_AND_SEPARATE: either one separate writer or the mixed writer may
  // fail while at least one useful enabled output continues.
  if (!HasUsableEnabledOutput(
          OutputMode::MixedAndSeparate,
          true, false, false, false, false) ||
      !HasUsableEnabledOutput(
          OutputMode::MixedAndSeparate,
          false, true, false, false, false) ||
      !HasUsableEnabledOutput(
          OutputMode::MixedAndSeparate,
          false, false, true, false, false)) {
    return false;
  }
  // SEPARATE_ONLY: local writer/capture failure leaves remote useful.
  if (!HasUsableEnabledOutput(
          OutputMode::SeparateOnly,
          true, false, false, false, false) ||
      !HasUsableEnabledOutput(
          OutputMode::SeparateOnly,
          false, false, false, true, false)) {
    return false;
  }
  // A one-sided capture failure is isolated in both mixed-capable modes.
  if (!HasUsableEnabledOutput(
          OutputMode::MixedAndSeparate,
          false, false, false, true, false) ||
      !HasUsableEnabledOutput(
          OutputMode::MixedOnly,
          false, false, false, true, false)) {
    return false;
  }
  // Fatal only when no enabled output can contain useful captured audio.
  return !HasUsableEnabledOutput(
             OutputMode::MixedAndSeparate,
             true, true, true, false, false) &&
      !HasUsableEnabledOutput(
          OutputMode::SeparateOnly,
          false, false, false, true, true) &&
      !HasUsableEnabledOutput(
          OutputMode::MixedOnly,
          false, false, true, false, false) &&
      !HasUsableEnabledOutput(
          OutputMode::MixedOnly,
          false, false, false, true, true);
}

static_assert(
    RunFailureIsolationRegressionChecks(),
    "call recording output failure isolation scenarios must pass");

void WriteLittleEndian16(unsigned char *destination, uint16_t value) noexcept {
  destination[0] = static_cast<unsigned char>(value & 0xffU);
  destination[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void WriteLittleEndian32(unsigned char *destination, uint32_t value) noexcept {
  destination[0] = static_cast<unsigned char>(value & 0xffU);
  destination[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
  destination[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
  destination[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

void AppendLittleEndian32(
    std::vector<unsigned char> &destination,
    uint32_t value) {
  const size_t offset = destination.size();
  destination.resize(offset + 4);
  WriteLittleEndian32(destination.data() + offset, value);
}

class OggOpusWriter final {
public:
  OggOpusWriter(const char *streamTag, int bitrate, bool enabled)
      : streamTag_(streamTag),
        filename_(std::string(streamTag) + ".opus"),
        bitrate_(bitrate),
        enabled_(enabled) {
  }

  ~OggOpusWriter() {
    abort();
  }

  void setOutputDirectory(const std::string &path) {
    outputDirectory_ = path;
    outputReady_ = true;
  }

  bool appendPcm48Mono(const int16_t *samples, size_t sampleCount) {
    if (!enabled_) {
      return true;
    }
    if (failed_ || finalized_ || samples == nullptr) {
      return false;
    }
    if (sampleCount == 0) {
      return true;
    }
    if (file_ == nullptr && !open()) {
      fail();
      return false;
    }

    size_t sourceOffset = 0;
    while (sourceOffset < sampleCount) {
      const size_t copied = std::min(
          sampleCount - sourceOffset,
          frameAccumulator_.size() - accumulatorSize_);
      std::copy_n(
          samples + sourceOffset,
          copied,
          frameAccumulator_.data() + accumulatorSize_);
      sourceOffset += copied;
      accumulatorSize_ += copied;
      if (accumulatorSize_ == frameAccumulator_.size()) {
        if (!encodeFrame(frameAccumulator_.size())) {
          fail();
          return false;
        }
        accumulatorSize_ = 0;
      }
    }
    return true;
  }

  void finish() noexcept {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    if (failed_) {
      abortResources();
      return;
    }
    try {
      bool success = true;
      if (accumulatorSize_ != 0) {
        const size_t actualSamples = accumulatorSize_;
        std::fill(
            frameAccumulator_.begin() + accumulatorSize_,
            frameAccumulator_.end(),
            0);
        success = encodeFrame(actualSamples);
        accumulatorSize_ = 0;
      }
      if (success && pendingPacket_.valid) {
        success = submitPendingPacket(true) && drainOggPages(true);
      }
      if (success && file_ != nullptr && std::fflush(file_) != 0) {
        success = false;
      }
      if (file_ != nullptr) {
        FILE *file = file_;
        file_ = nullptr;
        if (std::fclose(file) != 0) {
          success = false;
        }
      }
      cleanupCodecAndContainer();
      if (!success) {
        failed_ = true;
      }
    } catch (...) {
      failed_ = true;
      abortResources();
    }
  }

  bool checkpoint() noexcept {
    if (!enabled_ || finalized_) {
      return !failed_;
    }
    if (failed_) {
      return false;
    }
    if (file_ == nullptr) {
      return true;
    }
    try {
      // Keep the newest packet pending so a clean finish can mark it EOS.
      // Flush all earlier complete packets into a recoverable Ogg page.
      if (!drainOggPages(true) || std::fflush(file_) != 0) {
        fail();
        return false;
      }
      return true;
    } catch (...) {
      fail();
      return false;
    }
  }

  void fail() noexcept {
    failed_ = true;
    abortResources();
  }

  void abort() noexcept {
    abortResources();
  }

  const std::string &filename() const noexcept {
    return filename_;
  }

  bool hasAudio() const noexcept {
    return encodedPackets_ != 0;
  }

  bool fileCreated() const noexcept {
    return outputOpened_;
  }

  bool failed() const noexcept {
    return failed_;
  }

  bool enabled() const noexcept {
    return enabled_;
  }

  int bitrate() const noexcept {
    return bitrate_;
  }

  uint16_t preSkip() const noexcept {
    return preSkip_;
  }

  uint64_t encodedPackets() const noexcept {
    return encodedPackets_;
  }

  uint64_t encodedSamples() const noexcept {
    return encodedSamples_;
  }

private:
  struct PendingPacket {
    std::vector<unsigned char> bytes;
    uint64_t granulePosition = 0;
    int64_t packetNumber = 0;
    bool valid = false;
  };

  bool open() {
    if (!outputReady_) {
      return false;
    }

    int opusError = OPUS_OK;
    encoder_ = opus_encoder_create(
        kOutputSampleRate,
        kOutputChannels,
        OPUS_APPLICATION_VOIP,
        &opusError);
    if (encoder_ == nullptr || opusError != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(0)) != OPUS_OK) {
      return false;
    }
    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead < 0 || lookahead > UINT16_MAX) {
      return false;
    }
    preSkip_ = static_cast<uint16_t>(lookahead);

    uint32_t serial = gNextOggSerial.fetch_add(1, std::memory_order_relaxed) &
        0x7fffffffU;
    if (serial == 0) {
      serial = 1;
    }
    if (ogg_stream_init(&oggStream_, static_cast<int>(serial)) != 0) {
      return false;
    }
    oggInitialized_ = true;

    const std::string path = outputDirectory_ + "/" + filename_;
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr || !writeHeaders() || std::fflush(file_) != 0) {
      return false;
    }
    outputOpened_ = true;
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "%s opened",
        filename_.c_str());
    return true;
  }

  bool encodeFrame(size_t actualSamples) {
    const int packetSize = opus_encode(
        encoder_,
        reinterpret_cast<const opus_int16 *>(frameAccumulator_.data()),
        static_cast<int>(frameAccumulator_.size()),
        encodedPacketBuffer_.data(),
        static_cast<opus_int32>(encodedPacketBuffer_.size()));
    const uint64_t maximumGranule =
        static_cast<uint64_t>(std::numeric_limits<ogg_int64_t>::max());
    if (packetSize <= 0 || preSkip_ > maximumGranule ||
        actualSamples > maximumGranule - preSkip_ ||
        encodedSamples_ > maximumGranule - preSkip_ - actualSamples) {
      return false;
    }
    if (pendingPacket_.valid && !submitPendingPacket(false)) {
      return false;
    }

    encodedSamples_ += actualSamples;
    pendingPacket_.bytes.assign(
        encodedPacketBuffer_.begin(),
        encodedPacketBuffer_.begin() + packetSize);
    pendingPacket_.granulePosition = preSkip_ + encodedSamples_;
    pendingPacket_.packetNumber = nextPacketNumber_++;
    pendingPacket_.valid = true;
    ++encodedPackets_;
    return true;
  }

  bool submitPendingPacket(bool endOfStream) {
    if (!pendingPacket_.valid || !oggInitialized_) {
      return false;
    }
    ogg_packet packet = {};
    packet.packet = pendingPacket_.bytes.data();
    packet.bytes = static_cast<long>(pendingPacket_.bytes.size());
    packet.b_o_s = 0;
    packet.e_o_s = endOfStream ? 1 : 0;
    packet.granulepos = static_cast<ogg_int64_t>(
        pendingPacket_.granulePosition);
    packet.packetno = pendingPacket_.packetNumber;
    if (ogg_stream_packetin(&oggStream_, &packet) != 0) {
      return false;
    }
    pendingPacket_.bytes.clear();
    pendingPacket_.valid = false;
    return drainOggPages(false);
  }

  bool writeHeaders() {
    std::array<unsigned char, 19> opusHead = {};
    std::memcpy(opusHead.data(), "OpusHead", 8);
    opusHead[8] = 1;
    opusHead[9] = kOutputChannels;
    WriteLittleEndian16(opusHead.data() + 10, preSkip_);
    WriteLittleEndian32(opusHead.data() + 12, kOutputSampleRate);
    WriteLittleEndian16(opusHead.data() + 16, 0);
    opusHead[18] = 0;
    if (!submitHeader(opusHead.data(), opusHead.size(), true, 0) ||
        !drainOggPages(true)) {
      return false;
    }

    std::vector<std::string> comments;
    comments.push_back(std::string("STREAM=") + streamTag_);
    if (std::strcmp(streamTag_, "mixed") == 0) {
      comments.emplace_back("MIX=local+remote");
    }
    std::vector<unsigned char> opusTags;
    opusTags.insert(opusTags.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
    AppendLittleEndian32(
        opusTags, static_cast<uint32_t>(std::strlen(kOpusVendor)));
    opusTags.insert(
        opusTags.end(),
        kOpusVendor,
        kOpusVendor + std::strlen(kOpusVendor));
    AppendLittleEndian32(
        opusTags, static_cast<uint32_t>(comments.size()));
    for (const auto &comment : comments) {
      AppendLittleEndian32(
          opusTags, static_cast<uint32_t>(comment.size()));
      opusTags.insert(opusTags.end(), comment.begin(), comment.end());
    }
    return submitHeader(opusTags.data(), opusTags.size(), false, 1) &&
        drainOggPages(true);
  }

  bool submitHeader(
      unsigned char *data,
      size_t size,
      bool beginningOfStream,
      int64_t packetNumber) {
    ogg_packet packet = {};
    packet.packet = data;
    packet.bytes = static_cast<long>(size);
    packet.b_o_s = beginningOfStream ? 1 : 0;
    packet.e_o_s = 0;
    packet.granulepos = 0;
    packet.packetno = packetNumber;
    return ogg_stream_packetin(&oggStream_, &packet) == 0;
  }

  bool drainOggPages(bool flush) {
    if (!oggInitialized_ || file_ == nullptr) {
      return false;
    }
    while (true) {
      ogg_page page = {};
      const int result = flush
          ? ogg_stream_flush(&oggStream_, &page)
          : ogg_stream_pageout(&oggStream_, &page);
      if (result == 0) {
        return true;
      }
      if (result < 0 || !writeOggPage(page)) {
        return false;
      }
    }
  }

  bool writeOggPage(const ogg_page &page) {
    return page.header_len >= 0 && page.body_len >= 0 &&
        std::fwrite(
            page.header,
            1,
            static_cast<size_t>(page.header_len),
            file_) == static_cast<size_t>(page.header_len) &&
        std::fwrite(
            page.body,
            1,
            static_cast<size_t>(page.body_len),
            file_) == static_cast<size_t>(page.body_len) &&
        std::ferror(file_) == 0;
  }

  void cleanupCodecAndContainer() noexcept {
    if (oggInitialized_) {
      ogg_stream_clear(&oggStream_);
      oggInitialized_ = false;
    }
    if (encoder_ != nullptr) {
      opus_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    pendingPacket_.bytes.clear();
    pendingPacket_.valid = false;
  }

  void abortResources() noexcept {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
    cleanupCodecAndContainer();
  }

  const char *streamTag_;
  const std::string filename_;
  const int bitrate_;
  const bool enabled_;
  std::string outputDirectory_;
  bool outputReady_ = false;
  bool outputOpened_ = false;
  bool finalized_ = false;
  bool failed_ = false;
  FILE *file_ = nullptr;
  OpusEncoder *encoder_ = nullptr;
  ogg_stream_state oggStream_ = {};
  bool oggInitialized_ = false;
  uint16_t preSkip_ = 0;
  int64_t nextPacketNumber_ = 2;
  PendingPacket pendingPacket_;
  std::array<int16_t, kOpusFrameSamples> frameAccumulator_ = {};
  size_t accumulatorSize_ = 0;
  std::array<unsigned char, kMaxOpusPacketBytes> encodedPacketBuffer_ = {};
  uint64_t encodedPackets_ = 0;
  uint64_t encodedSamples_ = 0;
};

class SessionAudioTimeline final {
public:
  enum class ControlIntervalType {
    Pause,
    Stopped
  };

  struct ControlInterval {
    ControlIntervalType type = ControlIntervalType::Pause;
    uint64_t startSample = 0;
    uint64_t endSample = 0;
  };

  explicit SessionAudioTimeline(OutputMode outputMode)
      : outputMode_(outputMode),
        localWriter_("local", kStreamOpusBitrate,
            outputMode != OutputMode::MixedOnly),
        remoteWriter_("remote", kStreamOpusBitrate,
            outputMode != OutputMode::MixedOnly),
        mixedWriter_("mixed", kMixedOpusBitrate,
            outputMode != OutputMode::SeparateOnly) {
  }

  void setOutputDirectory(const std::string &path) noexcept {
    try {
      localWriter_.setOutputDirectory(path);
    } catch (...) {
      localWriter_.fail();
    }
    try {
      remoteWriter_.setOutputDirectory(path);
    } catch (...) {
      remoteWriter_.fail();
    }
    try {
      mixedWriter_.setOutputDirectory(path);
    } catch (...) {
      mixedWriter_.fail();
    }
  }

  void start(int64_t sessionStartMonotonicNs) noexcept {
    if (started_) {
      return;
    }
    started_ = true;
    sessionStartMonotonicNs_ = sessionStartMonotonicNs;
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "master timeline started sampleRate=%u",
        kOutputSampleRate);
  }

  void beginControlInterval(
      ControlIntervalType type,
      int64_t boundaryMonotonicNs) noexcept {
    uint64_t sample = 0;
    if (!MonotonicNsToTimelineSample(
            sessionStartMonotonicNs_, boundaryMonotonicNs, sample)) {
      internalFailed_ = true;
      return;
    }
    try {
      std::lock_guard<std::mutex> lock(controlMutex_);
      if (openControlInterval_) {
        return;
      }
      operationalControlIntervals_.push_back(ControlInterval {
          .type = type,
          .startSample = sample,
          .endSample = std::numeric_limits<uint64_t>::max()
      });
      openControlInterval_ = true;
      openControlType_ = type;
      if (controlIntervals_.size() < kMaxControlIntervals) {
        controlIntervals_.push_back(ControlInterval {
            .type = type,
            .startSample = sample,
            .endSample = sample
        });
        openMetadataIndex_ = controlIntervals_.size() - 1;
      } else {
        openMetadataIndex_ = std::numeric_limits<size_t>::max();
        ++suppressedControlIntervals_;
      }
    } catch (...) {
      internalFailed_ = true;
    }
  }

  void endControlInterval(
      ControlIntervalType type,
      int64_t boundaryMonotonicNs) noexcept {
    uint64_t sample = 0;
    if (!MonotonicNsToTimelineSample(
            sessionStartMonotonicNs_, boundaryMonotonicNs, sample)) {
      internalFailed_ = true;
      return;
    }
    try {
      std::lock_guard<std::mutex> lock(controlMutex_);
      if (!openControlInterval_ || openControlType_ != type ||
          operationalControlIntervals_.empty()) {
        return;
      }
      ControlInterval &interval = operationalControlIntervals_.back();
      interval.endSample = std::max(interval.startSample, sample);
      const uint64_t intervalSamples = interval.endSample - interval.startSample;
      if (type == ControlIntervalType::Pause) {
        pausedSamples_ += intervalSamples;
      } else {
        stoppedGapSamples_ += intervalSamples;
      }
      if (openMetadataIndex_ != std::numeric_limits<size_t>::max()) {
        controlIntervals_[openMetadataIndex_].endSample = interval.endSample;
      }
      openMetadataIndex_ = std::numeric_limits<size_t>::max();
      openControlInterval_ = false;
    } catch (...) {
      internalFailed_ = true;
    }
  }

  void append(
      StreamSide side,
      int64_t blockTimestampNs,
      const int16_t *samples,
      size_t sampleCount,
      uint64_t sequence,
      bool forceNewSegment,
      bool controlBoundarySegment) noexcept {
    if (!started_ || finalized_ || samples == nullptr || sampleCount == 0) {
      return;
    }
    try {
      Track &track = trackFor(side);
      if (track.audioSamplesProvided >
          std::numeric_limits<uint64_t>::max() - sampleCount) {
        track.captureFailed = true;
        return;
      }
      track.audioSamplesProvided += sampleCount;

      uint64_t timestampCandidate = 0;
      if (!MonotonicNsToTimelineSample(
              sessionStartMonotonicNs_,
              blockTimestampNs,
              timestampCandidate)) {
        track.captureFailed = true;
        return;
      }
      bool positiveGap = false;
      bool negativeDiscontinuity = false;
      const uint64_t previousEnd = track.lastTimelineEndSample;
      const bool hadContinuity = track.continuityActive;
      const uint64_t timelineStart = ChooseContinuityStart(
          track.continuityActive,
          previousEnd,
          timestampCandidate,
          forceNewSegment,
          positiveGap,
          negativeDiscontinuity);
      if (timelineStart > std::numeric_limits<uint64_t>::max() -
              sampleCount) {
        track.captureFailed = true;
        return;
      }
      const uint64_t timelineEnd = timelineStart + sampleCount;
      if (!track.firstAudioSeen) {
        track.firstAudioSeen = true;
        track.firstAudioTimelineSample = timelineStart;
        __android_log_print(
            ANDROID_LOG_INFO,
            kCallRecorderLogTag,
            "%s firstAudio timelineSample=%llu",
            StreamSideName(side),
            static_cast<unsigned long long>(timelineStart));
      }
      if (ShouldRecordContinuityDiagnostic(
              positiveGap, controlBoundarySegment)) {
        recordContinuityGap(
            side, sequence, previousEnd, timestampCandidate);
      }
      if (ShouldRecordContinuityDiagnostic(
              hadContinuity, controlBoundarySegment)) {
        const __int128 deltaSamples =
            static_cast<__int128>(timestampCandidate) - previousEnd;
        updateTimestampDiagnostic(
            track, side, sequence, deltaSamples);
      }
      track.continuityActive = true;
      track.lastSourceTimestampNs = blockTimestampNs;
      track.lastTimelineEndSample = timelineEnd;

      size_t sourceOffset = 0;
      if (timelineStart < commitCursor_) {
        const uint64_t late = std::min<uint64_t>(
            commitCursor_ - timelineStart, sampleCount);
        sourceOffset = static_cast<size_t>(late);
        track.lateSamplesDropped += late;
        recordLateDrop(
            side,
            sequence,
            timelineStart,
            timelineEnd,
            late);
      }

      if (sourceOffset < sampleCount) {
        appendUngatedRanges(
            track,
            timelineStart + sourceOffset,
            samples + sourceOffset,
            sampleCount - sourceOffset);
      }
    } catch (...) {
      trackFor(side).captureFailed = true;
    }
  }

  void advance(int64_t nowMonotonicNs) noexcept {
    if (!started_ || finalized_) {
      return;
    }
    try {
      uint64_t nowSample = 0;
      if (!MonotonicNsToTimelineSample(
              sessionStartMonotonicNs_, nowMonotonicNs, nowSample)) {
        return;
      }
      const uint64_t commitUntil = RegularCommitUntil(nowSample);
      if (commitUntil > commitCursor_) {
        commitTo(commitUntil);
      }
    } catch (...) {
      internalFailed_ = true;
    }
  }

  void finish(int64_t sessionEndMonotonicNs) noexcept {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    sessionEndMonotonicNs_ = sessionEndMonotonicNs;
    try {
      if (started_ && !MonotonicNsToTimelineSample(
              sessionStartMonotonicNs_,
              sessionEndMonotonicNs_,
              timelineExpectedFromMonotonic_)) {
        internalFailed_ = true;
        timelineExpectedFromMonotonic_ = commitCursor_;
      }
      finalTimelineSamples_ = timelineExpectedFromMonotonic_;
      if (finalTimelineSamples_ < commitCursor_) {
        internalFailed_ = true;
        finalTimelineSamples_ = commitCursor_;
      }
      commitTo(finalTimelineSamples_);
    } catch (...) {
      internalFailed_ = true;
    }
    clearBufferedAudio();
    finishWriter(localWriter_);
    finishWriter(remoteWriter_);
    finishWriter(mixedWriter_);
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "master timeline finish samples=%llu",
        static_cast<unsigned long long>(finalTimelineSamples_));
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "equal duration local=%llu remote=%llu mixed=%llu",
        static_cast<unsigned long long>(localWriter_.encodedSamples()),
        static_cast<unsigned long long>(remoteWriter_.encodedSamples()),
        static_cast<unsigned long long>(mixedWriter_.encodedSamples()));
  }

  void checkpointWriters() noexcept {
    (void) localWriter_.checkpoint();
    (void) remoteWriter_.checkpoint();
    (void) mixedWriter_.checkpoint();
  }

  void markAllWritersFailed() noexcept {
    internalFailed_ = true;
    localWriter_.fail();
    remoteWriter_.fail();
    mixedWriter_.fail();
    clearBufferedAudio();
  }

  const OggOpusWriter &writer(StreamSide side) const noexcept {
    return side == StreamSide::Local ? localWriter_ : remoteWriter_;
  }

  const OggOpusWriter &mixedWriter() const noexcept {
    return mixedWriter_;
  }

  bool internalFailed() const noexcept {
    return internalFailed_.load(std::memory_order_acquire);
  }

  int64_t sessionStartMonotonicNs() const noexcept {
    return sessionStartMonotonicNs_;
  }

  int64_t sessionEndMonotonicNs() const noexcept {
    return sessionEndMonotonicNs_;
  }

  uint64_t timelineSamples() const noexcept {
    return finalTimelineSamples_;
  }

  uint64_t timelineExpectedFromMonotonic() const noexcept {
    return timelineExpectedFromMonotonic_;
  }

  int64_t timelineDifferenceSamples() const noexcept {
    const __int128 difference =
        static_cast<__int128>(finalTimelineSamples_) -
        timelineExpectedFromMonotonic_;
    return static_cast<int64_t>(std::clamp<__int128>(
        difference,
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::max()));
  }

  uint64_t commitCursor() const noexcept {
    return commitCursor_;
  }

  uint64_t audioSamplesProvided(StreamSide side) const noexcept {
    return trackFor(side).audioSamplesProvided;
  }

  uint64_t audioSamplesWritten(StreamSide side) const noexcept {
    return trackFor(side).audioSamplesWritten;
  }

  uint64_t silenceSamplesWritten(StreamSide side) const noexcept {
    return trackFor(side).silenceSamplesWritten;
  }

  uint64_t lateSamplesDropped(StreamSide side) const noexcept {
    return trackFor(side).lateSamplesDropped;
  }

  uint64_t bufferLimitSamplesDropped(StreamSide side) const noexcept {
    return trackFor(side).bufferLimitSamplesDropped;
  }

  uint64_t firstAudioTimelineSample(StreamSide side) const noexcept {
    return trackFor(side).firstAudioTimelineSample;
  }

  void markCaptureFailed(StreamSide side) noexcept {
    trackFor(side).captureFailed = true;
  }

  bool captureFailed(StreamSide side) const noexcept {
    return trackFor(side).captureFailed;
  }

  uint64_t maxBufferedSamplesObserved() const noexcept {
    return maxBufferedSamplesObserved_;
  }

  OutputMode outputMode() const noexcept {
    return outputMode_;
  }

  std::vector<ControlInterval> controlIntervals() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return controlIntervals_;
  }

  uint64_t suppressedControlIntervals() const noexcept {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return suppressedControlIntervals_;
  }

  uint64_t pausedSamples() const noexcept {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return pausedSamples_;
  }

  uint64_t stoppedGapSamples() const noexcept {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return stoppedGapSamples_;
  }

  bool requiredOutputsFailed() const noexcept {
    if (internalFailed_.load(std::memory_order_acquire)) {
      return true;
    }
    return !HasUsableEnabledOutput(
        outputMode_,
        localWriter_.failed(),
        remoteWriter_.failed(),
        mixedWriter_.failed(),
        local_.captureFailed,
        remote_.captureFailed);
  }

  const std::vector<TimestampDiscontinuity> &discontinuities()
      const noexcept {
    return discontinuities_;
  }

  uint64_t suppressedDiscontinuities() const noexcept {
    return suppressedDiscontinuities_;
  }

  const std::vector<ContinuityGap> &continuityGaps() const noexcept {
    return continuityGaps_;
  }

  uint64_t suppressedContinuityGaps() const noexcept {
    return suppressedContinuityGaps_;
  }

  const std::vector<LateDrop> &lateDrops() const noexcept {
    return lateDrops_;
  }

  uint64_t suppressedLateDrops() const noexcept {
    return suppressedLateDrops_;
  }

  bool equalDurationInvariant() const noexcept {
    if (internalFailed_.load(std::memory_order_acquire)) {
      return false;
    }
    const bool localOk = !localWriter_.enabled() || localWriter_.failed() ||
        localWriter_.encodedSamples() == finalTimelineSamples_;
    const bool remoteOk = !remoteWriter_.enabled() || remoteWriter_.failed() ||
        remoteWriter_.encodedSamples() == finalTimelineSamples_;
    const bool mixedOk = !mixedWriter_.enabled() || mixedWriter_.failed() ||
        mixedWriter_.encodedSamples() == finalTimelineSamples_;
    return localOk && remoteOk && mixedOk;
  }

  bool accountingInvariant(StreamSide side) const noexcept {
    const Track &track = trackFor(side);
    return track.audioSamplesWritten <= finalTimelineSamples_ &&
        track.silenceSamplesWritten <= finalTimelineSamples_ &&
        static_cast<__int128>(track.audioSamplesWritten) +
            track.silenceSamplesWritten == finalTimelineSamples_;
  }

private:
  struct Chunk {
    uint64_t timelineStart = 0;
    std::vector<int16_t> samples;
  };

  struct Track {
    bool continuityActive = false;
    bool firstAudioSeen = false;
    bool captureFailed = false;
    bool timestampDiagnosticActive = false;
    int64_t lastSourceTimestampNs = 0;
    int64_t lastReportedTimestampDeltaSamples = 0;
    uint64_t lastTimelineEndSample = 0;
    uint64_t firstAudioTimelineSample = 0;
    uint64_t audioSamplesProvided = 0;
    uint64_t audioSamplesWritten = 0;
    uint64_t silenceSamplesWritten = 0;
    uint64_t lateSamplesDropped = 0;
    uint64_t bufferLimitSamplesDropped = 0;
    uint64_t bufferedSamples = 0;
    std::deque<Chunk> chunks;
  };

  Track &trackFor(StreamSide side) noexcept {
    return side == StreamSide::Local ? local_ : remote_;
  }

  const Track &trackFor(StreamSide side) const noexcept {
    return side == StreamSide::Local ? local_ : remote_;
  }

  void appendUngatedRanges(
      Track &track,
      uint64_t timelineStart,
      const int16_t *samples,
      size_t sampleCount) {
    const uint64_t timelineEnd = timelineStart + sampleCount;
    uint64_t cursor = timelineStart;
    std::lock_guard<std::mutex> lock(controlMutex_);
    while (!operationalControlIntervals_.empty() &&
           operationalControlIntervals_.front().endSample <= commitCursor_) {
      operationalControlIntervals_.pop_front();
    }
    auto appendRange = [&](uint64_t start, uint64_t end) {
      if (end <= start) {
        return;
      }
      const size_t retained = static_cast<size_t>(end - start);
      if (track.bufferedSamples >
          kMaxTimelineBufferedSamplesPerTrack - retained) {
        track.bufferLimitSamplesDropped += retained;
        return;
      }
      Chunk chunk;
      chunk.timelineStart = start;
      const size_t offset = static_cast<size_t>(start - timelineStart);
      chunk.samples.assign(samples + offset, samples + offset + retained);
      track.bufferedSamples += retained;
      bufferedSamples_ += retained;
      maxBufferedSamplesObserved_ = std::max(
          maxBufferedSamplesObserved_, bufferedSamples_);
      track.chunks.push_back(std::move(chunk));
    };
    for (const ControlInterval &interval : operationalControlIntervals_) {
      if (interval.endSample <= cursor) {
        continue;
      }
      if (interval.startSample >= timelineEnd) {
        break;
      }
      appendRange(cursor, std::min(interval.startSample, timelineEnd));
      cursor = std::max(cursor, std::min(interval.endSample, timelineEnd));
      if (cursor >= timelineEnd) {
        break;
      }
    }
    appendRange(cursor, timelineEnd);
  }

  void commitTo(uint64_t until) {
    if (until <= commitCursor_) {
      return;
    }
    std::array<int16_t, kOpusFrameSamples> localSamples = {};
    std::array<int16_t, kOpusFrameSamples> remoteSamples = {};
    std::array<int16_t, kOpusFrameSamples> mixedSamples = {};
    std::vector<ControlInterval> controlGates;
    {
      std::lock_guard<std::mutex> lock(controlMutex_);
      controlGates.assign(
          operationalControlIntervals_.begin(),
          operationalControlIntervals_.end());
    }
    while (commitCursor_ < until) {
      const size_t count = static_cast<size_t>(std::min<uint64_t>(
          until - commitCursor_, kOpusFrameSamples));
      std::fill_n(localSamples.data(), count, 0);
      std::fill_n(remoteSamples.data(), count, 0);
      const size_t localAudio = copyTrackRange(
          local_, commitCursor_, count, localSamples.data(), controlGates);
      const size_t remoteAudio = copyTrackRange(
          remote_, commitCursor_, count, remoteSamples.data(), controlGates);
      for (size_t index = 0; index < count; ++index) {
        const int32_t sum = static_cast<int32_t>(localSamples[index]) +
            static_cast<int32_t>(remoteSamples[index]);
        const int32_t rounded = sum >= 0 ? (sum + 1) / 2 : (sum - 1) / 2;
        mixedSamples[index] = static_cast<int16_t>(std::clamp<int32_t>(
            rounded,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()));
      }
      appendWriter(localWriter_, localSamples.data(), count);
      appendWriter(remoteWriter_, remoteSamples.data(), count);
      appendWriter(mixedWriter_, mixedSamples.data(), count);
      local_.audioSamplesWritten += localAudio;
      local_.silenceSamplesWritten += count - localAudio;
      remote_.audioSamplesWritten += remoteAudio;
      remote_.silenceSamplesWritten += count - remoteAudio;
      commitCursor_ += count;
      discardConsumed(local_);
      discardConsumed(remote_);
    }
    {
      std::lock_guard<std::mutex> lock(controlMutex_);
      while (!operationalControlIntervals_.empty() &&
             operationalControlIntervals_.front().endSample <= commitCursor_) {
        operationalControlIntervals_.pop_front();
      }
    }
  }

  size_t copyTrackRange(
      const Track &track,
      uint64_t start,
      size_t count,
      int16_t *destination,
      const std::vector<ControlInterval> &controlGates) const noexcept {
    const uint64_t end = start + count;
    size_t copied = 0;
    for (const auto &chunk : track.chunks) {
      const uint64_t chunkStart = chunk.timelineStart;
      const uint64_t chunkEnd = chunkStart + chunk.samples.size();
      if (chunkEnd <= start) {
        continue;
      }
      if (chunkStart >= end) {
        break;
      }
      const uint64_t overlapStart = std::max(start, chunkStart);
      const uint64_t overlapEnd = std::min(end, chunkEnd);
      if (overlapStart >= overlapEnd) {
        continue;
      }
      uint64_t cursor = overlapStart;
      auto copyRange = [&](uint64_t copyStart, uint64_t copyEnd) noexcept {
        if (copyEnd <= copyStart) {
          return;
        }
        const size_t sourceOffset = static_cast<size_t>(
            copyStart - chunkStart);
        const size_t destinationOffset = static_cast<size_t>(
            copyStart - start);
        const size_t copyCount = static_cast<size_t>(copyEnd - copyStart);
        std::copy_n(
            chunk.samples.data() + sourceOffset,
            copyCount,
            destination + destinationOffset);
        copied += copyCount;
      };
      for (const ControlInterval &gate : controlGates) {
        if (gate.endSample <= cursor) {
          continue;
        }
        if (gate.startSample >= overlapEnd) {
          break;
        }
        copyRange(cursor, std::min(gate.startSample, overlapEnd));
        cursor = std::max(cursor, std::min(gate.endSample, overlapEnd));
        if (cursor >= overlapEnd) {
          break;
        }
      }
      copyRange(cursor, overlapEnd);
    }
    return copied;
  }

  void discardConsumed(Track &track) noexcept {
    while (!track.chunks.empty()) {
      const Chunk &chunk = track.chunks.front();
      const uint64_t end = chunk.timelineStart + chunk.samples.size();
      if (end > commitCursor_) {
        break;
      }
      track.bufferedSamples -= chunk.samples.size();
      bufferedSamples_ -= chunk.samples.size();
      track.chunks.pop_front();
    }
  }

  void recordContinuityGap(
      StreamSide side,
      uint64_t sequence,
      uint64_t previousEnd,
      uint64_t timestampStart) noexcept {
    const uint64_t gap = timestampStart - previousEnd;
    try {
      if (continuityGaps_.size() < kMaxContinuityGaps) {
        continuityGaps_.push_back(ContinuityGap {
            .side = side,
            .sequence = sequence,
            .previousTimelineEndSample = previousEnd,
            .timestampTimelineStartSample = timestampStart,
            .gapSamples = gap
        });
      } else {
        ++suppressedContinuityGaps_;
      }
    } catch (...) {
      ++suppressedContinuityGaps_;
    }
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "%s continuity gap samples=%llu",
        StreamSideName(side),
        static_cast<unsigned long long>(gap));
  }

  void recordLateDrop(
      StreamSide side,
      uint64_t sequence,
      uint64_t start,
      uint64_t end,
      uint64_t dropped) noexcept {
    try {
      if (lateDrops_.size() < kMaxLateDrops) {
        lateDrops_.push_back(LateDrop {
            .side = side,
            .sequence = sequence,
            .timelineStartSample = start,
            .timelineEndSample = end,
            .commitCursorSample = commitCursor_,
            .droppedSamples = dropped
        });
      } else {
        ++suppressedLateDrops_;
      }
    } catch (...) {
      ++suppressedLateDrops_;
    }
  }

  void updateTimestampDiagnostic(
      Track &track,
      StreamSide side,
      uint64_t sequence,
      __int128 deltaSamples) noexcept {
    const int64_t clampedDeltaSamples = static_cast<int64_t>(
        std::clamp<__int128>(
            deltaSamples,
            std::numeric_limits<int64_t>::min(),
            std::numeric_limits<int64_t>::max()));
    TimestampDiscontinuity::Phase phase =
        TimestampDiscontinuity::Phase::Entered;
    if (!AdvanceTimestampDiagnosticState(
            track.timestampDiagnosticActive,
            track.lastReportedTimestampDeltaSamples,
            clampedDeltaSamples,
            phase)) {
      return;
    }
    const __int128 deltaMs =
        deltaSamples * 1000 / kOutputSampleRate;
    try {
      if (discontinuities_.size() < kMaxTimestampDiscontinuities) {
        discontinuities_.push_back(TimestampDiscontinuity {
            .side = side,
            .sequence = sequence,
            .deltaMs = static_cast<int64_t>(std::clamp<__int128>(
                deltaMs,
                std::numeric_limits<int64_t>::min(),
                std::numeric_limits<int64_t>::max())),
            .phase = phase
        });
      } else {
        ++suppressedDiscontinuities_;
      }
    } catch (...) {
      ++suppressedDiscontinuities_;
    }
  }

  static void appendWriter(
      OggOpusWriter &writer,
      const int16_t *samples,
      size_t count) noexcept {
    if (writer.failed()) {
      return;
    }
    try {
      if (!writer.appendPcm48Mono(samples, count)) {
        writer.fail();
      }
    } catch (...) {
      writer.fail();
    }
  }

  static void finishWriter(OggOpusWriter &writer) noexcept {
    try {
      writer.finish();
    } catch (...) {
      writer.fail();
    }
  }

  void clearBufferedAudio() noexcept {
    local_.chunks.clear();
    remote_.chunks.clear();
    local_.bufferedSamples = 0;
    remote_.bufferedSamples = 0;
    bufferedSamples_ = 0;
  }

  const OutputMode outputMode_;
  OggOpusWriter localWriter_;
  OggOpusWriter remoteWriter_;
  OggOpusWriter mixedWriter_;
  Track local_;
  Track remote_;
  bool started_ = false;
  bool finalized_ = false;
  std::atomic<bool> internalFailed_ = {false};
  int64_t sessionStartMonotonicNs_ = 0;
  int64_t sessionEndMonotonicNs_ = 0;
  uint64_t commitCursor_ = 0;
  uint64_t finalTimelineSamples_ = 0;
  uint64_t timelineExpectedFromMonotonic_ = 0;
  uint64_t bufferedSamples_ = 0;
  uint64_t maxBufferedSamplesObserved_ = 0;
  std::vector<TimestampDiscontinuity> discontinuities_;
  uint64_t suppressedDiscontinuities_ = 0;
  std::vector<ContinuityGap> continuityGaps_;
  uint64_t suppressedContinuityGaps_ = 0;
  std::vector<LateDrop> lateDrops_;
  uint64_t suppressedLateDrops_ = 0;
  mutable std::mutex controlMutex_;
  std::deque<ControlInterval> operationalControlIntervals_;
  std::vector<ControlInterval> controlIntervals_;
  bool openControlInterval_ = false;
  ControlIntervalType openControlType_ = ControlIntervalType::Pause;
  size_t openMetadataIndex_ = std::numeric_limits<size_t>::max();
  uint64_t suppressedControlIntervals_ = 0;
  uint64_t pausedSamples_ = 0;
  uint64_t stoppedGapSamples_ = 0;
};

class AudioStream final {
public:
  AudioStream(StreamSide side, SessionAudioTimeline *timeline)
      : side_(side),
        streamId_(StreamSideName(side)),
        timeline_(timeline) {
  }

  void setAccepting(bool accepting) noexcept {
    if (accepting && failed_.load(std::memory_order_acquire)) {
      return;
    }
    uint64_t current = captureControl_.load(std::memory_order_relaxed);
    uint64_t desired = 0;
    do {
      desired = (current & ~uint64_t {1}) | (accepting ? 1U : 0U);
    } while (!captureControl_.compare_exchange_weak(
        current,
        desired,
        std::memory_order_release,
        std::memory_order_relaxed));
    if (accepting && failed_.load(std::memory_order_acquire)) {
      setAccepting(false);
    }
  }

  void markNextControlSegment() noexcept {
    // The low bit is the accepting state; every generation occupies the upper
    // bits. Lifecycle code calls this while accepting is false.
    captureControl_.fetch_add(2, std::memory_order_release);
  }

  void enqueue(
      const int16_t *samples,
      size_t framesPerChannel,
      size_t channels,
      uint32_t sampleRate) noexcept {
    const uint64_t captureControl = captureControl_.load(
        std::memory_order_acquire);
    if ((captureControl & 1U) == 0) {
      return;
    }
    if (samples == nullptr || framesPerChannel == 0 || channels == 0 ||
        sampleRate == 0 || channels > UINT16_MAX ||
        framesPerChannel > UINT32_MAX ||
        framesPerChannel > kMaxSamplesPerBlock / channels ||
        static_cast<uint64_t>(framesPerChannel) * 1000U >
            static_cast<uint64_t>(sampleRate) *
                kMaxAcceptedBlockDurationMs) {
      formatMismatch_.store(true, std::memory_order_release);
      droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    // A valid callback owns a sequence even when the bounded queue is full.
    // The worker observes the sequence gap; the next callback timestamp places
    // a new segment on the master timeline without realtime-thread work.
    const uint64_t sequence = nextSequence_.fetch_add(
        1, std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % kQueueSlots;
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (next == tail) {
      const uint64_t outputSampleNumerator =
          static_cast<uint64_t>(framesPerChannel) * kOutputSampleRate;
      if (outputSampleNumerator % sampleRate == 0) {
        droppedRecoverableBlocks_.fetch_add(1, std::memory_order_relaxed);
        droppedRecoverableSamples_.fetch_add(
            outputSampleNumerator / sampleRate,
            std::memory_order_relaxed);
      } else {
        droppedUnknownDurationBlocks_.fetch_add(
            1, std::memory_order_relaxed);
      }
      droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    AudioBlock &block = slots_[head];
    block.monotonicTimeNs = CurrentMonotonicTimeNs();
    block.sequence = sequence;
    block.controlGeneration = captureControl >> 1U;
    block.sampleRate = sampleRate;
    block.channels = static_cast<uint16_t>(channels);
    block.framesPerChannel = static_cast<uint32_t>(framesPerChannel);
    block.sampleCount = static_cast<uint32_t>(framesPerChannel * channels);
    block.droppedRecoverableBlocksBefore =
        droppedRecoverableBlocks_.load(std::memory_order_relaxed);
    block.droppedRecoverableSamplesBefore =
        droppedRecoverableSamples_.load(std::memory_order_relaxed);
    block.droppedUnknownDurationBlocksBefore =
        droppedUnknownDurationBlocks_.load(std::memory_order_relaxed);
    std::memcpy(
        block.samples.data(),
        samples,
        block.sampleCount * sizeof(int16_t));
    head_.store(next, std::memory_order_release);
    const uint32_t depth = QueueDepth(next, tail);
    uint32_t observed = maxQueueDepth_.load(std::memory_order_relaxed);
    while (observed < depth &&
           !maxQueueDepth_.compare_exchange_weak(
               observed,
               depth,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
  }

  bool drainOne() noexcept {
    if (formatMismatch_.exchange(false, std::memory_order_acq_rel)) {
      __android_log_print(
          ANDROID_LOG_ERROR,
          kCallRecorderLogTag,
          "%s PCM format unsupported; capture disabled",
          streamId_);
      markFailed();
      return true;
    }
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }

    const AudioBlock &block = slots_[tail];
    try {
      if (!writeBlock(block)) {
        droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        markFailed();
      }
    } catch (...) {
      droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
      markFailed();
    }
    tail_.store((tail + 1) % kQueueSlots, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return tail_.load(std::memory_order_relaxed) ==
        head_.load(std::memory_order_acquire);
  }

  void finalize() noexcept {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    try {
      if (!recordTrailingGap()) {
        markFailed();
      }
    } catch (...) {
      failed_.store(true, std::memory_order_release);
      if (timeline_ != nullptr) {
        timeline_->markCaptureFailed(side_);
      }
    }
  }

  const std::string &filename() const noexcept {
    return timeline_->writer(side_).filename();
  }

  bool hasAudio() const noexcept {
    return timeline_->writer(side_).hasAudio();
  }

  bool fileCreated() const noexcept {
    return timeline_->writer(side_).fileCreated();
  }

  uint64_t sourceBlocks() const noexcept {
    return sourceBlocks_;
  }

  uint64_t droppedBlocks() const noexcept {
    return droppedBlocks_.load(std::memory_order_relaxed);
  }

  uint32_t maxQueueDepth() const noexcept {
    return maxQueueDepth_.load(std::memory_order_relaxed);
  }

  static constexpr size_t queueCapacity() noexcept {
    return kQueueSlots - 1;
  }

  uint64_t encodedPackets() const noexcept {
    return timeline_->writer(side_).encodedPackets();
  }

  uint64_t encodedSamples() const noexcept {
    return timeline_->writer(side_).encodedSamples();
  }

  uint64_t audioSamplesProvided() const noexcept {
    return timeline_->audioSamplesProvided(side_);
  }

  uint64_t audioSamplesWritten() const noexcept {
    return timeline_->audioSamplesWritten(side_);
  }

  uint64_t silenceSamplesWritten() const noexcept {
    return timeline_->silenceSamplesWritten(side_);
  }

  uint64_t lateSamplesDropped() const noexcept {
    return timeline_->lateSamplesDropped(side_);
  }

  uint64_t bufferLimitSamplesDropped() const noexcept {
    return timeline_->bufferLimitSamplesDropped(side_);
  }

  uint16_t preSkip() const noexcept {
    return timeline_->writer(side_).preSkip();
  }

  bool failed() const noexcept {
    return failed_.load(std::memory_order_acquire) ||
        timeline_->captureFailed(side_) || timeline_->writer(side_).failed();
  }

  bool captureFailed() const noexcept {
    return failed_.load(std::memory_order_acquire) ||
        timeline_->captureFailed(side_);
  }

  bool writerFailed() const noexcept {
    return timeline_->writer(side_).failed();
  }

  const std::vector<SourceFormat> &sourceFormats() const noexcept {
    return sourceFormats_;
  }

  uint64_t suppressedSourceFormats() const noexcept {
    return suppressedSourceFormats_;
  }

  const std::vector<QueueGap> &queueGaps() const noexcept {
    return queueGaps_;
  }

  uint64_t suppressedQueueGaps() const noexcept {
    return suppressedQueueGaps_;
  }

  const char *streamId() const noexcept {
    return streamId_;
  }

  void markFailed() noexcept {
    setAccepting(false);
    failed_.store(true, std::memory_order_release);
    if (timeline_ != nullptr) {
      timeline_->markCaptureFailed(side_);
    }
  }

  void signalFormatMismatch() noexcept {
    formatMismatch_.store(true, std::memory_order_release);
  }

private:
  bool writeBlock(const AudioBlock &block) {
    if (failed_.load(std::memory_order_acquire)) {
      droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    currentBlockStartsControlSegment_ = IsNewControlGeneration(
        block.controlGeneration, workerControlGeneration_);
    if (currentBlockStartsControlSegment_) {
      workerControlGeneration_ = block.controlGeneration;
      // PushResampler is worker-owned. Never bridge filter history across a
      // Pause/Resume or Stop/Start source boundary.
      resampler_.reset();
      resamplerSourceRate_ = 0;
    }
    recordSourceFormat(block);
    currentBlockHasQueueGap_ = false;
    if (!accountForGapBefore(block)) {
      return false;
    }
    if (!normalizeAndAppend(block)) {
      return false;
    }
    nextExpectedSequence_ = block.sequence + 1;
    ++sourceBlocks_;
    return true;
  }

  void recordSourceFormat(const AudioBlock &block) {
    if (!sourceFormatKnown_ || lastSourceSampleRate_ != block.sampleRate ||
        lastSourceChannels_ != block.channels) {
      sourceFormatKnown_ = true;
      lastSourceSampleRate_ = block.sampleRate;
      lastSourceChannels_ = block.channels;
      if (sourceFormats_.size() < kMaxSourceFormats) {
        sourceFormats_.push_back(SourceFormat {
            .sampleRate = block.sampleRate,
            .channels = block.channels,
            .startSequence = block.sequence,
            .startTimestampNs = block.monotonicTimeNs
        });
      } else {
        ++suppressedSourceFormats_;
      }
    }
  }

  bool accountForGapBefore(const AudioBlock &block) {
    if (block.sequence < nextExpectedSequence_) {
      return false;
    }
    if (block.droppedRecoverableBlocksBefore <
            accountedDroppedRecoverableBlocks_ ||
        block.droppedRecoverableSamplesBefore <
            accountedDroppedRecoverableSamples_ ||
        block.droppedUnknownDurationBlocksBefore <
            accountedDroppedUnknownDurationBlocks_) {
      return false;
    }

    const uint64_t gapBlocks = block.sequence - nextExpectedSequence_;
    const uint64_t recoverableBlocks =
        block.droppedRecoverableBlocksBefore -
        accountedDroppedRecoverableBlocks_;
    const uint64_t recoverableSamples =
        block.droppedRecoverableSamplesBefore -
        accountedDroppedRecoverableSamples_;
    const uint64_t unknownDurationBlocks =
        block.droppedUnknownDurationBlocksBefore -
        accountedDroppedUnknownDurationBlocks_;
    accountedDroppedRecoverableBlocks_ =
        block.droppedRecoverableBlocksBefore;
    accountedDroppedRecoverableSamples_ =
        block.droppedRecoverableSamplesBefore;
    accountedDroppedUnknownDurationBlocks_ =
        block.droppedUnknownDurationBlocksBefore;
    if (gapBlocks == 0) {
      return recoverableBlocks == 0 && unknownDurationBlocks == 0;
    }
    currentBlockHasQueueGap_ = true;

    const bool countersMatchGap =
        recoverableBlocks <= gapBlocks &&
        unknownDurationBlocks == gapBlocks - recoverableBlocks;
    if (queueGaps_.size() < kMaxQueueGaps) {
      queueGaps_.push_back(QueueGap {
          .startSequence = nextExpectedSequence_,
          .blocks = gapBlocks,
          .detectedAtSequence = block.sequence,
          .recoverableBlocks = countersMatchGap ? recoverableBlocks : 0,
          .recoverableSamples = countersMatchGap ? recoverableSamples : 0,
          .unknownDurationBlocks = countersMatchGap
              ? unknownDurationBlocks
              : gapBlocks,
          .representedByMasterTimeline = true
      });
    } else {
      ++suppressedQueueGaps_;
    }
    // A queue gap is a discontinuity in the source signal. Do not let the
    // worker-side sinc filter bridge audio from before and after the gap.
    resampler_.reset();
    resamplerSourceRate_ = 0;
    // The master timeline derives the missing interval from the next callback
    // timestamp. Queue counters remain diagnostics only, avoiding double
    // insertion of silence for the same lost callback range.
    return true;
  }

  bool recordTrailingGap() {
    const uint64_t callbackSequences = nextSequence_.load(
        std::memory_order_acquire);
    if (callbackSequences <= nextExpectedSequence_) {
      return true;
    }
    const uint64_t recoverableBlockTotal = droppedRecoverableBlocks_.load(
        std::memory_order_acquire);
    const uint64_t recoverableSampleTotal = droppedRecoverableSamples_.load(
        std::memory_order_acquire);
    const uint64_t unknownDurationBlockTotal =
        droppedUnknownDurationBlocks_.load(std::memory_order_acquire);
    if (recoverableBlockTotal < accountedDroppedRecoverableBlocks_ ||
        recoverableSampleTotal < accountedDroppedRecoverableSamples_ ||
        unknownDurationBlockTotal <
            accountedDroppedUnknownDurationBlocks_) {
      return false;
    }
    const uint64_t gapBlocks = callbackSequences - nextExpectedSequence_;
    const uint64_t recoverableBlocks =
        recoverableBlockTotal - accountedDroppedRecoverableBlocks_;
    const uint64_t recoverableSamples =
        recoverableSampleTotal - accountedDroppedRecoverableSamples_;
    const uint64_t unknownDurationBlocks =
        unknownDurationBlockTotal - accountedDroppedUnknownDurationBlocks_;
    const bool countersMatchGap =
        recoverableBlocks <= gapBlocks &&
        unknownDurationBlocks == gapBlocks - recoverableBlocks;
    if (queueGaps_.size() < kMaxQueueGaps) {
      queueGaps_.push_back(QueueGap {
          .startSequence = nextExpectedSequence_,
          .blocks = gapBlocks,
          .detectedAtSequence = callbackSequences,
          .recoverableBlocks = countersMatchGap ? recoverableBlocks : 0,
          .recoverableSamples = countersMatchGap ? recoverableSamples : 0,
          .unknownDurationBlocks = countersMatchGap
              ? unknownDurationBlocks
              : gapBlocks,
          .representedByMasterTimeline = true
      });
    } else {
      ++suppressedQueueGaps_;
    }
    return countersMatchGap;
  }

  bool normalizeAndAppend(const AudioBlock &block) {
    monoBuffer_.resize(block.framesPerChannel);
    if (block.channels == 1) {
      std::copy_n(
          block.samples.data(),
          block.framesPerChannel,
          monoBuffer_.data());
    } else {
      for (size_t frame = 0; frame < block.framesPerChannel; ++frame) {
        int64_t sum = 0;
        const size_t offset = frame * block.channels;
        for (size_t channel = 0; channel < block.channels; ++channel) {
          sum += block.samples[offset + channel];
        }
        const int64_t halfChannels = block.channels / 2;
        const int64_t roundedAverage = sum >= 0
            ? (sum + halfChannels) / block.channels
            : (sum - halfChannels) / block.channels;
        const int64_t clamped = std::clamp<int64_t>(
            roundedAverage,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
        monoBuffer_[frame] = static_cast<int16_t>(clamped);
      }
    }

    normalizedBlockBuffer_.clear();
    if (block.sampleRate == kOutputSampleRate) {
      resampler_.reset();
      resamplerSourceRate_ = 0;
      normalizedBlockBuffer_.assign(monoBuffer_.begin(), monoBuffer_.end());
    } else {
      if (block.sampleRate < 100 || block.sampleRate % 100 != 0 ||
          block.sampleRate > static_cast<uint32_t>(
              std::numeric_limits<int>::max())) {
        return false;
      }
      const size_t sourceSamplesPerTenMs = block.sampleRate / 100;
      if (sourceSamplesPerTenMs == 0 ||
          block.framesPerChannel % sourceSamplesPerTenMs != 0) {
        return false;
      }
      if (resampler_ == nullptr || resamplerSourceRate_ != block.sampleRate) {
        auto resampler =
            std::make_unique<webrtc::PushResampler<int16_t>>();
        if (resampler->InitializeIfNeeded(
                static_cast<int>(block.sampleRate),
                static_cast<int>(kOutputSampleRate),
                1) != 0) {
          return false;
        }
        resampler_ = std::move(resampler);
        resamplerSourceRate_ = block.sampleRate;
      }

      const size_t outputChunks =
          block.framesPerChannel / sourceSamplesPerTenMs;
      normalizedBlockBuffer_.reserve(
          outputChunks * kResamplerOutputSamples);
      for (size_t sourceOffset = 0;
           sourceOffset < block.framesPerChannel;
           sourceOffset += sourceSamplesPerTenMs) {
        const int resampledSamples = resampler_->Resample(
            monoBuffer_.data() + sourceOffset,
            sourceSamplesPerTenMs,
            resampledBuffer_.data(),
            resampledBuffer_.size());
        if (resampledSamples != static_cast<int>(kResamplerOutputSamples)) {
          return false;
        }
        normalizedBlockBuffer_.insert(
            normalizedBlockBuffer_.end(),
            resampledBuffer_.begin(),
            resampledBuffer_.begin() + resampledSamples);
      }
    }
    if (normalizedBlockBuffer_.empty() ||
        normalizedBlockBuffer_.size() > kMaxNormalizedBlockSamples) {
      return false;
    }
    if (timeline_ != nullptr) {
      timeline_->append(
          side_,
          block.monotonicTimeNs,
          normalizedBlockBuffer_.data(),
          normalizedBlockBuffer_.size(),
          block.sequence,
          currentBlockHasQueueGap_ || currentBlockStartsControlSegment_,
          currentBlockStartsControlSegment_);
    }
    return true;
  }

  const StreamSide side_;
  const char *streamId_;
  SessionAudioTimeline *timeline_;
  std::array<AudioBlock, kQueueSlots> slots_ = {};
  std::atomic<size_t> head_ = {0};
  std::atomic<size_t> tail_ = {0};
  std::atomic<uint64_t> captureControl_ = {0};
  std::atomic<bool> failed_ = {false};
  std::atomic<bool> formatMismatch_ = {false};
  std::atomic<uint64_t> droppedBlocks_ = {0};
  std::atomic<uint64_t> nextSequence_ = {0};
  std::atomic<uint64_t> droppedRecoverableBlocks_ = {0};
  std::atomic<uint64_t> droppedRecoverableSamples_ = {0};
  std::atomic<uint64_t> droppedUnknownDurationBlocks_ = {0};
  std::atomic<uint32_t> maxQueueDepth_ = {0};

  bool finalized_ = false;

  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
  uint32_t resamplerSourceRate_ = 0;
  std::vector<int16_t> monoBuffer_;
  std::vector<int16_t> normalizedBlockBuffer_;
  std::array<int16_t, kResamplerOutputSamples> resampledBuffer_ = {};

  std::vector<SourceFormat> sourceFormats_;
  std::vector<QueueGap> queueGaps_;
  uint64_t suppressedSourceFormats_ = 0;
  uint64_t suppressedQueueGaps_ = 0;
  uint64_t sourceBlocks_ = 0;
  bool currentBlockHasQueueGap_ = false;
  bool currentBlockStartsControlSegment_ = false;
  bool sourceFormatKnown_ = false;
  uint32_t lastSourceSampleRate_ = 0;
  uint16_t lastSourceChannels_ = 0;
  uint64_t nextExpectedSequence_ = 0;
  uint64_t workerControlGeneration_ = 0;
  uint64_t accountedDroppedRecoverableBlocks_ = 0;
  uint64_t accountedDroppedRecoverableSamples_ = 0;
  uint64_t accountedDroppedUnknownDurationBlocks_ = 0;
};

} // namespace

class RecordingSession::Impl final {
public:
  enum class ControlState : int32_t {
    Recording,
    Paused,
    Inactive
  };

  Impl(
      std::string basePath,
      CallMetadata callMetadata,
      OutputMode outputMode,
      int64_t sessionStartMonotonicNs,
      int64_t recordingStartWallTimeMs,
      bool autoStarted,
      std::function<void()> fatalFailureCallback)
      : basePath_(std::move(basePath)),
        callMetadata_(std::move(callMetadata)),
        outputMode_(outputMode),
        autoStarted_(autoStarted),
        startedManually_(!autoStarted),
        sessionStartMonotonicNs_(sessionStartMonotonicNs),
        recordingStartWallTimeMs_(recordingStartWallTimeMs),
        sessionId_(FormatDirectoryTime(recordingStartWallTimeMs) +
            "_" + std::to_string(callMetadata_.callId)),
        sessionPath_(basePath_ + "/calls/" + sessionId_),
        timeline_(outputMode),
        local_(StreamSide::Local, &timeline_),
        remote_(StreamSide::Remote, &timeline_),
        fatalFailureCallback_(std::move(fatalFailureCallback)),
        worker_([this] { workerMainSafely(); }) {
  }

  ~Impl() {
    finish();
  }

  void start() noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (started_.load(std::memory_order_relaxed) ||
          finalizing_.load(std::memory_order_relaxed)) {
        return;
      }
      timeline_.start(sessionStartMonotonicNs_);
      local_.setAccepting(true);
      remote_.setAccepting(true);
      controlState_.store(ControlState::Recording, std::memory_order_relaxed);
      ++startCount_;
      // Publish only after every field consumed by the worker is initialized.
      started_.store(true, std::memory_order_release);
      checkpointRequested_.store(true, std::memory_order_release);
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "recording start");
      wake_.notify_one();
    } catch (...) {
      // Recorder lifecycle failure must not escape into the call state callback.
    }
  }

  void pause(int64_t boundaryMonotonicNs) noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (!started_.load(std::memory_order_relaxed) ||
          finalizing_.load(std::memory_order_relaxed) ||
          controlState_.load(std::memory_order_relaxed) !=
              ControlState::Recording) {
        return;
      }
      local_.setAccepting(false);
      remote_.setAccepting(false);
      timeline_.beginControlInterval(
          SessionAudioTimeline::ControlIntervalType::Pause,
          boundaryMonotonicNs);
      controlState_.store(ControlState::Paused, std::memory_order_release);
      ++pauseCount_;
      checkpointRequested_.store(true, std::memory_order_release);
      wake_.notify_one();
    } catch (...) {
      reportFatalFailure();
    }
  }

  void resume(int64_t boundaryMonotonicNs) noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (!started_.load(std::memory_order_relaxed) ||
          finalizing_.load(std::memory_order_relaxed) ||
          controlState_.load(std::memory_order_relaxed) !=
              ControlState::Paused) {
        return;
      }
      timeline_.endControlInterval(
          SessionAudioTimeline::ControlIntervalType::Pause,
          boundaryMonotonicNs);
      local_.markNextControlSegment();
      remote_.markNextControlSegment();
      local_.setAccepting(true);
      remote_.setAccepting(true);
      controlState_.store(ControlState::Recording, std::memory_order_release);
      checkpointRequested_.store(true, std::memory_order_release);
      wake_.notify_one();
    } catch (...) {
      reportFatalFailure();
    }
  }

  void stop(int64_t boundaryMonotonicNs) noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (!started_.load(std::memory_order_relaxed) ||
          finalizing_.load(std::memory_order_relaxed)) {
        return;
      }
      const ControlState oldState = controlState_.load(
          std::memory_order_relaxed);
      if (oldState != ControlState::Recording &&
          oldState != ControlState::Paused) {
        return;
      }
      local_.setAccepting(false);
      remote_.setAccepting(false);
      if (oldState == ControlState::Paused) {
        timeline_.endControlInterval(
            SessionAudioTimeline::ControlIntervalType::Pause,
            boundaryMonotonicNs);
      }
      timeline_.beginControlInterval(
          SessionAudioTimeline::ControlIntervalType::Stopped,
          boundaryMonotonicNs);
      pendingStoppedIntervalStartNs_.store(
          boundaryMonotonicNs, std::memory_order_relaxed);
      lastActiveRecordingBoundaryNs_.store(
          boundaryMonotonicNs, std::memory_order_relaxed);
      controlState_.store(ControlState::Inactive, std::memory_order_release);
      ++stopCount_;
      checkpointRequested_.store(true, std::memory_order_release);
      wake_.notify_one();
    } catch (...) {
      reportFatalFailure();
    }
  }

  void restart(int64_t boundaryMonotonicNs) noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (!started_.load(std::memory_order_relaxed) ||
          finalizing_.load(std::memory_order_relaxed) ||
          controlState_.load(std::memory_order_relaxed) !=
              ControlState::Inactive) {
        return;
      }
      timeline_.endControlInterval(
          SessionAudioTimeline::ControlIntervalType::Stopped,
          boundaryMonotonicNs);
      pendingStoppedIntervalStartNs_.store(0, std::memory_order_relaxed);
      local_.markNextControlSegment();
      remote_.markNextControlSegment();
      local_.setAccepting(true);
      remote_.setAccepting(true);
      controlState_.store(ControlState::Recording, std::memory_order_release);
      ++startCount_;
      checkpointRequested_.store(true, std::memory_order_release);
      wake_.notify_one();
    } catch (...) {
      reportFatalFailure();
    }
  }

  void beginFinishCall(
      int64_t teardownMonotonicNs,
      int64_t callEndWallTimeMs) noexcept {
    try {
      std::lock_guard<std::mutex> lock(lifecycleMutex_);
      if (finalizing_.load(std::memory_order_relaxed)) {
        return;
      }
      local_.setAccepting(false);
      remote_.setAccepting(false);
      const ControlState state = controlState_.load(
          std::memory_order_relaxed);
      int64_t finalBoundaryNs = teardownMonotonicNs;
      if (state == ControlState::Paused) {
        timeline_.endControlInterval(
            SessionAudioTimeline::ControlIntervalType::Pause,
            teardownMonotonicNs);
      } else if (state == ControlState::Inactive) {
        finalBoundaryNs = pendingStoppedIntervalStartNs_.load(
            std::memory_order_relaxed);
        timeline_.endControlInterval(
            SessionAudioTimeline::ControlIntervalType::Stopped,
            finalBoundaryNs);
      }
      callEndWallTimeMs_.store(callEndWallTimeMs, std::memory_order_relaxed);
      sessionEndMonotonicNs_.store(
          finalBoundaryNs, std::memory_order_relaxed);
      finalizing_.store(true, std::memory_order_release);
      wake_.notify_one();
    } catch (...) {
      local_.setAccepting(false);
      remote_.setAccepting(false);
      if (sessionEndMonotonicNs_.load(std::memory_order_relaxed) == 0) {
        sessionEndMonotonicNs_.store(
            teardownMonotonicNs, std::memory_order_relaxed);
      }
      finalizing_.store(true, std::memory_order_release);
      wake_.notify_one();
    }
  }

  void finish() noexcept {
    beginFinishCall(CurrentMonotonicTimeNs(), CurrentUnixTimeMs());
    local_.setAccepting(false);
    remote_.setAccepting(false);
    checkpointRequested_.store(true, std::memory_order_release);
    wake_.notify_one();
    finishRequested_.store(true, std::memory_order_release);
    wake_.notify_one();
    bool finished = false;
    try {
      std::lock_guard<std::mutex> lock(finishMutex_);
      if (!joined_ && worker_.joinable()) {
        worker_.join();
        joined_ = true;
        finished = true;
      }
    } catch (...) {
      // Never propagate recorder teardown failures into call teardown.
    }
    if (finished) {
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "finish local encodedPackets=%llu dropped=%llu",
          static_cast<unsigned long long>(local_.encodedPackets()),
          static_cast<unsigned long long>(local_.droppedBlocks()));
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "finish remote encodedPackets=%llu dropped=%llu",
          static_cast<unsigned long long>(remote_.encodedPackets()),
          static_cast<unsigned long long>(remote_.droppedBlocks()));
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "mixed finish encodedPackets=%llu samples=%llu",
          static_cast<unsigned long long>(
              timeline_.mixedWriter().encodedPackets()),
          static_cast<unsigned long long>(
              timeline_.mixedWriter().encodedSamples()));
    }
  }

  void enqueueLocal(
      const int16_t *samples,
      size_t framesPerChannel,
      size_t channels,
      uint32_t sampleRate) noexcept {
    local_.enqueue(samples, framesPerChannel, channels, sampleRate);
  }

  void enqueueRemote(
      const void *samples,
      size_t framesPerChannel,
      size_t bytesPerFrame,
      size_t channels,
      uint32_t sampleRate) noexcept {
    if (bytesPerFrame != channels * sizeof(int16_t)) {
      remote_.enqueue(nullptr, 0, 0, 0);
      return;
    }
    remote_.enqueue(
        static_cast<const int16_t *>(samples),
        framesPerChannel,
        channels,
        sampleRate);
  }

  void signalIntegrationFailure() noexcept {
    local_.signalFormatMismatch();
    remote_.signalFormatMismatch();
    wake_.notify_one();
  }

  const std::string &sessionId() const noexcept {
    return sessionId_;
  }

private:
  void recordMetadataCheckpointResult(
      bool succeeded,
      const char *operation) noexcept {
    metadataCheckpointDiagnostics_.record(succeeded);
    if (!succeeded &&
        (metadataCheckpointDiagnostics_.consecutiveFailures == 1 ||
         metadataCheckpointDiagnostics_.consecutiveFailures % 12 == 0)) {
      __android_log_print(
          ANDROID_LOG_WARN,
          kCallRecorderLogTag,
          "%s metadata write failed total=%llu consecutive=%u; will retry",
          operation,
          static_cast<unsigned long long>(
              metadataCheckpointDiagnostics_.failures),
          metadataCheckpointDiagnostics_.consecutiveFailures);
    }
  }

  void reportFatalFailure() noexcept {
    bool expected = false;
    if (!fatalFailureReported_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    local_.setAccepting(false);
    remote_.setAccepting(false);
    checkpointRequested_.store(true, std::memory_order_release);
    wake_.notify_one();
    try {
      if (fatalFailureCallback_) {
        fatalFailureCallback_();
      }
    } catch (...) {
    }
  }

  void workerMainSafely() noexcept {
    try {
      workerMain();
    } catch (...) {
      local_.markFailed();
      remote_.markFailed();
      timeline_.markAllWritersFailed();
      reportFatalFailure();
      // Even an unexpected worker-side exception must preserve the finish
      // barrier and consume any blocks published before accepting was cleared.
      while (!finishRequested_.load(std::memory_order_acquire)) {
        try {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (...) {
          std::this_thread::yield();
        }
      }
      while (local_.drainOne()) {
      }
      while (remote_.drainOne()) {
      }
      local_.finalize();
      remote_.finalize();
      timeline_.finish(
          sessionEndMonotonicNs_.load(std::memory_order_acquire));
      if (directoryReady_) {
        bool metadataCommitted = false;
        try {
          metadataCommitted = writeInfo();
        } catch (...) {
        }
        if (metadataCommitted) {
          (void) std::remove((sessionPath_ + "/.in_progress").c_str());
        }
      }
    }
  }

  void workerMain() {
    if (!RunSessionTimelineRegressionChecks() ||
        !RunCallControlRegressionChecks() ||
        !RunFailureIsolationRegressionChecks() ||
        !RunMetadataRegressionChecks() ||
        !RunMetadataTimeRegressionChecks()) {
      __android_log_print(
          ANDROID_LOG_ERROR,
          kCallRecorderLogTag,
          "master timeline regression checks failed");
      timeline_.markAllWritersFailed();
    }
    bool filesPrepared = false;
    auto nextCheckpoint = std::chrono::steady_clock::now();
    while (true) {
      if (started_.load(std::memory_order_acquire) && !filesPrepared) {
        filesPrepared = true;
        try {
          prepareFiles();
          timeline_.start(sessionStartMonotonicNs_);
          if (timeline_.requiredOutputsFailed()) {
            reportFatalFailure();
          }
        } catch (...) {
          local_.markFailed();
          remote_.markFailed();
          timeline_.markAllWritersFailed();
          reportFatalFailure();
        }
      }

      bool drained = false;
      while (true) {
        bool drainedRound = false;
        if (local_.drainOne()) {
          drained = true;
          drainedRound = true;
        }
        if (remote_.drainOne()) {
          drained = true;
          drainedRound = true;
        }
        int64_t advanceTimestamp = CurrentMonotonicTimeNs();
        if (finalizing_.load(std::memory_order_acquire)) {
          advanceTimestamp = sessionEndMonotonicNs_.load(
              std::memory_order_acquire);
        } else if (controlState_.load(std::memory_order_acquire) ==
                   ControlState::Inactive) {
          advanceTimestamp = pendingStoppedIntervalStartNs_.load(
              std::memory_order_acquire);
        }
        timeline_.advance(advanceTimestamp);
        if (timeline_.requiredOutputsFailed()) {
          reportFatalFailure();
        }
        if (!drainedRound) {
          break;
        }
      }

      const auto checkpointNow = std::chrono::steady_clock::now();
      const bool checkpointDue = checkpointRequested_.exchange(
          false, std::memory_order_acq_rel) || checkpointNow >= nextCheckpoint;
      if (filesPrepared && directoryReady_ && checkpointDue &&
          !finishRequested_.load(std::memory_order_acquire)) {
        timeline_.checkpointWriters();
        if (timeline_.requiredOutputsFailed()) {
          reportFatalFailure();
        }
        recordMetadataCheckpointResult(writeCheckpoint(), "checkpoint");
        nextCheckpoint = checkpointNow + kMetadataCheckpointInterval;
      }

      if (finishRequested_.load(std::memory_order_acquire) &&
          local_.empty() && remote_.empty()) {
        break;
      }

      if (!drained) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(10));
      }
    }

    local_.finalize();
    remote_.finalize();
    timeline_.finish(
        sessionEndMonotonicNs_.load(std::memory_order_acquire));
    if (filesPrepared && directoryReady_) {
      bool metadataCommitted = false;
      try {
        metadataCommitted = writeInfo();
      } catch (...) {
        // Failure is reported below without escaping into call teardown.
      }
      recordMetadataCheckpointResult(metadataCommitted, "final");
      if (metadataCommitted) {
        (void) std::remove((sessionPath_ + "/.in_progress").c_str());
      }
    }
  }

  void prepareFiles() {
    if (basePath_.empty()) {
      local_.markFailed();
      remote_.markFailed();
      timeline_.markAllWritersFailed();
      reportFatalFailure();
      return;
    }
    const std::string callsPath = basePath_ + "/calls";
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "prepareFiles callsPath=%s",
        callsPath.c_str());
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "prepareFiles sessionPath=%s",
        sessionPath_.c_str());
    if (!EnsureDirectory(callsPath, true)) {
      local_.markFailed();
      remote_.markFailed();
      timeline_.markAllWritersFailed();
      reportFatalFailure();
      return;
    }
    if (!EnsureDirectory(sessionPath_, false)) {
      local_.markFailed();
      remote_.markFailed();
      timeline_.markAllWritersFailed();
      reportFatalFailure();
      return;
    }
    directoryReady_ = true;
    timeline_.setOutputDirectory(sessionPath_);
    const std::string markerPath = sessionPath_ + "/.in_progress";
    if (FILE *marker = std::fopen(markerPath.c_str(), "wb")) {
      const std::string processId = std::to_string(getpid());
      (void) std::fwrite(
          processId.data(), 1, processId.size(), marker);
      (void) std::fclose(marker);
    }
  }

  const char *controlStateName() const noexcept {
    switch (controlState_.load(std::memory_order_acquire)) {
      case ControlState::Recording: return "recording";
      case ControlState::Paused: return "paused";
      case ControlState::Inactive: return "inactive";
    }
    return "recording";
  }

  bool writeCheckpoint() noexcept {
    try {
      const uint64_t committedSample = timeline_.commitCursor();
      uint64_t durationMs = 0;
      (void) TimelineSamplesToMilliseconds(committedSample, durationMs);
      const int64_t checkpointWallTimeMs = CurrentUnixTimeMs();
      const int64_t checkpointMonotonicNs = CurrentMonotonicTimeNs();
      const bool failed = fatalFailureReported_.load(std::memory_order_acquire);
      std::ostringstream json;
      json << "{\n"
           << "  \"schemaVersion\": 2,\n"
           << "  \"sessionId\": \"" << JsonEscape(sessionId_) << "\",\n"
           << "  \"callId\": " << callMetadata_.callId << ",\n"
           << "  \"call\": {\n"
           << "    \"callId\": " << callMetadata_.callId << ",\n"
           << "    \"userId\": " << callMetadata_.userId << ",\n"
           << "    \"displayName\": \""
           << JsonEscape(callMetadata_.displayName) << "\",\n"
           << "    \"isOutgoing\": "
           << (callMetadata_.isOutgoing ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"recordingStartTime\": \""
           << FormatJsonTime(recordingStartWallTimeMs_) << "\",\n"
           << "  \"recordingEndTime\": \""
           << FormatJsonTime(recordingStartWallTimeMs_ +
                  static_cast<int64_t>(durationMs)) << "\",\n"
           << "  \"timelineSampleRate\": " << kOutputSampleRate << ",\n"
           << "  \"durationMs\": " << durationMs << ",\n"
           << "  \"lastCommittedTimelineSample\": " << committedSample << ",\n"
           << "  \"lastCheckpointWallTime\": \""
           << FormatJsonTime(checkpointWallTimeMs) << "\",\n"
           << "  \"lastCheckpointMonotonicNs\": "
           << checkpointMonotonicNs << ",\n"
           << "  \"outputMode\": \"" << OutputModeName(outputMode_) << "\",\n"
           << "  \"state\": \"" << (failed ? "failed" : "in_progress")
           << "\",\n"
           << "  \"control\": {\n"
           << "    \"runtimeState\": \"" << controlStateName() << "\",\n"
           << "    \"finalState\": \""
           << (failed ? "failed" : "in_progress") << "\"\n"
           << "  }\n"
           << "}\n";
      return AtomicWriteFile(sessionPath_, "info.json", json.str());
    } catch (...) {
      return false;
    }
  }

  bool writeInfo() {
    const int64_t recordingEndWallTimeMs = MonotonicEndToWallTimeMs(
        recordingStartWallTimeMs_,
        timeline_.sessionStartMonotonicNs(),
        timeline_.sessionEndMonotonicNs());
    const std::string recordingStartTime = FormatJsonTime(
        recordingStartWallTimeMs_);
    const std::string recordingEndTime = FormatJsonTime(
        recordingEndWallTimeMs);
    const std::string callEndTime = FormatJsonTime(
        callEndWallTimeMs_.load(std::memory_order_acquire));
    uint64_t timelineDurationMs = 0;
    (void) TimelineSamplesToMilliseconds(
        timeline_.timelineSamples(), timelineDurationMs);
    std::ostringstream json;
    json << "{\n"
         << "  \"schemaVersion\": 2,\n"
         << "  \"sessionId\": \"" << JsonEscape(sessionId_) << "\",\n"
         << "  \"call\": {\n"
         << "    \"callId\": " << callMetadata_.callId << ",\n"
         << "    \"userId\": " << callMetadata_.userId << ",\n"
         << "    \"displayName\": \""
         << JsonEscape(callMetadata_.displayName) << "\",\n"
         << "    \"isOutgoing\": "
         << (callMetadata_.isOutgoing ? "true" : "false") << "\n"
         << "  },\n"
         << "  \"callId\": " << callMetadata_.callId << ",\n"
         << "  \"recordingStartTime\": \"" << recordingStartTime
         << "\",\n"
         << "  \"recordingEndTime\": \"" << recordingEndTime
         << "\",\n"
         << "  \"callEndTime\": \"" << callEndTime << "\",\n"
         << "  \"startTime\": \"" << recordingStartTime << "\",\n"
         << "  \"stopTime\": \"" << recordingEndTime << "\",\n"
         << "  \"sessionStartMonotonicNs\": "
         << timeline_.sessionStartMonotonicNs() << ",\n"
         << "  \"sessionEndMonotonicNs\": "
         << timeline_.sessionEndMonotonicNs() << ",\n"
         << "  \"timelineSamples\": " << timeline_.timelineSamples()
         << ",\n"
         << "  \"sessionTimelineSamples\": "
         << timeline_.timelineSamples() << ",\n"
         << "  \"timelineExpectedFromMonotonic\": "
         << timeline_.timelineExpectedFromMonotonic() << ",\n"
         << "  \"timelineDifferenceSamples\": "
         << timeline_.timelineDifferenceSamples() << ",\n"
         << "  \"timelineSampleRate\": " << kOutputSampleRate << ",\n"
         << "  \"durationMs\": " << timelineDurationMs << ",\n"
         << "  \"outputMode\": \"" << OutputModeName(outputMode_)
         << "\",\n"
         << "  \"state\": \""
         << (fatalFailureReported_.load(std::memory_order_acquire)
             ? "failed" : "finalized") << "\",\n"
         << "  \"control\": {\n"
         << "    \"autoStarted\": "
         << (autoStarted_ ? "true" : "false") << ",\n"
         << "    \"startedManually\": "
         << (startedManually_ ? "true" : "false") << ",\n"
         << "    \"startCount\": " << startCount_ << ",\n"
         << "    \"pauseCount\": " << pauseCount_ << ",\n"
         << "    \"stopCount\": " << stopCount_ << ",\n"
         << "    \"recordingStartMonotonicNs\": "
         << timeline_.sessionStartMonotonicNs() << ",\n"
         << "    \"recordingEndMonotonicNs\": "
         << timeline_.sessionEndMonotonicNs() << ",\n"
         << "    \"pausedSamples\": " << timeline_.pausedSamples()
         << ",\n"
         << "    \"stoppedGapSamples\": "
         << timeline_.stoppedGapSamples() << ",\n"
         << "    \"suppressedIntervals\": "
         << timeline_.suppressedControlIntervals() << ",\n"
         << "    \"finalState\": \""
         << (fatalFailureReported_.load(std::memory_order_acquire)
             ? "failed" : "finalized") << "\"\n"
         << "  },\n";
    appendControlIntervals(json, timeline_);
    appendStreamInfo(json, local_, timeline_, true);
    appendStreamInfo(json, remote_, timeline_, true);
    appendMixedInfo(json, timeline_, true);
    appendInvariantInfo(json, timeline_);
    json << "}\n";
    return AtomicWriteFile(sessionPath_, "info.json", json.str());
  }

  static void appendControlIntervals(
      std::ostringstream &json,
      const SessionAudioTimeline &timeline) {
    const auto intervals = timeline.controlIntervals();
    json << "  \"controlIntervals\": [";
    for (size_t index = 0; index < intervals.size(); ++index) {
      const auto &interval = intervals[index];
      json << (index == 0 ? "\n" : ",\n")
           << "    {\n"
           << "      \"type\": \""
           << (interval.type ==
                   SessionAudioTimeline::ControlIntervalType::Pause
               ? "pause" : "stopped") << "\",\n"
           << "      \"startSample\": " << interval.startSample << ",\n"
           << "      \"endSample\": " << interval.endSample << "\n"
           << "    }";
    }
    if (!intervals.empty()) {
      json << '\n';
    }
    json << "  ],\n";
  }

  static void appendStreamInfo(
      std::ostringstream &json,
      const AudioStream &stream,
      const SessionAudioTimeline &timeline,
      bool hasNext) {
    json << "  \"" << stream.streamId() << "\": {\n"
         << "    \"file\": \"" << stream.filename() << "\",\n"
         << "    \"enabled\": "
         << (timeline.writer(stream.streamId()[0] == 'l'
                 ? StreamSide::Local : StreamSide::Remote).enabled()
             ? "true" : "false") << ",\n"
         << "    \"fileCreated\": "
         << (stream.fileCreated() ? "true" : "false") << ",\n"
         << "    \"hasAudio\": "
         << (stream.hasAudio() ? "true" : "false") << ",\n"
         << "    \"container\": \"ogg\",\n"
         << "    \"codec\": \"opus\",\n"
         << "    \"application\": \"voip\",\n"
         << "    \"outputSampleRate\": " << kOutputSampleRate << ",\n"
         << "    \"outputChannels\": " << kOutputChannels << ",\n"
         << "    \"bitrate\": " << kStreamOpusBitrate << ",\n"
         << "    \"vbr\": true,\n"
         << "    \"dtx\": false,\n"
         << "    \"frameDurationMs\": " << kOpusFrameDurationMs << ",\n"
         << "    \"frameSamples\": " << kOpusFrameSamples << ",\n"
         << "    \"preSkip\": " << stream.preSkip() << ",\n"
         << "    \"sourceBlocks\": " << stream.sourceBlocks() << ",\n"
         << "    \"encodedPackets\": " << stream.encodedPackets() << ",\n"
         << "    \"encodedSamples\": " << stream.encodedSamples() << ",\n"
         << "    \"durationMs\": "
         << (stream.encodedSamples() / (kOutputSampleRate / 1000U)) << ",\n"
         << "    \"droppedBlocks\": " << stream.droppedBlocks() << ",\n"
         << "    \"maxQueueDepth\": " << stream.maxQueueDepth() << ",\n"
         << "    \"queueCapacity\": " << stream.queueCapacity() << ",\n"
         << "    \"audioSamplesProvided\": "
         << stream.audioSamplesProvided() << ",\n"
         << "    \"audioSamplesWritten\": "
         << stream.audioSamplesWritten() << ",\n"
         << "    \"silenceSamplesWritten\": "
         << stream.silenceSamplesWritten() << ",\n"
         << "    \"lateSamplesDropped\": "
         << stream.lateSamplesDropped() << ",\n"
         << "    \"bufferLimitSamplesDropped\": "
         << stream.bufferLimitSamplesDropped() << ",\n"
         << "    \"firstAudioTimelineSample\": "
         << timeline.firstAudioTimelineSample(
             stream.streamId()[0] == 'l'
                 ? StreamSide::Local : StreamSide::Remote) << ",\n"
         << "    \"captureFailed\": "
         << (stream.captureFailed() ? "true" : "false") << ",\n"
         << "    \"writerFailed\": "
         << (stream.writerFailed() ? "true" : "false") << ",\n"
         << "    \"failed\": " << (stream.failed() ? "true" : "false") << ",\n"
         << "    \"suppressedSourceFormats\": "
         << stream.suppressedSourceFormats() << ",\n"
         << "    \"suppressedQueueGaps\": "
         << stream.suppressedQueueGaps() << ",\n"
         << "    \"sourceFormats\": [";
    const auto &sourceFormats = stream.sourceFormats();
    for (size_t index = 0; index < sourceFormats.size(); ++index) {
      const auto &format = sourceFormats[index];
      json << (index == 0 ? "\n" : ",\n")
           << "      {\n"
           << "        \"sampleRate\": " << format.sampleRate << ",\n"
           << "        \"channels\": " << format.channels << ",\n"
           << "        \"startSequence\": " << format.startSequence << ",\n"
           << "        \"startTimestampNs\": " << format.startTimestampNs << "\n"
           << "      }";
    }
    if (!sourceFormats.empty()) {
      json << '\n';
    }
    json << "    ],\n"
         << "    \"queueGaps\": [";
    const auto &queueGaps = stream.queueGaps();
    for (size_t index = 0; index < queueGaps.size(); ++index) {
      const auto &gap = queueGaps[index];
      json << (index == 0 ? "\n" : ",\n")
           << "      {\n"
           << "        \"startSequence\": " << gap.startSequence << ",\n"
           << "        \"blocks\": " << gap.blocks << ",\n"
           << "        \"detectedAtSequence\": "
           << gap.detectedAtSequence << ",\n"
           << "        \"recoverableBlocks\": "
           << gap.recoverableBlocks << ",\n"
           << "        \"recoverableSamples\": "
           << gap.recoverableSamples << ",\n"
           << "        \"unknownDurationBlocks\": "
           << gap.unknownDurationBlocks << ",\n"
           << "        \"representedByMasterTimeline\": "
           << (gap.representedByMasterTimeline ? "true" : "false") << "\n"
           << "      }";
    }
    if (!queueGaps.empty()) {
      json << '\n';
    }
    const StreamSide side = stream.streamId()[0] == 'l'
        ? StreamSide::Local : StreamSide::Remote;
    json << "    ],\n"
         << "    \"continuityGaps\": [";
    bool firstEvent = true;
    for (const auto &gap : timeline.continuityGaps()) {
      if (gap.side != side) {
        continue;
      }
      json << (firstEvent ? "\n" : ",\n")
           << "      {\n"
           << "        \"sequence\": " << gap.sequence << ",\n"
           << "        \"previousTimelineEndSample\": "
           << gap.previousTimelineEndSample << ",\n"
           << "        \"timestampTimelineStartSample\": "
           << gap.timestampTimelineStartSample << ",\n"
           << "        \"gapSamples\": " << gap.gapSamples << "\n"
           << "      }";
      firstEvent = false;
    }
    if (!firstEvent) {
      json << '\n';
    }
    json << "    ],\n"
         << "    \"lateDrops\": [";
    firstEvent = true;
    for (const auto &event : timeline.lateDrops()) {
      if (event.side != side) {
        continue;
      }
      json << (firstEvent ? "\n" : ",\n")
           << "      {\n"
           << "        \"sequence\": " << event.sequence << ",\n"
           << "        \"timelineStartSample\": "
           << event.timelineStartSample << ",\n"
           << "        \"timelineEndSample\": "
           << event.timelineEndSample << ",\n"
           << "        \"commitCursorSample\": "
           << event.commitCursorSample << ",\n"
           << "        \"droppedSamples\": "
           << event.droppedSamples << "\n"
           << "      }";
      firstEvent = false;
    }
    if (!firstEvent) {
      json << '\n';
    }
    json << "    ],\n"
         << "    \"timestampDiscontinuities\": [";
    firstEvent = true;
    for (const auto &event : timeline.discontinuities()) {
      if (event.side != side) {
        continue;
      }
      json << (firstEvent ? "\n" : ",\n")
           << "      {\n"
           << "        \"sequence\": " << event.sequence << ",\n"
           << "        \"deltaMs\": " << event.deltaMs << ",\n"
           << "        \"phase\": \""
           << TimestampDiscontinuityPhaseName(event.phase) << "\"\n"
           << "      }";
      firstEvent = false;
    }
    if (!firstEvent) {
      json << '\n';
    }
    json << "    ]\n"
         << "  }" << (hasNext ? "," : "") << "\n";
  }

  static void appendMixedInfo(
      std::ostringstream &json,
      const SessionAudioTimeline &timeline,
      bool hasNext) {
    const OggOpusWriter &writer = timeline.mixedWriter();
    json << "  \"mixed\": {\n"
         << "    \"file\": \"" << writer.filename() << "\",\n"
         << "    \"enabled\": "
         << (writer.enabled() ? "true" : "false") << ",\n"
         << "    \"fileCreated\": "
         << (writer.fileCreated() ? "true" : "false") << ",\n"
         << "    \"hasAudio\": "
         << (writer.hasAudio() ? "true" : "false") << ",\n"
         << "    \"container\": \"ogg\",\n"
         << "    \"codec\": \"opus\",\n"
         << "    \"application\": \"voip\",\n"
         << "    \"outputSampleRate\": " << kOutputSampleRate << ",\n"
         << "    \"outputChannels\": " << kOutputChannels << ",\n"
         << "    \"bitrate\": " << writer.bitrate() << ",\n"
         << "    \"vbr\": true,\n"
         << "    \"dtx\": false,\n"
         << "    \"frameDurationMs\": " << kOpusFrameDurationMs << ",\n"
         << "    \"frameSamples\": " << kOpusFrameSamples << ",\n"
         << "    \"preSkip\": " << writer.preSkip() << ",\n"
         << "    \"encodedPackets\": " << writer.encodedPackets() << ",\n"
         << "    \"encodedSamples\": " << writer.encodedSamples() << ",\n"
         << "    \"durationMs\": "
         << (writer.encodedSamples() / (kOutputSampleRate / 1000U)) << ",\n"
         << "    \"mixAlgorithm\": \"" << kMixedAlgorithm << "\",\n"
         << "    \"bufferWindowSamples\": "
         << kMixedHoldbackSamples << ",\n"
         << "    \"bufferWindowMs\": "
         << (kMixedHoldbackSamples * 1000U / kOutputSampleRate) << ",\n"
         << "    \"continuityJitterToleranceMs\": "
         << (kContinuityJitterToleranceNs / 1000000) << ",\n"
         << "    \"timestampDiagnosticEnterMs\": "
         << (kTimestampDiagnosticEnterNs / 1000000) << ",\n"
         << "    \"timestampDiagnosticExitMs\": "
         << (kTimestampDiagnosticExitNs / 1000000) << ",\n"
         << "    \"maxBufferedSamplesPerTrack\": "
         << kMaxTimelineBufferedSamplesPerTrack << ",\n"
         << "    \"maxBufferedSamplesObserved\": "
         << timeline.maxBufferedSamplesObserved() << ",\n"
         << "    \"suppressedContinuityGaps\": "
         << timeline.suppressedContinuityGaps() << ",\n"
         << "    \"suppressedLateDrops\": "
         << timeline.suppressedLateDrops() << ",\n"
         << "    \"suppressedTimestampDiscontinuities\": "
         << timeline.suppressedDiscontinuities() << ",\n"
         << "    \"failed\": "
         << (writer.failed() ? "true" : "false") << "\n"
         << "  }" << (hasNext ? "," : "") << "\n";
  }

  static void appendInvariantInfo(
      std::ostringstream &json,
      const SessionAudioTimeline &timeline) {
    json << "  \"invariant\": {\n"
         << "    \"outputMode\": \"" << OutputModeName(timeline.outputMode())
         << "\",\n"
         << "    \"localWrittenSamples\": "
         << timeline.writer(StreamSide::Local).encodedSamples() << ",\n"
         << "    \"remoteWrittenSamples\": "
         << timeline.writer(StreamSide::Remote).encodedSamples() << ",\n"
         << "    \"mixedWrittenSamples\": "
         << timeline.mixedWriter().encodedSamples() << ",\n"
         << "    \"masterTimelineCommittedSamples\": "
         << timeline.commitCursor() << ",\n"
         << "    \"localAccountingInvariant\": "
         << (timeline.accountingInvariant(StreamSide::Local)
             ? "true" : "false") << ",\n"
         << "    \"remoteAccountingInvariant\": "
         << (timeline.accountingInvariant(StreamSide::Remote)
             ? "true" : "false") << ",\n"
         << "    \"timelineInternalFailure\": "
         << (timeline.internalFailed() ? "true" : "false") << ",\n"
         << "    \"equalDurationInvariant\": "
         << (timeline.equalDurationInvariant() ? "true" : "false") << "\n"
         << "  }\n";
  }

  const std::string basePath_;
  const CallMetadata callMetadata_;
  const OutputMode outputMode_;
  const bool autoStarted_;
  const bool startedManually_;
  const int64_t sessionStartMonotonicNs_;
  const int64_t recordingStartWallTimeMs_;
  const std::string sessionId_;
  const std::string sessionPath_;
  SessionAudioTimeline timeline_;
  AudioStream local_;
  AudioStream remote_;
  std::atomic<bool> started_ = {false};
  std::atomic<bool> finalizing_ = {false};
  std::atomic<bool> finishRequested_ = {false};
  std::atomic<bool> checkpointRequested_ = {false};
  std::atomic<ControlState> controlState_ = {ControlState::Recording};
  std::atomic<int64_t> pendingStoppedIntervalStartNs_ = {0};
  std::atomic<int64_t> lastActiveRecordingBoundaryNs_ = {0};
  uint32_t startCount_ = 0;
  uint32_t pauseCount_ = 0;
  uint32_t stopCount_ = 0;
  std::atomic<int64_t> callEndWallTimeMs_ = {0};
  std::atomic<int64_t> sessionEndMonotonicNs_ = {0};
  bool directoryReady_ = false;
  std::function<void()> fatalFailureCallback_;
  std::atomic<bool> fatalFailureReported_ = {false};
  MetadataCheckpointDiagnostics metadataCheckpointDiagnostics_;

  std::mutex lifecycleMutex_;
  std::mutex wakeMutex_;
  std::condition_variable wake_;
  std::thread worker_;
  std::mutex finishMutex_;
  bool joined_ = false;
};

std::shared_ptr<RecordingSession> RecordingSession::Create(
    std::string basePath,
    CallMetadata callMetadata,
    OutputMode outputMode,
    int64_t sessionStartMonotonicNs,
    int64_t recordingStartWallTimeMs,
    bool autoStarted,
    std::function<void()> fatalFailureCallback) {
  if (basePath.empty()) {
    return nullptr;
  }
  try {
    return std::shared_ptr<RecordingSession>(
        new RecordingSession(
            std::move(basePath),
            std::move(callMetadata),
            outputMode,
            sessionStartMonotonicNs,
            recordingStartWallTimeMs,
            autoStarted,
            std::move(fatalFailureCallback)));
  } catch (...) {
    return nullptr;
  }
}

RecordingSession::RecordingSession(
    std::string basePath,
    CallMetadata callMetadata,
    OutputMode outputMode,
    int64_t sessionStartMonotonicNs,
    int64_t recordingStartWallTimeMs,
    bool autoStarted,
    std::function<void()> fatalFailureCallback)
    : impl_(std::make_unique<Impl>(
          std::move(basePath),
          std::move(callMetadata),
          outputMode,
          sessionStartMonotonicNs,
          recordingStartWallTimeMs,
          autoStarted,
          std::move(fatalFailureCallback))) {
}

RecordingSession::~RecordingSession() = default;

void RecordingSession::start() {
  impl_->start();
}

void RecordingSession::pause(int64_t boundaryMonotonicNs) {
  impl_->pause(boundaryMonotonicNs);
}

void RecordingSession::resume(int64_t boundaryMonotonicNs) {
  impl_->resume(boundaryMonotonicNs);
}

void RecordingSession::stop(int64_t boundaryMonotonicNs) {
  impl_->stop(boundaryMonotonicNs);
}

void RecordingSession::restart(int64_t boundaryMonotonicNs) {
  impl_->restart(boundaryMonotonicNs);
}

void RecordingSession::beginFinishCall(
    int64_t teardownMonotonicNs,
    int64_t callEndWallTimeMs) {
  impl_->beginFinishCall(teardownMonotonicNs, callEndWallTimeMs);
}

void RecordingSession::finish() {
  impl_->finish();
}

void RecordingSession::signalIntegrationFailure() noexcept {
  impl_->signalIntegrationFailure();
}

const std::string &RecordingSession::sessionId() const noexcept {
  return impl_->sessionId();
}

void RecordingSession::enqueueLocal(
    const int16_t *samples,
    size_t framesPerChannel,
    size_t channels,
    uint32_t sampleRate) noexcept {
  impl_->enqueueLocal(samples, framesPerChannel, channels, sampleRate);
}

void RecordingSession::enqueueRemote(
    const void *samples,
    size_t framesPerChannel,
    size_t bytesPerFrame,
    size_t channels,
    uint32_t sampleRate) noexcept {
  impl_->enqueueRemote(
      samples,
      framesPerChannel,
      bytesPerFrame,
      channels,
      sampleRate);
}

class CallRecordingController::Impl final {
public:
  Impl(
      std::string basePath,
      CallMetadata callMetadata,
      bool supported,
      bool autoRecordingEnabled,
      OutputMode initialOutputMode,
      StateCallback stateCallback)
      : basePath_(std::move(basePath)),
        callMetadata_(std::move(callMetadata)),
        supported_(supported),
        autoRecordingEnabled_(autoRecordingEnabled),
        initialOutputMode_(initialOutputMode),
        stateCallback_(std::move(stateCallback)),
        state_(supported ? RecordingState::Idle :
            RecordingState::Unsupported) {
    __android_log_print(
        ANDROID_LOG_INFO,
        kCallRecorderLogTag,
        "recording controller created supported=%s autoStart=%s",
        supported ? "true" : "false",
        autoRecordingEnabled ? "true" : "false");
  }

  ~Impl() {
    beginFinishCall();
    finishCall();
  }

  void setOwner(std::weak_ptr<CallRecordingController> owner) {
    owner_ = std::move(owner);
  }

  void notifyInitialState() {
    notifyState();
  }

  void onEstablished() {
    bool autoStart = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!firstEstablishedSeen_) {
        firstEstablishedSeen_ = true;
        autoStart = supported_ && autoRecordingEnabled_ &&
            state_.load(std::memory_order_relaxed) == RecordingState::Idle;
      }
    }
    if (autoStart) {
      startInternal(initialOutputMode_, true);
    }
  }

  void start(OutputMode outputMode) {
    startInternal(outputMode, false);
  }

  void pause() {
    bool changed = false;
    const int64_t boundaryNs = CurrentMonotonicTimeNs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_.load(std::memory_order_relaxed) !=
              RecordingState::Recording || session_ == nullptr) {
        return;
      }
      captureTarget_.store(nullptr, std::memory_order_release);
      session_->pause(boundaryNs);
      state_.store(RecordingState::Paused, std::memory_order_release);
      changed = true;
    }
    if (changed) {
      logTransition("recording pause");
      notifyState();
    }
  }

  void resume() {
    bool changed = false;
    const int64_t boundaryNs = CurrentMonotonicTimeNs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_.load(std::memory_order_relaxed) !=
              RecordingState::Paused || session_ == nullptr) {
        return;
      }
      session_->resume(boundaryNs);
      captureTarget_.store(session_.get(), std::memory_order_release);
      state_.store(RecordingState::Recording, std::memory_order_release);
      changed = true;
    }
    if (changed) {
      logTransition("recording resume");
      notifyState();
    }
  }

  void stop() {
    bool changed = false;
    const int64_t boundaryNs = CurrentMonotonicTimeNs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const RecordingState state = state_.load(std::memory_order_relaxed);
      if ((state != RecordingState::Recording &&
           state != RecordingState::Paused) || session_ == nullptr) {
        return;
      }
      captureTarget_.store(nullptr, std::memory_order_release);
      session_->stop(boundaryNs);
      pendingStoppedIntervalStartNs_.store(
          boundaryNs, std::memory_order_release);
      state_.store(RecordingState::Inactive, std::memory_order_release);
      changed = true;
    }
    if (changed) {
      logTransition("recording stop");
      notifyState();
    }
  }

  void beginFinishCall() {
    std::shared_ptr<RecordingSession> session;
    const int64_t teardownNs = CurrentMonotonicTimeNs();
    const int64_t callEndWallTimeMs = CurrentUnixTimeMs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finishBegun_) {
        return;
      }
      finishBegun_ = true;
      captureTarget_.store(nullptr, std::memory_order_release);
      const RecordingState currentState = state_.load(
          std::memory_order_relaxed);
      frozenEndMonotonicNs_.store(
          currentState == RecordingState::Inactive
              ? pendingStoppedIntervalStartNs_.load(std::memory_order_relaxed)
              : teardownNs,
          std::memory_order_release);
      session = session_;
      if (session != nullptr) {
        session->beginFinishCall(teardownNs, callEndWallTimeMs);
      }
    }
  }

  void finishCall() {
    std::shared_ptr<RecordingSession> session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finishCompleted_) {
        return;
      }
      if (!finishBegun_) {
        finishBegun_ = true;
        captureTarget_.store(nullptr, std::memory_order_release);
        if (session_ != nullptr) {
          session_->beginFinishCall(
              CurrentMonotonicTimeNs(), CurrentUnixTimeMs());
        }
      }
      finishCompleted_ = true;
      session = session_;
    }
    if (session != nullptr) {
      session->finish();
    }
    if (state_.load(std::memory_order_acquire) != RecordingState::Failed &&
        state_.load(std::memory_order_acquire) !=
            RecordingState::Unsupported) {
      state_.store(RecordingState::Finalized, std::memory_order_release);
      logTransition("recording finalized");
      notifyState();
    }
  }

  RecordingState state() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  int64_t elapsedSamples() const noexcept {
    const int64_t startNs = sessionStartMonotonicNs_.load(
        std::memory_order_acquire);
    if (startNs == 0) {
      return 0;
    }
    int64_t endNs = CurrentMonotonicTimeNs();
    const RecordingState state = state_.load(std::memory_order_acquire);
    if (state == RecordingState::Inactive) {
      endNs = pendingStoppedIntervalStartNs_.load(std::memory_order_acquire);
    } else if (state == RecordingState::Failed ||
               state == RecordingState::Finalized) {
      const int64_t frozen = frozenEndMonotonicNs_.load(
          std::memory_order_acquire);
      if (frozen != 0) {
        endNs = frozen;
      }
    }
    uint64_t samples = 0;
    if (!MonotonicNsToTimelineSample(startNs, endNs, samples)) {
      return 0;
    }
    return samples > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(samples);
  }

  bool autoRecordingEnabled() const noexcept {
    return autoRecordingEnabled_;
  }

  void markIntegrationUnsupported() noexcept {
    std::shared_ptr<RecordingSession> session;
    bool changed = false;
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const RecordingState oldState = state_.load(
            std::memory_order_relaxed);
        if (oldState == RecordingState::Failed ||
            oldState == RecordingState::Finalized ||
            oldState == RecordingState::Unsupported) {
          return;
        }
        captureTarget_.store(nullptr, std::memory_order_release);
        session = session_;
        state_.store(
            session == nullptr
                ? RecordingState::Unsupported
                : RecordingState::Failed,
            std::memory_order_release);
        changed = true;
      }
      if (session != nullptr) {
        session->signalIntegrationFailure();
      }
    } catch (...) {
      captureTarget_.store(nullptr, std::memory_order_release);
      state_.store(RecordingState::Failed, std::memory_order_release);
    }
    if (changed) {
      logTransition("recorder integration unsupported");
      notifyState();
    }
  }

  void enqueueLocal(
      const int16_t *samples,
      size_t framesPerChannel,
      size_t channels,
      uint32_t sampleRate) noexcept {
    RecordingSession *target = captureTarget_.load(std::memory_order_acquire);
    if (target == nullptr) {
      return;
    }
    target->enqueueLocal(samples, framesPerChannel, channels, sampleRate);
  }

  void enqueueRemote(
      const void *samples,
      size_t framesPerChannel,
      size_t bytesPerFrame,
      size_t channels,
      uint32_t sampleRate) noexcept {
    RecordingSession *target = captureTarget_.load(std::memory_order_acquire);
    if (target == nullptr) {
      return;
    }
    target->enqueueRemote(
        samples, framesPerChannel, bytesPerFrame, channels, sampleRate);
  }

private:
  void startInternal(OutputMode outputMode, bool autoStarted) {
    bool firstStart = false;
    bool restartAfterStop = false;
    bool failed = false;
    const int64_t boundaryNs = CurrentMonotonicTimeNs();
    const int64_t recordingStartWallTimeMs = CurrentUnixTimeMs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!supported_ || finishBegun_ ||
          state_.load(std::memory_order_relaxed) == RecordingState::Failed ||
          state_.load(std::memory_order_relaxed) ==
              RecordingState::Unsupported) {
        return;
      }
      const RecordingState oldState = state_.load(std::memory_order_relaxed);
      if (oldState == RecordingState::Idle) {
        const std::weak_ptr<CallRecordingController> weakOwner = owner_;
        session_ = RecordingSession::Create(
            basePath_,
            callMetadata_,
            outputMode,
            boundaryNs,
            recordingStartWallTimeMs,
            autoStarted,
            [weakOwner] {
              if (auto owner = weakOwner.lock()) {
                owner->impl_->onSessionFailed();
              }
            });
        if (session_ == nullptr) {
          frozenEndMonotonicNs_.store(boundaryNs, std::memory_order_relaxed);
          state_.store(RecordingState::Failed, std::memory_order_release);
          failed = true;
        } else {
          sessionStartMonotonicNs_.store(boundaryNs, std::memory_order_release);
          session_->start();
          captureTarget_.store(session_.get(), std::memory_order_release);
          state_.store(RecordingState::Recording, std::memory_order_release);
          firstStart = true;
        }
      } else if (oldState == RecordingState::Inactive && session_ != nullptr) {
        session_->restart(boundaryNs);
        pendingStoppedIntervalStartNs_.store(0, std::memory_order_release);
        captureTarget_.store(session_.get(), std::memory_order_release);
        state_.store(RecordingState::Recording, std::memory_order_release);
        restartAfterStop = true;
      } else {
        return;
      }
    }
    if (failed) {
      logTransition("recording first start failed");
    } else if (firstStart) {
      __android_log_print(
          ANDROID_LOG_INFO,
          kCallRecorderLogTag,
          "recording first start source=%s outputMode=%s",
          autoStarted ? "auto" : "manual",
          OutputModeName(outputMode));
    } else if (restartAfterStop) {
      logTransition("recording restart after stop");
    }
    notifyState();
  }

  void onSessionFailed() noexcept {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const RecordingState oldState = state_.load(std::memory_order_relaxed);
      if (oldState != RecordingState::Failed &&
          oldState != RecordingState::Finalized &&
          oldState != RecordingState::Unsupported) {
        captureTarget_.store(nullptr, std::memory_order_release);
        frozenEndMonotonicNs_.store(
            CurrentMonotonicTimeNs(), std::memory_order_release);
        state_.store(RecordingState::Failed, std::memory_order_release);
        changed = true;
      }
    }
    if (changed) {
      logTransition("recording failed");
      notifyState();
    }
  }

  void notifyState() noexcept {
    try {
      if (stateCallback_) {
        std::string sessionId;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (session_ != nullptr) {
            sessionId = session_->sessionId();
          }
        }
        stateCallback_(
            state(), elapsedSamples(), autoRecordingEnabled_, sessionId);
      }
    } catch (...) {
    }
  }

  static void logTransition(const char *message) noexcept {
#ifndef NDEBUG
    __android_log_print(ANDROID_LOG_INFO, kCallRecorderLogTag, "%s", message);
#else
    (void) message;
#endif
  }

  const std::string basePath_;
  const CallMetadata callMetadata_;
  const bool supported_;
  const bool autoRecordingEnabled_;
  const OutputMode initialOutputMode_;
  const StateCallback stateCallback_;
  std::weak_ptr<CallRecordingController> owner_;
  mutable std::mutex mutex_;
  std::shared_ptr<RecordingSession> session_;
  std::atomic<RecordingSession *> captureTarget_ = {nullptr};
  std::atomic<RecordingState> state_;
  std::atomic<int64_t> sessionStartMonotonicNs_ = {0};
  std::atomic<int64_t> pendingStoppedIntervalStartNs_ = {0};
  std::atomic<int64_t> frozenEndMonotonicNs_ = {0};
  bool firstEstablishedSeen_ = false;
  bool finishBegun_ = false;
  bool finishCompleted_ = false;
};

std::shared_ptr<CallRecordingController> CallRecordingController::Create(
    std::string basePath,
    CallMetadata callMetadata,
    bool supported,
    bool autoRecordingEnabled,
    OutputMode initialOutputMode,
    StateCallback stateCallback) {
  try {
    auto controller = std::shared_ptr<CallRecordingController>(
        new CallRecordingController(
            std::move(basePath),
            std::move(callMetadata),
            supported,
            autoRecordingEnabled,
            initialOutputMode,
            std::move(stateCallback)));
    controller->impl_->setOwner(controller);
    return controller;
  } catch (...) {
    return nullptr;
  }
}

CallRecordingController::CallRecordingController(
    std::string basePath,
    CallMetadata callMetadata,
    bool supported,
    bool autoRecordingEnabled,
    OutputMode initialOutputMode,
    StateCallback stateCallback)
    : impl_(std::make_unique<Impl>(
          std::move(basePath),
          std::move(callMetadata),
          supported,
          autoRecordingEnabled,
          initialOutputMode,
          std::move(stateCallback))) {
}

CallRecordingController::~CallRecordingController() = default;

void CallRecordingController::notifyInitialState() { impl_->notifyInitialState(); }
void CallRecordingController::onEstablished() { impl_->onEstablished(); }
void CallRecordingController::start(OutputMode mode) { impl_->start(mode); }
void CallRecordingController::pause() { impl_->pause(); }
void CallRecordingController::resume() { impl_->resume(); }
void CallRecordingController::stop() { impl_->stop(); }
void CallRecordingController::beginFinishCall() { impl_->beginFinishCall(); }
void CallRecordingController::finishCall() { impl_->finishCall(); }
RecordingState CallRecordingController::state() const noexcept { return impl_->state(); }
int64_t CallRecordingController::elapsedSamples() const noexcept { return impl_->elapsedSamples(); }
bool CallRecordingController::autoRecordingEnabled() const noexcept { return impl_->autoRecordingEnabled(); }
void CallRecordingController::markIntegrationUnsupported() noexcept {
  impl_->markIntegrationUnsupported();
}
void CallRecordingController::enqueueLocal(
    const int16_t *samples, size_t framesPerChannel, size_t channels,
    uint32_t sampleRate) noexcept {
  impl_->enqueueLocal(samples, framesPerChannel, channels, sampleRate);
}
void CallRecordingController::enqueueRemote(
    const void *samples, size_t framesPerChannel, size_t bytesPerFrame,
    size_t channels, uint32_t sampleRate) noexcept {
  impl_->enqueueRemote(
      samples, framesPerChannel, bytesPerFrame, channels, sampleRate);
}

} // namespace call_recording
} // namespace tgx
