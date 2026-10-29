# soju

Движок для запуска Windows-игр на macOS (Apple Silicon): Wine, собранный под игры из Steam. На нём работает [carafe](https://github.com/userTrv/carafe) — приложение для обычных пользователей.

Что soju добавляет к Wine:

- **Steam-игры без Windows-Steam.** Игра говорит со Steam для Mac через мост Steamworks: достижения, статистика, лидерборды, облачные сохранения. Второй клиент Steam внутри Wine не нужен.
- **Графика.** DirectX 11/12 → Metal через D3DMetal из Apple Game Porting Toolkit, в планах открытый DXMT. Вулкан-путь (DXVK + MoltenVK) — позже.
- **Rust там, где он к месту:** мост Steamworks, уведомления о достижениях, инструменты сборки и проверки. Само ядро Wine остаётся на C.

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
