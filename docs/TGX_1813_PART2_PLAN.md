# TGX 1813 Recorder — PART 2 plan

## Fixed comparison points

- Old upstream: `67501370426da0bce4774d96cc0df6591cde55b0`.
- Recorder v1.0.1: `adebc69b2c0291a6f58bac4256cfcefff17edc9b`.
- New upstream: `9312ace35291f68fdd66ddff348d7321bee2a4bd`.
- Compatible tgcalls: `8010b9b7d85eeff024a21869826c7b8e1d2906b0`.
- PART 2 must port only the Recorder delta from old upstream to new upstream.

## `tgvoip.cpp` migration map

| Recorder feature | Old Recorder location | 1813 location / context | Action in PART 2 |
|---|---|---|---|
| Recorder includes and logging | Lines 18–20 and anonymous namespace near 110 | Includes begin near line 16; no Recorder includes | Add only `CallRecorder.h`, adapter, Android logging, and required standard headers. Do not restore libtgvoip/JNIUtilities includes. |
| Controller ownership | `TgCallsContext`, lines 432–438 | `TgCallsContext`, line 416 | Add shared controller ownership and duplicate-stop guard beside existing tgcalls/controller members. |
| Configuration parsing | `newInstance`, lines 469–539 | `newInstance`, line 433; base parsing precedes endpoint setup | Read call ID, Recorder metadata/path/settings after normal call configuration. Keep 1813 parsing unchanged. |
| `supportsCallRecording` | Lines 440–444 | No equivalent; available tgcalls versions come from current `Meta` registrations | Revalidate the allowlist against 1813 versions; never re-add legacy `2.4.4`. |
| `EvaluateRecorderIntegrationSupport` | Lines 526–531 | No equivalent | Evaluate tgcalls version plus hook capabilities before constructing/wiring recording. Failure must disable recording, not calls. |
| Controller creation | Lines 654–684 | After `javaController` creation and before descriptor at line 593 | Create with metadata, auto-record setting, output mode, and Java state callback. Keep ownership local until context creation succeeds. |
| Java recording-state callback | Lines 666–683 | Existing `JniWrapper::runSafely` callback pattern | Add only the Recorder callback/signature; preserve 1813 JNI attachment/lifecycle behavior. |
| Descriptor creation | Descriptor begins line 690 | Descriptor begins line 593 | Extend the 1813 descriptor in place; do not copy the old descriptor wholesale. |
| `stateUpdated` | Lines 724–738 | Lines 627–632 | Capture controller, call `onEstablished()` once state is established, then preserve existing Java state delivery. |
| `ConfigureRecorderTgCallsHooks` | Lines 767–770 | Between descriptor construction and proxy/persistent-state processing | Wire hooks only when integration is supported and the controller exists. |
| `Meta::Create` and context | Lines 790–800 | Lines 679–686 | Store the controller before `Meta::Create`, retain current version/descriptor flow, notify initial recording state after successful setup. |
| Start/Pause/Resume/Stop JNI | Lines 891–917 | Insert near other `TgCallsController` JNI operations before destroy | Add thin null-safe controller calls; no file I/O or recorder work in JNI glue. |
| Elapsed-samples JNI | Lines 919–925 | Same JNI operations area | Add the null-safe query only if still required by the Java API. |
| Teardown | `destroyInstance`, lines 928–959 | `destroyInstance`, line 775 | Guard duplicate stop, call `beginFinishCall()` before tgcalls stop, and `finishCall()` before context deletion/stop completion handling. Preserve 1813 final-state persistence and Java `handleStop`. |

### Upstream 1813 invariants for `tgvoip.cpp`

- Keep `InstanceImplLegacy` registration and include removed.
- Do not restore libtgvoip JNI, `tgvoipRegisterNatives`, or old `os/android/JNIUtilities.h` plumbing.
- Keep the 1813 `sharedJVM`, `DoWithJNI`, `JNI_OnLoad`, and thread attach/detach lifecycle.
- Keep the current `InstanceImpl`, `InstanceV2Impl`, and `InstanceV2ReferenceImpl` registrations.
- Keep the 1813 descriptor fields, proxy handling, persistent-state flow, and `Meta::Create(version, std::move(descriptor))` ordering.
- Keep stop callback handling, stats/debug-log delivery, Java reference release, and context deletion intact.
- Do not revive any `DISABLE_TGCALLS`/VoIPController build fallback removed from the active 1813 build.

Risk: **HIGH**. The Recorder patch overlaps `newInstance`, descriptor callbacks, and asynchronous teardown. Port semantic units individually; do not apply the old file patch wholesale.

## `VoIP.java` minimal delta

Source of truth is Recorder v1.0.1 lines 363–406, applied to the 1813 `instantiateAndConnect` flow around lines 359–426.

- Add the recording base-path helper using the application files directory, with nullable failure behavior.
- Resolve the peer with `tdlib.cache().user(call.userId)`.
- Derive `displayName` from that user, defaulting to an empty string.
- Pass `call.id` as `callId` and `call.userId` as `recordingUserId`.
- Pass `recordingBasePath`, `Settings.instance().isAutoRecordingCallsEnabled()`, and `Settings.instance().getCallRecordingOutputMode()`.
- Extend only the 1813 `CallConfiguration` call; preserve all existing argument order outside the new Recorder fields.

### Upstream 1813 invariants for `VoIP.java`

- Keep the tgcalls-only selection path; never restore `VoIPController` or libtgvoip fallback selection.
- Keep `BuildConfig.CALLS_AVAILABLE` guards around native version discovery and call-server behavior.
- Keep `Flavor.initializeWebRTC(context)`.
- Keep the 1813 `getAvailableVersions` behavior and restricted-version filtering.
- Keep `CONNECTION_MIN_LAYER = 65` and `CONNECTION_MAX_LAYER = 92` as the protocol bounds.
- Keep `new TgCallsController(..., version)` and current failure cleanup.

Risk: **HIGH** for wholesale patching, **MEDIUM** for the isolated metadata/configuration delta. Imports and constructor order must follow the 1813 file, not the old Recorder file.

## Native CMake delta

File: `app/jni/tgvoip/CMakeLists.txt`.

- Add `CallRecorder.cpp` and `recorder/RecorderTgCallsAdapter.cpp` to `TGCALLS_LIB` sources.
- Add `opus` and `ogg` to the existing `target_link_libraries(${TGCALLS_LIB} ...)` block.
- Preserve 1813 WebRTC/tgcalls sources, include paths, compile definitions, and multi-config linker options.
- Do not restore `BuildTelegramVoIP.cmake`; 1813 deliberately removed it.

Risk: **MEDIUM**. The textual change is small, but source availability and target names must match the parent native build graph.

## Ordered PART 2 execution

### 1. `CallConfiguration`

- Source of truth: Recorder v1.0.1 `CallConfiguration.java`, compared with the 1813 class.
- Files: `app/src/main/java/org/thunderdog/challegram/voip/CallConfiguration.java` and the existing output-mode annotation only if compilation requires it.
- Expected change: add `callId`, `recordingUserId`, `recordingDisplayName`, nullable `recordingBasePath`, `autoRecordingEnabled`, and `recordingOutputMode` fields, constructor arguments, and assignments.
- Danger/invariant: retain all 1813 fields and constructor ordering; metadata remains immutable and nullable annotations remain accurate.

### 2. `VoIP.java`

- Source of truth: the minimal delta above, not the complete old file.
- Files: `app/src/main/java/org/thunderdog/challegram/voip/VoIP.java`.
- Expected change: compute Recorder metadata/settings and pass them into `CallConfiguration`.
- Danger/invariant: preserve tgcalls-only selection, `CALLS_AVAILABLE`, WebRTC initialization, protocol layers, and cleanup.

### 3. Native CMake

- Source of truth: Recorder v1.0.1 source/link additions on top of the 1813 CMake file.
- Files: `app/jni/tgvoip/CMakeLists.txt`.
- Expected change: two Recorder sources plus `opus` and `ogg` links.
- Danger/invariant: no `BuildTelegramVoIP.cmake`, libtgvoip target, or old linker configuration.

### 4. `tgvoip.cpp`

- Source of truth: the migration table above and compatible tgcalls descriptor hook.
- Files: `app/jni/tgvoip/tgvoip.cpp` plus already-migrated Recorder native sources.
- Expected change: metadata parsing, controller lifecycle, descriptor hook, Java state callback, JNI controls, and ordered teardown.
- Danger/invariant: preserve 1813 JNI lifecycle and descriptor/stop behavior; recording failure must never break calls; no realtime file I/O.

### 5. `TgCallsController` JNI/API adaptation

- Source of truth: native JNI entry points and Recorder v1.0.1 Java declarations, adapted to the 1813 controller API.
- Files: `app/src/main/java/org/thunderdog/challegram/voip/TgCallsController.java` and related interface only when required.
- Expected change: expose recording controls/state callback and keep signatures exactly aligned with JNI.
- Danger/invariant: do not broaden API changes or disturb normal call state/stop delivery.

### 6. Compile

- Source of truth: compiler diagnostics after the logical migration stages are committed or otherwise isolated.
- Files: no speculative edits before diagnostics.
- Expected change: run the required debug compilation once source wiring is complete.
- Danger/invariant: first distinguish missing migrated files from genuine 1813 API incompatibilities.

### 7. Fix compile errors

- Source of truth: exact first-party compiler/linker errors.
- Files: only files directly implicated by errors.
- Expected change: minimal API, include, type, or JNI-signature adaptations.
- Danger/invariant: no legacy restoration, broad refactor, UI expansion, mixing, or Opus feature work.

### 8. Debug build

- Source of truth: repository build contract.
- Files: none beyond verified fixes.
- Expected change: run `assembleLatestArm64Debug`, require `BUILD SUCCESSFUL`, and locate the generated APK.
- Danger/invariant: preserve calling behavior and report failures without hiding them; do not create a release build.

## PART 2 implementation report

- Base: Telegram X `9312ace35291f68fdd66ddff348d7321bee2a4bd` (`0.29.0.1813`).
- Native dependencies: tgcalls `8010b9b7d85eeff024a21869826c7b8e1d2906b0`; WebRTC `6ecff4f2446ff7d4ce38ca1c764f023e44dbcb1b` (unchanged).
- Added unchanged Recorder v1.0.1 sources: `CallRecorder.cpp/.h` and `recorder/RecorderTgCallsAdapter.cpp/.h`.
- Adapted 1813 files: native CMake, `tgvoip.cpp`, `CallConfiguration`, `VoIP`, `TgCallsController`, `VoIPInstance`, `ConnectionStateListener`, and the minimal Recorder settings storage API. Added the two small recording annotations.
- CMake adds the two Recorder translation units and links `opus`/`ogg`, while retaining the 1813 multi-config linker flags and 16 KB alignment. No legacy libtgvoip target or build script was restored.
- `tgvoip.cpp` retains 1813 `sharedJVM`, `DoWithJNI`, registrations, descriptor construction, and `Meta::Create` flow. Recorder metadata/controller ownership, hook wiring, state callback, and JNI controls were added in place.
- Teardown calls `beginFinishCall()` before `tgcalls::stop`; the compatibility tgcalls stop path destroys its internal callback owners via `ThreadLocalObject::reset`, then invokes completion; completion calls `finishCall()` before Java stop delivery and context destruction.
- Recording versions remain exactly `7.0.0`, `8.0.0`, `9.0.0`, `12.0.0`, and `13.0.0`.
- Recorder Firebase configuration was restored from v1.0.1 because the 1813 upstream client does not match the Recorder application ID. No release version/name was changed.
- First build error: Google Services had no client for the Recorder application ID. Replacing only the tracked configuration with the exact Recorder v1.0.1 version resolved it.
- Final command: `./gradlew assembleLatestArm64Debug --console=plain` — `BUILD SUCCESSFUL`.
- APK: `app/build/outputs/apk/latestArm64/debug/Telegram-X-Recorder-0.29.0.1813-arm64-v8a-debug.apk`.
- No matching `ApplicationIdentityTest`, `CallForegroundStateMachineTest`, or Recorder foundation unit tests are present in the PART 2 tree, so no separate unit-test task was run. Runtime validation was not performed.
- PART 3 remains responsible for call controls/UI, settings UI, recording repository/browser/details, playback, and share/save/ZIP flows.

## PART 3 implementation report

- Base: completed PART 2 at `2f9860cf02dc587313efd27b1c64fe79d107af31` on `upgrade/tgx-0.29.0.1813`. The PART 2 native recorder foundation and exact supported-version set (`7.0.0`, `8.0.0`, `9.0.0`, `12.0.0`, `13.0.0`) were not changed.
- Modified call integration: `TGCallService`, `CallManager`, `CallController`, `AndroidManifest.xml`, and the minimal call-control resources. Added `CallForegroundStateMachine` and its focused unit test.
- Modified settings integration: `SettingsThemeController` plus the minimal IDs and strings for automatic recording, output mode, recording status, and Start/Pause/Resume/Stop controls. No recordings-list entry or PART 4 UI was added.
- State propagation: the native `VoIPInstance` remains authoritative. `TGCallService` delegates controls to the existing instance, posts native state callbacks onto the UI thread, and gives a newly attached listener an immediate native state/elapsed-time snapshot. Listener removal and replacement are idempotent.
- Controls: `IDLE` shows Start; `RECORDING` shows Pause and Stop; `PAUSED` shows Resume and Stop; `INACTIVE` shows Continue, which resumes the same native call recording session. `FAILED`, `UNSUPPORTED`, and `FINALIZED` expose no recording controls and do not affect the call.
- Timer: `CallController` reuses the existing periodic call UI update path, reads authoritative native `elapsedSamples`, and displays `elapsedSamples / 48000`. No separate timer, thread, or handler was added. Pause time and a resumed Stop-to-Start gap follow the native master timeline; the stopped value remains frozen until continuation.
- Settings storage remains `calls_auto_record` (default `false`) and `calls_recording_output` (default `MIXED_AND_SEPARATE`). Available output modes are `MIXED_AND_SEPARATE`, `MIXED_ONLY`, and `SEPARATE_ONLY`. Native code snapshots the output mode on the first Start, so later preference changes do not mutate the active session.
- Activity recreation/background reattaches to the same service and receives an immediate current snapshot; reconnect keeps the same `VoIPInstance`/recorder controller. Neither path creates another recording session or resets the timer, and auto-start remains guarded by the native first-established/session state.
- FGS decision: retained the production Recorder `CallForegroundStateMachine` integration because Telegram X 1813 still had no equivalent transition guard. Incoming ringing uses `SHORT_SERVICE`; answering promotes to `MICROPHONE` only after the required permission/service checks. This preserves the 1813 call implementation while covering the Android 14+ transition.
- Tests: the first focused test compilation exposed the repository's missing JUnit dependency. Added only `testImplementation("junit:junit:4.13.2")`; `CallForegroundStateMachineTest` then passed with `testLatestArm64DebugUnitTest`.
- Final build: `./gradlew assembleLatestArm64Debug --console=plain` — `BUILD SUCCESSFUL` (301 actionable tasks).
- APK: `app/build/outputs/apk/latestArm64/debug/Telegram-X-Recorder-0.29.0.1813-arm64-v8a-debug.apk`.
- Remaining PART 4: repository/browser/details integration, playback, share/save/ZIP/delete, recordings list/settings entry, and crash-recovery UI. None was started in PART 3. Runtime validation was not performed.
