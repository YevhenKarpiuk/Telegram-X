# Telegram X Recorder

## Цель и результат
Обновление Recorder 1.2.0 / Telegram X 0.29.0.1816 до Recorder 1.3.0 / Telegram X 0.29.1.1819 завершено локально. Подробности и SHA/пути APK: [docs/TGX_1819_RECORDER_UPGRADE.md](docs/TGX_1819_RECORDER_UPGRADE.md).

22.10.2026 по прямому разрешению пользователя опубликованы ветка upgrade/recorder-tgx-1819 и Latest-релиз [tgxr-v1.3.0](https://github.com/YevhenKarpiuk/Telegram-X/releases/tag/tgxr-v1.3.0). В релизе три Release APK, Debug, четыре файла .sha256 и SHA256SUMS.txt; все 9 размеров и SHA-256 подтверждены GitHub API. Тег указывает на 07294c70492368a5ac0a057978f96a73ca735132, код APK — 18a774d8.

Затем пользователь явно разрешил перенести 1.3.0 в call-recording-dev и call-recording и отправить обе ветки без force push. Для обеих удалённых веток исходная точка ebbc5573 является предком готового обновления; применяется fast-forward и атомарный push. main и релизный тег не меняются.

## Ветка и зависимости
- Интеграционный код 18a774d8bfa31b67cd98f56ccfdd383d22a92323; родители ebbc5573 и upstream c26c0efa34b294179b48d909c7a5d64b6038b00d. Последующие коммиты меняют только документацию. call-recording-dev, call-recording и upgrade/recorder-tgx-1819 синхронизируются на одну вершину; рабочая ветка основного каталога — call-recording-dev.
- Backup backup/recorder-before-1819 = ebbc5573. В каталоге ../Telegram-X-call-recording были 263 незакоммиченных изменения окончаний строк. Он сохранён на backup/call-recording-worktree-before-1.3.0 (7f8032be); содержимое всех 263 путей проверено побайтно. Исходные правки не включены в перенос. Snapshot и patch: builds/call-recording-worktree-before-promote.{json,patch}.
- TDLib wrapper b47cd822, source a94b83d6; core bc5320f4, leveldb a57a3557, td utils 61d4ea0b. Все pinned submodules совпадают; LFS fsck PASS; критичные SHA опубликованы.
- tgcalls Recorder 29e43315 (форк YevhenKarpiuk/tgcalls, ветка upgrade/tgx-1816-recorder), upstream engine 1a00b961, WebRTC 7b03082. В актуальном upstream tgcalls/WebRTC не менялись, PCM hooks и безопасное завершение сохранены.

## Реализация и решения
- Сохранены ka.soft.tgxr, Telegram X Recorder, сертификат 1.2.0, старый формат/каталог записей, все режимы и управление записью.
- Интегрированы новые SDK flavors/ABI splits, Gradle 9.8.2, TDLib и остальные изменения upstream. Recorder version 1.3.0, внутренний номер 5; версия Android выше предыдущей.
- CMake принимает новые tens/qrs; старая дублирующая настройка relocs удалена в пользу upstream RELR. Сохранены foreground state machine и permission/answer семантика через CallManager → TGCallService.prepareForIncomingCallAnswer.
- Native Recorder, JNI, repository, playback/export и управление звонками сохранены без изменений относительно 1.2.0. Новые тесты foreground transitions; обновлён identity test.
- Новый latest minSdk 33; дополнительно собраны tens (API 24–28) и qrs (29–32), чтобы сохранить охват прежних APK.

## Проверки и артефакты
- JDK C:/tools/java/jdk-25.0.4.1+1, SDK C:/tools/android/sdk, NDK 30.0.16248370, MSYS2 UCRT64; использовать существующие local.properties и signing configuration.
- Полная финальная команда: gradlew.bat :app:assembleLatestDebug :app:testLatestDebugUnitTest :app:assembleLatestRelease :app:assembleTensRelease :app:assembleQrsRelease -Pabi=arm --console=plain --max-workers=4. BUILD SUCCESSFUL, 38m 47s, 926 задач. Лог builds/tgx-1819-final-build.log.
- Все 36 тестов PASS: repository 24, foreground 9, identity 3; ошибок/пропусков нет.
- Четыре APK: app/build/outputs/apk/latest/debug/Telegram-X-Recorder-v1.3.0-debug.apk; latest/release/Telegram-X-Recorder-v1.3.0.apk; tens/release/Telegram-X-Recorder-v1.3.0-nougatOreoPie.apk; qrs/release/Telegram-X-Recorder-v1.3.0-QRS.apk (последние три пути также под app/build/outputs/apk/).
- Для каждого APK PASS: подпись и сертификат 1.2.0, versionCode, ARM32/ARM64, ELF/imports, native exports и DEX поля/методы JNI, ZIP и ARM64 ELF alignment 16 KiB. Результаты builds/tgx-1819-verification.json; скрипт builds/verify1819.py.

## Ограничения и следующий шаг
- ADB работает, устройств нет. Реальные звонки/обе стороны/три режима, пауза, синхронизация, playback/export реального аудио и обновление установленного приложения с сохранением данных на устройстве не проверены. APK готовы к установке и таким проверкам.
- Существующая правка окончаний строк tdlib/source/openssl/test/smime-eml/pkcs7-empty-digest-set.eml и .codex-remote-attachments/ сохранены. Pre/post patch совпадают побайтно, копии в builds/. Секреты, ключи и local.properties не переписывались.
- Для сборки Isolated Projects требует configuration cache; не использовать --no-configuration-cache. Сохранять проверки, Recorder hooks, формат данных и локальные пользовательские правки. main не менять; обновление call-recording-dev/call-recording разрешено текущим запросом, только fast-forward без force push.
- Публикация завершена: релиз id 420492239, draft=false, prerelease=false, Latest=true; опубликован 22.10.2026. Снимок API и результаты проверки — builds/tgxr-1.3.0-release-published.json. Следующий шаг — установка подходящего Release поверх 1.2.0 и проверка реальных звонков на устройстве.
