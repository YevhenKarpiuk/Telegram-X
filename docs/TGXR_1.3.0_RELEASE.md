# Telegram X Recorder 1.3.0

Recorder обновлён с Telegram X 0.29.0.1816 до **0.29.1.1819**.

- Upstream: `c26c0efa34b294179b48d909c7a5d64b6038b00d`.
- Код APK: `18a774d8bfa31b67cd98f56ccfdd383d22a92323`, ветка `upgrade/recorder-tgx-1819`.
- Package: `ka.soft.tgxr`, подпись совпадает с Recorder 1.2.0.
- Каждый APK содержит ARM32 и ARM64.
- Сохранены PCM hooks, запись обеих сторон, три режима, пауза/продолжение, восстановление, список записей, воспроизведение, экспорт и удаление. Формат и каталог записей не менялись.
- Интегрированы обновления TDLib и Telegram X, SDK flavors/ABI splits, Gradle 9.8.2. tgcalls Recorder `29e43315` и WebRTC `7b03082` сохранены.
- Debug и все три Release собраны успешно; 36 unit tests прошли без ошибок и пропусков. Проверены DEX/JNI, ARM-библиотеки, подписи и выравнивание на 16 KiB.

## Скачать

[GitHub Release tgxr-v1.3.0](https://github.com/YevhenKarpiuk/Telegram-X/releases/tag/tgxr-v1.3.0)

Выберите Release APK под версию Android. Существующий Recorder удалять не нужно.

| Android | APK | versionCode |
| --- | --- | ---: |
| 13+ | [Telegram-X-Recorder-v1.3.0.apk](https://github.com/YevhenKarpiuk/Telegram-X/releases/download/tgxr-v1.3.0/Telegram-X-Recorder-v1.3.0.apk) | 1819500 |
| 10–12 | [Telegram-X-Recorder-v1.3.0-QRS.apk](https://github.com/YevhenKarpiuk/Telegram-X/releases/download/tgxr-v1.3.0/Telegram-X-Recorder-v1.3.0-QRS.apk) | 1819400 |
| 7–9 | [Telegram-X-Recorder-v1.3.0-nougatOreoPie.apk](https://github.com/YevhenKarpiuk/Telegram-X/releases/download/tgxr-v1.3.0/Telegram-X-Recorder-v1.3.0-nougatOreoPie.apk) | 1819300 |


Рядом с каждым APK опубликован файл `.sha256`, общий список — `SHA256SUMS.txt`.

## Что ещё не проверено

Тестового устройства не было. После обновления реальные входящие/исходящие звонки, звук обеих сторон, три режима записи, пауза/продолжение, восстановление, воспроизведение и экспорт реальных записей на телефоне не проверены. Установка поверх прежней версии с сохранением данных на устройстве также не проверялась; package/сертификат/возрастающий versionCode и старый формат данных сохранены.

[Подробный отчёт интеграции](TGX_1819_RECORDER_UPGRADE.md).
