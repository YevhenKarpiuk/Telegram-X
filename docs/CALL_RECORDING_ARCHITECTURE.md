# Call Recording Architecture

## Scope and verified revision

This document describes the audio path actually present in Telegram X at
revision `67501370426d` (version 1808), with tgcalls at
`332e581d6349c22460090ddf4b0aa19985b63330` and WebRTC at
`6ecff4f2446ff7d4ce38ca1c764f023e44dbcb1b`.

The implementation covered here captures separate local and remote PCM hooks
for ordinary 1:1 Telegram audio calls and encodes each copy to a standard Ogg
Opus file on the recorder worker. The current status is M5 recorder control,
M5.1 UI metadata, M6 recordings browser/playback, and M7 share/save/delete for
`InstanceV2Impl` versions 7, 8, 9, 12, and 13. Group calls, stereo split, and
transcoded export formats remain explicitly out of scope.

The design is intentionally session/stream based so that adding group calls
does not require replacing a two-global-stream recorder.

## Application identity / Firebase push

The recorder application uses Android application ID `ka.soft.tgxr` while the
original Telegram X Java namespace remains `org.thunderdog.challegram`.
`app.experimental=false` selects the standard Telegram X Firebase build path,
with Android client configuration in `app/google-services.json`. Push delivery
reuses the existing Telegram X Firebase-to-TDLib `registerDevice` pipeline.
Firebase service-account credentials are external and MUST NOT be committed.

## Executive decision

The least invasive design that captures the required signals is:

1. Represent a recording as one `RecordingSession` containing any number of
   `AudioStream` objects.
2. Capture local audio through WebRTC's existing
   `PeerConnectionFactoryDependencies.audio_frame_processor` extension. An
   identity/pass-through processor receives the final processed `AudioFrame`
   after AEC/NS/AGC and before `AudioSendStream` sends it to the encoder.
3. Capture the 1:1 remote side with WebRTC's existing
   `CreateAudioDeviceWithDataObserver` ADM decorator. Its `OnRenderData`
   callback runs after decode, jitter buffering, mixing, reverse-stream APM,
   and output resampling, immediately before the platform audio device consumes
   the block.
4. In every callback, only copy metadata and samples into a preallocated,
   bounded per-stream queue. A separate recorder worker owns file creation,
   format conversion/resampling, writes, finalization, and error handling.
5. Keep the call engine independent of recorder success. A missing, stopped,
   failed, or full recorder is a no-op from the call's point of view.

This uses public extension points already compiled into this tree and does not
require changing WebRTC's capture, APM, mixer, decoder, encoder, or Android
audio-device implementations.

## Session and stream model

The recorder must not have process-global `local` and `remote` buffers. The
minimum model is:

```text
RecordingSession
  sessionId
  sessionType: OneToOne | GroupCall
  lifecycle: Created -> Armed -> Recording -> Stopping -> Finished | Failed
  streams: map<streamId, AudioStream>

AudioStream
  direction: Local | Remote
  streamId
  participantId: optional
  sampleRate
  channels
  sampleFormat
  role: Main | Participant | MixedRemote
  boundedQueue
  droppedBlocks
```

`streamId` is a recorder-owned stable identifier, not an array index. For a
1:1 session the initial stream set is simply:

```text
local             direction=Local,  role=Main
remote            direction=Remote, role=Main
```

For a future group-call session it can be:

```text
local             direction=Local,  role=Main
remote:ssrc:A     direction=Remote, role=Participant, participantId=A if known
remote:ssrc:B     direction=Remote, role=Participant, participantId=B if known
remote:ssrc:C     direction=Remote, role=Participant, participantId=C if known
remote:mixed      direction=Remote, role=MixedRemote
```

The stream's source format must be recorded from callback metadata rather than
assumed globally. The writer may normalize each output to a chosen storage
format off the audio thread, but source format and output format are distinct
metadata.

## Java service to native tgcalls

### Actual 1:1 control path

```text
TGCallService
  initiateActualCall()
    -> VoIP.instantiateAndConnect(...)
       -> choose a mutually supported library version
       -> new TgCallsController(..., version)
          -> native newInstance(version, configuration, options)
             -> tgcalls::Meta::Create(version, Descriptor)
```

Concrete locations:

- `app/src/main/java/org/thunderdog/challegram/service/TGCallService.java`
  - class `TGCallService`
  - method `initiateActualCall()` calls `VoIP.instantiateAndConnect()`.
  - `releaseTgCalls()` calls `VoIPInstance.performDestroy()`.
- `app/src/main/java/org/thunderdog/challegram/voip/VoIP.java`
  - class `VoIP`
  - method `instantiateAndConnect()` builds `CallConfiguration` and
    `CallOptions`, selects the first common library version, constructs
    `TgCallsController`, and calls `initializeAndConnect()`.
- `app/src/main/java/org/thunderdog/challegram/voip/TgCallsController.java`
  - constructor calls native `newInstance()`.
  - `performDestroy()` calls native `destroyInstance()`.
- `app/jni/tgvoip/tgvoip.cpp`
  - JNI function `voip_TgCallsController_newInstance` translates Java state to
    `tgcalls::Descriptor`, invokes `tgcalls::Meta::Create()`, and stores the
    result in `TgCallsContext`.
  - JNI function `voip_TgCallsController_destroyInstance` calls
    `Instance::stop()` and deletes `TgCallsContext` only from its completion.
- `app/jni/tgvoip/third_party/tgcalls/tgcalls/Instance.cpp`
  - `Meta::Create()` selects the native implementation by negotiated version.

The implementations registered in `tgcalls::initialize()` are not one single
pipeline:

| Negotiated version | Implementation | Media construction |
| --- | --- | --- |
| 2.4.4 | `InstanceImplLegacy` | legacy libtgvoip, not the WebRTC ADM path documented below |
| 2.7.7, 5.0.0 | `InstanceImpl` -> `Manager` -> `MediaManager` | WebRTC media engine |
| 7.0.0, 8.0.0, 9.0.0, 12.0.0, 13.0.0 | `InstanceV2Impl` | WebRTC channels/media engine |
| 10.0.0, 11.0.0 | `InstanceV2ReferenceImpl` | WebRTC PeerConnection |

The current latest protocol entries, 12.0.0 and 13.0.0, therefore run through
`InstanceV2Impl`. A complete 1:1 recorder should wire the same extension points
in all three WebRTC-backed constructions rather than silently depend on which
version TDLib negotiates. Version 2.4.4 needs a separate legacy milestone or an
explicit unsupported/no-recording result; it must not be mistaken for an ADM
call.

### Threads created by tgcalls

- `InstanceImpl` creates `WebRTC-Manager`; `Manager` then creates work on the
  shared media, worker, and network threads from `StaticThreads`.
- V2 implementations likewise construct on the tgcalls media thread and build
  media on the shared WebRTC worker/network threads.
- These management threads create and configure ADM/channels. They are not the
  threads on which recurring PCM callbacks run.

## Android AudioDeviceModule actually used

Every WebRTC-backed 1:1 implementation first honors
`Descriptor.createAudioDeviceModule`; if it is empty, Android calls:

```text
webrtc::CreateAndroidAudioDeviceModule(kPlatformDefaultAudio)
```

Locations:

- `tgcalls/MediaManager.cpp`, `MediaManager::createAudioDeviceModule()`
- `tgcalls/v2/InstanceV2Impl.cpp`,
  `InstanceV2ImplInternal::createAudioDeviceModule()`
- `tgcalls/v2/InstanceV2ReferenceImpl.cpp`,
  `InstanceV2ReferenceImplInternal::createAudioDeviceModule()`
- `webrtc/sdk/android/native_api/audio_device_module/audio_device_android.cc`,
  `CreateAndroidAudioDeviceModule()`

AAudio sources are listed as disabled in this project's CMake. At runtime the
platform-default factory chooses:

- OpenSL ES input and output when Android reports low-latency input/output; or
- Java `AudioRecord` and `AudioTrack` otherwise.

The factory explicitly requests mono input and mono output. The sample rate is
not hardcoded to 48 kHz at the hardware boundary. It is read from
`AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE`; if unavailable it falls back to
16 kHz, and the old-emulator override is 8 kHz. Most target devices report
48 kHz, but recorder code must trust the callback's `samples_per_sec`.

All ADM-to-`AudioTransport` blocks are 10 ms. For device rate `R`, each block
contains `R / 100` frames. PCM is signed 16-bit native-endian, interleaved;
current Android construction has one channel. In the ADM callback API,
`nBytesPerSample` is populated as bytes per complete frame
(`channels * sizeof(int16_t)`), despite its historical name.

Java fallback callback threads are:

- capture: `AudioRecordJavaThread`, `THREAD_PRIORITY_URGENT_AUDIO`;
- playout: `AudioTrackJavaThread`, `THREAD_PRIORITY_URGENT_AUDIO`.

With OpenSL ES, callbacks run on the corresponding OpenSL ES realtime callback
threads; their Java names do not apply. Recorder callbacks must therefore be
treated as realtime regardless of selected Android audio layer.

## Local audio path

### Verified path: microphone to encoder

For the Java ADM path (OpenSL ES reaches the same `AudioDeviceBuffer` boundary):

```text
android.media.AudioRecord.read()
  WebRtcAudioRecord.AudioRecordThread.run()
  -> nativeDataIsRecorded(...)
  -> AudioRecordJni::DataIsRecorded()
  -> AudioDeviceBuffer::SetRecordedBuffer()
  -> AudioDeviceBuffer::DeliverRecordedData()
  -> AudioTransportImpl::RecordedDataIsAvailable()
     -> RemixAndResample(..., encoder format, AudioFrame)
     -> ProcessCaptureFrame(..., AudioProcessing, AudioFrame)
        -> WebRTC AEC / NS / AGC / filters
     -> AsyncAudioProcessing::Process(AudioFrame)       [when configured]
        -> AudioFrameProcessor::Process(AudioFrame)     [selected hook]
        -> AudioTransportImpl::SendProcessedData()
     -> AudioSendStream::SendAudioData()
     -> ChannelSend::ProcessAndEncodeAudio()
     -> encoder queue "AudioEncoder"
     -> AudioCodingModule::Add10MsData()
     -> Opus encode / RTP packetization / tgcalls transport
```

Relevant files and methods:

- `webrtc/sdk/android/src/java/org/webrtc/audio/WebRtcAudioRecord.java`
  - `WebRtcAudioRecord.AudioRecordThread.run()` reads one 10 ms direct buffer.
- `webrtc/sdk/android/src/jni/audio_device/audio_record_jni.cc`
  - `AudioRecordJni::DataIsRecorded()` forwards the buffer and timing data.
- `webrtc/modules/audio_device/audio_device_buffer.cc`
  - `AudioDeviceBuffer::DeliverRecordedData()` calls
    `AudioTransport::RecordedDataIsAvailable()`.
- `webrtc/audio/audio_transport_impl.cc`
  - `AudioTransportImpl::RecordedDataIsAvailable()` converts the device block
    to the active sender's rate/channel count and calls `ProcessCaptureFrame()`.
  - `AudioTransportImpl::SendProcessedData()` is the exact post-APM boundary.
- `webrtc/modules/audio_processing/audio_processing_impl.cc`
  - `AudioProcessingImpl` runs echo cancellation, noise suppression, gain
    control, and the remaining capture adjustments.
- `webrtc/audio/audio_send_stream.cc`
  - `AudioSendStream::SendAudioData()` receives final processed PCM and passes
    it to `ChannelSend`.
- `webrtc/audio/channel_send.cc`
  - `ChannelSend::ProcessAndEncodeAudio()` posts to `AudioEncoder` and calls
    `AudioCodingModule::Add10MsData()`.

### Selected local PCM hook

Use WebRTC's existing `webrtc::AudioFrameProcessor`, supplied through
`PeerConnectionFactoryDependencies.audio_frame_processor`.

The concrete callback is the identity processor
`RecordingAudioFrameProcessor::Process(std::unique_ptr<webrtc::AudioFrame>)`.
It must:

1. copy/enqueue the frame into the local `AudioStream`;
2. immediately invoke the sink installed by `SetSink()` with the same,
   unmodified frame.

WebRTC documents and implements this callback as post-capture-processing and
pre-encoding. `AsyncAudioProcessing` invokes it on a normal-priority task queue
named `AsyncAudioProcessing`, off the realtime capture thread. The downstream
sink returns the frame to `AudioTransportImpl::SendProcessedData()` and then to
the normal encoder path.

For the current Opus 1:1 configuration the selected hook sees:

| Property | Value |
| --- | --- |
| file | `webrtc/api/audio/audio_frame_processor.h` (API), integration in the three tgcalls media constructors |
| class/method | future `RecordingAudioFrameProcessor::Process()` implementing `webrtc::AudioFrameProcessor` |
| callback position | after `ProcessCaptureFrame()`, before `AudioSendStream::SendAudioData()` and Opus |
| sample rate | 48,000 Hz (Opus encoder's `SampleRateHz`) |
| channels | 1 (Opus defaults to mono because no `stereo=1` parameter is negotiated) |
| sample format | signed PCM16, native-endian, interleaved (`AudioFrame::data()`) |
| block size | 480 frames/channel = 480 samples = 960 bytes = 10 ms |
| execution | WebRTC task queue `AsyncAudioProcessing`, not the ADM realtime thread; encoding resumes asynchronously after the processor returns the frame |

The callback must still avoid file I/O: blocking this queue delays delivery to
the encoder and can create outgoing-audio gaps.

### Why ADM capture is not the selected local hook

`CreateAudioDeviceWithDataObserver` also exposes
`AudioDeviceDataObserver::OnCaptureData()`. In this tree it is called by the ADM
decorator *before* forwarding to
`AudioTransportImpl::RecordedDataIsAvailable()`. Therefore it contains raw
device PCM, before WebRTC software AEC/NS/AGC and before resampling to the Opus
sender format. The `audioSamples` argument is const and
`RecordedDataIsAvailable()` does not return processed PCM to the ADM.

Consequently an ADM wrapper alone cannot capture the required post-WebRTC local
signal. `OnCaptureData()` is useful as an optional diagnostic/raw-microphone
stream, but must not be labelled as the transmitted local stream.

Using `AudioProcessingBuilder::SetCapturePostProcessing()` would avoid a core
patch but is also not exact: WebRTC still applies capture-level post adjustment
and unmute suppression after that custom processor. `AudioFrameProcessor` is
the clean existing API for the exact pre-encoder frame.

## Remote audio path

### Verified path: network to speaker

For current 12.0.0/13.0.0 calls, `InstanceV2Impl` connects the WebRTC voice
channel to `NativeNetworkingImpl`'s `RtpTransport`. The receive side then is:

```text
encrypted network packet / NativeNetworkingImpl / RtpTransport
  -> WebRTC VoiceChannel receive channel
  -> ChannelReceive::OnRtpPacket()
  -> ChannelReceive::OnReceivedPayloadData()
  -> AcmReceiver::InsertPacket()
  -> NetEqImpl::InsertPacket()                 [jitter buffer]

playout demand, every 10 ms:
  platform output callback
  -> AudioTrackJni::GetPlayoutData()           [Java path]
  -> AudioDeviceBuffer::RequestPlayoutData()
  -> AudioTransportImpl::NeedMorePlayData()
     -> AudioMixerImpl::Mix()
        -> ChannelReceive::GetAudioFrameWithInfo()
           -> AcmReceiver::GetAudio()
              -> NetEqImpl::GetAudio()         [Opus decode / PLC]
           -> optional per-stream RawAudioSink
        -> FrameCombiner                       [all receive sources]
     -> ProcessReverseAudioFrame(APM)
     -> resample to ADM output rate
  -> ADMWrapper / AudioDeviceDataObserver::OnRenderData() [selected hook]
  -> AudioDeviceBuffer::GetPlayoutData()
  -> android.media.AudioTrack.write()          [Java path]
```

Important concrete files:

- `tgcalls/v2/InstanceV2Impl.cpp`
  - `IncomingV2AudioChannel` constructs the receive voice channel and installs
    the current level-only `AudioSinkImpl` through `SetRawAudioSink()`.
- `webrtc/audio/channel_receive.cc`
  - `ChannelReceive::OnRtpPacket()` and `OnReceivedPayloadData()` feed ACM.
  - `ChannelReceive::GetAudioFrameWithInfo()` pulls decoded 10 ms PCM and calls
    the optional per-stream sink before volume/panning and mixing.
- `webrtc/modules/audio_coding/acm2/acm_receiver.cc`
  - `AcmReceiver::InsertPacket()` and `GetAudio()` bridge to NetEq.
- `webrtc/modules/audio_coding/neteq/neteq_impl.cc`
  - `NetEqImpl::InsertPacket()` and `GetAudio()` perform jitter-buffered decode,
    packet-loss concealment, and playout timing.
- `webrtc/modules/audio_mixer/audio_mixer_impl.cc`
  - `AudioMixerImpl::Mix()` requests each active `ChannelReceive` source and
    combines them.
- `webrtc/audio/audio_transport_impl.cc`
  - `AudioTransportImpl::NeedMorePlayData()` mixes, supplies reverse audio to
    APM, and resamples to the device rate.
- `webrtc/modules/audio_device/audio_device_data_observer.cc`
  - `ADMWrapper::NeedMorePlayData()` first delegates to WebRTC, then calls
    `AudioDeviceDataObserver::OnRenderData()` with the filled buffer.
- `webrtc/sdk/android/src/jni/audio_device/audio_track_jni.cc`
  - `AudioTrackJni::GetPlayoutData()` copies the final block to Android.

### Selected remote PCM hook

Use a recorder observer implementing
`webrtc::AudioDeviceDataObserver::OnRenderData()`, attached with the already
compiled `webrtc::CreateAudioDeviceWithDataObserver(baseAdm, observer)`.

For a 1:1 call, the mixer contains the one remote receive source, so this is the
remote side as actually rendered. It also includes NetEq PLC and timing
corrections and follows output-route resampling. It does not include microphone
audio.

| Property | Value |
| --- | --- |
| file | `webrtc/modules/audio_device/audio_device_data_observer.cc` |
| class/method | recorder observer, `AudioDeviceDataObserver::OnRenderData()`; invoked by existing `ADMWrapper::NeedMorePlayData()` |
| callback position | after NetEq decode, receive-source mixer, reverse APM, and output resample; before platform playout |
| sample rate | runtime ADM output rate `R`; normally 48,000 Hz on target hardware, but callback metadata is authoritative (16 kHz fallback, 8 kHz old emulator, or another Android native rate are possible) |
| channels | 1 in the current Android ADM construction |
| sample format | signed PCM16, native-endian, interleaved |
| block size | `R / 100` frames/channel = 10 ms; at 48 kHz, 480 samples / 960 bytes |
| execution | realtime playout callback: `AudioTrackJavaThread` for Java ADM or the OpenSL ES output callback thread |

The observer must copy no more than the returned/output sample count. The
current wrapper supplies the requested 10 ms `nSamples`, and normal
`AudioTransportImpl` fills the complete block. Error/silence handling should
still be defensive.

### Alternative remote hook and why it is not primary for 1:1

`IncomingV2AudioChannel` already installs a `webrtc::AudioSinkInterface` via
`SetRawAudioSink()`. Its `OnData()` receives per-SSRC decoded Opus PCM before
receive volume and mixer processing: 48 kHz, mono by default, signed PCM16,
480 frames/10 ms, on the playout/audio callback path.

That point is valuable for future participant-separated group recording and
for a fixed 48 kHz source, but `OnRenderData()` is the smaller, version-neutral
1:1 change and most exactly represents what reaches the speaker. The recorder
model supports both roles (`Participant` and `MixedRemote`) without conflating
them.

## Can both sides be intercepted by an ADM wrapper?

Partially:

| Signal | ADM observer availability | Semantics | Suitable for milestone |
| --- | --- | --- | --- |
| local capture | yes, `OnCaptureData()` | raw microphone, pre-software APM | no, not the requested processed/transmitted PCM |
| remote render | yes, `OnRenderData()` | decoded/mixed/render-resampled output | yes |

The repository already contains the necessary wrapper. The implementation
creates the normal Android ADM, wraps it with
`CreateAudioDeviceWithDataObserver`, and provides it via
`Descriptor.createAudioDeviceModule`; it does not add another forwarding ADM.

## Realtime-safe queue and writer

### Queue topology

Use one bounded SPSC ring per `AudioStream`, with a single recorder writer
thread draining all active streams. This avoids a single contended global
queue and naturally extends to group participants.

Each stream and its ring are created on a control/worker thread before the sink
becomes visible to an audio callback. The callback holds a direct, lifetime-safe
stream handle; it must not perform a map lookup requiring a mutex. Group-call
streams are similarly registered when the receive channel/SSRC mapping is
created, not lazily from the first PCM callback.

Preallocated slots contain:

```text
stream handle / streamId
monotonic capture timestamp
sequence number
sampleRate
channels
sampleFormat
framesPerChannel
payload length
fixed-capacity sample storage
```

Capacity must cover the largest accepted WebRTC 10 ms block and channel count,
not only the common 480-sample mono case. Reject/drop an invalid oversized
block safely rather than allocating in the callback. The implementation also
rejects blocks longer than 20 ms; the active local and remote WebRTC hooks
normally deliver 10 ms blocks, and this guard keeps the worker-side mixed
holdback formally bounded even for corrupt metadata.

### Callback rules

Allowed:

- load atomic session/stream state;
- reserve a preallocated ring slot;
- copy metadata and contiguous PCM bytes;
- publish the slot with release semantics;
- increment atomic counters on drop/error.

Forbidden:

- file open/write/flush/close;
- heap allocation or vector growth;
- blocking locks, waits, condition-variable waits, or thread joins;
- JNI calls, Java callbacks, logging, format conversion, resampling, or WAV
  header updates.

If a queue is full, drop the newest block, increment `droppedBlocks`, and
continue the call. Recording backpressure must never propagate into WebRTC.
The worker may be notified with a nonblocking event/atomic wake mechanism; it
must also poll safely enough that a lost wakeup cannot stall draining.

### Writer responsibilities

The recorder worker is the only owner of file descriptors, resamplers,
encoders, container writers, and the mixed timeline. It:

- creates output files and writes Ogg Opus identification/comment headers;
- drains blocks in per-stream sequence order;
- downmixes to mono and resamples accepted 10/20 ms source blocks to 48 kHz with WebRTC's
  `PushResampler<int16_t>`;
- handles runtime source-format changes by reinitializing only the worker-side
  resampler while retaining one logical Ogg stream per side;
- feeds timestamped normalized PCM to the bounded master session timeline;
- constructs equal-length local, remote, and mixed 20 ms master frames, using
  silence wherever a side has no PCM;
- accumulates 960 samples per 20 ms Opus frame and encodes side files at
  32 kbit/s VBR and the mixed file at 48 kbit/s VBR;
- periodically records drop/error counters outside realtime callbacks;
- emits EOS with an exact final granule position, flushes, closes, and reports
  the session result.

`OggOpusWriter` is shared by all three outputs but remains below the master
timeline. A side writer failure does not stop normalization or the other
writers, a mixed writer failure does not affect the side writers, and no
recorder failure is propagated into the call.

## Recorder lifecycle for a 1:1 call

1. `voip_TgCallsController_newInstance` creates one non-global
   `RecordingSession(sessionType=OneToOne)` owned by `TgCallsContext` and
   pre-registers `local` and `remote` streams.
2. The recorder worker and bounded queues are prepared before the ADM and
   `AudioFrameProcessor` hooks can emit data. If preparation fails, both hooks
   remain harmless pass-through/no-op components.
3. The session becomes `Recording` on the first desired lifecycle boundary
   (recommended: tgcalls `State::Established`). Pre-establishment callbacks are
   ignored unless a later product requirement explicitly asks to retain them.
4. Route changes do not create a new logical remote stream. Callback format
   metadata detects a device-rate change, and the writer handles it off-thread.
5. Java `TgCallsController.performDestroy()` enters JNI
   `destroyInstance()`. `beginStop()` disables both stream producers and marks
   the session `Stopping`, but does not permit the worker to exit.
6. `InstanceV2Impl::stop()` resets its media-thread `ThreadLocalObject` before
   invoking the external completion. Destruction releases the channels, call,
   async processor, and ADM, so completion is the synchronization point proving
   PCM callbacks have stopped. Only in this completion does `finish()` publish
   `finishRequested`; the worker then drains all remaining blocks, finalizes
   files and metadata, and is joined.
7. Delete `TgCallsContext` and its `RecordingSession` after recorder
   finalization. A timeout/failure in finalization abandons recording resources
   safely and must not prevent the normal `handleStop` call or call teardown.

Callback objects should use weak/shared lifetime tokens rather than raw access
to a deleted context. Stop is idempotent because service teardown and native
failure paths may converge.

## Milestone 1 and 2 implementation

The minimal 1:1 implementation set is:

- `app/jni/tgvoip/CallRecorder.h` and `CallRecorder.cpp` (new)
  - `RecordingSession`, `AudioStream`, bounded rings, writer, the ADM data
    observer, and identity `RecordingAudioFrameProcessor`.
- `app/jni/tgvoip/tgvoip.cpp`
  - own the session in `TgCallsContext`;
  - provide the wrapped Android ADM through
    `Descriptor.createAudioDeviceModule`;
  - provide the identity frame processor factory;
  - bind start/stop to tgcalls state and stop completion.
- `app/jni/tgvoip/CMakeLists.txt`
  - compile the recorder source into `tgcallsjni` and link the existing `opus`
    and `ogg` targets.
- `app/jni/tgvoip/third_party/tgcalls/tgcalls/Instance.h`
  - add a neutral factory/callback field to `Descriptor` for creating a
    `webrtc::AudioFrameProcessor`; tgcalls must not depend on recorder storage
    details.
- `app/jni/tgvoip/third_party/tgcalls/tgcalls/v2/InstanceV2Impl.cpp`
  - install it into `PeerConnectionFactoryDependencies` for versions 7-9 and
    12-13;
  - destroy media hooks before reporting stop completion.
- `app/jni/tgvoip/third_party/tgcalls/tgcalls/ThreadLocalObject.h`
  - expose completion-bearing media-thread reset for the stop barrier;
  - make a queued `perform()` arriving after reset a safe no-op.
- `CallConfiguration.java` and `VoIP.java`
  - pass the TDLib call id and absolute `getFilesDir()` path to the
    native control side.

No WebRTC source is changed. `audio_device_data_observer.cc`,
`audio_transport_impl.cc`, `audio_processing_impl.cc`, `audio_send_stream.cc`,
`channel_send.cc`, `channel_receive.cc`, NetEq, mixer, and Android audio-device
files are traced dependencies, not edit targets.

`InstanceV2ReferenceImpl` (versions 10-11), `MediaManager` (2.7.7/5.0.0), and
legacy 2.4.4 are deliberately unsupported in this milestone. Their descriptors
receive no recorder factories, so calls using those engines retain their normal
behavior without producing recording files.

Each supported call owns one `RecordingSession` through `TgCallsContext`.
Callbacks also hold shared session handles, preventing dangling pointers during
asynchronous teardown. Two 127-block-capacity SPSC rings use 128 preallocated
slots each; every slot accepts up to 3,840 interleaved PCM16 samples (the maximum
accepted 10 ms block at 192 kHz stereo). A full or invalid block is dropped and
counted. The callbacks do not notify the worker; it polls at 10 ms, so callback
work is limited to atomic operations, metadata assignment, a clock read, and
`memcpy`.

The worker normalizes each side to 48 kHz mono PCM before the Milestone 3 master
timeline writes `local.opus` and `remote.opus` as independent Ogg logical
streams. It records every actual source rate/channel transition in `info.json`;
speaker, earpiece, and Bluetooth route changes therefore reconfigure the
worker-side resampler but never rotate the output file. Multichannel input is
averaged with a widened accumulator and clamped back to PCM16 before resampling.

Each valid callback receives a sequence number before the ring-full check. On
queue overflow, atomics retain cumulative dropped-block counts and any exact
duration derivable from `framesPerChannel/sampleRate`; successful ring entries
snapshot those counters. The worker records each resulting gap in `info.json`
and resets the resampler across the discontinuity. The original Milestone 2
side-only writer inserted the known 48-kHz-equivalent silence directly; the
Milestone 3 master timeline supersedes that behavior and leaves the timestamped
interval empty, so the common commit loop writes it once as silence without
double accounting.

The Opus encoder uses `OPUS_APPLICATION_VOIP`, 32 kbit/s, VBR enabled, and DTX
disabled. `OpusHead` stores the encoder lookahead as pre-skip and `OpusTags`
uses `Telegram X Recorder` plus a non-sensitive stream comment. The last
encoded packet remains pending until another packet exists; at finish the final
partial PCM frame is zero-padded only for encoding, its granule position counts
only actual samples, and the pending packet is submitted with EOS. A stream
codec/container/file failure marks only that output failed; normalization may
continue for the mixed writer while the other stream and call continue. Output
is created only after
`State::Established` under
`getFilesDir()/calls/YYYY-MM-DD_HH-MM-SS_<callId>/`.

## Milestone 3 — one monotonic master timeline

Milestone 3 uses one rule for all temporal decisions:

```text
One call = one monotonic master timeline.
Audio presence does not define time. The session clock defines time.
Missing PCM is silence.
```

The local post-APM and remote render capture hooks are unchanged. Their
realtime callbacks still do only a steady-clock read, sequence/format metadata,
atomics, a bounded SPSC-ring operation, and `memcpy`. All downmixing,
resampling, timeline placement, mixing, Opus encoding, and file I/O remain on
the recorder worker.

### Session clock and ownership

`RecordingSession::start()` captures `sessionStartMonotonicNs` at the first
`State::Established`. `beginStop()` captures `sessionEndMonotonicNs` before the
unchanged tgcalls stop barrier. Both values and callback timestamps use the
same `steady_clock` domain. Unix timestamps remain directory/diagnostic data
only and never align media.

`SessionAudioTimeline` owns the local, remote, and mixed writers plus the
single commit cursor. An overflow-safe nearest-sample conversion maps a
monotonic timestamp to the 48 kHz session coordinate:

```text
timelineSample =
    round((timestampNs - sessionStartMonotonicNs) * 48000 / 1_000_000_000)
```

Each worker-side normalized source block retains its real duration: 480
samples for a 10 ms block, 960 samples for a 20 ms block, or the corresponding
result after source-rate conversion. Callback arrival order is never used as a
cross-stream clock.

### Continuity segments and gaps

Each side tracks `lastSourceTimestampNs`, `lastTimelineEndSample`, and whether
a continuity segment is active. Inside a segment, normalized sample progression
is the clock. If a callback timestamp candidate differs from the expected next
sample by at most 50 ms, the new block starts exactly at the previous block end.
Thus ordinary +/-5, 10, or 20 ms scheduling jitter creates neither holes nor
overlaps.

If the timestamp candidate is more than 50 ms after the previous end, the
callback opens a new segment at its timestamp-derived position. The uncovered
interval is not materialized as a PCM allocation; it remains implicit silence.
A 250 ms pause, a multi-second reconnect, or a route-switch callback gap is
therefore preserved at its real duration even though the commit holdback is
500 ms. A negative timestamp discontinuity never moves audio backward:
placement remains at the previous segment end, and deviations crossing the
separate diagnostic threshold enter the bounded diagnostic state.

An SPSC sequence gap forces a new segment from the next callback timestamp even
when only one 10 or 20 ms callback was lost and the general 50 ms threshold
would otherwise classify it as jitter. Queue counters are diagnostics only;
`AudioStream` does not separately append silence. This prevents both gap
compression and double insertion.

### Clock-driven bounded commit

The worker advances the master clock even when both callback queues are empty:

```text
nowTimelineSample = monotonicToTimeline(CurrentMonotonicTimeNs())
commitUntil = floorTo20ms(nowTimelineSample - 24000)
```

The 24,000-sample holdback is commit latency, not a source-derived watermark
and not a minimum gap length. Normal commits use 960-sample (20 ms) intervals.
For every interval the timeline initializes local and remote frames to silence,
copies any overlapping real PCM, calculates
`round((local + remote) / 2)` with an `int32_t` accumulator and PCM16 clamp, and
passes exactly the same sample count to all three writers. No AGC, limiter,
compressor, or automatic gain is applied.

After a range is committed, chunks ending in that range are discarded. A
callback whose complete real position is already behind the commit cursor is
dropped. For a partial overlap, only its committed prefix is dropped and the
remaining suffix keeps its original position. These samples and bounded event
details are reported as `lateSamplesDropped`/`lateDrops`; late PCM is never
rebased into the present.

Timeline chunk payload has an explicit hard cap of 29,280 PCM16 samples per
side:

```text
24,000 holdback
+ 2,400 continuity tolerance
+ 3 * 960 maximum normalized callback blocks
= 29,280 samples per side
```

Across two tracks that is 58,560 samples or 117,120 bytes. The cap includes
commit-frame alignment and overlap bookkeeping margin and is independent of
call length. Long silent gaps allocate no PCM. Separately, the two fixed
128-slot realtime rings contain 1,966,080 bytes of sample payload, and the
worker downmix/normalized/resampler scratch buffers have a 21,120-byte PCM
payload bound. Ogg/Opus writers each retain only one 20 ms PCM accumulator,
one encoded-packet buffer, and one pending packet.

### Equal duration and finish

After `beginStop()` has disabled callback acceptance, the unchanged tgcalls
barrier lets the worker drain both fixed queues. `SessionAudioTimeline::finish`
then commits through

```text
finalTimelineSamples =
    round((sessionEndMonotonicNs - sessionStartMonotonicNs) * 48000 / 1e9)
```

including trailing silence for an absent or failed side. It does this even if
no audio callback occurred, so an established five-second call produces three
five-second silence files. The reusable `OggOpusWriter` remains unaware of
timestamps, sides, gaps, and call state. It zero-pads only the encoder input for
a final partial frame; the Ogg granule position counts only real master-timeline
samples and the final packet carries EOS.

For every writer that completes successfully, the construction enforces:

```text
localWrittenSamples
== remoteWrittenSamples
== mixedWrittenSamples
== masterTimelineCommittedSamples
== finalTimelineSamples
```

Writer failures remain isolated: a failed local writer does not stop remote or
mixed, a failed mixed writer does not stop either side, and capture or
normalization failure turns that logical side into silence without affecting
the call. `info.json` records session monotonic bounds, total timeline samples,
per-side provided/audio/silence/late counts, source formats, queue gaps,
bounded continuity/timestamp/late events, each writer's encoded sample count,
and `equalDurationInvariant`. The obsolete per-stream produced watermark,
`timelineBase`, mapping epochs, resume rebases, and `timelineRebases[]` no
longer exist.

A constexpr regression helper is enforced by `static_assert` and also checked
when the worker starts. It covers continuous streams, delayed first audio,
local/remote/simultaneous pauses, a 250 ms pause, route/queue gaps, callback
jitter, full and partial late drops, equal writer counts, bounded buffering,
and a five-second silence-only session model.

## v5.1 — diagnostic cleanup

Continuity placement and timestamp diagnostics deliberately use different
thresholds:

```text
continuity jitter tolerance:       50 ms
timestamp diagnostic enter:       >100 ms
timestamp diagnostic exit:         <60 ms
meaningful active-state update:    >50 ms change
```

The 50 ms continuity tolerance still decides whether positive timestamp delay
starts a real segment, so 100, 119, 250 ms, and multi-second callback gaps are
not compressed. The diagnostic state is only observability: it cannot move
PCM. Ordinary Android/WebRTC deviations around -50 to -52 ms remain below the
100 ms entry boundary and therefore no longer generate repeated JSON entries.

Once diagnostics enter an exceptional state, a stable delta is debounced. A
new event is retained only when its magnitude changes meaningfully, or when it
falls below 60 ms and the state is recorded as `restored`. Events and their
suppressed counter remain bounded.

Repeated tgcalls `State::Established` transitions during reconnect still call
the idempotent `RecordingSession::start()`, but `recording start` is logged only
inside the actual not-started to started transition. Similarly, an existing
`files/calls` directory is reported as successful `mkdir ... success existing`;
`EEXIST` is an error only when the path is not an acceptable directory.

## Milestone 4 — long-call stability

The v5 master timeline remains authoritative and is not reset by
`Reconnecting`/`Established`, route changes, silence, or absence of both audio
streams. Its final length is always converted once from the absolute
`sessionStartMonotonicNs` and `sessionEndMonotonicNs` boundary. There is no
per-callback duration rounding in the master clock and therefore no model that
can accumulate one sample of error per block.

Within each uninterrupted source segment, exact normalized sample progression
remains the stream clock. A callback timestamp is converted from the absolute
session anchor only for initial positioning, continuity decisions, and
diagnostics. WebRTC source blocks are normalized in complete 10 ms units:
16 kHz/160 frames and 48 kHz/480 frames both produce exactly 480 timeline
samples; stereo-to-mono changes affect sample values, not frames per channel or
timeline position.

The deterministic worker check now covers 60 seconds, 30 minutes, and two hours.
The two-hour boundary converts to exactly 345,600,000 samples, reconstructing
the same value from 720,000 synthetic 10 ms callbacks; the drift is zero. It
also verifies that the final Ogg granule arithmetic remains far below signed
64-bit limits and that converting the result to milliseconds yields exactly
7,200,000 ms.

Reconnect simulations cover:

- both sides present for 0..10 and 15..30 seconds, leaving exactly five seconds
  of silence in 10..15;
- either local or remote alone through that interval;
- both streams absent while the master clock continues;
- three successive 2, 5, and 1 second gaps without cumulative offset;
- stop while both streams are absent and an entirely silence-only call.

Additional checks cover 100, 119, and 250 ms gaps below the 500 ms holdback,
ordinary +/-5/10/20/30/40 ms jitter, 48 kHz mono/stereo and 16 kHz mono source
transitions, one and multiple forced queue-loss segments, full and partial late
blocks, and isolated virtual writer failure. A queue loss creates exactly the
timestamp-derived missing interval; no second silence insertion occurs. A late
range is discarded rather than rebased, while a future suffix of a partially
late block remains available.

For each successful side, diagnostics verify:

```text
audioSamplesWritten + silenceSamplesWritten == timelineSamples
```

`info.json` also exposes `sessionTimelineSamples`,
`timelineExpectedFromMonotonic`, `timelineDifferenceSamples`, explicit capture,
writer, and timeline-internal failure flags, side accounting booleans, and the
existing equal-duration invariant. Each SPSC queue publishes an atomic
`maxQueueDepth` plus its fixed capacity of 127 slots, allowing long device tests
to detect worker backlog without locks, allocation, or callback logging.

The PCM bounds are unchanged: 29,280 samples per timeline side, 117,120 bytes
across both timeline tracks, plus fixed callback rings and bounded worker
scratch. Continuity, late-drop, timestamp, source-format, and queue-gap
diagnostic vectors all have fixed event limits and suppressed counters. Three
Opus encoders remain worker-only; complexity is unchanged until device
measurements demonstrate a load problem.

## Milestone 5 — recording control and call UI

Milestone 5 keeps the v6 audio architecture intact. There is still one absolute
monotonic master timeline, normalized to 48 kHz mono, and each enabled output
advances by the same number of samples. The vendored tgcalls PCM patch is
unchanged.

### Ownership and dormant mode

Each supported 1:1 tgcalls call owns a lightweight
`CallRecordingController`. The ADM render observer and post-APM processor keep
that controller alive for the lifetime of the call. The controller contains an
atomic `RecordingSession *` capture target and an optional owning
`shared_ptr<RecordingSession>`.

Before the first Start, and while paused or inactive, the capture target is
null. Both realtime hooks do one atomic load and return. They do not copy PCM,
allocate, enqueue, encode, perform file I/O or JNI, log, or create a recording
directory. The first Start creates exactly one session, worker, directory,
writer set, and master timeline. The worker remains alive after Stop so a later
Start can continue the same Ogg streams.

### State machine

```text
IDLE --Start--> RECORDING --Pause--> PAUSED --Resume--> RECORDING
                       \--Stop------> INACTIVE
PAUSED ------------------Stop-------> INACTIVE
INACTIVE --Start--------------------> RECORDING

call teardown -> FINALIZED
fatal recorder failure -> FAILED
unpatched tgcalls version -> UNSUPPORTED
```

The first `State::Established` calls `Start` only when the call's global
auto-record snapshot is enabled. Later `Reconnecting -> Established`
transitions do not touch recording control state. A manual Start never changes
the global preference.

### Timeline zero and exact boundaries

The first manual or automatic Start captures `CurrentMonotonicTimeNs()` and
defines sample zero. Time spent in the call before that boundary is not part of
the recording. Pause, Resume, Stop, restart, and teardown use the same steady
clock as `AudioBlock`.

Control boundaries are converted through the existing absolute-nanoseconds to
48 kHz sample conversion. A bounded control-gate list is applied both when a
worker receives a PCM block and again when buffered PCM is committed. The
second check is essential for the race where a callback copied a block just
before Pause/Stop but the block crosses the newly published boundary. Only the
ungated prefix/suffix is retained; the gated range is silence in all enabled
outputs.

Realtime acceptance is disabled before publishing Pause/Stop. A callback that
already loaded the previous target may still enqueue, but commit-time trimming
makes it unable to leak PCM across the boundary. Resume/restart publishes the
same session as the capture target after its exact opening boundary is stored.

Each `AudioStream` also has an atomic capture-control token containing the
accepting bit and a control generation. Resume/restart increments that
generation while capture is disabled. A callback copies the token generation
into its queued `AudioBlock`; on the first block with a new generation the
worker resets its worker-owned `PushResampler` and forces a new continuity
segment at the block's absolute timestamp. Therefore even 20/30/40 ms control
gaps bypass the ordinary 50 ms jitter rule without lifecycle/UI code touching
worker-owned resampler or track state. The already-recorded `controlInterval`
is the only gap diagnostic for this transition: the forced block does not add
a duplicate `continuityGap` or `timestampDiscontinuity`.

### Pause versus Stop

Pause always advances the timeline. If a call ends while paused, the interval
from Pause through teardown is committed as silence and appears as a `pause`
control interval.

Stop makes the session dormant without finalizing it. It does not write Ogg
EOS, close a file, destroy an encoder, reset granule progression, or create a
new session. The controller stores a pending Stop boundary and the worker's
logical end remains frozen there.

If another Start occurs in the same call, the pending interval is closed at the
new Start. The worker then extends the absolute master timeline through the new
boundary, so the Stop-to-Start range becomes silence and PCM resumes in the
existing files and Ogg logical streams. Granule positions therefore represent
`audio + stopped silence + audio` without chaining.

If no Start follows the final Stop, teardown finalizes at the pending Stop
boundary rather than the call-end boundary. No trailing silence is written.
For example, `Start 01:00, Stop 02:00, call end 10:00` produces one minute, not
nine. In contrast, `Start 01:00, Stop 02:00, Start 05:00, call end 10:00`
includes the three-minute stopped gap and ends at ten minutes.

### Output modes

The global output mode defaults to `MIXED_AND_SEPARATE`:

```text
MIXED_AND_SEPARATE -> local.opus, remote.opus, mixed.opus, info.json
MIXED_ONLY         -> mixed.opus, info.json
SEPARATE_ONLY      -> local.opus, remote.opus, info.json
```

Manual Start passes the current preference to native code. Automatic Start
uses the preference captured in `CallConfiguration`. The chosen mode is stored
in the first `RecordingSession` and is immutable. A Start after Stop ignores a
newly passed mode because it reuses that same session and writer set.

The invariant is evaluated only for enabled writers:

```text
MIXED_AND_SEPARATE: local == remote == mixed == timeline
MIXED_ONLY:                           mixed == timeline
SEPARATE_ONLY:       local == remote          == timeline
```

### Java/JNI/UI flow

The Java API exposes `startCallRecording`, `pauseCallRecording`,
`resumeCallRecording`, and `stopCallRecording`. Native transitions push
`handleCallRecordingStateChanged(state, elapsedSamples, autoEnabled)` through
the existing safe JNI callback mechanism. `TGCallService` remains the bridge
and authoritative call owner; a recreated `CallController` immediately reads
the current state and native elapsed-sample value and then receives pushed
transitions.

The call screen displays the global auto-record snapshot separately from the
current-call status. `RECORDING` offers Pause and Stop; `PAUSED` offers Resume
and Stop; `IDLE` and `INACTIVE` offer Start/Continue; failures and unsupported
versions have no action button. The timer is the master-session duration. It
grows while recording or paused, freezes at an inactive Stop, and jumps forward
by a stopped interval when Start makes that interval part of the timeline.
`CallController.updateCallState()` refreshes it from the existing call-duration
loop, so no additional high-frequency timer is used. The Calls settings retain
`ConfirmCalls` and `ConfirmCallsDesc`; automatic recording and output selection
live in their own Call recording section with `CallRecordingSettingsDesc`.

Writer and capture failures remain per-output/per-side. A failed local writer
does not stop remote or mixed, a failed mixed writer does not stop either
separate writer, and a failed capture side is represented by silence while any
other useful enabled output continues. A fatal callback is reserved for an
internal timeline/session failure, an unexpected worker exception, directory
creation failure, or the point where no enabled output can contain useful
captured audio. Metadata continues to report `captureFailed`, `writerFailed`,
and `timelineInternalFailure` independently.

### Metadata and bounded diagnostics

`info.json` adds the immutable output mode, control origin/counts, first and
final monotonic boundaries, `pausedSamples`, `stoppedGapSamples`, final state,
and bounded `controlIntervals` with distinct `pause` and `stopped` types. At
most 256 interval records are serialized; additional records increment
`suppressedIntervals`. Operational gates are discarded after their ranges are
committed, so repeated Start/Stop cycles do not create unbounded worker state.
The existing total per-track `silenceSamplesWritten` still includes all silence
(control, missing source, reconnect, and queue gaps); the two control counters
are classifications and do not replace that total.

### Deterministic control regressions

Compile-time and worker-start checks cover:

- three recording fragments separated by five- and three-second Stop gaps,
  producing exactly 38 seconds in the same timeline;
- final Stop after ten seconds followed by a sixty-second live-call tail,
  producing exactly ten seconds;
- call teardown five seconds into Pause after ten seconds of audio, producing
  exactly fifteen seconds;
- first manual Start after five minutes of call time, producing no pre-start
  samples;
- PCM blocks crossing either edge of a control boundary, including a fully
  gated block and a block entirely outside the interval;
- Pause gaps of 20, 30, 40, and 2000 ms and Stop gaps of 20, 30, 40, and
  5000 ms, all anchored strictly after the control boundary without duplicate
  continuity/timestamp diagnostics;
- isolated local, remote, and mixed writer/capture failures, plus the fatal
  cases where no useful enabled output remains.

These checks prove arithmetic and boundary selection in the native binary.
Real-device audio routing, actual callback scheduling, encoded-file inspection,
and UI behavior still require the ARM64 runtime test plan below.

## Future Group Call Recording

Group calls are not implemented by Telegram X's Android bridge today. The
tgcalls group-call sources are compiled by `BuildTgCalls.cmake`, but searches
outside the vendored tgcalls sources find no Java/JNI construction of
`GroupInstanceCustomImpl`, `GroupInstanceReferenceImpl`, or
`GroupInstanceDescriptor`. The findings below describe the available native
tgcalls architecture to preserve for future integration, not an active
`TGCallService` group-call path.

### Does a group call use the same ADM?

`GroupInstanceCustomImpl` uses the same architecture and, on Android, the same
factory:

```text
GroupInstanceCustomInternal::createAudioDeviceModule()
  -> Descriptor.createWrappedAudioDeviceModule, if supplied
  -> Descriptor.createAudioDeviceModule, if supplied
  -> CreateAndroidAudioDeviceModule(kPlatformDefaultAudio)
  -> CreateAudioDeviceWithDataObserver(...)
  -> PlatformInterface::wrapAudioDeviceModule(...)
```

It owns one ADM per group instance, shared by its local capture and final mixed
playout. Therefore the same recorder ADM observer can provide `local raw`
(diagnostic only) and `remote:mixed`.

`GroupInstanceReferenceImpl` has the same descriptor factory fields and prefers
`createWrappedAudioDeviceModule`/`createAudioDeviceModule`. Its hardcoded final
fallback currently calls generic `AudioDeviceModule::Create()` rather than the
Android-specific factory, so future Android integration should always inject
the known-good wrapped Android ADM instead of relying on that fallback.

### Where multiple remote streams are mixed

Each active WebRTC receive stream is registered as an
`AudioMixer::Source`. On every `AudioTransportImpl::NeedMorePlayData()` pull:

1. `AudioMixerImpl::Mix()` computes an output rate from active sources.
2. It calls `ChannelReceive::GetAudioFrameWithInfo()` for every source.
3. Each channel pulls a 10 ms decoded frame from ACM/NetEq.
4. `FrameCombiner::Combine()` mixes the sources.
5. `NeedMorePlayData()` runs reverse APM and resamples the mixed result to the
   ADM rate.
6. ADM `OnRenderData()` sees the final `remote:mixed` block.

The mixer implementation is
`webrtc/modules/audio_mixer/audio_mixer_impl.cc`, class
`webrtc::AudioMixerImpl`, method `Mix()`.

### Are separate decoded participant streams available before the mixer?

Yes.

`GroupInstanceCustomImpl.cpp` creates one `IncomingAudioChannel` per
`ChannelId`/remote audio SSRC. In its constructor it installs
`AudioSinkImpl` using:

```text
receive_channel()->SetRawAudioSink(networkSsrc, sink)
```

WebRTC invokes that sink in
`ChannelReceive::GetAudioFrameWithInfo()` immediately after `AcmReceiver`/NetEq
produces decoded PCM and before per-stream output gain and the mixer. The
current `AudioSinkImpl::OnData()` already adapts the WebRTC block to
`tgcalls::AudioFrame` and calls the descriptor's existing
`GroupInstanceDescriptor.onAudioFrame(actualSsrc, frame)`.

That existing callback provides:

- stream key: `actualSsrc`;
- sample rate from the decoded block (Opus path is 48 kHz);
- channel count (Opus defaults to mono without `stereo=1`);
- signed PCM16 data;
- 10 ms / 480 frames per channel at 48 kHz.

It is already the natural future hook for
`remote participant A/B/C`, and it requires no ADM or WebRTC modification.
`GroupInstanceReferenceImpl` similarly creates a distinct remote audio
transceiver/track per discovered SSRC and already attaches one
`GRAudioLevelSink` (`webrtc::AudioTrackSinkInterface`) per track. Extending that
sink to forward PCM provides equivalent per-SSRC blocks.

Both per-participant sink variants execute on WebRTC's playout/audio path and
must use the same copy/enqueue-only rules as the ADM render callback.

### Can PCM be mapped to participant/user/SSRC?

SSRC mapping is available; user mapping is available in the custom group path
when control-plane descriptions have arrived:

- `GroupInstanceCustomInternal::addIncomingAudioChannel(ChannelId ssrc,
  int64_t userId, ...)` receives both values.
- `MediaChannelDescription` contains `audioSsrc` and `userId`.
- `VideoChannelDescription` additionally contains `endpointId`.
- `_incomingAudioChannels` is keyed by `ChannelId`; `_channelBySsrc` tracks
  network SSRC routing.
- The current `onAudioFrame` callback exports only `actualSsrc`, not `userId`.
  Future integration should register/update the recorder stream on the media
  thread with `{streamId, ssrc, optional userId}` when
  `addIncomingAudioChannel()` is called. The realtime sink then keeps a direct
  `AudioStream` handle.

For `GroupInstanceReferenceImpl`, `_remoteSsrcs` maps discovered SSRCs to
individual transceivers and sinks. It calls
`requestMediaChannelDescriptions({ssrc}, ...)`, but the current reference
implementation discards the returned descriptions in that callback, so it does
not yet retain `userId`. Future recorder integration must persist the returned
`MediaChannelDescription` mapping before claiming participant identity.

An SSRC is a transport stream identifier, not a permanent user identifier. It
can be replaced/reused as participants reconnect. Therefore:

- use recorder-owned `streamId` as the stable file/manifest key;
- store SSRC as time-scoped metadata;
- keep `participantId` optional until verified from control-plane data;
- rotate or update mapping events when SSRC/user association changes;
- never guess a user from audio level, endpoint order, or sink index.

### Future group-call stream plan

When Android group calls are actually bridged, reuse the same
`RecordingSession` core:

- local processed: the same identity `AudioFrameProcessor` installed in the
  group PeerConnection/media dependencies;
- remote participant: existing per-SSRC `onAudioFrame`/track sink, one
  preallocated `AudioStream` per receive channel;
- mixed remote: the same ADM `OnRenderData()` observer, labelled
  `role=MixedRemote` rather than a participant;
- mapping: register `{SSRC, userId?, endpointId?}` on the media/control thread
  before enabling the sink.

Group recording is intentionally not part of the 1:1 implementation stage.

## Risks and required safeguards

### Realtime and call-quality risks

- Any allocation, lock contention, file I/O, conversion, JNI, or logging in a
  PCM callback can cause underruns or outgoing gaps. Preallocate and copy only.
- `AudioFrameProcessor` adds WebRTC's asynchronous processing queue to the send
  path. The identity processor must return the frame immediately; stress-test
  latency and continuity on ARM64.
- A full ring must drop recording data, never delay WebRTC.
- OpenSL ES and Java ADM use different callback threads. Code must not depend on
  Java thread names or thread affinity beyond one producer per stream.

### Signal-semantics risks

- ADM `OnCaptureData()` is raw and must not be presented as post-AEC local.
- ADM `OnRenderData()` is mixed/final playout. In a group call it cannot provide
  participant separation.
- Per-stream `RawAudioSink` is before receive gain/mixing, so it may differ from
  exactly what the user hears. Store its role explicitly.
- Local hook placement must remain after all normal APM. A custom APM
  postprocessor is close but not identical to encoder input in this WebRTC
  revision.
- Mute behavior must be tested. The recorded local stream should follow the
  product decision (normally transmitted/active audio); it must not
  accidentally retain microphone speech while the user expects mute.

### Format and synchronization risks

- Android output sample rate can change with route/device behavior; never
  hardcode remote ADM output to 48 kHz.
- Local and remote callbacks use different clocks/threads. Preserve monotonic
  timestamps and sequence numbers; do not assume callback arrival order equals
  media time.
- NetEq can generate PLC/concealment audio. Remote rendered recording should
  intentionally include it because the listener heard it.
- WAV has one fixed format per file. Normalize on the writer or rotate files on
  a format change; do neither in callbacks.
- Queue drops need counters and timeline metadata so later analysis does not
  silently interpret gaps as silence.

### Lifecycle and compatibility risks

- Recorder ownership must follow `TgCallsContext`, not a process global, so
  teardown cannot leak into a later call.
- `Instance::stop()` is asynchronous. Destroying hooks or queues before its
  completion risks use-after-free.
- Recorder stop/finalization must be bounded and idempotent and must never
  suppress the existing Java `handleStop()` callback.
- All negotiated WebRTC-backed versions need the processor factory wired.
  Version 2.4.4 is a distinct libtgvoip path.
- Submodule updates can move hook ordering. Re-verify
  `AudioTransportImpl::RecordedDataIsAvailable()`, `AsyncAudioProcessing`, and
  ADM observer order whenever tgcalls/WebRTC revisions change.

## Validation plan for the implementation milestone

After implementation, prove the encoded output before adding UI:

1. Build `latestArm64Debug` after each logical stage.
2. Make a real 1:1 call on an ARM64 device and record two independent streams.
3. Verify `local.opus` is standard 48 kHz mono Ogg Opus and contains
   APM-processed, not raw microphone, audio.
4. Verify remote callback metadata against the active Android output rate and
   confirm it contains the decoded voice/PLC heard on the device.
5. Test mute/unmute, speaker/earpiece/Bluetooth route changes, reconnect, remote
   silence, packet loss, and normal/failed hangup.
6. Artificially stall/fail the writer and fill rings; the call must remain
   intelligible and connected while dropped/error counters increase.
7. Confirm teardown under sanitizers/log checks has no callback-after-free and
   that each Ogg stream has a valid EOS/granule position after callbacks stop.
8. Verify `mixed.opus` is standard 48 kHz mono Ogg Opus, preserves a delayed
   start on either side, and does not drift during ordinary callback jitter.
9. Verify one-sided calls, unequal endings, an injected per-side queue drop,
   and isolated local/remote/mixed writer failures.

## Milestone 6 — metadata, browser, and playback

### Immutable call snapshot and schema v1

`VoIP.instantiateAndConnect()` snapshots only the peer fields required by the
recordings UI: `callId`, `userId`, the current display name, and call
direction. `CallConfiguration` carries that immutable snapshot through JNI as
part of recorder construction. Native code never queries TDLib and no phone,
username, biography, avatar, contact list, authorization data, or API
credentials are persisted.

New `info.json` files begin with `schemaVersion: 1`. Their `sessionId` is the
session directory basename; the display name is never part of the internal
path. The canonical UI fields are:

```json
{
  "schemaVersion": 1,
  "sessionId": "2026-09-12_16-48-01_1",
  "call": {
    "callId": 1,
    "userId": 123456789,
    "displayName": "John Doe",
    "isOutgoing": true
  },
  "recordingStartTime": "2026-09-12T13:48:01.135Z",
  "recordingEndTime": "2026-09-12T13:48:33.011Z",
  "callEndTime": "2026-09-12T13:48:42.877Z",
  "timelineSampleRate": 48000
}
```

`startTime` and `stopTime` remain aliases for old tooling. At first Start the
controller captures the wall-clock time adjacent to the monotonic boundary.
The recording end wall time is derived from that pair and the master timeline:

```text
recordingStartWallTimeMs
  + (sessionEndMonotonicNs - sessionStartMonotonicNs) / 1,000,000
```

Consequently a final Stop freezes `recordingEndTime`, while `callEndTime` is
captured later at call teardown. A wall-clock change during the call cannot
alter recording duration. Native JSON escaping handles quotes, backslashes,
the short control escapes, every other U+0000..U+001F byte, and preserves
UTF-8 names and emoji.

### Repository and compatibility

`CallRecordingRepository` is the only layer that scans `files/calls`, parses
metadata, checks actual outputs, computes sizes/status, sorts, deletes, and
prepares exports. Scan and parsing run through TGX `Background`; an immutable
list is posted to the UI thread. Audio files are only stat-ed during a scan and
are never loaded into memory. The parser has a 2 MiB metadata bound and isolates
each directory so one malformed session cannot hide healthy siblings.

The immutable `CallRecordingItem` contains identity, peer snapshot, canonical
times, duration, output mode, actual track availability/sizes, status, and
pause/stopped intervals. Missing `schemaVersion` is accepted for v6/v7/v7.1.
Legacy fallback uses the directory basename, unknown peer/direction, then
`startTime`, directory time, and filesystem time. A missing or malformed
`info.json` still produces an `INCOMPLETE` row and any real non-empty Opus file
remains playable/exportable.

Status is derived as follows:

- `IN_PROGRESS`: `.in_progress` belongs to the current process;
- `FAILED`: the final control state is fatal or the timeline reports an
  internal failure;
- `COMPLETED`: metadata is finalized and all outputs required by its mode
  exist without an enabled writer failure;
- `INCOMPLETE`: every other recoverable state, including missing metadata or
  expected output.

Actual fixed files (`mixed.opus`, `local.opus`, `remote.opus`) always determine
button visibility. Filenames from JSON are descriptive and are never resolved.
Every session must canonicalize to a direct child of `files/calls`, and every
known file must canonicalize directly inside that session.

### Browser, details, and local playback

Settings keeps the existing `ConfirmCalls`/`ConfirmCallsDesc` block and adds a
separate Call recording section containing automatic recording, output files,
and the `Call recordings` entry. The recordings controller asynchronously
loads newest-first rows. A cached current TDLib user supplies the latest name
and avatar; otherwise the immutable name snapshot and a placeholder are used.

Details show peer, direction, start time, duration, total size, status, actual
track buttons, and merged chronological `controlIntervals`. Zero-length final
Stop markers are filtered by the repository. Interval positions retain tenths
of a second and use `timelineSampleRate` from metadata. The parser may infer it
from consistent stream output rates and falls back to 48 kHz for legacy files.

Playback uses one lightweight Android `MediaPlayer` for local Ogg Opus. Track
selection releases the previous player, details destruction releases the
current player, and audio focus is requested and abandoned explicitly. The
screen refuses to start playback while a Telegram call service is active and
the progress loop stops playback if a call appears. Open/in-progress Ogg files
cannot be played.

## Milestone 7 — export, sharing, SAF save, and delete

Details exposes Share, Save to…, and Delete recording. Share and Save offer
only tracks that really exist, plus All files. Single-track export copies the
original Ogg Opus unchanged and uses `audio/ogg`; no transcoding is performed.
All files uses `ZipOutputStream` and includes only existing non-empty tracks
plus `info.json` when present.

All copies are streaming with a fixed 32 KiB buffer. Share artifacts live only
under a unique `cache/call_recording_exports/export_<time>_<pid>_<counter>/`
directory. Preparing another export never deletes or overwrites a fresh prior
artifact, so a Sharesheet recipient may open its granted URI later. Before an
export, cleanup considers only canonical, direct-child directories matching
that recorder-owned ID format and removes entries older than 24 hours. It does
not follow symlinks, touch unowned entries, or delete the current export, which
is created after cleanup. Failed exports remove only their own partial
directory. The friendly name is sanitized code point by code point, replaces
filesystem separators, reserved punctuation, whitespace, and controls, limits
names to 64 input code points, and falls back to `Unknown`.

Sharing resolves the cache artifact through the existing TGX `FileProvider`,
requires a `content://` URI, supplies `ClipData`, and grants temporary read
permission. Saving uses `ACTION_CREATE_DOCUMENT` and streams directly to the
user-selected SAF `Uri`; no legacy storage permission or arbitrary Downloads
path is used. Neither action uploads anything automatically.

Delete requires confirmation and recursively removes exactly one canonical
direct child of `files/calls`. A missing directory, symlink/path escape, or
partial deletion is failure; external targets are left untouched. Active
recordings reject playback, Share, Save, and Delete. A delete failure returns
to the asynchronously refreshed list so surviving files remain visible.

### Deterministic M6/M7 regression coverage

`CallRecordingRepositoryTest` covers schema v1, v7.1-style metadata without a
schema, missing/malformed metadata, missing expected output, isolated writer
failure, fatal status, zero-length control intervals, real-files-win behavior,
newest-first order, and a 1,000-directory scan. Export tests cover each single
track, full/mixed-only/separate-only ZIP contents, legacy/incomplete/failed
sessions with surviving output, missing output, and a multi-megabyte streaming
copy. Export-lifetime tests cover A→B with A unchanged, three retained
Mixed/Local/ZIP artifacts, parallel exports with unique directories, and
24-hour stale cleanup that preserves fresh/current/unowned/symlink entries.
Security tests cover active export/delete, normal and missing deletion,
partial deletion through a symlink, and `../` traversal. Filename tests cover
ASCII, Cyrillic/Ukrainian, quotes, separators/reserved punctuation, emoji,
empty input, and length limiting. Native recorder regressions additionally
cover JSON escaping and monotonic recording-end conversion.

These deterministic tests do not replace device validation. The browser UI,
Android codec support, audio focus, Sharesheet recipient grants, SAF providers,
and physical deletion behavior must still be exercised on the target ARM64
device with both new schema-v1 and old v7.1 recordings.
