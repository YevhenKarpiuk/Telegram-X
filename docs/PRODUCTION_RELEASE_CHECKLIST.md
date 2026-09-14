# Telegram X Recorder production release checklist

This checklist is reusable for production releases of `ka.soft.tgxr`. A
successful static build does not replace runtime validation on the target
device.

## 1. Source and version baseline

- [ ] Work on `call-recording-dev`; do not commit or push before review.
- [ ] Record `git status`, `git log -5 --oneline`, and
      `git submodule status`.
- [ ] Confirm tgcalls is clean and at the approved production commit.
- [ ] Confirm dependencies and submodule pointers were not updated.
- [ ] Confirm the upstream Telegram X Android versionCode/versionName still
      preserve update ordering.
- [ ] Set the independent `TGXR_RELEASE_VERSION` and
      `TGXR_RELEASE_NAME` constants for the recorder release.

## 2. Production signing

The release key and all passwords must remain outside this repository.
Production release builds use an external properties file referenced by the
ignored root `local.properties`.

If the v1 key does not exist, create it interactively so the owner chooses and
records the passwords:

```bash
mkdir -p /home/ka/private
chmod 700 /home/ka/private
keytool -genkeypair -v \
  -keystore /home/ka/private/tgxr-release.jks \
  -alias tgxr-release \
  -keyalg RSA -keysize 4096 -validity 10000
```

Create `/home/ka/private/tgxr-signing.properties`, set mode `600`, and add:

```properties
keystore.file=/home/ka/private/tgxr-release.jks
keystore.password=CHOOSE_AND_STORE_SECURELY
key.alias=tgxr-release
key.password=CHOOSE_AND_STORE_SECURELY
```

Then add this non-secret pointer to the ignored `local.properties`:

```properties
keystore.file=/home/ka/private/tgxr-signing.properties
```

- [ ] The release key is not the Android debug key.
- [ ] No keystore, signing properties, or password is tracked or staged.
- [ ] Back up the keystore and its credentials in at least two independent,
      secure locations.

> Losing the production signing key prevents normal updates of the same
> `ka.soft.tgxr` application.

## 3. Static tests and builds

```bash
./gradlew :app:testLatestArm64DebugUnitTest \
  --tests org.thunderdog.challegram.voip.recording.CallRecordingRepositoryTest \
  --tests org.thunderdog.challegram.service.CallForegroundStateMachineTest \
  --tests org.thunderdog.challegram.ApplicationIdentityTest \
  --rerun-tasks --console=plain

./gradlew assembleLatestArm64Debug --console=plain
./gradlew assembleLatestArm64Release --console=plain
```

- [ ] All selected unit tests pass.
- [ ] M8 repository/crash-recovery cases pass.
- [ ] M11 native adapter compile-time regression checks pass during native
      compilation.
- [ ] Debug regression build finishes with `BUILD SUCCESSFUL`.
- [ ] Release build finishes with `BUILD SUCCESSFUL`.
- [ ] Release is signed; an unsigned APK is never published.
- [ ] Run `./gradlew clean`, rebuild release, and confirm
      `BUILD SUCCESSFUL`.
- [ ] Optionally run `./gradlew bundleLatestArm64Release`.

## 4. Firebase, identity, and artifact inspection

- [ ] `processLatestArm64ReleaseGoogleServices` ran successfully.
- [ ] Generated `google_app_id`, `gcm_defaultSenderId`, and `project_id`
      resources exist without printing their values.
- [ ] Package is `ka.soft.tgxr`; label is `Telegram X Recorder`.
- [ ] APK contains only `arm64-v8a`.
- [ ] Manifest does not set `android:debuggable=true` or `testOnly=true`.
- [ ] No `.debug` application ID suffix is present.
- [ ] FileProvider authority is derived from `ka.soft.tgxr`.
- [ ] Call recording settings and resources survive R8/resource shrinking.
- [ ] No ASAN, UBSAN, or development-only native instrumentation is enabled.
- [ ] Record the mapping and native debug-symbol archive paths for
      symbolication; do not place symbols in the public APK directory.

Verify signing and record only the public certificate fingerprint:

```bash
apksigner verify --verbose --print-certs RELEASE.apk
```

- [ ] Verification says `Verified`.
- [ ] Certificate SHA-256 matches the securely recorded production
      certificate.
- [ ] APK is not signed with the Android debug certificate.

## 5. Secret audit

- [ ] Review `git status`, `git diff --stat`, and `git diff`.
- [ ] Confirm `local.properties`, `build.log`, `*.jks`,
      `*.keystore`, signing properties, passwords, Telegram API values, and
      Firebase Admin/service-account credentials are not tracked.
- [ ] Search source and APK for `BEGIN PRIVATE KEY`, `service_account`,
      `private_key_id`, and `firebase-admin`.
- [ ] Confirm no FCM token, Telegram API hash, authorization token, phone code,
      private key, or service-account credential is logged.
- [ ] Confirm verbose `TGX-Push`, `TGX-Call-FGS`, and recorder transition
      logs are gated in release while warnings/errors remain available.

## 6. Debug-to-production migration

The debug and production APKs use the same package but different signatures.
Do not sign production with the debug key to permit an in-place install.
Before uninstalling the debuggable build, save recordings from Windows:

```bat
C:\tools\android\sdk\platform-tools\adb.exe exec-out run-as ka.soft.tgxr tar -C files/calls -cf - . > C:\Users\ka\Downloads\tgxr-recordings-before-release.tar
```

- [ ] Confirm the backup exists and has a reasonable non-zero size.
- [ ] Treat production installation as fresh application data.
- [ ] Re-authorize the Telegram account; do not use unsupported private-data
      migration hacks.

## 7. Motorola G84 / Android 15 runtime gate

- [ ] Fresh production app launches, login works, and there is no crash.
- [ ] FCM registers without `EXPERIMENTAL_BUILD_DETECTED`,
      `Configuration unavailable!`, `APP_PUSH_APIKEY_MISSING`, or
      `FirebaseApp is not initialized`.
- [ ] A background incoming push is received.
- [ ] A locked-screen incoming call rings without an FGS crash and Answer
      promotes from `SHORT_SERVICE` to `MICROPHONE`.
- [ ] An outgoing one-to-one call works.
- [ ] Start, Pause, Resume, Stop, and Restart work.
- [ ] `local.opus`, `remote.opus`, and `mixed.opus` are valid.
- [ ] Browser, playback, Share, Save, ZIP, and Delete work.
- [ ] Share Mixed/Local/Remote/All works through the production FileProvider.
- [ ] Forced process termination recovers the recording as `INTERRUPTED`
      and playback works.
- [ ] Recorder hook API is the expected version and both hooks are available.
- [ ] A 15-minute call has matching local/remote/mixed durations,
      `timelineDifferenceSamples == 0`, `queue drops == 0`, and plays back.

Only after this device checklist may the release be called
`PRODUCTION RUNTIME PASS`.

## 8. Public artifacts

Place only public files in `result/release-v1/`:

- [ ] `Telegram-X-Recorder-v1.0-arm64-v8a.apk`
- [ ] Optional AAB
- [ ] `SHA256SUMS.txt`
- [ ] `release-info.txt`

Do not copy the keystore, passwords, signing properties, service-account
files, private keys, or `local.properties` into the artifact directory.
Record APK size, SHA-256, signing certificate SHA-256, package, label,
versionCode, versionName, ABI, debuggable state, build date, Telegram X/TDLib/
tgcalls baselines, recorder hook API, and build type in `release-info.txt`.
