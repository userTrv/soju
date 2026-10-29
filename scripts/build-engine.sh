#!/bin/bash
# Сборка движка soju: Wine из исходников CrossOver + наши патчи, x86_64 для Rosetta,
# Windows-часть для i386 и x86_64 (wow64 — 32-битные игры без 32-битного хоста).
#
#   scripts/build-engine.sh [каталог-результата]      по умолчанию dist/soju-engine
#
# Рабочие файлы (архивы, исходники, объектники) — в $SOJU_WORK, по умолчанию
# ~/Library/Caches/soju. Повторный запуск пересобирает только изменившееся.
# Нужны: Xcode Command Line Tools, Rosetta 2, bison >= 3 (brew install bison).
set -euo pipefail

# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"
ROOT="$SOJU_ROOT"
OUT="${1:-$ROOT/dist/soju-engine}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

# Хост движка — x86_64 под Rosetta. Компилятор при этом работает нативно:
# --host=x86_64-apple-darwin, а тестовые программы configure запускает Rosetta,
# поэтому autoconf не считает сборку кросс-компиляцией.
HOST=x86_64-apple-darwin
export MACOSX_DEPLOYMENT_TARGET=11.0
# Компиляторы Apple по полному пути: в PATH дальше будет llvm-mingw со своим clang.
# Без обёртки /usr/bin/clang SDK надо назвать явно.
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
CC_X64="$(xcrun -f clang) -arch x86_64"
CXX_X64="$(xcrun -f clang++) -arch x86_64"

arch -x86_64 /usr/bin/true 2>/dev/null || die "нужна Rosetta 2: softwareupdate --install-rosetta"
BISON="$(brew --prefix bison 2>/dev/null)/bin/bison"
[[ -x "$BISON" ]] || BISON="$(command -v bison)"
"$BISON" --version | head -1 | grep -qE ' 3\.' || die "нужен bison 3.x: brew install bison"

# Отпечаток патчей: записывается в SOJU, по нему видно, из чего собран движок.
patches_id() {
  find "$ROOT/patches/wine" -name '*.patch' -type f | sort | xargs cat 2>/dev/null | shasum -a 256 | cut -c1-12
}

mkdir -p "$W"/{dl,src,obj,tools,deps}

# --- инструменты ---
MINGW="$(ensure_mingw)"

# --- исходники ---
CX_TAR="$W/dl/$(basename "$CROSSOVER_URL")"
fetch "$CROSSOVER_URL" "$CROSSOVER_SHA256" "$CX_TAR"
PID="$(patches_id)"
# Исходники и объектники лежат на постоянном месте. Сменились патчи — дерево собирается
# заново рядом, а в рабочее переносятся только файлы с другим содержимым: make пересоберёт
# лишь то, что затронули патчи.
SRC="$W/src/crossover-$CROSSOVER_VERSION"
if [[ "$(cat "$SRC/.patches" 2>/dev/null)" != "$PID" ]]; then
  say "готовлю исходники CrossOver $CROSSOVER_VERSION"
  rm -rf "$SRC.new" && mkdir -p "$SRC.new"
  tar xzf "$CX_TAR" -C "$SRC.new" --strip-components 1 sources/wine
  for p in $(find "$ROOT/patches/wine" -name '*.patch' -type f | sort); do
    say "патч $(basename "$p")"
    patch -d "$SRC.new/wine" -p1 --forward --quiet < "$p"
  done
  mkdir -p "$SRC"
  rsync -rlc --delete --exclude /.patches "$SRC.new/" "$SRC/"
  rm -rf "$SRC.new"
  echo "$PID" > "$SRC/.patches"
fi

# --- freetype (шрифты; без него игры остаются без текста) ---
DEPS="$W/deps/x86_64"
if [[ ! -f "$DEPS/lib/libfreetype.6.dylib" ]]; then
  FT_TAR="$W/dl/$(basename "$FREETYPE_URL")"
  fetch "$FREETYPE_URL" "$FREETYPE_SHA256" "$FT_TAR"
  say "собираю freetype $FREETYPE_VERSION"
  rm -rf "$W/obj/freetype" && mkdir -p "$W/obj/freetype"
  tar xJf "$FT_TAR" -C "$W/obj/freetype" --strip-components 1
  (cd "$W/obj/freetype" && ./configure --host="$HOST" --prefix="$DEPS" \
      CC="$CC_X64" --enable-shared --disable-static \
      --without-png --without-bzip2 --without-brotli --without-harfbuzz --with-zlib=yes >/dev/null \
    && make -j"$JOBS" >/dev/null && make install >/dev/null)
  install_name_tool -id @rpath/libfreetype.6.dylib "$DEPS/lib/libfreetype.6.dylib"
fi

# --- Wine ---
OBJ="$W/obj/wine-$CROSSOVER_VERSION"
export PATH="$MINGW/bin:$(dirname "$BISON"):$PATH"
# pkg-config видит только наши x86_64-зависимости, не arm64-библиотеки Homebrew.
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH=
if [[ ! -f "$OBJ/Makefile" ]]; then
  say "configure Wine"
  mkdir -p "$OBJ"
  (cd "$OBJ" && "$SRC/wine/configure" --host="$HOST" --prefix="$OUT" \
      CC="$CC_X64" CXX="$CXX_X64" \
      LDFLAGS="-Wl,-rpath,@loader_path/../.." \
      FREETYPE_CFLAGS="-I$DEPS/include/freetype2" FREETYPE_LIBS="-L$DEPS/lib -lfreetype" \
      --enable-archs=i386,x86_64 --disable-tests --disable-win16 \
      --with-mingw --with-freetype --with-coreaudio --with-opencl \
      --without-x --without-wayland --without-alsa --without-oss --without-pulse --without-udev \
      --without-v4l2 --without-capi --without-dbus --without-gphoto --without-sane --without-krb5 \
      --without-netapi --without-pcap --without-usb --without-gstreamer --without-sdl \
      --without-gnutls --without-vulkan --without-inotify --without-gssapi --without-cups \
      > "$OBJ.configure.log" 2>&1) || { tail -30 "$OBJ.configure.log"; die "configure упал, лог: $OBJ.configure.log"; }
  grep -E '^configure: (WARNING|error)' "$OBJ.configure.log" || true
fi

say "собираю Wine ($JOBS потоков)"
# Инструменты сборки (tools/sfnt2fon) ищут freetype по rpath @loader_path/../.. — это корень
# объектников. DYLD_* не годится: SIP стирает их при запуске /usr/bin/make.
ln -sf "$DEPS/lib/libfreetype.6.dylib" "$OBJ/libfreetype.6.dylib"
make -C "$OBJ" -j"$JOBS" > "$OBJ.make.log" 2>&1 || { grep -E 'error' "$OBJ.make.log" | tail -30; die "make упал, лог: $OBJ.make.log"; }

say "устанавливаю в $OUT"
rm -rf "$OUT"
make -C "$OBJ" install-lib > "$OBJ.install.log" 2>&1 || { tail -30 "$OBJ.install.log"; die "make install упал"; }
cp -a "$DEPS/lib/"libfreetype*.dylib "$OUT/lib/"
for addon in mono:WINE_MONO gecko:WINE_GECKO_X86 gecko:WINE_GECKO_X64; do
  dir="${addon%%:*}" var="${addon#*:}"
  url="$(eval echo "\$${var}_URL")" sha="$(eval echo "\$${var}_SHA256")"
  fetch "$url" "$sha" "$W/dl/$(basename "$url")"
  mkdir -p "$OUT/share/wine/$dir"
  cp "$W/dl/$(basename "$url")" "$OUT/share/wine/$dir/"
done
# bin/soju — wine с настройками движка: msync, D3DMetal, если подключён.
cat > "$OUT/bin/soju" <<'EOF'
#!/bin/bash
# soju <программа> [аргументы] — как wine, с настройками движка.
here="$(cd "$(dirname "$0")/.." && pwd)"
export WINEMSYNC="${WINEMSYNC:-1}"
d3dm="$here/lib/external/libd3dshared.dylib"
if [[ -f "$d3dm" && "${SOJU_D3DMETAL:-1}" == 1 ]]; then
  export CX_APPLEGPTK_LIBD3DSHARED_PATH="$d3dm" CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal
fi
exec "$here/bin/wine" "$@"
EOF
chmod +x "$OUT/bin/soju"
cat > "$OUT/SOJU" <<EOF
crossover $CROSSOVER_VERSION
wine $(sed 's/^Wine version //' "$SRC/wine/VERSION")
patches $PID
built $(date -u +%Y-%m-%dT%H:%MZ)
EOF
say "готово: $OUT ($(du -sh "$OUT" | cut -f1))"
