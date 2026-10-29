# Общее для скриптов soju: пути, загрузки с проверкой sha256, кросс-компилятор.
# Подключается через `source`; ждёт, что вызывающий уже сделал `set -euo pipefail`.

SOJU_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Рабочий каталог: архивы, исходники, объектники, инструменты, собранные мосты.
W="${SOJU_WORK:-$HOME/Library/Caches/soju}"
# shellcheck source=../sources.lock
source "$SOJU_ROOT/sources.lock"

say() { printf '\033[1m==> %s\033[0m\n' "$*" >&2; }
die() { printf 'ошибка: %s\n' "$*" >&2; exit 1; }

# fetch URL SHA256 FILE [ЗАГОЛОВОК] — скачать и сверить хеш; уже скачанный файл тоже сверяется.
# URL может быть списком зеркал через пробел: пробуются по очереди (хеш один, так что зеркало
# не может подсунуть другой файл).
# Для api.github.com берётся GITHUB_TOKEN, если задан (в CI без него упираемся в лимит запросов):
# заголовок идёт через файловый дескриптор, чтобы токен не попал в аргументы процесса.
fetch() {
  local urls="$1" sha="$2" file="$3" hdr="${4:-}" url ok=0
  if [[ ! -f "$file" ]]; then
    say "скачиваю $(basename "$file")"
    mkdir -p "$(dirname "$file")"
    local args=(-fsSL --retry 3 -A 'Mozilla/5.0' -o "$file.part")
    [[ -n "$hdr" ]] && args+=(-H "$hdr")
    for url in $urls; do
      if [[ -n "${GITHUB_TOKEN:-}" && "$url" == https://api.github.com/* ]]; then
        curl "${args[@]}" -H @<(printf 'Authorization: Bearer %s\n' "$GITHUB_TOKEN") "$url" && { ok=1; break; }
      else
        curl "${args[@]}" "$url" && { ok=1; break; }
      fi
      say "не скачалось с $url"
    done
    [[ "$ok" == 1 ]] || { rm -f "$file.part"; die "не скачал $(basename "$file") ни с одного адреса"; }
    mv "$file.part" "$file"
  fi
  echo "$sha  $file" | shasum -a 256 -c --status || { rm -f "$file"; die "sha256 не совпал: $file"; }
}

# llvm-mingw: PE-компилятор и для Wine, и для моста Steamworks. Печатает каталог инструмента.
ensure_mingw() {
  local dir="$W/tools/llvm-mingw-$LLVM_MINGW_VERSION" tar="$W/dl/$(basename "$LLVM_MINGW_URL")"
  if [[ ! -x "$dir/bin/x86_64-w64-mingw32-clang" ]]; then
    fetch "$LLVM_MINGW_URL" "$LLVM_MINGW_SHA256" "$tar"
    say "распаковываю llvm-mingw"
    rm -rf "$dir" "$dir.tmp" && mkdir -p "$dir.tmp"
    tar xJf "$tar" -C "$dir.tmp" --strip-components 1
    mv "$dir.tmp" "$dir"
  fi
  echo "$dir"
}
