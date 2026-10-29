#!/bin/bash
# Быстрая проверка собранного движка: префикс без окон, Windows 10, 64 и 32 бита (wow64).
#
#   scripts/smoke-engine.sh [движок]      по умолчанию dist/soju-engine
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"
ENGINE="${1:-$SOJU_ROOT/dist/soju-engine}"
[[ -x "$ENGINE/bin/soju" ]] || die "нет движка $ENGINE"
MINGW="$(ensure_mingw)"

t="$(mktemp -d "${TMPDIR:-/tmp}/soju-smoke.XXXXXX")"
trap 'WINEPREFIX="$t/pfx" "$ENGINE/bin/wineserver" -k 2>/dev/null || true; rm -rf "$t"' EXIT
export WINEPREFIX="$t/pfx" WINEDEBUG=-all
fail=0
check() { if [[ "$2" == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: «$2», ждали «$3»"; fail=1; fi; }

# Окно «Установить Mono?» повесило бы wineboot — ограничиваем по времени.
( "$ENGINE/bin/soju" wineboot --init >/dev/null 2>&1; "$ENGINE/bin/wineserver" -w ) &
pid=$!
for _ in $(seq 300); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
if kill -0 "$pid" 2>/dev/null; then kill "$pid"; die "wineboot не закончил за 5 минут"; fi
check "Mono в префиксе" "$([[ -d "$WINEPREFIX/drive_c/windows/mono" ]] && echo yes)" yes
check "версия Windows" "$("$ENGINE/bin/soju" cmd /c ver 2>/dev/null | tr -d '\r' | grep -o 'Windows 10' | head -1)" "Windows 10"

"$MINGW/bin/x86_64-w64-mingw32-clang" -O1 -o "$t/bits64.exe" "$SOJU_ROOT/tests/bits.c"
"$MINGW/bin/i686-w64-mingw32-clang" -O1 -o "$t/bits32.exe" "$SOJU_ROOT/tests/bits.c"
check "64 бита" "$("$ENGINE/bin/soju" "$t/bits64.exe" 2>/dev/null | tr -d '\r')" "ptr=8 wow64=0"
check "32 бита через wow64" "$("$ENGINE/bin/soju" "$t/bits32.exe" 2>/dev/null | tr -d '\r')" "ptr=4 wow64=1"
exit "$fail"
