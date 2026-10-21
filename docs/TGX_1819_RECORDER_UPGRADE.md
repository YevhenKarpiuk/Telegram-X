# Telegram X Recorder 1.3.0 — обновление до Telegram X 1819

## Источники и резервная точка

- Исходный Recorder: 1.2.0, Telegram X 0.29.0.1816, `ebbc5573`.
- Актуальный `TGX-Android/Telegram-X:main`, полученный и проверенный через `git ls-remote` 21.10.2026: `c26c0efa34b294179b48d909c7a5d64b6038b00d`, Telegram X 1819.
- Рабочая ветка: `upgrade/recorder-tgx-1819`.
- Резервная ветка: `backup/recorder-before-1819` на исходном HEAD.
- Ветки `main`, `call-recording`, `call-recording-dev` не изменяются. Push, публикация и merge в них не выполняются.

## Интеграция

Upstream переписал часть истории после предыдущей интеграции. Выполнено обычное слияние с сохранением обеих историй и проверкой итогового дерева относительно upstream и Recorder 1.2.0. Разрешены конфликты `app/build.gradle.kts`, `app/jni/CMakeLists.txt`, `TGCallService`, gitlink TDLib и LevelDB. TDLib и LevelDB взяты строго на закреплённых upstream SHA.

Включены изменения upstream: новые SDK flavors (`tens`, `qrs`, `latest`), ABI splits вместо ABI flavors, Gradle 9.8.2 и обновлённые библиотеки, Kotlin генераторы, оптимизация ресурсов и локализаций, обновлённые baseline profiles, исправления чатов, уведомлений, foreground service и разрешения полноэкранного уведомления.

Пересечение изменений относительно дерева 1816: `.gitmodules`, `app/build.gradle.kts`, `AndroidManifest.xml`, `TGCallService`, `UI`, `Settings`. В CMake также проверены различия из-за переписанной истории: удалена повторно внесённая старая настройка `pack-dyn-relocs=android`, которая перекрывала новую upstream настройку RELR. В whitelist CMake добавлены новые flavors `tens` и `qrs`.

Версия Recorder подготовлена как `1.3.0`, внутренний номер `5`. Сохранены `ka.soft.tgxr`, `Telegram X Recorder`, FileProvider authority и существующая конфигурация подписи. Номера Android версии рассчитываются новой формулой upstream и должны быть выше 1816300 из Recorder 1.2.0.

## Сохранение Recorder

- tgcalls и WebRTC между выбранными деревьями upstream не менялись; повторная интеграция PCM hooks не требуется.
- Сохранены post-processing локальный PCM, remote audio observer, очереди, синхронизация, смешанная/раздельная/полная запись, управление паузой и завершением, порядок остановки engine и финализации Recorder.
- `CallRecorder`, адаптер tgcalls, JNI, интерфейсы управления, repository, метаданные, восстановление, проигрывание, экспорт и удаление сохранены. Compile-time assertions остаются включены.
- Foreground state machine Recorder уже реализует безопасную семантику upstream: SHORT_SERVICE во время входящего вызова, MICROPHONE перед `AcceptCall` после проверки RECORD_AUDIO, переход к Activity при отсутствии разрешения. Сохранён этот путь через `CallManager.prepareForIncomingCallAnswer`; прямой дополнительный вызов `U.startForeground` из upstream не дублируется, поскольку он не учитывал состояние Recorder и его manifest.
- Формат и каталог существующих записей, настройки и package не меняются.

## Зависимости

| Компонент | Закреплённый SHA |
| --- | --- |
| tgcalls Recorder | `29e43315d7799e52199a1aad14bbf5e7248e97cb` |
| tgcalls upstream внутри Recorder merge | `1a00b96152e93278a617946eaf0a55d7c4290f6a` |
| WebRTC | `7b03082ff07aa442bcbb09ea54b8c33b4fef7e6d` |
| TDLib wrapper | `b47cd822037a4c38916f0b78f57298f10a88821e` |
| TDLib source | `a94b83d67ab4c8397c6245e541b31a4131ea2f79` |
| X-Core | `bc5320f419b6ba91a90d5d9ace84740c7ff3217c` |
| LevelDB wrapper | `a57a3557f1a6cb74f2c345757312f70f6158c235` |
| LevelDB source | `aa5479bbf47e9df86e0afbb89e6246085f22cdd4` |
| tdlib-utils | `61d4ea0b66cfdeed9352ba0a84e5746e528559d5` |
| OpenSSL source | `45e844fa2a14ec92d146bd8f5778ac130b6625fb` |
| FFmpeg | `e594a5185950dae2cdd54802bed4d677e7829b12` |

`29e43315` доступен в удалённом `YevhenKarpiuk/tgcalls`, ветка `upgrade/tgx-1816-recorder`; `.gitmodules` указывает на этот форк и ветку. Использован pinned `git submodule update --init --recursive`, без `--remote`. Полный снимок: `builds/tgx-1819-submodules.txt`. `git lfs fsck` TDLib прошёл.

Первоначальный fetch рекурсивно запросил недоступный промежуточный SHA `c2655b5` из истории LevelDB; обновление на конечный gitlink `a57a3557` и его submodules прошло успешно. Недоступный промежуточный SHA для итоговой сборки не нужен.

Существующая локальная правка окончаний строк в `tdlib/source/openssl/test/smime-eml/pkcs7-empty-digest-set.eml` оставлена на месте, резервная копия — `builds/pre-1819-openssl.patch`. `.codex-remote-attachments/` не изменяется. Секреты, ключи и `local.properties` не перезаписываются.

## Сборка и проверки

JDK 25: `C:/tools/java/jdk-25.0.4.1+1`, Android SDK `C:/tools/android/sdk`, NDK `30.0.16248370`, MSYS2 UCRT64 из существующей конфигурации.

`tasks --all -Pabi=arm --console=plain --max-workers=4`: **BUILD SUCCESSFUL**, 1m 58s. Новые задачи — `assembleLatestDebug`, `assembleLatestRelease`, `testLatestDebugUnitTest`; старых `assembleLatestUniversal*` больше нет. `-Pabi=arm` собирает обе ARM архитектуры в одном APK через штатную новую схему upstream.

Запущена команда (результат ожидается):

```powershell
$env:JAVA_HOME = 'C:/tools/java/jdk-25.0.4.1+1'
$env:MSYSTEM = 'UCRT64'
.\gradlew.bat :app:assembleLatestDebug :app:testLatestDebugUnitTest :app:assembleLatestRelease -Pabi=arm --console=plain --max-workers=4
```

Лог: `builds/tgx-1819-build.log`. Выполняются все существующие unit tests, включая `CallRecordingRepositoryTest`, `CallForegroundStateMachineTest`, `ApplicationIdentityTest`. Добавлены сценарии ответа из уведомления до окончания ringing и ожидания разрешения микрофона. Проверка identity обновлена на 1.3.0 и возрастающий versionCode относительно 1816300.

## Ограничения и оставшиеся проверки

ADB запускается, список устройств пуст. Реальные звонки, запись обеих сторон на устройстве, воспроизведение и обновление установленного APK без потери данных пока не проверены. Сборка не считается доказательством работы записи реальных звонков.

Новый upstream `latest` требует API 33, прежний `latest` требовал API 24. Для сохранения охвата Android 7–12 требуются отдельные штатные варианты `tens` (24–28) и `qrs` (29–32). Результаты сборок, подписи, JNI, APK и совместимости будут внесены после выполнения проверок.
