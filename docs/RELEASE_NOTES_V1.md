# Telegram X Recorder v1.0

Telegram X Recorder v1.0 is the first production release of the recorder build
for one-to-one Telegram audio calls.

## Included

- Manual and automatic recording of one-to-one audio calls.
- Mixed output plus separate local and remote Opus tracks.
- Start, pause, resume, stop, and restart controls.
- Recording browser and playback.
- Share, save, ZIP export, and delete actions.
- Recovery of interrupted recordings after a process crash or termination.
- Android 15 incoming calls while the app is in the background or the screen
  is locked, including the required foreground-service transition after
  answering.
- ARM64-v8a production build.

## Version identity

The Android package keeps the Telegram X compatible version scheme. This
release uses the separate recorder identity `TGXR 1.0` / release version
`1`. Preserving the upstream Android versionCode ordering allows a future
production release to update this installation normally.

## Known limitations

- Group calls are not supported.
- Voice chats are not supported.
- Automatic storage retention (M9) is not implemented.
- Additional privacy/security features planned as M10 are not implemented.
