# Telegram X Recorder 1.2.0

Новая версия Recorder на базе Telegram X `0.29.0.1816`.

- Upstream: `TGX-Android/Telegram-X:main`, `7e3e3a3b1d658addd10514e2efd97de6e3ba1a42`. Актуальность проверена через `git ls-remote` 2026-10-07.
- Recorder: `1.2.0`, внутренний номер релиза `4`, package `ka.soft.tgxr`.
- tgcalls: `29e43315d7799e52199a1aad14bbf5e7248e97cb`, объединены новые upstream-изменения и перехватчики PCM Recorder.
- Сохранены запись обеих сторон, три режима аудио, пауза/продолжение, метаданные, список записей, воспроизведение, экспорт, удаление и восстановление.
- Совместимость с новой базой, исправления Windows-сборки и ARM32 описаны в [отчёте интеграции](TGX_1816_UPGRADE.md).
- Номер Android versionCode сохраняет формулу upstream: `1816300` для Universal Release, `1816000` для Debug. Эта версия предназначена для локальной установки; номер Release совпадает с предварительным APK интеграции `1.1.0` на той же базе. Номер официально опубликованной старой версии Recorder ниже.
- Исходные ветки `call-recording-dev` и `call-recording` не изменены. Push, публикация релиза и merge не выполнялись.
- Реальные звонки на телефоне после обновления не проверены.

## Проверки версии 1.2.0

- Universal Debug и оптимизированный Universal Release собраны успешно: `BUILD SUCCESSFUL in 3m 7s`, 636 задач (56 выполнено, 580 актуальны).
- Release: `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latestUniversal/release/Telegram-X-Recorder-v1.2.0.apk`.
- Размер Release: `86037577` байт; SHA-256 `eb56d543123862c0107cda9436fd6d54fb9fad4e0d5a400f419a67f18094b035`.
- Подпись Release проверена (v2/v3); сертификат совпадает с предыдущим релизом. Проверены package, версия `1.2.0-universal` и наличие ARM32/ARM64.
- Оба APK прошли проверку ZIP alignment 16 KiB.
- Повторно пройдены `CallRecordingRepositoryTest` (24), `CallForegroundStateMachineTest` (7), `ApplicationIdentityTest` (3): всего 34 теста, ошибок и пропусков нет.
- Debug: `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latestUniversal/debug/Telegram-X-Recorder-v1.2.0-debug.apk`.
- Размер Debug: `104671813` байт; SHA-256 `5fd0e71e422680f321afbf72dfc6b6640089042ef8752e02b97aa61d17d74353`.
- Подпись Debug проверена; сертификат SHA-256 `9d7945562d0596fac43c0306d6bab071d8d7cb919cc7e3fa7f5d4438ecffcbd4`, совпадает с ключом предыдущего релиза.
- Проверены package, версия `1.2.0-universal-debug` и наличие ARM32/ARM64 в APK.

Команда сборки из корня проекта (JDK 25, MSYSTEM=UCRT64):

```powershell
.\gradlew.bat assembleLatestUniversalDebug :app:testLatestUniversalDebugUnitTest --tests '*CallRecordingRepositoryTest' --tests '*CallForegroundStateMachineTest' --tests '*ApplicationIdentityTest' assembleLatestUniversalRelease --console=plain --max-workers=4
```

Лог: `builds/tgxr-1.2.0-build.log` (локальный, игнорируется Git).
