#!/bin/bash
# Подключить D3DMetal к собранному движку soju.
#
#   scripts/install-d3dmetal.sh <wine из GPTK> [движок]     движок по умолчанию dist/soju-engine
#
# <wine из GPTK> — каталог с lib/external/libd3dshared.dylib и lib/external/D3DMetal.framework,
# например .../Game Porting Toolkit.app/Contents/Resources/wine. D3DMetal — библиотека Apple,
# в репозиторий и сборки soju она не попадает: её берут из своей копии GPTK.
#
# Что делается:
#  - D3DMetal.framework и libd3dshared.dylib копируются в lib/external;
#  - d3d10/d3d11/d3d12/dxgi и заглушки nvapi/nvngx/atidxx (PE) кладутся поверх встроенных Wine;
#  - их unix-половины — ссылки на libd3dshared.dylib, как в GPTK.
# Запуск с D3DMetal: CX_APPLEGPTK_LIBD3DSHARED_PATH=<движок>/lib/external/libd3dshared.dylib
# и CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal — это ставит bin/soju движка, когда D3DMetal на месте.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GPTK="${1:?нужен каталог wine из Game Porting Toolkit}"
ENGINE="${2:-$ROOT/dist/soju-engine}"

die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }
[[ -f "$GPTK/lib/external/libd3dshared.dylib" && -d "$GPTK/lib/external/D3DMetal.framework" ]] \
  || die "в $GPTK нет lib/external/{libd3dshared.dylib,D3DMetal.framework}"
[[ -f "$ENGINE/SOJU" ]] || die "$ENGINE — не движок soju (нет файла SOJU)"

mkdir -p "$ENGINE/lib/external"
rm -rf "$ENGINE/lib/external/D3DMetal.framework"
cp -a "$GPTK/lib/external/D3DMetal.framework" "$GPTK/lib/external/libd3dshared.dylib" "$ENGINE/lib/external/"

n=0
for arch in x86_64-windows i386-windows; do
  for dll in "$GPTK/lib/wine/$arch/"*.dll; do
    name="$(basename "$dll" .dll)"
    # Берём только DLL, у которых в GPTK есть unix-половина D3DMetal (x86_64-unix/<name>.so).
    [[ -L "$GPTK/lib/wine/x86_64-unix/$name.so" ]] || continue
    cp -f "$dll" "$ENGINE/lib/wine/$arch/"
    n=$((n + 1))
  done
done
for so in "$GPTK/lib/wine/x86_64-unix/"*.so; do
  [[ -L "$so" && "$(readlink "$so")" == *libd3dshared.dylib ]] || continue
  ln -sf ../../external/libd3dshared.dylib "$ENGINE/lib/wine/x86_64-unix/$(basename "$so")"
done
xattr -dr com.apple.quarantine "$ENGINE/lib/external" 2>/dev/null || true
sed -i '' '/^d3dmetal /d' "$ENGINE/SOJU"
printf 'd3dmetal %s\n' "$(defaults read "$GPTK/lib/external/D3DMetal.framework/Resources/Info.plist" CFBundleShortVersionString 2>/dev/null || echo '?')" >> "$ENGINE/SOJU"
echo "D3DMetal подключён: $n DLL"
