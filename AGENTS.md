# Telegram X Recorder

## Environment

- Ubuntu 24.04 under WSL2
- VS Code connected to WSL
- Target device: Android ARM64
- Development build: latestArm64Debug

## Build

After code changes always run:

./gradlew assembleLatestArm64Debug --console=plain

Task is complete only when build finishes with:

BUILD SUCCESSFUL

APK location:

app/build/outputs/apk/latestArm64/debug/

Find APK:

find app/build/outputs -type f -name "*.apk" -print

## Git

Work only in branch:

call-recording-dev

Do not modify main or call-recording unless explicitly requested.

Do not commit:
- local.properties
- Telegram api_id
- Telegram api_hash
- passwords
- private keys

Do not commit build.log.

## Call recording development

Goal: record Telegram 1:1 audio calls using internal digital PCM streams.

Requirements:

- Investigate the current TGCallService -> VoIP -> TgCallsController -> JNI -> tgcalls/WebRTC path before modifying it.
- Do not guess locations of PCM streams.
- Prefer minimal changes to Telegram X and tgcalls/WebRTC.
- First milestone is separate local and remote PCM capture.
- Do not implement UI, mixing or Opus until PCM capture is proven working.
- Never perform file I/O directly in realtime audio callbacks.
- Audio callback must only copy/enqueue data.
- Use a bounded queue/ring buffer and separate worker for file operations.
- Recording failure must never break the call.
- Preserve existing Telegram calling behavior.
- Keep changes as small and isolated as possible.
- Document discovered architecture.
- Build after every logical implementation stage.
