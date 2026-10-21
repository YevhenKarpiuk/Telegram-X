# Telegram X Recorder 1.3.0 — обновление до Telegram X 1819

## Источники и резервная точка

- Исходный Recorder: 1.2.0, Telegram X 0.29.0.1816, `ebbc5573`.
- Актуальный `TGX-Android/Telegram-X:main`, полученный и проверенный через `git ls-remote` 21.10.2026: `c26c0efa34b294179b48d909c7a5d64b6038b00d`, Telegram X 0.29.1.1819.
- Рабочая ветка: `upgrade/recorder-tgx-1819`.
- Коммит интеграции: `18a774d8bfa31b67cd98f56ccfdd383d22a92323` (родители: исходный Recorder и выбранный upstream).
- Резервная ветка: `backup/recorder-before-1819` на исходном HEAD.
- Ветки `main`, `call-recording`, `call-recording-dev` не изменены. Push, публикация и merge в них не выполнялись.

## Интеграция

Upstream переписал часть истории после предыдущей интеграции. Выполнено обычное слияние с сохранением обеих историй и проверкой итогового дерева относительно upstream и Recorder 1.2.0. Разрешены конфликты `app/build.gradle.kts`, `app/jni/CMakeLists.txt`, `TGCallService`, gitlink TDLib и LevelDB. TDLib и LevelDB взяты строго на закреплённых upstream SHA.

Включены изменения upstream: новые SDK flavors (`tens`, `qrs`, `latest`), ABI splits вместо ABI flavors, Gradle 9.8.2 и обновлённые библиотеки, Kotlin генераторы, оптимизация ресурсов и локализаций, обновлённые baseline profiles, исправления чатов, уведомлений, foreground service и разрешения полноэкранного уведомления.

Пересечение изменений относительно дерева 1816: `.gitmodules`, `app/build.gradle.kts`, `AndroidManifest.xml`, `TGCallService`, `UI`, `Settings`. В CMake также проверены различия из-за переписанной истории: удалена повторно внесённая старая настройка `pack-dyn-relocs=android`, которая перекрывала новую upstream настройку RELR. В whitelist CMake добавлены новые flavors `tens` и `qrs`.

Версия Recorder подготовлена как `1.3.0`, внутренний номер `5`. Сохранены `ka.soft.tgxr`, `Telegram X Recorder`, FileProvider authority и существующая конфигурация подписи. Android versionCode проверены: Debug 1819000, Release 1819300/1819400/1819500; все выше 1816300 из Recorder 1.2.0.

## Сохранение Recorder

- tgcalls и WebRTC между выбранными деревьями upstream не менялись; повторная интеграция PCM hooks не требуется.
- Сохранены post-processing локальный PCM, remote audio observer, очереди, синхронизация, смешанная/раздельная/полная запись, управление паузой и завершением, порядок остановки engine и финализации Recorder.
- `CallRecorder`, адаптер tgcalls, JNI, интерфейсы управления, repository, метаданные, восстановление, проигрывание, экспорт и удаление сохранены. Compile-time assertions остаются включены.
- Foreground state machine Recorder уже реализует безопасную семантику upstream: SHORT_SERVICE во время входящего вызова, MICROPHONE перед `AcceptCall` после проверки RECORD_AUDIO, переход к Activity при отсутствии разрешения. Сохранён этот путь через `CallManager` → `TGCallService.prepareForIncomingCallAnswer()`; прямой дополнительный вызов `U.startForeground` из upstream не дублируется, поскольку он не учитывал состояние Recorder и его manifest.
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

Окружение: JDK 25 (`C:/tools/java/jdk-25.0.4.1+1`), SDK `C:/tools/android/sdk`, NDK `30.0.16248370`, CMake 3.22.1, MSYS2 UCRT64 из существующей конфигурации. Gradle 9.8.2, AGP 9.4.1, Kotlin 2.4.21.

`tasks --all -Pabi=arm --console=plain --max-workers=4`: **BUILD SUCCESSFUL**, 1m 58s. Старые задачи `assembleLatestUniversal*` удалены upstream. Новая схема использует SDK flavors и ABI splits; `-Pabi=arm` штатно собирает ARM32 и ARM64 вместе в одном APK.

Первый полный проход Debug + unit tests + Release: **BUILD SUCCESSFUL in 21m 9s**, 462 задачи. Лог: `builds/tgx-1819-build.log`.

Финальная команда после интеграционного коммита:

```powershell
$env:JAVA_HOME = 'C:/tools/java/jdk-25.0.4.1+1'
$env:MSYSTEM = 'UCRT64'
.\gradlew.bat :app:assembleLatestDebug :app:testLatestDebugUnitTest :app:assembleLatestRelease :app:assembleTensRelease :app:assembleQrsRelease -Pabi=arm --console=plain --max-workers=4
```

Результат: **BUILD SUCCESSFUL in 38m 47s**, 926 задач (358 выполнено, 156 из кэша, 412 актуальны). Лог: `builds/tgx-1819-final-build.log`. Все APK содержат код и Git-информацию интеграционного коммита `18a774d8bfa31b67cd98f56ccfdd383d22a92323`; следующий коммит обновляет только документацию.

Все существующие unit tests запущены повторно на финальной версии:

| Набор | Тестов | Ошибки / пропуски |
| --- | ---: | --- |
| CallRecordingRepositoryTest | 24 | 0 / 0 |
| CallForegroundStateMachineTest | 9 | 0 / 0 |
| ApplicationIdentityTest | 3 | 0 / 0 |
| Всего | **36** | **0 / 0** |

Добавлены сценарии ответа из уведомления до завершения ringing и ожидания разрешения микрофона. Identity-тест проверяет Recorder 1.3.0, upstream 1819 и рост versionCode относительно 1816300. Repository-тесты покрывают старые схемы метаданных, interrupted sessions, recovery checkpoints, независимые экспорты, ZIP, удаление и ограничения путей. Это проверки файловых сценариев, не записи реального аудиосигнала.

Отчёт тестов: `app/build/reports/tests/testLatestDebugUnitTest/index.html`.

Для каждого из четырёх APK фактически проверены:

- Package `ka.soft.tgxr`, label `Telegram X Recorder`, minSdk, versionName и возрастающий versionCode.
- Наличие обеих ABI и основных библиотек Recorder, TDLib, LevelDB, OpenSSL, shared STL для каждой ABI; ELF machine соответствует архитектуре.
- Все пять JNI экспортов управления записью присутствуют в `libtgcallsjni.so` обеих ABI и отсутствуют в `libtdjni.so`.
- В DEX сохранены имена и типы полей `CallConfiguration`, native methods и `handleCallRecordingStateChanged`. R8 mapping сохраняет классы `CallConfiguration`, `TgCallsController`, `VoIPInstance` и callback. Minification/resource shrinking не отключались.
- Все динамические зависимости библиотек доступны в APK либо являются библиотеками Android.
- ZIP alignment 16 KiB и LOAD alignment всех ARM64 ELF не ниже 16 KiB.
- Подпись действительна, сертификат совпадает с APK Recorder 1.2.0: `9d7945562d0596fac43c0306d6bab071d8d7cb919cc7e3fa7f5d4438ecffcbd4`.

Для `tens` проверены APK Signature v2/v3; для `qrs` и `latest` — v3. При minSdk 29/33 новая сборка штатно не создаёт v2. Проверка выполнялась текущим `apksigner`, без подмены ключа или ослабления проверок.

Локальный проверочный скрипт: `builds/verify1819.py`. Результаты: `builds/tgx-1819-verification.json`, `builds/tgx-1819-apk-verification.log`, подробные signature/badging/ELF в `builds/tgx-1819-verify/`. Эталон сертификата — существующий APK 1.2.0 в `latestUniversal/release`. Эти локальные материалы и APK игнорируются Git.

## APK

Каждый APK содержит `arm64-v8a` и `armeabi-v7a`. Новый `latest` требует API 33 вместо прежнего API 24, поэтому дополнительно собраны штатные `tens` и `qrs`, сохраняющие охват устройств Recorder 1.2.0.

| Android | Тип / flavor | versionCode | versionName | Размер, байт |
| --- | --- | ---: | --- | ---: |
| 13+ | debug / latest | 1819000 | `1.3.0-arm-debug` | 129865037 |
| 13+ | release / latest | 1819500 | `1.3.0-arm` | 81155438 |
| 7–9 | release / tens | 1819300 | `1.3.0-nougatOreoPie` | 76366112 |
| 10–12 | release / qrs | 1819400 | `1.3.0-QRS` | 82573278 |

Полные пути и SHA-256:

- `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latest/debug/Telegram-X-Recorder-v1.3.0-debug.apk`
  SHA-256: `de829d2fbab4363c8717095d1126d389357b968da8552e98b2f8fc375297b4a2`.
- `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/latest/release/Telegram-X-Recorder-v1.3.0.apk`
  SHA-256: `2383d5547d78daf7bfb40bd0a9fdc652b9bf69c928a290c3e16bcdad3860c39c`.
- `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/tens/release/Telegram-X-Recorder-v1.3.0-nougatOreoPie.apk`
  SHA-256: `1c9bf3c86e28e39d92464f3d3bf37ca9c00752eb14eac6c6103557bf78864037`.
- `C:/Users/ka/Documents/prog/m-l/Telegram-X-call-recording-dev/app/build/outputs/apk/qrs/release/Telegram-X-Recorder-v1.3.0-QRS.apk`
  SHA-256: `3c92790061386cb316ce6d25face98e23608d7b21c2b82fcf4e775056d348740`.

## Совместимость и ограничения

Совпадение package и сертификата и больший versionCode позволяют установку поверх Recorder 1.2.0 на соответствующем Android. Каталог записей, FileProvider authority, формат JSON/Opus и настройки сохранены; основной код Recorder, repository, playback/export и управления звонками не отличается от 1.2.0. Фактическая установка обновлением с сохранением пользовательских данных на телефоне не проверялась.

ADB проверен повторно после сборок: устройств нет. Не проверены реальные входящие/исходящие звонки, наличие обеих сторон в аудио, три режима, пауза/продолжение, синхронизация, завершение сессий и восстановление после принудительной остановки на устройстве, воспроизведение и экспорт реальных записей. Сборка и unit tests не являются доказательством этих аппаратных сценариев.

Готовность: исходники интегрированы, Debug и подписанные Release собраны и прошли перечисленные проверки. APK готовы к установке и дальнейшему тестированию на устройстве. Следующий шаг — выбрать APK по версии Android, установить поверх 1.2.0 без удаления приложения и проверить звонки/записи/экспорт/восстановление во всех режимах.

## Замечания окружения и итоговое состояние Git

- Попытка одноразового запуска `--no-configuration-cache` отклонена Gradle: upstream Isolated Projects требует configuration cache. Финальный проход использует штатные настройки, исходники для обхода проверки не менялись. Лог попытки: `builds/tgx-1819-cache-option-attempt.log`.
- В логах остаются предупреждения Opus/Opusfile про отсутствующие Git tags в shallow submodules, deprecated API/Gradle features и D8 ReCaptcha Companion. Все сборки успешны; предупреждения не скрывались.
- Gitlinks всех submodules совпадают с закреплёнными SHA. TDLib LFS прошёл `fsck`; доступность итоговых SHA TDLib, X-Core, LevelDB, tdlib-utils, WebRTC и tgcalls подтверждена через удалённые refs.
- Upstream повторно проверен после всех сборок: `c26c0efa34b294179b48d909c7a5d64b6038b00d`.
- `main`, `call-recording`, `call-recording-dev` остались на исходных SHA. Рабочая ветка — `upgrade/recorder-tgx-1819`, backup — `backup/recorder-before-1819`.
- Начальная правка OpenSSL сохранена побайтно: SHA-256 pre/post patch `194d8b049aaff5e482c8f3e271a3a3460d91f5c565d0df91f79578a47ab2879a`. В статусе остаётся исходное `m tdlib` и пользовательская `.codex-remote-attachments/`; это не новые незакоммиченные изменения реализации.
- Секреты, signing properties, JKS и `local.properties` не перезаписывались. Push, Release и merge в исходные ветки не выполнялись.
