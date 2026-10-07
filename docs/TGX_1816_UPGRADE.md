# Telegram X Recorder: Telegram X 1816 integration

## Итог

- Обновлено до `TGX-Android/Telegram-X:main` — `7e3e3a3b1d658addd10514e2efd97de6e3ba1a42`, база `0.29.0.1816`. SHA повторно проверен в конце работы.
- Работа выполнена в `upgrade/call-recording-latest`. Исходная `call-recording-dev` и backup остаются на `5e580f66aa13a743948db41332fa40e12c3d5d3e`; стабильная ветка не изменена.
- tgcalls: `29e43315d7799e52199a1aad14bbf5e7248e97cb` — merge нового upstream `1a00b961` с Recorder `8010b9b7`. Перехватчики PCM и безопасное завершение сохранены; commit пока локальный.
- Единственный Git-конфликт — указатель tgcalls. Восемь пересекающихся файлов проверены; новая логика upstream и Recorder объединены.
- Адаптирован `CallRecorder.cpp` для ARM32; исправлены `UI.java` и проверка Git worktree. Исправлены локальные CRLF и старые пути signing-конфигурации. Формат записей, хранилище, UI и бизнес-логика не переписаны.
- Все 34 unit tests прошли без ошибок и пропусков. Дополнительно прошли 2 млн сравнений арифметики с исходной реализацией.
- Universal Debug и оптимизированный Universal Release собраны под Windows. Оба APK подписаны вашим ключом; package `ka.soft.tgxr`, Recorder `1.1.0`, ARM32 + ARM64.
- Release: **86037577 байт**, SHA-256 `39a101969d06516eb639ea6600b85a9121eb2379a1393871c24e928c61c713fc`.
- Debug: **104671809 байт**, SHA-256 `83b2c7e5563da91bd7b202d1df9f06d16f164da4bb9ad5dbf1526f08a118f485`.
- Полные пути APK, результаты проверки подписи, зависимости и новые commit перечислены ниже. Реальные звонки на устройстве после обновления не проверялись.
- Push и merge в `call-recording-dev` не выполнялись. Для переноса локального tgcalls сохранён проверенный bundle.

## Recovery points and source versions

- Original `call-recording-dev`: `5e580f66aa13a743948db41332fa40e12c3d5d3e`.
- Backup: `backup/call-recording-dev-before-upstream-update`.
- Integration branch: `upgrade/call-recording-latest`.
- Merge base: `9312ace35291f68fdd66ddff348d7321bee2a4bd` (Telegram X 1813).
- Fetched upstream: `TGX-Android/Telegram-X:main`, `7e3e3a3b1d658addd10514e2efd97de6e3ba1a42`, Telegram X `0.29.0.1816`.
- Existing Recorder commits include the native port (`b908e8f2`), JNI (`2f9860cf`), call controls (`e7113ee6`), settings (`51e7788e`), repository/recovery (`ad75bd85`), browser/export (`23fc7a27`), and release identity (`8191ab52`). Earlier Recorder history remains reachable through `5e580f66`.

## tgcalls integration

- Previous Recorder SHA: `8010b9b7d85eeff024a21869826c7b8e1d2906b0`.
- Previous upstream base: `236c2d53f4e5cbf608fe1f72bf6cfae1c56784ba`.
- New upstream SHA required by Telegram X: `1a00b96152e93278a617946eaf0a55d7c4290f6a` (`production`, not the default `development` branch).
- Result: `29e43315d7799e52199a1aad14bbf5e7248e97cb`, local branch `upgrade/tgx-1816-recorder`.
- tgcalls backup: `backup/recorder-before-tgx-1816`.
- Original fork recording commit: `60d59eadb793a63830fc3bd58682926486692996`; its 1813 adaptation is `8010b9b7`.
- The merge retains the post-processing local PCM factory in `Descriptor` / `InstanceV2Impl`, the existing audio-device factory for remote PCM, and `ThreadLocalObject::reset` / completion ordering that tears down the engine before finalizing recording.
- New engine fixes, MTProto transport, group broadcast resampling, and gzip limits are inherited from upstream. Existing recording engine support remains `7.0.0`, `8.0.0`, `9.0.0`, `12.0.0`, `13.0.0`.
- The `.gitmodules` source remains `YevhenKarpiuk/tgcalls`. No blind remote update was used.
- The tgcalls integration commit is local and must be published to the fork before a fresh remote clone can fetch the superproject's new gitlink. No push was authorized or performed.
- A verified complete-history bundle is saved at `builds/tgcalls-upgrade-1816.bundle` with the integration and backup branches for local recovery or transfer.

Other pinned native versions after integration:

| Component | SHA |
| --- | --- |
| WebRTC | `7b03082ff07aa442bcbb09ea54b8c33b4fef7e6d` |
| TDLib Android wrapper | `3b99c64a56d1392f7dcf0a246121c1677db8208b` |
| TDLib source | `42e6a5259551178d1dab54a22ad96d14bd906e20` |
| FFmpeg | `e594a5185950dae2cdd54802bed4d677e7829b12` |

## Recorder architecture and merge review

Local audio is intercepted after WebRTC processing; remote audio is intercepted at the audio-device render observer. Realtime callbacks enqueue PCM into the bounded recorder queues. The worker normalizes/resamples audio to the existing 48 kHz timeline, writes mixed and/or separate Opus tracks, and maintains existing session JSON and recovery markers.

`TgCallsController` and `VoIPInstance` expose the JNI controls and authoritative state. `TGCallService` retains recording callbacks and guarded microphone foreground-service promotion. `CallController` reads the existing native sample clock. The repository, active-session registry, browser/details controllers, playback, FileProvider sharing, SAF save, ZIP export, deletion, recovery, and settings storage are preserved.

Eight files overlap between Recorder changes and upstream: `app/build.gradle.kts`, tgvoip `CMakeLists.txt`, tgcalls gitlink, `TGCallService`, `CallManager`, `SettingsThemeController`, `Settings`, and `strings.xml`. Text merges retain both implementations. The only Git conflict was the tgcalls gitlink, resolved to the independently merged Recorder/upstream commit.

Upstream changes include `AppContext` migration, native Windows build support, JGit metadata extraction, submodule/LFS validation, Java 25, Gradle 9.8.0, AGP 9.4.1, NDK 30, newer TDLib/WebRTC/FFmpeg, and compiler flags. New upstream `AppContext` calls are retained alongside Recorder additions.

Package identity remains `ka.soft.tgxr`, product name `Telegram X Recorder`, Recorder version `1.1.0` / release integer `3`. Existing Firebase configuration matches the supplied configuration semantically and remains unchanged. Credentials and signing material are only referenced through ignored local configuration.

## Windows environment and compatibility fixes

- JDK: `C:/tools/java/jdk-25.0.4.1+1` (Temurin; download SHA-256 verified).
- SDK: `C:/tools/android/sdk`, platforms `android-37.2` and `android-37.0` (the latter installed automatically by Gradle for `compileSdk=37`), build-tools `37.0.0`.
- Primary NDK: `C:/tools/android/sdk/ndk/30.0.16248370`.
- CMake: `3.22.1` from the Android SDK.
- MSYS2: local ignored `builds/tools/msys64`, UCRT64, with git, git-lfs, perl, make and diffutils. A mirror connection reset was resolved by retrying the signed package transaction.
- Gradle wrapper: `9.8.0`; Git LFS is installed.
- All 56 recursive submodules match their pinned SHA. TDLib LFS was pulled after checkout. Local `core.longpaths=true` removes Windows status warnings; all 28 TDLib symlinks were restored as real symlinks to avoid Git LFS filtering text placeholders as changed files. Submodules are clean.
- `ValidateGitSetupTask` accepts a valid Git worktree `.git` file through JGit, retaining repository, submodule and LFS validation.
- `UI`'s remaining live call to removed `getAppContext()` is migrated to `AppContext.get()`.
- A `tasks --all` attempt compiled buildSrc successfully, then stopped because TDLib had not finished initialization (`tdlib/version.txt` absent). This was a dependency-order issue, not bypassed validation.
- The first Universal build exposed the system Git for Windows `core.autocrlf=true`: libvpx ARM32 assembly generation embedded CR in include filenames. Local `core.autocrlf=false` / `core.eol=lf` are applied in the main repository and all submodules. Already materialized CRLF text in libvpx, FFmpeg and Opus was restored to the original LF bytes; their indexes still exactly match their pinned commits. No dependency source changes were committed. Use LF when initializing these submodules on Windows.
- Supplied Windows signing properties referenced their old transfer directory. The first release compilation completed but signature verification correctly rejected its unsigned APK; after wiring the actual signing properties, Gradle also reported the old JKS path. Only ignored local configuration is corrected: `local.properties` refers to `builds/signing.local.properties`, which retains the supplied credentials and points to `C:/Users/ka/sec/tgxr-release.jks`. Original files in `sec` are not modified. The unsigned intermediate is not a deliverable.
- ARM32 exposed pre-existing Recorder assumptions from the earlier ARM64-only builds: primitive `__int128` is unsupported, and a `std::min` mixed `size_t` and `uint64_t`. `CallRecorder.cpp` now uses the already-linked `absl::int128` for wide signed arithmetic, exact quotient/remainder conversions for constexpr timeline operations, and explicit `uint64_t` for that minimum. Metadata saturation, diagnostic hysteresis, sample rounding, accounting, mixing, file format and storage are preserved.
- All previous compile-time regression checks remain enabled. New checks cover half-sample rounding, the full signed timestamp span, maximum sample counts and callback overflow. A host differential check compares the actual updated functions with the original backup source for 1,000,000 checks with intrinsic int128 and another 1,000,000 with Abseil's software fallback; both pass. Local reproduction: `powershell -NoProfile -ExecutionPolicy Bypass -File builds/check-recorder-arithmetic.ps1` (MinGW GCC required).

## Validation results

`tasks --all --console=plain` succeeds and confirms `assembleLatestUniversalDebug`, `assembleLatestUniversalRelease`, and `:app:testLatestUniversalDebugUnitTest`. Upstream's Universal variant includes `arm64-v8a` and `armeabi-v7a`.

`assembleLatestUniversalDebug --console=plain --max-workers=4` succeeds after the fixes (300 actionable tasks; final incremental run 10 seconds). Java, Kotlin, both native Recorder ABI builds, linkage and APK packaging pass.

`:app:testLatestUniversalDebugUnitTest` with filters for the three required suites succeeds: `CallRecordingRepositoryTest` 24, `CallForegroundStateMachineTest` 7, `ApplicationIdentityTest` 3. Total 34 tests, zero failures/errors/skips; 169 actionable tasks, 1 minute 12 seconds.

Debug artifact:

- Path: `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latestUniversal/debug/Telegram-X-Recorder-v1.1.0-debug.apk`.
- Size: `104671809` bytes.
- SHA-256: `83b2c7e5563da91bd7b202d1df9f06d16f164da4bb9ad5dbf1526f08a118f485`.
- Verified package `ka.soft.tgxr`, versionCode `1816000`, versionName `1.1.0-universal-debug`, label `Telegram X Recorder`, ABIs `arm64-v8a` and `armeabi-v7a`, compileSdk `37`.

Final combined Debug rebuild and the same 34 tests succeed with the corrected signing configuration: 324 actionable tasks, 2 minutes 24 seconds. Debug signature verification passes with the production certificate.

Release artifact:

- Command: `assembleLatestUniversalRelease --console=plain --max-workers=4` — `BUILD SUCCESSFUL` (343 actionable tasks; final incremental run 1 minute 48 seconds). R8 minification and resource shrinking remain enabled.
- Path: `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latestUniversal/release/Telegram-X-Recorder-v1.1.0.apk`.
- Size: `86037577` bytes.
- SHA-256: `39a101969d06516eb639ea6600b85a9121eb2379a1393871c24e928c61c713fc`.
- Verified package `ka.soft.tgxr`, versionCode `1816300`, versionName `1.1.0-universal`, label `Telegram X Recorder`, ABIs `arm64-v8a` and `armeabi-v7a`, compileSdk `37`.
- Signature verification: PASS, APK Signature Schemes v2 and v3. Certificate SHA-256 `9d7945562d0596fac43c0306d6bab071d8d7cb919cc7e3fa7f5d4438ecffcbd4`, matching the recorded previous Recorder production certificate.
- ZIP alignment: PASS, 16 KiB, for both APKs. All 11 packaged ARM64 ELF libraries have 16 KiB LOAD alignment.
- JNI recording exports are present for both ABI builds. R8 mapping preserves `CallConfiguration`, `TgCallsController`, `VoIPInstance`, and `handleCallRecordingStateChanged`.
- No merge markers or uncommitted dependency changes remain. Private configuration/keystore filenames are absent from the release APK; signing files and `local.properties` are ignored by Git.
- Both artifacts contain application code at `4ced5098b1105d3e341dd87e6a236c790b87dfd4`. The final report commit changes documentation only.

Device call-flow validation is not performed for this integration; no claim of new physical-device runtime validation is made.

## Local commits and reproduction

Integration branch first-parent commits:

1. `3c704379` — Merge Telegram X main build 1816 preserving Recorder integration.
2. `f1aa4170` — Fix Windows worktree validation and remaining AppContext migration.
3. `4ced5098` — Make Recorder timeline arithmetic portable to ARM32.
4. Final documentation commit — Document Telegram X 1816 Windows Recorder upgrade and validation.

tgcalls integration commit: `29e43315` — Merge TGX tgcalls production for Telegram X 1816 with recorder hooks.

The original branch and backup remain unchanged. Final Git status is clean on `upgrade/call-recording-latest`; the exact commit list and status are saved locally in `builds/final-git-state.txt`. No remote branches were updated and no force-push was performed.

Run from the integration checkout in PowerShell:

```powershell
$env:JAVA_HOME = 'C:\tools\java\jdk-25.0.4.1+1'
$env:MSYSTEM = 'UCRT64'
.\gradlew.bat assembleLatestUniversalDebug --console=plain --max-workers=4
.\gradlew.bat :app:testLatestUniversalDebugUnitTest --console=plain --max-workers=4
.\gradlew.bat assembleLatestUniversalRelease --console=plain --max-workers=4
```

These commands use the existing ignored local SDK/MSYS2/signing configuration. The default system Java environment was not changed.
