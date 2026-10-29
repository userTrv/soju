#!/bin/bash
# Подключить D3DMetal к собранному движку soju.
#
#   scripts/install-d3dmetal.sh [источник] [движок]     движок по умолчанию dist/soju-engine
#   <движок>/bin/soju-d3dmetal [источник]               то же из готового движка (так зовёт carafe)
#
# D3DMetal — библиотека Apple, в репозиторий и архивы soju она не попадает: её берут из своей
# копии Game Porting Toolkit. Без источника (или с пустым "") скрипт ищет его сам, по порядку:
#  а) движок GPTK, который скачал carafe: ~/Library/Application Support/carafe/engines/*/Game Porting Toolkit.app;
#  б) приложение /Applications/Game Porting Toolkit*.app (и ~/Applications);
#  в) смонтированный образ GPTK от Apple: /Volumes/Game Porting Toolkit*, /Volumes/Evaluation environment
#     for Windows games* — D3DMetal там в redist/lib;
#  г) CrossOver.app: Contents/SharedSupport/CrossOver/lib64/apple_gptk.
# Источником можно назвать и любой из этих каталогов явно: .app, его Contents/Resources/wine,
# корень образа, каталог lib — годится всё, где есть external/libd3dshared.dylib и external/D3DMetal.framework.
#
# Что делается:
#  - D3DMetal.framework и libd3dshared.dylib копируются в lib/external;
#  - d3d10/d3d11/d3d12/dxgi и заглушки nvapi/nvngx/atidxx (PE) кладутся поверх встроенных Wine;
#  - их unix-половины — ссылки на libd3dshared.dylib, как в GPTK.
# Запуск с D3DMetal: CX_APPLEGPTK_LIBD3DSHARED_PATH=<движок>/lib/external/libd3dshared.dylib
# и CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal — это ставит bin/soju движка, когда D3DMetal на месте.
set -euo pipefail

die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

# Скрипт лежит либо в scripts/ репозитория, либо в bin/ движка (bin/soju-d3dmetal).
HERE="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -f "$HERE/SOJU" ]]; then DEFAULT_ENGINE="$HERE"; else DEFAULT_ENGINE="$HERE/dist/soju-engine"; fi
SRC="${1:-}"
ENGINE="${2:-$DEFAULT_ENGINE}"
[[ -f "$ENGINE/SOJU" ]] || die "$ENGINE — не движок soju (нет файла SOJU)"

# gptk_lib КАТАЛОГ — печатает каталог lib источника (в нём external/ и wine/), если это GPTK.
gptk_lib() {
  local d
  for d in "$1/lib" "$1" "$1/redist/lib" "$1/Contents/Resources/wine/lib"; do
    if [[ -f "$d/external/libd3dshared.dylib" && -d "$d/external/D3DMetal.framework" ]]; then
      echo "$d"; return 0
    fi
  done
  return 1
}

# Кандидаты по порядку а–г; внутри группы — сначала новые версии.
candidates() {
  local carafe="$HOME/Library/Application Support/carafe/engines"
  [[ -d "$carafe" ]] && find "$carafe" -mindepth 1 -maxdepth 1 -type d | sort -rV \
    | while IFS= read -r e; do echo "$e/Game Porting Toolkit.app"; done
  for d in /Applications "$HOME/Applications"; do
    [[ -d "$d" ]] && find "$d" -mindepth 1 -maxdepth 1 -name 'Game Porting Toolkit*.app' | sort -rV
  done
  find /Volumes -mindepth 1 -maxdepth 1 \( -name 'Game Porting Toolkit*' -o -name 'Evaluation environment for Windows games*' \) 2>/dev/null | sort -rV
  echo "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib64/apple_gptk"
  echo "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/apple_gptk"
}

if [[ -n "$SRC" ]]; then
  LIB="$(gptk_lib "$SRC")" || die "в $SRC нет D3DMetal (external/libd3dshared.dylib и external/D3DMetal.framework)"
else
  LIB=""
  while IFS= read -r c; do
    LIB="$(gptk_lib "$c")" && break
    LIB=""
  done < <(candidates)
  if [[ -z "$LIB" ]]; then
    cat >&2 <<'EOF'
ошибка: D3DMetal не найден.

D3DMetal входит в Apple Game Porting Toolkit, распространять его нельзя — нужна своя копия:
  1. Откройте https://developer.apple.com/games/game-porting-toolkit/ и войдите со своим Apple ID
     (бесплатная учётная запись разработчика подходит).
  2. Скачайте образ Game Porting Toolkit (.dmg) и откройте его двойным щелчком — он смонтируется в /Volumes.
  3. Запустите этот скрипт ещё раз: он найдёт D3DMetal в образе сам.
Или укажите источник явно: install-d3dmetal.sh "<Game Porting Toolkit.app или каталог образа>".
Подходит и уже установленный Game Porting Toolkit.app, движок GPTK от carafe или CrossOver.
EOF
    exit 1
  fi
fi
EXT="$LIB/external"
VER="$(defaults read "$EXT/D3DMetal.framework/Resources/Info.plist" CFBundleShortVersionString 2>/dev/null || echo '?')"
echo "D3DMetal $VER: $EXT"

# DLL D3DMetal — те, у которых в источнике есть unix-половина D3DMetal (x86_64-unix/<name>.so):
# ссылка на libd3dshared.dylib или её копия.
d3dm_so() { [[ -L "$1" && "$(readlink "$1")" == *libd3dshared.dylib ]] || { [[ -f "$1" && ! -L "$1" ]] && cmp -s "$1" "$EXT/libd3dshared.dylib"; }; }
names=()
for so in "$LIB/wine/x86_64-unix/"*.so; do
  [[ -e "$so" || -L "$so" ]] || continue
  d3dm_so "$so" && names+=("$(basename "$so" .so)")
done
[[ ${#names[@]} -gt 0 ]] || die "в $LIB/wine нет DLL D3DMetal (x86_64-unix/*.so → libd3dshared.dylib)"

mkdir -p "$ENGINE/lib/external"
rm -rf "$ENGINE/lib/external/D3DMetal.framework"
cp -a "$EXT/D3DMetal.framework" "$EXT/libd3dshared.dylib" "$ENGINE/lib/external/"

n=0
for name in "${names[@]}"; do
  for arch in x86_64-windows i386-windows; do
    [[ -f "$LIB/wine/$arch/$name.dll" ]] || continue
    cp -f "$LIB/wine/$arch/$name.dll" "$ENGINE/lib/wine/$arch/"
    n=$((n + 1))
  done
  ln -sf ../../external/libd3dshared.dylib "$ENGINE/lib/wine/x86_64-unix/$name.so"
done
[[ "$n" -gt 0 ]] || die "в $LIB/wine нет PE-DLL D3DMetal"
xattr -dr com.apple.quarantine "$ENGINE/lib/external" 2>/dev/null || true
sed -i '' '/^d3dmetal /d' "$ENGINE/SOJU"
printf 'd3dmetal %s\n' "$VER" >> "$ENGINE/SOJU"
echo "D3DMetal подключён к $ENGINE: $n DLL"
