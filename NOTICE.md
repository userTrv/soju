# Состав soju и лицензии

Код soju (скрипты, патчи, мост Steamworks) распространяется по GNU LGPL 2.1 или любой более
поздней версии — см. `LICENSE`. Так же лицензирован Wine, на котором собран движок.

## Что входит в архив движка

| Часть | Лицензия | Исходники |
|---|---|---|
| Wine из исходников CrossOver | LGPL 2.1+ (`share/doc/soju/wine/`) | исходники CrossOver той же версии (`CROSSOVER_URL` в `sources.lock`) и патчи из `patches/wine/` этого репозитория на коммите релиза |
| FreeType | FTL или GPLv2 на выбор (`share/doc/soju/freetype/`) | `FREETYPE_URL` в `sources.lock` |
| Wine Mono (`share/wine/mono`) | MIT и лицензии Mono, текст внутри пакета | [wine-mono](https://gitlab.winehq.org/mono/wine-mono) |
| Wine Gecko (`share/wine/gecko`) | MPL 2.0, текст внутри пакета | [wine-gecko](https://gitlab.winehq.org/wine/wine-gecko) |

Windows-часть собрана llvm-mingw; его рантайм (mingw-w64, compiler-rt, libc++) распространяется
по свободным лицензиям, разрешающим включение в собранные программы.

## Чего в архиве нет

- **D3DMetal** — часть Game Porting Toolkit Apple, распространять его нельзя. `bin/soju-d3dmetal`
  подключает его из копии GPTK, установленной у пользователя.
- **Steamworks SDK** (`libsteam_api.dylib`, эталонные `steam_api64.dll`) — собственность Valve.
  `bin/soju-bridge` скачивает нужный файл при сборке моста на машине пользователя; в релизы и в
  репозиторий эти файлы не попадают.

soju не связан с CodeWeavers, Apple или Valve. Steam — товарный знак Valve Corporation.
