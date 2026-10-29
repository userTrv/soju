# soju

Движок для запуска Windows-игр на macOS (Apple Silicon): Wine, собранный под игры из Steam. На нём работает [carafe](https://github.com/userTrv/carafe) — приложение для обычных пользователей.

Что soju добавляет к Wine:

- **Steam-игры без Windows-Steam.** Игра говорит со Steam для Mac через мост Steamworks: достижения, статистика, лидерборды, облачные сохранения. Второй клиент Steam внутри Wine не нужен.
- **Графика.** DirectX 11/12 → Metal через D3DMetal из Apple Game Porting Toolkit, в планах открытый DXMT. Вулкан-путь (DXVK + MoltenVK) — позже.
- **Rust там, где он к месту:** мост Steamworks (сейчас на C, переезжает), уведомления о достижениях, инструменты сборки и проверки. Само ядро Wine остаётся на C.

## Основа

Wine из открытых исходников CrossOver ([codeweavers.com/crossover/source](https://www.codeweavers.com/crossover/source), LGPL). Сейчас это CrossOver 26.3.0 = Wine 11.0, в котором уже есть:

- хуки winemac.drv для D3DMetal и DXMT;
- msync — синхронизация потоков на примитивах Mach, быстрее esync;
- wow64: 32-битные игры работают в 64-битном процессе, 32-битный хост не нужен.

Версии и хеши всех источников — в [`sources.lock`](sources.lock). Наши правки Wine — патчами в [`patches/wine`](patches/wine).

## Сборка

Нужны Xcode Command Line Tools, Rosetta 2 и bison 3 (`brew install bison`). Остальное скрипт скачает сам и сверит sha256.

```bash
scripts/build-engine.sh
```

Результат — `dist/soju-engine`; запуск программ — `dist/soju-engine/bin/soju <exe>` (это wine с настройками движка). Wine Mono и Wine Gecko лежат в движке и ставятся в префикс сами, без окна «Установить?». Рабочие файлы лежат в `~/Library/Caches/soju` (`SOJU_WORK`), повторная сборка пересобирает только изменившееся.

Движок собирается под x86_64 и работает под Rosetta: Windows-игры — это x86-код, и Rosetta переводит его быстрее любого эмулятора. Компилятор при этом работает нативно.

## D3DMetal

D3DMetal — закрытая библиотека Apple, распространять её нельзя, поэтому в движок она не входит. Её берут из своей копии Game Porting Toolkit:

```bash
scripts/install-d3dmetal.sh "<GPTK>/Contents/Resources/wine"
```

После этого `bin/soju` включает D3DMetal сам (`SOJU_D3DMETAL=0` — без него). Пересборка движка его стирает: скрипт нужно запустить снова.

## Что проверено

- Префикс создаётся без окон, msync включается, `ver` — Windows 10.
- 32-битная программа работает через wow64.
- Megabonk (Unity, D3D11) через carafe: D3DMetal рисует (Unity видит Direct3D 11.1), мост в Steam для Mac — Steam инициализирован, лидерборды грузятся.

## Мост в Steam для Mac

Игра под Wine говорит со Steamworks через заглушку `steam_api64.dll`, которая передаёт вызовы нативному `soju-steamd`, а тот — запущенному Steam для Mac от имени игры. Windows-клиент Steam не нужен.

```bash
bin/soju-bridge build <steam_api64.dll игры>          # мост в ~/Library/Caches/soju/bridge/<sha256 DLL>
bin/soju-bridge exec <мост> <appid> soju Game.exe     # хелпер + игра; заглушку кладут в игру вместо оригинала
bin/soju-bridge test 1.64                              # сверка с нативным Steam на Spacewar (480)
```

Вместо пути к DLL можно указать версию SDK — тогда берётся эталонный `steam_api64.dll` из Steamworks.NET (`bin/soju-bridge sdks` — каталог). carafe делает всё это сам.

Работают оба способа, которыми игры зовут Steamworks:

- **flat API** — Unity со Steamworks.NET и подобные обёртки. Проверено на Megabonk (лидерборды с никами и аватарами) и тестом на Spacewar.
- **C++-интерфейсы** — Unreal и свои движки: `SteamUser()` и прочие аксессоры, методы через vtable, `CCallback`/`CCallResult` с `SteamAPI_RunCallbacks`. Проверено тестом на Spacewar, на реальной C++-игре пока нет.

Как устроено:

- SDK игры определяется по версиям интерфейсов, зашитым в её `steam_api64.dll` (`bridge/sdk_detect.py`). Если подходят несколько SDK, берётся самый полный.
- Каталог SDK — `bridge/sdk/<версия>/`: `steam_api.json` из Steamworks.NET и `source` — коммит Steamworks.NET с `libsteam_api.dylib` того же SDK и эталонным `steam_api64.dll`, у обоих sha256. Сейчас это 1.57, 1.60, 1.61, 1.62, 1.63 и 1.64. Новый SDK — новый каталог и строка в матрице `.github/workflows/bridge.yml`.
- `bridge/gen.py` генерирует обе стороны из `steam_api.json` и самой DLL игры. Колбэки и структуры перекладываются между pack 8 (Windows) и pack 4 (Mac).
- Слоты vtable берутся из кода flat-обёрток той же DLL: `mov rax,[rcx]; jmp [rax+N]`. В json нет приватных методов SDK, а место в vtable они занимают, поэтому json один не годится. Отсюда же видно, где MSVC переставляет перегрузки и где возвращает `CSteamID` через скрытый указатель.
- Интерфейс отдаётся игре объектом с vtable, и flat-функции принимают его наравне с сырым указателем. Версию интерфейса не из SDK игры заглушка не выдаёт: вызов по чужой vtable уронил бы игру.
- Связь — TCP на 127.0.0.1 с одноразовым токеном (`SOJU_BRIDGE_PORT`, `SOJU_BRIDGE_TOKEN`). Хелпер живёт, пока игра подключена. Лог вызовов — `SOJU_BRIDGE_LOG`.
- Заглушка собирается тем же llvm-mingw, что и Wine. Пересборка подменяет файлы переименованием, запущенная игра её переживает.
- `soju-steamd cloud status|pull|push` синхронизирует сейвы Steam Auto-Cloud через сервис `Cloud` клиента Steam — его вызывает carafe.

### Проверка

При каждой сборке `bridge/build.sh` сверяет с компиляторами то, что вычислил генератор:

- раскладки всех структур (`bridge/test/layout_check.py`): sizeof и offsetof у clang под Windows x64 (pack 8) и под Mac arm64 (pack 4);
- слоты vtable (`bridge/test/gen_header.py` + `vt_check.py`): по слотам из DLL восстанавливается C++-заголовок, clang под MSVC раскладывает его, и раскладка должна совпасть со слотами DLL.

CI (`.github/workflows/bridge.yml`) делает это для каждого SDK каталога и собирает тесты; Steam ему не нужен.

`bin/soju-bridge test` запускает `bridge/test/steamtest.c` дважды: под Mac с настоящим `libsteam_api.dylib` и под Wine через мост, оба от имени Spacewar (appid 480, тестовое приложение Valve). Вывод должен совпасть строка в строку.

- Flat API: ник, аватар, страна; ачивка — выдать, проверить на сервере, вернуть как было; стат `NumGames` +1; лидерборд — найти, отправить 1 с `KeepBest`, скачать свою запись с деталями; Steam Cloud — записать, прочитать, удалить файл. Call result — и через ManualDispatch, как в Steamworks.NET, и через `ISteamUtils::GetAPICallResult`, как в C++-играх.
- C++ (`bridge/test/cpptest.cpp`) компилируется в двух ABI: clang++ под Mac (Itanium) и clang под `x86_64-pc-windows-msvc` (как MSVC у игр). Проверяются инлайновый аксессор через `SteamInternal_ContextInit`, `GetSteamID()` через скрытый указатель, интерфейсы через методы `ISteamClient`, обе перегрузки `GetStat`, `CCallback` на `UserStatsStored_t` и `CCallResult` на `FindLeaderboard`.

Spacewar после теста остаётся в истории игр, а в его лидерборде — запись с результатом 1.

Открытый вопрос: под мостом примерно в 3 прогонах из 15 сервер не показал переключение ачивки даже через 7 секунд, хотя `StoreStats` ответил успехом. Нативно такого не было ни разу из 17. Причина не найдена, поэтому тест печатает EResult и номер попытки.

## CI

- `bridge.yml` — мост под каждый SDK каталога: проверки раскладок и vtable, сборка заглушки, хелпера и тестов.
- `engine.yml` — полная сборка движка и `scripts/smoke-engine.sh`: префикс без окон, Windows 10, 64 и 32 бита.
