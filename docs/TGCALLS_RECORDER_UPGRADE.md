# tgcalls Recorder Upgrade Guide

## Current baseline

The production-tested 1:1 call recorder uses:

| Component | Baseline |
| --- | --- |
| Telegram X branch | `call-recording-dev` |
| tgcalls repository | `https://github.com/YevhenKarpiuk/tgcalls.git` |
| tgcalls branch | `call-recording` |
| tgcalls commit | `60d59eadb793a63830fc3bd58682926486692996` |
| tgcalls upstream base | `332e581d6349c22460090ddf4b0aa19985b63330` |
| Recorder integration descriptor | integration `1`, tgcalls hook API `1`, local hook `1`, remote hook `1` |

Do not change the parent repository's tgcalls pointer while investigating an
upgrade. Build and runtime-test a temporary compatibility branch first. The
git SHA identifies this documented baseline; runtime code depends only on the
small hook contract and its version, not on a SHA comparison.

## Integration boundary

`app/jni/tgvoip/recorder/RecorderTgCallsAdapter.{h,cpp}` is the application-side
boundary. It is the only recorder file that should know about all of:

- `tgcalls::Descriptor` factories;
- `webrtc::AudioFrameProcessor` and `webrtc::AudioFrame`;
- `webrtc::AudioDeviceDataObserver` and
  `webrtc::CreateAudioDeviceWithDataObserver`;
- construction of the Android `AudioDeviceModule`.

The contract has two data outputs and minimal installation/lifetime behavior:

```text
LOCAL:  owned AudioFrame for duration of Process/sink callback
        -> copy PCM16 + callback metadata to recorder local SPSC queue

REMOTE: borrowed OnRenderData buffer for duration of callback
        -> copy PCM16 + callback metadata to recorder remote SPSC queue
```

The adapter does not contain Ogg/Opus writing, filesystem paths, JSON,
metadata, UI, settings, session policy, mixing, or export. Those remain in
Telegram X recorder code outside tgcalls. No PCM callback crosses JNI or calls
Java.

`kRecorderIntegrationDescriptor` records integration, tgcalls contract, local
hook, and remote hook versions. Increment the affected value only when its
meaning or signatures change.

## Production tgcalls patch audit

Audit command:

```bash
git -C app/jni/tgvoip/third_party/tgcalls diff \
  332e581d6349c22460090ddf4b0aa19985b63330..\
  60d59eadb793a63830fc3bd58682926486692996
```

The patch is generic: it has no `CallRecorder`, recording session, file, JSON,
or Telegram X setting knowledge.

| File/change | Purpose and direction | Lifecycle/API dependency | Failure signal after upgrade |
| --- | --- | --- | --- |
| `tgcalls/Instance.h`: forward-declare `AudioFrameProcessor`; add `Descriptor::createAudioFrameProcessor` | Provides the generic LOCAL processor factory | Descriptor is moved into `InstanceV2ImplInternal`; factory owns/replaces the existing processor | Adapter signature assertion or member access fails to compile |
| `tgcalls/v2/InstanceV2Impl.cpp`: store and invoke the factory before `EnableMedia()` | Injects LOCAL post-APM/pre-encoder PCM processing into `PeerConnectionFactoryDependencies.audio_frame_processor` | Called on tgcalls media-thread initialization; resulting processor is owned by the WebRTC media engine | Compile error if dependencies/member/type move; semantic review required if call order moves |
| `tgcalls/ThreadLocalObject.h`: nullable `perform()` and `reset(completion)` | Makes teardown safe; not an audio-direction hook | Value is reset on its owning tgcalls thread before completion | Compile conflict or changed stop ordering; must not bypass callback-source destruction |
| `tgcalls/v2/InstanceV2Impl.cpp`: `stop()` resets `_internal` before completion | Stops LOCAL and REMOTE callback owners before Telegram X finalizes the recorder | Completion is invoked only after internal media objects are destroyed on their owner thread | Lifetime test/review must fail if completion can precede source destruction |

REMOTE capture needs no recorder-specific tgcalls source patch. Telegram X
already supplies the generic `Descriptor::createAudioDeviceModule` factory,
and the adapter decorates its ADM with WebRTC's public render observer.

## Local PCM hook

Exact path:

```text
TGCallService -> VoIP -> TgCallsController JNI
  -> RecorderTgCallsAdapter::ConfigureRecorderTgCallsHooks
  -> tgcalls::Descriptor::createAudioFrameProcessor
  -> InstanceV2ImplInternal::start
  -> PeerConnectionFactoryDependencies.audio_frame_processor
  -> WebRTC AudioFrameProcessor::Process / installed sink
  -> CallRecordingController::enqueueLocal
  -> RecordingSession local bounded SPSC queue
  -> recorder worker
```

The `AudioFrameProcessor` API describes `Process()` as off the realtime audio
capture path, but the adapter still obeys realtime restrictions because sink
execution and future WebRTC implementations may be latency-sensitive.

Current format contract:

- sample type: signed native-endian PCM16, compile-checked from
  `AudioFrame::data()`;
- sample rates accepted by the existing normalization path: 8, 16, 32, 44.1,
  or 48 kHz;
- channels: one or two interleaved channels; stereo is explicitly downmixed on
  the recorder worker;
- callback size: complete 10 ms chunks, no more than 20 ms;
- ownership: the unique `AudioFrame` is valid only until it is forwarded;
  recorder enqueue copies samples and retains no pointer;
- lifetime: WebRTC owns the processor; the processor holds a shared controller
  reference until the media engine destroys it.

An unexpected format is not reinterpreted. The callback publishes an atomic
failure sentinel without logging or allocation. The recorder worker logs once,
marks that capture direction failed, and applies the existing useful-output
isolation policy.

## Remote PCM hook

Exact path:

```text
TGCallService -> VoIP -> TgCallsController JNI
  -> RecorderTgCallsAdapter::ConfigureRecorderTgCallsHooks
  -> Descriptor::createAudioDeviceModule
  -> CreateAndroidAudioDeviceModule
  -> CreateAudioDeviceWithDataObserver
  -> AudioDeviceDataObserver::OnRenderData
  -> CallRecordingController::enqueueRemote
  -> RecordingSession remote bounded SPSC queue
  -> recorder worker
```

Current format contract is signed native-endian PCM16 at the callback-provided
rate, one or two interleaved channels, in complete 10 ms chunks up to 20 ms.
The historical `bytesPerSample` callback argument is validated as the byte
width of a complete interleaved frame (`channels * sizeof(int16_t)`) in this
WebRTC baseline. The Android production construction requests mono and normally
reports 48 kHz, but 8/16/32/44.1 kHz route/device rates remain supported.

`OnRenderData` runs on the selected platform ADM playout callback thread. Its
buffer is borrowed only for the callback. The bounded queue copies it; no
pointer, observer buffer, or ADM storage is retained.

## Compile-time and runtime capability checks

`RecorderTgCallsAdapter.cpp` deliberately fails compilation when:

- `Descriptor::createAudioFrameProcessor` changes type;
- `Descriptor::createAudioDeviceModule` changes type;
- `AudioFrame::data()` is no longer `const int16_t *`.

The adapter also compiles constexpr regression cases for:

- both hooks available;
- local hook missing;
- remote hook missing;
- disabled configuration and destroyed callback target;
- supported and unsupported PCM formats;
- repeated create/destroy capability decisions.

At session integration, API version and local/remote availability are logged
once. Successful local/remote installation is logged once. Installation
failure changes the recorder to `UNSUPPORTED` before a session exists, or
`FAILED` and signals both capture streams if a session already exists. The
original ADM/processor is returned where possible, so the Telegram call
continues. The recorder never treats unavailable hooks as a successful empty
recording.

No diagnostic is emitted per audio frame. Format mismatch diagnostics are
worker-side and one-shot because the affected stream stops accepting data.

## Realtime invariants

For both directions the recurring path remains:

```text
native callback
  -> atomic state/monotonic timestamp/memcpy
  -> bounded per-direction SPSC queue
  -> recorder worker
  -> normalization/timeline/Opus/Ogg/filesystem/metadata
```

Never add JNI, Java callbacks, filesystem I/O, JSON, Opus work, an unbounded
allocation, or a blocking mutex to a PCM callback. Queue overflow and recorder
failure must not back-pressure, crash, or stop the Telegram call.

M11 does not change the 48 kHz mono master timeline, 20 ms Opus packets,
bitrates, VBR, DTX, 500 ms mix holdback, controls, crash recovery, or metadata
schema.

## Ownership and teardown

Ownership is deliberate:

- `TgCallsContext` owns the `tgcalls::Instance` and a shared
  `CallRecordingController` reference.
- Descriptor factories capture a shared controller while tgcalls initializes.
- WebRTC owns the local processor; the ADM wrapper owns the remote observer.
- Processor and observer each retain a shared controller, preventing a dangling
  callback target.
- `CallRecordingController` owns the current `RecordingSession`; its callback
  target is published to audio callbacks as an atomic raw pointer only while
  accepting. The owning shared session is not released during that interval.
- `RecordingSession` owns its worker and joins it before destruction.

Actual teardown ordering:

```text
TgCallsController.destroyInstance
  -> CallRecordingController.beginFinishCall
       -> clear atomic capture target / stop queue acceptance
       -> publish final boundary to recorder worker
  -> tgcalls::Instance::stop
       -> stop media
       -> ThreadLocalObject::reset internal media on owner thread
          (processor and ADM observer are destroyed; no new callbacks)
       -> invoke stop completion
  -> CallRecordingController.finishCall
       -> worker drains queues
       -> timeline and Opus/Ogg finalization
       -> final metadata attempt
  -> delete TgCallsContext / release remaining owners
```

Callbacks that race with `beginFinishCall` see a null capture target and
return. No recorder mutex was added to this path.

## Upgrade risk matrix

| Component | Risk | Verify after upgrade |
| --- | --- | --- |
| tgcalls `Instance`/`Descriptor` API | Medium | Factory fields still move into the selected 1:1 implementation and are invoked exactly once |
| `ThreadLocalObject` | High | Reset executes on the owner thread and stop completion cannot run before internal media destruction |
| WebRTC audio processor | High | Hook remains post-APM/pre-encoder; `Process`/`SetSink` ownership and forwarding semantics are unchanged |
| `AudioDeviceDataObserver` | High | Observer still receives final mixed render PCM immediately before playout; byte-count meaning is unchanged |
| PCM format | High | PCM16, rate, channel count, interleaving, callback duration, and actual `nSamplesOut` semantics |
| Callback threading | High | No callback moved to a thread where current copy/enqueue work is unsafe; no concurrent producer per SPSC stream |
| Lifetime/ownership | High | Processor/observer die before recorder finalization and cannot call a destroyed target |
| Telegram X JNI bridge | Medium | Descriptor is configured before `Meta::Create`; begin/finish ordering and state callbacks remain valid |

Clean patch application lowers merge risk only. It does not lower a High
semantic risk until the corresponding static, build, and runtime gates pass.

## Compatibility experiment recorded for M11

On 2026-09-13 the public TGX-Android remote exposed
`production=332e581d6349c22460090ddf4b0aa19985b63330` and
`development=2faee3b5524f54d56c91c2058c00e11c656a74b3`; it exposed no newer
production/development commit. The recorder hook patch was therefore tested in
an isolated clone against the alternative `development` revision
`2faee3b5524f54d56c91c2058c00e11c656a74b3`.

Result:

- `git apply --check`: clean;
- actual apply: clean, 3 files, 25 insertions, 5 deletions;
- `git diff --check`: clean;
- the Descriptor factory, `PeerConnectionFactoryDependencies` injection point,
  `ThreadLocalObject`, and stop/reset contexts were present with compatible
  signatures and ordering;
- recorder hook affected: yes, cleanly portable;
- Telegram X integration affected: not evaluated by an APK build because the
  experiment intentionally did not change the parent submodule and the isolated
  clone was not a full parent/dependency checkout.

This is patch/API analysis, not proof of runtime compatibility and not a reason
to update the production pointer.

## Upgrade procedure

1. Fetch upstream in the tgcalls repository.
2. Select the desired upstream commit and record its SHA/release notes.
3. Create an isolated clone or worktree and a local
   `compat-test/<short-sha>` branch.
4. Generate the production hook patch from the documented base and hook commit.
5. Run `git apply --check` in the compatibility worktree.
6. Apply the patch or rebase/cherry-pick the hook commit; record conflicts and
   upstream-modified files.
7. Review semantic placement, not just signatures: local post-APM/pre-encoder,
   remote final render, formats, callback threads, and ownership.
8. Resolve API changes only in the compatibility branch. Stop if integrating
   the candidate requires broad Telegram X changes unrelated to recorder hooks.
9. Do not update the parent submodule pointer yet.
10. Build the native/full Android target and confirm adapter static assertions.
11. Run repository, FGS, identity, and any adapter/synthetic recorder tests.
12. Install the experimental APK on an ARM64 target.
13. Test an outgoing call with distinct local and remote speech.
14. Test incoming foreground answer and recording.
15. Test incoming background/locked flow from FCM through ringing, Answer, and
    the microphone foreground service.
16. Verify local, remote, and mixed playback and track identity.
17. Exercise Start, Pause, Resume, Stop, and Restart.
18. Run the crash-recovery gate below.
19. Run the long-call gate below.
20. Only after every required gate passes, update the parent submodule pointer
    in a separately reviewed production-upgrade change.

### Useful static commands

```bash
git diff --check
git diff --submodule
git -C app/jni/tgvoip/third_party/tgcalls status
git -C app/jni/tgvoip/third_party/tgcalls rev-parse HEAD
./gradlew :app:testLatestArm64DebugUnitTest --rerun-tasks --console=plain
./gradlew assembleLatestArm64Debug --console=plain
find app/build/outputs -type f -name "*.apk" -print
```

## Runtime compatibility gate

### Outgoing

- Start a 1:1 call.
- Speak locally and remotely in distinguishable intervals.
- Verify `local.opus`, `remote.opus`, and `mixed.opus` contain the correct
  sources.

### Incoming foreground

- Receive, answer, start/auto-start recording, and verify all outputs.

### Incoming background or locked

- Verify FCM -> ringing -> Answer -> microphone FGS -> recording without
  changing the existing background-call architecture.

### Controls and files

- Exercise Start, Pause, Resume, Stop, and Restart.
- Verify local, remote, mixed, and metadata files.
- Require `timelineDifferenceSamples == 0` and queue drops `== 0` in a healthy
  test.

### Long call

- Mandatory minimum: at least 15 minutes.
- Preferred: 30–40 minutes.
- Compare timeline and track lengths; check drops, sync, seek/playback, and
  finalization.

### Crash recovery

```text
Start recording -> kill process -> reopen -> status INTERRUPTED
  -> playback/export retained media
```

This gate is mandatory after every future tgcalls/WebRTC upgrade, not only for
the original crash-recovery milestone.

## Rollback procedure

1. Do not commit the candidate parent pointer until all gates pass.
2. If a gate fails, keep the production parent pointer at
   `60d59eadb793a63830fc3bd58682926486692996`.
3. Preserve the compatibility branch and failure notes only if useful for the
   next attempt; do not modify or force-update stable `call-recording`.
4. Rebuild the baseline APK and rerun the affected runtime gate to distinguish
   candidate regressions from device/environment failures.
5. Do not work around recorder incompatibility by moving callbacks into JNI or
   Java, weakening format checks, or bypassing teardown ordering.
