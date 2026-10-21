# Telegram X Recorder

## Цель
Обновить Recorder 1.2.0 / Telegram X 1816 до актуального upstream/main, собрать и проверить Recorder 1.3.0 для ARM32/ARM64. Работать в новой upgrade/recorder-tgx-1819, исходные ветки не менять. Не выполнять push, публикацию или merge в исходные ветки.

## Контекст и ограничения
- Android Java/Kotlin/C++, TDLib, WebRTC, tgcalls с PCM hooks из YevhenKarpiuk/tgcalls.
- Исходный HEAD ebbc5573, текущая ветка при старте upgrade/call-recording-latest; call-recording-dev на том же коммите.
- Существующие локальные изменения: содержимое tdlib (ещё не исследовано), неотслеживаемая .codex-remote-attachments/. Сохранять их.
- Сохранять package ka.soft.tgxr, подпись, старый формат записей, все режимы и управление записью; не менять секреты и local.properties.
- Документация базы: docs/TGXR_1.2.0_RELEASE.md, docs/TGX_1816_UPGRADE.md.
- Окружение ранее: JDK 25, SDK/NDK в C:/tools/android/sdk, MSYS2 UCRT64 в builds/tools/msys64. Проверить фактическое состояние.

## Состояние и проверки
- Прочитаны документы прежней интеграции. Новые сборки и тесты запущены, результат пока ожидается.
- Обычный Git submodule status заблокирован sandbox при обращении к общему gitdir worktree; требуется штатное разрешение инструментов.

## Интеграция 1819
- Проверенный upstream/main: c26c0efa34b294179b48d909c7a5d64b6038b00d, version.app=1819 (21.10.2026). История upstream переписана относительно 1816, потребовался merge с проверкой дополнительных конфликтов.
- Backup: backup/recorder-before-1819 = ebbc5573. Новая ветка upgrade/recorder-tgx-1819; merge ещё не закоммичен.
- tgcalls 29e43315 и WebRTC 7b03082 не менялись в upstream. Форк tgcalls публикует 29e43315 в upgrade/tgx-1816-recorder, SHA доступен для клонирования.
- TDLib b47cd822 (source a94b83d6), core bc5320f4, leveldb a57a3557, td utils 61d4ea0b; выполнены pinned submodule update и tdlib LFS pull.
- Локальная правка OpenSSL — только окончания строк test/smime-eml/pkcs7-empty-digest-set.eml, сохранена на месте и в builds/pre-1819-openssl.patch.
- Разрешены конфликты Gradle, CMake, TGCallService. Сохранены Recorder foreground state machine и upstream семантика microphone promotion/permission UI. Убрана дублирующая старая опция relocs, добавлены новые SDK flavors в CMake whitelist.
- Подготовлены версия 1.3.0 / внутренний номер 5 и регрессионные проверки foreground переходов.

## Следующий шаг
- `tasks --all -Pabi=arm`: BUILD SUCCESSFUL (1m 58s), Gradle 9.8.2. Задачи :app:assembleLatestDebug, :app:testLatestDebugUnitTest, :app:assembleLatestRelease запущены с -Pabi=arm, лог builds/tgx-1819-build.log.
- Новый latest требует API 33 вместо 24; дополнительно собрать штатные tens (24–28) и qrs (29–32) для сохранения охвата прежней версии.
- ADB работает, устройств нет. Фактическое обновление приложения и реальные звонки не проверены.
- Отчёт docs/TGX_1819_RECORDER_UPGRADE.md создан, результаты пока ожидаются. Проверочный локальный скрипт: builds/verify1819.py.
- Следующий шаг: закончить сборки/тесты, проверить подписи, ARM ELF/JNI и версии APK, затем обновить отчёт.
