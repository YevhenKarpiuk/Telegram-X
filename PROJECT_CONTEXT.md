# Telegram X Recorder

## Цель и результат
Обновление Recorder 1.2.0 / Telegram X 0.29.0.1816 до Recorder 1.3.0 / Telegram X 0.29.1.1819 завершено локально. Подробности и SHA/пути APK: [docs/TGX_1819_RECORDER_UPGRADE.md](docs/TGX_1819_RECORDER_UPGRADE.md).

22.10.2026 пользователь явно разрешил обновить GitHub и опубликовать релизы. Готовится публикация ветки upgrade/recorder-tgx-1819 и релиза tgxr-v1.3.0 с тремя Release APK, Debug и SHA-256. Исходные main/call-recording/call-recording-dev сохраняются согласно исходному условию; force push и merge в них не выполняются.

## Ветка и зависимости
- Рабочая ветка upgrade/recorder-tgx-1819, интеграционный код 18a774d8bfa31b67cd98f56ccfdd383d22a92323; родители ebbc5573 и upstream c26c0efa34b294179b48d909c7a5d64b6038b00d. Финальный дополнительный коммит меняет только документацию.
- Backup backup/recorder-before-1819 = ebbc5573. Исходные main, call-recording и call-recording-dev не изменены. Push/публикация/merge в них не выполнялись.
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
- Для сборки Isolated Projects требует configuration cache; не использовать --no-configuration-cache. Сохранять проверки, Recorder hooks, формат данных и исходные ветки.
- Следующий шаг: отправить upgrade-ветку, опубликовать tgxr-v1.3.0 и проверить remote SHA/файлы релиза. Затем требуется проверка реальных звонков на устройстве.
