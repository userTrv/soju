#!/bin/bash
# Архив движка для раздачи: soju-engine-<версия>.tar.xz, .sha256 к нему и манифест engine.json.
#
#   scripts/package-engine.sh [движок] [каталог-результата] [версия]
#
# По умолчанию движок dist/soju-engine, результат — dist/release, версия — <crossover>-<патчи>
# из файла SOJU движка (для релиза по тегу engine-v<версия> — версия из тега).
#
# Архив — такой, будто install-d3dmetal не запускали: D3DMetal — библиотека Apple, раздавать её
# нельзя. Подключённый D3DMetal вычищается из копии движка (исходный движок не меняется):
# lib/external, ссылки x86_64-unix/*.so → libd3dshared.dylib, PE-DLL D3DMetal (вместо d3d1x/dxgi
# возвращаются DLL Wine из объектников сборки), строка d3dmetal в SOJU. Затем проверка: что-то
# от D3DMetal осталось — ошибка. В движок кладётся bin/soju-d3dmetal: им carafe подключает
# D3DMetal из копии GPTK пользователя.
#
# Размер: с Mach-O (bin, x86_64-unix, dylib) снимается отладочная информация (strip -S), с PE —
# llvm-strip --strip-debug из llvm-mingw. Под конец архив распаковывается во временный каталог
# и на нём идёт scripts/smoke-engine.sh.
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"
ROOT="$SOJU_ROOT"
ENGINE="${1:-$ROOT/dist/soju-engine}"
OUT="${2:-$ROOT/dist/release}"
[[ -f "$ENGINE/SOJU" && -x "$ENGINE/bin/wine" ]] || die "$ENGINE — не движок soju (нет SOJU или bin/wine)"
ENGINE="$(cd "$ENGINE" && pwd)"

soju_field() { sed -n "s/^$1 //p" "$ENGINE/SOJU" | head -1; }
CX="$(soju_field crossover)" WINE_VER="$(soju_field wine)" PID="$(soju_field patches)" BUILT="$(soju_field built)"
[[ -n "$CX" && -n "$PID" ]] || die "в $ENGINE/SOJU нет строк crossover и patches"
VERSION="${3:-$CX-$PID}"
[[ "$VERSION" =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]] || die "версия «$VERSION»: только буквы, цифры, . _ -"
NAME="soju-engine-$VERSION"
MINGW="$(ensure_mingw)"
STRIP_PE="$MINGW/bin/llvm-strip"

mkdir -p "$OUT" "$W/tmp"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d "$W/tmp/package.XXXXXX")"
trap 'rm -rf "$t"' EXIT
STAGE="$t/stage/$NAME"
mkdir -p "$t/stage"

kb() { du -sk "$1" | cut -f1; }
mb() { echo "$(( ($1 + 512) / 1024 )) МБ"; }

# Имена DLL D3DMetal: всё, у чего unix-половина — ссылка на libd3dshared.dylib, и известные имена.
D3DM_KNOWN="d3d10 d3d11 d3d12 dxgi nvapi64 nvngx nvngx-on-metalfx atidxx64"

say "копирую $ENGINE"
cp -a "$ENGINE" "$STAGE"

# --- вычистить D3DMetal ---
OBJ="$W/obj/wine-$CX"
names="$D3DM_KNOWN"
while IFS= read -r so; do
  [[ "$(readlink "$so")" == *libd3dshared* ]] || continue
  n="$(basename "$so" .so)"
  [[ " $names " == *" $n "* ]] || names="$names $n"
  rm -f "$so"
done < <(find "$STAGE/lib/wine/x86_64-unix" -type l)
if [[ -e "$STAGE/lib/external" ]] || grep -q '^d3dmetal ' "$STAGE/SOJU"; then
  say "убираю D3DMetal из копии"
  # DLL Wine с теми же именами берутся из объектников той же сборки — только если патчи совпадают.
  obj_ok=0
  [[ -d "$OBJ" && "$(cat "$W/src/crossover-$CX/.patches" 2>/dev/null)" == "$PID" ]] && obj_ok=1
  for n in $names; do
    for arch in x86_64-windows i386-windows; do
      f="$STAGE/lib/wine/$arch/$n.dll" o="$OBJ/dlls/$n/$arch/$n.dll"
      [[ -f "$f" ]] || continue
      if [[ -f "$o" && "$obj_ok" == 1 ]]; then cp -f "$o" "$f"
      elif [[ -d "$OBJ/dlls/$n" ]]; then die "$arch/$n.dll: нужна DLL Wine из объектников сборки $PID, а в $OBJ другая сборка или её нет"
      else rm -f "$f"
      fi
    done
  done
  rm -rf "$STAGE/lib/external"
  sed -i '' '/^d3dmetal /d' "$STAGE/SOJU"
fi
sed -i '' '/^version /d' "$STAGE/SOJU"
printf 'version %s\n' "$VERSION" >> "$STAGE/SOJU"
install -m 755 "$ROOT/scripts/install-d3dmetal.sh" "$STAGE/bin/soju-d3dmetal"

# check_clean ДВИЖОК [full] — от D3DMetal ничего не осталось. full — ещё и поиск по содержимому
# (до strip: метки сборки Apple живут в отладочной информации) и сверка DLL с объектниками.
check_clean() {
  local e="$1" bad=0 f n arch o
  complain() { printf 'D3DMetal в архиве: %s\n' "$*" >&2; bad=1; }
  [[ -e "$e/lib/external" ]] && complain "lib/external"
  grep -q '^d3dmetal ' "$e/SOJU" && complain "строка d3dmetal в SOJU"
  while IFS= read -r f; do complain "${f#"$e"/}"; done < <(find "$e" \( -iname '*d3dshared*' -o -iname 'D3DMetal*' \))
  while IFS= read -r f; do
    [[ "$(readlink "$f")" == *d3dshared* || "$(readlink "$f")" == *external/* ]] && complain "ссылка ${f#"$e"/} → $(readlink "$f")"
  done < <(find "$e" -type l)
  for n in nvapi64 nvngx nvngx-on-metalfx atidxx64; do
    for arch in x86_64-windows i386-windows; do
      [[ -e "$e/lib/wine/$arch/$n.dll" ]] && complain "lib/wine/$arch/$n.dll"
    done
  done
  if [[ "${2:-}" == full ]]; then
    # D3DMetalDLLs, D3D4Mac, com.apple.xbs — пути сборки Apple в DLL D3DMetal; у Wine их нет
    # (Wine сам упоминает только libd3dshared.dylib и «D3DMetal dxgi», это не они).
    while IFS= read -r f; do complain "${f#"$e"/} (сборка Apple)"; done \
      < <(grep -rlaE 'D3DMetalDLLs|D3D4Mac|com\.apple\.xbs' "$e/lib" || true)
    if [[ -d "$OBJ" && "$(cat "$W/src/crossover-$CX/.patches" 2>/dev/null)" == "$PID" ]]; then
      for n in $D3DM_KNOWN; do
        for arch in x86_64-windows i386-windows; do
          f="$e/lib/wine/$arch/$n.dll" o="$OBJ/dlls/$n/$arch/$n.dll"
          [[ -f "$f" && -f "$o" ]] || continue
          cmp -s "$f" "$o" || complain "lib/wine/$arch/$n.dll не совпадает с DLL Wine из $OBJ"
        done
      done
    else
      say "объектников сборки $PID нет — DLL с объектниками не сверяю"
    fi
  fi
  [[ "$bad" == 0 ]]
}
check_clean "$STAGE" full || die "в архив попал D3DMetal"

# --- strip ---
before="$(kb "$STAGE")"
pe_marks() { grep -rlaF 'Wine builtin DLL' "$STAGE/lib/wine" | wc -l | tr -d ' '; }
marks="$(pe_marks)"
say "снимаю отладочную информацию"
# Mach-O: bin, x86_64-unix, dylib. Ссылки не трогаем — strip пошёл бы по ним.
find "$STAGE/bin" "$STAGE/lib" -path "$STAGE/lib/wine/*-windows" -prune -o -type f -print0 \
  | xargs -0 file --print0 | while IFS= read -r -d '' f && IFS= read -r desc; do
      [[ "$desc" == *Mach-O* ]] && printf '%s\0' "$f"
    done | xargs -0 strip -S 2>"$t/strip.log"
# У части .so отладочной информации нет и так — strip об этом предупреждает, это не ошибка.
grep -v 'already stripped' "$t/strip.log" >&2 || true
# PE: только PE32/PE32+, не NE-шрифты и не данные.
find "$STAGE/lib/wine" -path '*-windows/*' -type f -print0 \
  | xargs -0 file --print0 | while IFS= read -r -d '' f && IFS= read -r desc; do
      [[ "$desc" == *PE32* ]] && printf '%s\0' "$f"
    done | xargs -0 -n 64 "$STRIP_PE" --strip-debug
[[ "$(pe_marks)" == "$marks" ]] || die "после llvm-strip у DLL пропала метка «Wine builtin DLL»: было $marks, стало $(pe_marks)"
after="$(kb "$STAGE")"
check_clean "$STAGE" || die "в архив попал D3DMetal"

# --- архив ---
ARCHIVE="$OUT/$NAME.tar.xz"
say "упаковываю $NAME.tar.xz"
rm -f "$ARCHIVE" "$ARCHIVE.sha256"
# Без расширенных атрибутов и ._-файлов macOS; владелец — root, у пользователя файлы станут его.
# xz — встроенный в tar (libarchive): отдельная утилита не нужна. Он однопоточный, -9 — пара минут.
COPYFILE_DISABLE=1 tar -C "$t/stage" --no-mac-metadata --no-xattrs --uid 0 --gid 0 --uname root --gname wheel \
  --options "xz:compression-level=9" -cJf "$ARCHIVE.part" "$NAME"
mv "$ARCHIVE.part" "$ARCHIVE"
SHA="$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)"
(cd "$OUT" && printf '%s  %s\n' "$SHA" "$NAME.tar.xz" > "$NAME.tar.xz.sha256")
SIZE="$(stat -f %z "$ARCHIVE")"
MACOS="$(otool -l "$STAGE/bin/wine" | awk '/LC_BUILD_VERSION/ {b=1} b && $1 == "minos" {print $2; exit}')"
[[ -n "$MACOS" ]] || MACOS="${MACOSX_DEPLOYMENT_TARGET:-11.0}"

# engine.json — манифест для carafe, формат описан в README («Релизы»).
cat > "$OUT/engine.json" <<EOF
{
  "format": 1,
  "name": "soju-engine",
  "version": "$VERSION",
  "crossover": "$CX",
  "wine": "$WINE_VER",
  "patches": "$PID",
  "built": "$BUILT",
  "archive": "$NAME.tar.xz",
  "root": "$NAME",
  "sha256": "$SHA",
  "size": $SIZE,
  "unpacked_size": $((after * 1024)),
  "arch": "x86_64",
  "rosetta": true,
  "macos_min": "$MACOS",
  "d3dmetal": {
    "bundled": false,
    "required": false,
    "install": "bin/soju-d3dmetal"
  }
}
EOF
plutil -lint -s "$OUT/engine.json" >/dev/null 2>&1 || python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$OUT/engine.json" \
  || die "engine.json не разбирается"

# --- проверка распакованного ---
say "распаковываю и проверяю"
mkdir -p "$t/unpack"
(cd "$OUT" && shasum -a 256 -c --status "$NAME.tar.xz.sha256") || die "sha256 архива не сошёлся"
tar -C "$t/unpack" -xJf "$ARCHIVE"
rm -rf "$t/stage"
U="$t/unpack/$NAME"
[[ -x "$U/bin/soju" && -x "$U/bin/soju-d3dmetal" ]] || die "в архиве нет bin/soju или bin/soju-d3dmetal"
check_clean "$U" || die "в распакованном архиве D3DMetal"
"$ROOT/scripts/smoke-engine.sh" "$U" || die "smoke-engine.sh на распакованном архиве не прошёл"

say "движок $(mb "$before") → после strip $(mb "$after"), архив $(mb $((SIZE / 1024)))"
say "готово: $ARCHIVE"
