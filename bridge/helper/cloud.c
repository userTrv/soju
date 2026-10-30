/* soju-steamd cloud: синхронизация Steam Auto-Cloud для Windows-игры через сервис Cloud
 * клиента Steam для Mac (ISteamUnifiedMessages), без браузера и без пароля.
 *
 * soju-steamd cloud status|pull|push --state FILE --backup DIR
 *     --root NAME=DIR ... --rule ROOT:PATH:PATTERN:RECURSIVE ... [--prefer local|cloud] [--log FILE]
 * Нужен SteamAppId в окружении.
 *
 * По каждому файлу — трёхстороннее сравнение с последней синхронизацией (--state): поменялся
 * только диск — выгрузить, только облако — скачать, обе стороны — конфликт. При конфликте без
 * --prefer pull и push ничего не переносят, печатают «  ! файл (конфликт: …)» и выходят с кодом 3.
 * --prefer local|cloud решает все конфликты в пользу одной стороны, вторая уходит в --backup.
 * status ничего не меняет, ни облако, ни диск, ни --state.
 *
 * soju-steamd cloud delete NAME... — удалить файлы из облака по именам, как их видит облако
 * (`%WinAppDataRoaming%sct/a.json` или `sct/a.json`). Диск и --state не трогает.
 * Коды выхода: 0 — готово, 1 — ошибка, 3 — конфликт. */
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#include <sys/file.h>

#include "steamd.h"
#include "cloud_decide.h"

extern void SteamAPI_Shutdown(void);
extern int32_t SteamAPI_GetHSteamPipe(void);
extern int32_t SteamAPI_GetHSteamUser(void);
extern void SteamAPI_RunCallbacks(void);
extern void *SteamInternal_CreateInterface(const char *ver);
extern void *SteamAPI_ISteamClient_GetISteamGenericInterface(void *self, int32_t user, int32_t pipe, const char *ver);

#define MAXF 512
#define SHA_LEN 20

typedef struct { char *name; uint8_t sha[SHA_LEN]; uint64_t ts; uint32_t size; } cfile;   /* в облаке */
typedef struct { char *name; char *path; uint8_t sha[SHA_LEN]; time_t mtime; size_t size; } lfile; /* локально */
typedef struct { char *name; char sha[SHA_LEN * 2 + 1]; } sentry;                            /* последняя синхронизация */
typedef struct { char *root; char *path; char *pattern; bool recursive; } rule;

static uint32_t g_app;
static void *g_um;
static char *g_roots[16][2];
static int g_nroots;
static rule g_rules[16];
static int g_nrules;
static cfile g_cloud[MAXF];
static int g_ncloud;
static lfile g_local[MAXF];
static int g_nlocal;
static sentry g_state[MAXF];
static int g_nstate;
static const char *g_backup;
static char g_stamp[32];

/* Свои stdout/stderr: настоящие уходят в лог, туда же пишет libsteam_api (а он печатает Steam ID). */
static FILE *g_out, *g_err;
#define OUT (g_out ? g_out : stdout)
#define ERR (g_err ? g_err : stderr)

static void quiet_steam(void) {
  fflush(stdout);
  fflush(stderr);
  int o = dup(STDOUT_FILENO), e = dup(STDERR_FILENO);
  const char *log = getenv("SOJU_BRIDGE_LOG");
  int q = log && *log ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
  if (q < 0) q = open("/dev/null", O_WRONLY);
  if (o < 0 || e < 0 || q < 0) return;
  g_out = fdopen(o, "w");
  g_err = fdopen(e, "w");
  setvbuf(g_out, NULL, _IOLBF, 0);
  setvbuf(g_err, NULL, _IONBF, 0);
  dup2(q, STDOUT_FILENO);
  dup2(q, STDERR_FILENO);
  close(q);
}

/* Для вывода: Steam ID (17 цифр с 7656) в путях сейвов — как 7656…; кольцо из 4 буферов,
 * чтобы в одном fprintf можно было замаскировать несколько строк. */
static const char *mask(const char *s) {
  static char ring[4][4096];
  static int k;
  char *o = ring[k++ & 3];
  size_t j = 0;
  for (size_t i = 0; s[i] && j + 8 < sizeof ring[0]; ) {
    size_t d = 0;
    while (isdigit((unsigned char)s[i + d])) d++;
    if (d == 17 && !strncmp(s + i, "7656", 4)) {
      memcpy(o + j, "7656\xe2\x80\xa6", 7);
      j += 7;
      i += d;
    } else if (d) {
      for (size_t t = 0; t < d && j + 8 < sizeof ring[0]; t++) o[j++] = s[i++];
    } else {
      o[j++] = s[i++];
    }
  }
  o[j] = 0;
  return o;
}

/* --- журнал: --log FILE, с отметками времени; Steam ID замаскированы, ссылок и заголовков нет --- */
static FILE *g_log;
static double g_t0;

static double now_s(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static void log_open(const char *path) {
  struct stat st;
  /* Больше 512 КБ — старое в .1, журнал с начала. */
  if (!stat(path, &st) && st.st_size > 512 * 1024) {
    char old[4200];
    snprintf(old, sizeof old, "%s.1", path);
    rename(path, old);
  }
  g_log = fopen(path, "a");
  if (g_log) setvbuf(g_log, NULL, _IOLBF, 0);
}

/* Строка журнала; %s-аргументы с путями маскирует вызывающий. */
static void logf_(const char *fmt, ...) {
  if (!g_log) return;
  char buf[2048], ts[16];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  double t = now_s();
  time_t sec = (time_t)t;
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&sec));
  fprintf(g_log, "%s.%03d +%6.2fs %s\n", ts, (int)((t - sec) * 1000), t - g_t0, buf);
}

/* Строка отчёта в stdout и в журнал. */
static void say(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fputs(buf, OUT);
  buf[strcspn(buf, "\n")] = 0;
  logf_("%s", buf);
}

/* Ошибка или предупреждение в stderr и в журнал. */
static void warn(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fputs(buf, ERR);
  buf[strcspn(buf, "\n")] = 0;
  logf_("%s", buf);
}

static void die(const char *msg) { warn("soju cloud: %s\n", msg); exit(1); }

/* --- protobuf --- */
static void pb_varint(bw *w, uint64_t v) { do { uint8_t b = v & 0x7f; v >>= 7; bw_u8(w, b | (v ? 0x80 : 0)); } while (v); }
static void pb_u(bw *w, int f, uint64_t v) { pb_varint(w, (uint64_t)f << 3); pb_varint(w, v); }
static void pb_b(bw *w, int f, const void *p, size_t n) { pb_varint(w, (uint64_t)f << 3 | 2); pb_varint(w, n); bw_raw(w, p, (uint32_t)n); }
static void pb_s(bw *w, int f, const char *s) { pb_b(w, f, s, strlen(s)); }

typedef struct { int field, type; uint64_t v; const uint8_t *data; size_t len; } pbf;

static uint64_t rd_varint(const uint8_t **p, const uint8_t *end) {
  uint64_t v = 0; int s = 0;
  while (*p < end && s < 64) { uint8_t b = *(*p)++; v |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
  return v;
}

static bool pb_next(const uint8_t **p, const uint8_t *end, pbf *f) {
  if (*p >= end) return false;
  uint64_t key = rd_varint(p, end);
  f->field = (int)(key >> 3); f->type = key & 7; f->data = NULL; f->len = 0; f->v = 0;
  switch (f->type) {
  case 0: f->v = rd_varint(p, end); return true;
  case 1: if (end - *p < 8) return false; memcpy(&f->v, *p, 8); *p += 8; return true;
  case 5: if (end - *p < 4) return false; memcpy(&f->v, *p, 4); *p += 4; return true;
  case 2: f->len = rd_varint(p, end); if ((size_t)(end - *p) < f->len) return false; f->data = *p; *p += f->len; return true;
  }
  return false;
}

static char *pb_strdup(const pbf *f) { char *s = malloc(f->len + 1); memcpy(s, f->data, f->len); s[f->len] = 0; return s; }

/* --- вызов сервиса Steam --- */
typedef uint64_t (*send_fn)(void *, const char *, const void *, uint32_t, uint64_t);
typedef bool (*info_fn)(void *, uint64_t, uint32_t *, int32_t *);
typedef bool (*data_fn)(void *, uint64_t, void *, uint32_t, bool);

/* EResult, после которых Steam обычно отвечает на повтор: нет ответа (0), Fail (2), Timeout (16),
 * ServiceUnavailable (20), RateLimitExceeded (84), TooManyPending (108). Сразу после выхода из
 * игры облако часто ещё занято её сессией. */
static bool transient(int32_t r) { return r == 0 || r == 2 || r == 16 || r == 20 || r == 84 || r == 108; }

/* Запрос к сервису Cloud. rpc_run отправляет пачку разом и ждёт ответы вместе: на N файлов —
 * один круг до Steam, а не N. */
typedef struct {
  const char *method;
  bw req;
  int32_t res;    /* EResult; 1 — успех */
  uint8_t *out;   /* ответ (malloc) */
  uint32_t outlen;
  uint64_t h;
  int st;         /* 0 — ждёт отправки, 1 — в пути, 2 — готов */
  int tries;
  double sent, at; /* когда ушёл; не раньше какого времени повторить */
} rpc;

/* Паузы перед повторами временного отказа: сначала короткие — Steam после выхода из игры обычно
 * оживает за секунды, — всего около минуты. Раньше было 5, 15, 45 с: облако освобождалось
 * через 6 с, а мы спали до 20-й. */
static const unsigned g_backoff[] = {1, 2, 4, 8, 15, 30};
#define NBACKOFF ((int)(sizeof g_backoff / sizeof g_backoff[0]))
#define RPC_TIMEOUT 15.0

/* SOJU_CLOUD_SERIAL=1 — запросы и HTTP по одному, как раньше: для сравнения скорости. */
static bool g_serial;

static void rpc_run(rpc *rs, int n) {
  void **vt = *(void ***)g_um;
  for (int i = 0; i < n; i++) {
    rs[i].st = 0; rs[i].tries = 0; rs[i].at = 0; rs[i].res = 0; rs[i].out = NULL; rs[i].outlen = 0;
  }
  int left = n;
  while (left) {
    double t = now_s();
    /* Первый раз — все сразу; повторы — по одному: TooManyPending может значить «слишком много
     * сразу», и повторять пачкой бессмысленно. */
    int inflight = 0, retrying = 0;
    for (int i = 0; i < n; i++)
      if (rs[i].st == 1) { inflight++; retrying += rs[i].tries > 0; }
    for (int i = 0; i < n; i++) {
      rpc *r = &rs[i];
      if (r->st != 0 || r->at > t) continue;
      if (g_serial && inflight) break;
      if (r->tries && retrying) continue;
      r->h = ((send_fn)vt[0])(g_um, r->method, r->req.p, r->req.n, 0);
      r->sent = now_s();
      r->st = 1;
      inflight++;
      retrying += r->tries > 0;
    }
    SteamAPI_RunCallbacks();
    for (int i = 0; i < n; i++) {
      rpc *r = &rs[i];
      if (r->st != 1) continue;
      uint32_t size = 0; int32_t res = 0;
      bool ready = ((info_fn)vt[1])(g_um, r->h, &size, &res);
      if (!ready && now_s() - r->sent < RPC_TIMEOUT) continue;
      if (!ready) { res = 0; size = 0; }
      free(r->out);
      r->out = malloc(size ? size : 1);
      r->outlen = size;
      if (res == 1 && size && !((data_fn)vt[2])(g_um, r->h, r->out, size, true)) res = 0;
      r->res = res;
      logf_("%s → EResult %d, %u Б, %.2f с%s", r->method, res, size, now_s() - r->sent, ready ? "" : " (нет ответа)");
      if (res != 1 && transient(res) && r->tries < NBACKOFF) {
        unsigned w = g_backoff[r->tries++];
        warn("soju cloud: %s EResult %d, повторяю через %u с\n", r->method, res, w);
        r->at = now_s() + w;
        r->st = 0;
      } else {
        r->st = 2;
        left--;
      }
    }
    if (left) usleep(5000);
  }
  for (int i = 0; i < n; i++) { free(rs[i].req.p); rs[i].req = (bw){0}; }
}

/* Один запрос; ответ в *out (malloc). */
static int32_t call(const char *method, bw *req, uint8_t **out, uint32_t *outlen) {
  rpc r = {.method = method, .req = *req};
  *req = (bw){0};
  rpc_run(&r, 1);
  *out = r.out;
  *outlen = r.outlen;
  return r.res;
}

/* --- HTTP --- */
typedef struct { uint8_t *p; size_t n; } mem;

static size_t on_data(char *p, size_t s, size_t n, void *u) {
  mem *m = u;
  m->p = realloc(m->p, m->n + s * n);
  memcpy(m->p + m->n, p, s * n);
  m->n += s * n;
  return s * n;
}

/* GET или PUT по описанию из ответа Cloud: хост, путь (подписанная ссылка) и заголовки. */
typedef struct {
  const char *method;
  char *host, *path;
  bool https;
  char *hdr[64];
  int nh;
  const uint8_t *body;
  size_t blen;
  mem resp;
  long code;
} hreq;

static void hreq_free(hreq *h) {
  free(h->host);
  free(h->path);
  for (int i = 0; i < h->nh; i++) free(h->hdr[i]);
  free(h->resp.p);
  memset(h, 0, sizeof *h);
}

/* Заголовок name/value из вложенного сообщения. */
static void pb_header(const pbf *f, hreq *h) {
  if (h->nh + 2 > 64) return;
  const uint8_t *q = f->data, *qe = f->data + f->len;
  pbf g;
  char *name = NULL, *value = NULL;
  while (pb_next(&q, qe, &g)) {
    if (g.field == 1 && g.type == 2) { free(name); name = pb_strdup(&g); }
    if (g.field == 2 && g.type == 2) { free(value); value = pb_strdup(&g); }
  }
  if (name && value) { h->hdr[h->nh++] = name; h->hdr[h->nh++] = value; }
  else { free(name); free(value); }
}

static CURL *http_easy(hreq *h, struct curl_slist **hl) {
  char url[4096];
  snprintf(url, sizeof url, "%s://%s%s", h->https ? "https" : "http", h->host, h->path);
  CURL *c = curl_easy_init();
  *hl = NULL;
  for (int i = 0; i + 1 < h->nh; i += 2) {
    char line[2048];
    snprintf(line, sizeof line, "%s: %s", h->hdr[i], h->hdr[i + 1]);
    *hl = curl_slist_append(*hl, line);
  }
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, *hl);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &h->resp);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  if (strcmp(h->method, "GET")) {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, h->method);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, h->body ? (const char *)h->body : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)h->blen);
  }
  return c;
}

static void http_done(hreq *h, CURL *c, CURLcode cc) {
  h->code = 0;
  if (cc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &h->code);
  double total = 0;
  curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
  /* Только метод и хост: путь и заголовки — подписанная ссылка. */
  logf_("HTTP %s %s → %ld%s%s, %zu Б, %.2f с", h->method, h->host, h->code, cc ? ", " : "",
        cc ? curl_easy_strerror(cc) : "", h->blen ? h->blen : h->resp.n, total);
}

/* Все запросы параллельно (curl multi); итог — в hs[i].code и hs[i].resp. */
static void http_run(hreq *hs, int n) {
  if (n <= 0) return;
  CURL **e = calloc((size_t)n, sizeof *e);
  struct curl_slist **hl = calloc((size_t)n, sizeof *hl);
  if (g_serial) {
    for (int i = 0; i < n; i++) {
      e[i] = http_easy(&hs[i], &hl[i]);
      http_done(&hs[i], e[i], curl_easy_perform(e[i]));
    }
  } else {
    CURLM *m = curl_multi_init();
    for (int i = 0; i < n; i++) {
      e[i] = http_easy(&hs[i], &hl[i]);
      curl_easy_setopt(e[i], CURLOPT_PRIVATE, (char *)&hs[i]);
      curl_multi_add_handle(m, e[i]);
    }
    int running = n;
    while (running) {
      curl_multi_perform(m, &running);
      CURLMsg *msg;
      int q;
      while ((msg = curl_multi_info_read(m, &q)))
        if (msg->msg == CURLMSG_DONE) {
          char *priv = NULL;
          curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
          http_done((hreq *)priv, msg->easy_handle, msg->data.result);
        }
      if (running) curl_multi_poll(m, NULL, 0, 500, NULL);
    }
    for (int i = 0; i < n; i++) curl_multi_remove_handle(m, e[i]);
    curl_multi_cleanup(m);
  }
  for (int i = 0; i < n; i++) { curl_easy_cleanup(e[i]); curl_slist_free_all(hl[i]); }
  free(e);
  free(hl);
}

/* --- файлы --- */
static void sha1(const uint8_t *p, size_t n, uint8_t out[SHA_LEN]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CC_SHA1(p, (CC_LONG)n, out);
#pragma clang diagnostic pop
}

static uint8_t *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *p = malloc(len > 0 ? (size_t)len : 1);
  *n = fread(p, 1, len > 0 ? (size_t)len : 0, f);
  fclose(f);
  return p;
}

static void mkdirs(const char *path) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *s = tmp + 1; *s; s++)
    if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
}

/* Атомарная запись с mtime = время из облака. */
static bool write_file(const char *path, const uint8_t *p, size_t n, time_t mtime) {
  char tmp[4200];
  snprintf(tmp, sizeof tmp, "%s.soju-tmp", path);
  mkdirs(path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(p, 1, n, f) == n;
  ok = fclose(f) == 0 && ok;
  if (ok && mtime) {
    struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
    utimes(tmp, tv);
  }
  if (!ok) unlink(tmp);
  return ok && rename(tmp, path) == 0;
}

static bool backup(const char *name, const uint8_t *p, size_t n, const char *side) {
  char safe[1024], path[4096];
  size_t k = 0;
  for (const char *c = name; *c && k + 1 < sizeof safe; c++) safe[k++] = (*c == '/' || *c == '%') ? '_' : *c;
  safe[k] = 0;
  snprintf(path, sizeof path, "%s/%s/%s.%s", g_backup, g_stamp, safe, side);
  if (write_file(path, p, n, 0)) return true;
  warn("soju cloud: не сохранил копию %s\n", mask(path));
  return false;
}

/* Имя в облаке → путь на диске: "%WinAppDataLocalLow%Ved/..." → "<root>/Ved/...". */
static char *local_path(const char *name) {
  if (name[0] != '%') return NULL;
  const char *e = strchr(name + 1, '%');
  if (!e) return NULL;
  for (int i = 0; i < g_nroots; i++)
    if (strlen(g_roots[i][0]) == (size_t)(e - name - 1) && !strncmp(g_roots[i][0], name + 1, (size_t)(e - name - 1))) {
      char *p = malloc(strlen(g_roots[i][1]) + strlen(e) + 2);
      sprintf(p, "%s/%s", g_roots[i][1], e + 1);
      return p;
    }
  return NULL;
}

static const char *root_dir(const char *root) {
  for (int i = 0; i < g_nroots; i++) if (!strcmp(g_roots[i][0], root)) return g_roots[i][1];
  return NULL;
}

static void scan(const rule *r, const char *dir, const char *rel) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && g_nlocal < MAXF) {
    if (e->d_name[0] == '.') continue;
    char path[4096], relp[2048];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    snprintf(relp, sizeof relp, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    struct stat st;
    if (stat(path, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      if (r->recursive) scan(r, path, relp);
    } else if (S_ISREG(st.st_mode) && !fnmatch(r->pattern, e->d_name, 0)) {
      lfile *l = &g_local[g_nlocal++];
      l->name = malloc(strlen(r->root) + strlen(r->path) + strlen(relp) + 8);
      sprintf(l->name, "%%%s%%%s/%s", r->root, r->path, relp);
      l->path = strdup(path);
      l->mtime = st.st_mtime;
      size_t n = 0;
      uint8_t *p = slurp(path, &n);
      l->size = n;
      sha1(p ? p : (const uint8_t *)"", p ? n : 0, l->sha);
      free(p);
    }
  }
  closedir(d);
}

/* --- состояние последней синхронизации --- */
static const char *g_state_path;

static void state_load(void) {
  FILE *f = fopen(g_state_path, "r");
  if (!f) return;
  char line[4200];
  while (fgets(line, sizeof line, f) && g_nstate < MAXF) {
    line[strcspn(line, "\n")] = 0;
    char *tab = strchr(line, '\t');
    if (!tab || tab - line != SHA_LEN * 2) continue;
    memcpy(g_state[g_nstate].sha, line, SHA_LEN * 2);
    g_state[g_nstate].sha[SHA_LEN * 2] = 0;
    g_state[g_nstate++].name = strdup(tab + 1);
  }
  fclose(f);
}

static const char *state_get(const char *name) {
  for (int i = 0; i < g_nstate; i++) if (!strcmp(g_state[i].name, name)) return g_state[i].sha;
  return NULL;
}

static void state_set(const char *name, const uint8_t sha[SHA_LEN]) {
  int i = 0;
  while (i < g_nstate && strcmp(g_state[i].name, name)) i++;
  if (i == g_nstate) { if (g_nstate == MAXF) return; g_state[g_nstate++].name = strdup(name); }
  cloud_hex(sha, g_state[i].sha);
}

static void state_save(void) {
  char tmp[4200];
  snprintf(tmp, sizeof tmp, "%s.tmp", g_state_path);
  mkdirs(g_state_path);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  for (int i = 0; i < g_nstate; i++) fprintf(f, "%s\t%s\n", g_state[i].sha, g_state[i].name);
  if (fclose(f) == 0) rename(tmp, g_state_path);
  else unlink(tmp);
}

/* --- облако --- */
static void load_cloud(void) {
  bw req = {0};
  pb_u(&req, 1, g_app);
  uint8_t *resp; uint32_t n;
  int32_t r = call("Cloud.GetAppFileChangelist#1", &req, &resp, &n);
  if (r != 1) { warn("soju cloud: GetAppFileChangelist EResult %d\n", r); exit(1); }
  char *prefixes[MAXF]; int np = 0;
  struct { char *file; uint32_t prefix; uint8_t sha[SHA_LEN]; uint64_t ts; uint32_t size; uint32_t persist; } raw[MAXF];
  int nr = 0;
  const uint8_t *p = resp, *end = resp + n;
  pbf f;
  while (pb_next(&p, end, &f)) {
    if (f.field == 4 && f.type == 2 && np < MAXF) prefixes[np++] = pb_strdup(&f);
    if (f.field == 2 && f.type == 2 && nr < MAXF) {
      memset(&raw[nr], 0, sizeof raw[nr]);
      const uint8_t *q = f.data, *qe = f.data + f.len;
      pbf g;
      while (pb_next(&q, qe, &g)) {
        if (g.field == 1 && g.type == 2) raw[nr].file = pb_strdup(&g);
        else if (g.field == 2 && g.type == 2 && g.len == SHA_LEN) memcpy(raw[nr].sha, g.data, SHA_LEN);
        else if (g.field == 3) raw[nr].ts = g.v;
        else if (g.field == 4) raw[nr].size = (uint32_t)g.v;
        else if (g.field == 5) raw[nr].persist = (uint32_t)g.v;
        else if (g.field == 7) raw[nr].prefix = (uint32_t)g.v;
      }
      nr++;
    }
  }
  free(resp);
  for (int i = 0; i < nr && g_ncloud < MAXF; i++) {
    if (!raw[i].file || raw[i].persist != 0) continue;  /* только живые файлы */
    const char *pre = raw[i].prefix < (uint32_t)np ? prefixes[raw[i].prefix] : "";
    cfile *c = &g_cloud[g_ncloud++];
    c->name = malloc(strlen(pre) + strlen(raw[i].file) + 1);
    sprintf(c->name, "%s%s", pre, raw[i].file);
    memcpy(c->sha, raw[i].sha, SHA_LEN);
    c->ts = raw[i].ts;
    c->size = raw[i].size;
  }
}

/* --- что делать с каждым файлом --- */
typedef struct {
  const char *name;
  const lfile *l;
  const cfile *c;
  enum cloud_act act;
  bool conflict;   /* был конфликт, решён через --prefer */
  bool get;        /* скачать облачную версию */
  bool write;      /* ...и записать её на диск (иначе — только в backups) */
  bool put;        /* выгрузить локальную */
  bool failed;
  uint8_t *data;   /* скачанное или то, что выгружаем */
  size_t n;
  uint8_t sha[SHA_LEN];
} item;

/* Имя без "%Root%путь/" из правила: видно подкаталог, а одинаковые файлы в разных папках различимы. */
static const char *short_name(const char *name) {
  const char *e = name[0] == '%' ? strchr(name + 1, '%') : NULL;
  if (!e) return name;
  for (int i = 0; i < g_nrules; i++) {
    size_t n = strlen(g_rules[i].path);
    if (!strncmp(e + 1, g_rules[i].path, n) && e[1 + n] == '/') return e + 2 + n;
  }
  return e + 1;
}

/* «  ! файл (конфликт: локально 2026-10-30T13:42:59+03:00, 1234 Б; в облаке …, 1200 Б)» */
static void say_conflict(const item *it) {
  char lt[32], ct[32];
  cloud_iso(it->l->mtime, lt);
  cloud_iso((time_t)it->c->ts, ct);
  say("  ! %s (конфликт: локально %s, %zu Б; в облаке %s, %u Б)\n", mask(short_name(it->name)), lt, it->l->size, ct,
      it->c->size);
}

static const lfile *find_local(const char *name) {
  for (int i = 0; i < g_nlocal; i++) if (!strcmp(g_local[i].name, name)) return &g_local[i];
  return NULL;
}

static const cfile *find_cloud(const char *name) {
  for (int i = 0; i < g_ncloud; i++) if (!strcmp(g_cloud[i].name, name)) return &g_cloud[i];
  return NULL;
}

/* Скачать облачные версии всех it[].get: запросы ссылок разом, потом HTTP разом. */
static void fetch_all(item *its, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) k += its[i].get;
  if (!k) return;
  rpc *rs = calloc((size_t)k, sizeof *rs);
  hreq *hs = calloc((size_t)k, sizeof *hs);
  item **who = calloc((size_t)k, sizeof *who);
  k = 0;
  for (int i = 0; i < n; i++) {
    if (!its[i].get) continue;
    who[k] = &its[i];
    rs[k].method = "Cloud.ClientFileDownload#1";
    pb_u(&rs[k].req, 1, g_app);
    pb_s(&rs[k].req, 2, its[i].name);
    k++;
  }
  rpc_run(rs, k);
  int nh = 0;
  item **hwho = calloc((size_t)k, sizeof *hwho);  /* чей это HTTP-запрос */
  for (int j = 0; j < k; j++) {
    item *it = who[j];
    if (rs[j].res != 1) {
      warn("soju cloud: ClientFileDownload %s EResult %d\n", mask(it->name), rs[j].res);
      it->failed = true;
      continue;
    }
    hreq *h = &hs[nh];
    h->method = "GET";
    h->https = true;
    bool encrypted = false;
    const uint8_t *p = rs[j].out, *end = rs[j].out + rs[j].outlen;
    pbf f;
    while (pb_next(&p, end, &f)) {
      if (f.field == 7 && f.type == 2) { free(h->host); h->host = pb_strdup(&f); }
      else if (f.field == 8 && f.type == 2) { free(h->path); h->path = pb_strdup(&f); }
      else if (f.field == 9) h->https = f.v;
      else if (f.field == 11) encrypted = f.v;
      else if (f.field == 10 && f.type == 2) pb_header(&f, h);
    }
    if (!h->host || !h->path || encrypted) {
      warn("soju cloud: %s — %s\n", mask(it->name), encrypted ? "зашифрован" : "нет ссылки");
      hreq_free(h);
      it->failed = true;
      continue;
    }
    hwho[nh++] = it;
  }
  http_run(hs, nh);
  for (int j = 0; j < nh; j++) {
    item *it = hwho[j];
    hreq *h = &hs[j];
    uint8_t sha[SHA_LEN];
    sha1(h->resp.p ? h->resp.p : (const uint8_t *)"", h->resp.n, sha);
    if (h->code != 200 || memcmp(sha, it->c->sha, SHA_LEN)) {
      warn("soju cloud: %s — HTTP %ld%s\n", mask(it->name), h->code, h->code == 200 ? ", sha1 не совпал" : "");
      it->failed = true;
    } else {
      it->data = h->resp.p;
      it->n = h->resp.n;
      h->resp = (mem){0};
    }
    hreq_free(h);
  }
  for (int j = 0; j < k; j++) free(rs[j].out);
  free(rs); free(hs); free(who); free(hwho);
}

/* Выгрузить все it[].put одной пачкой: начало пачки, запросы на загрузку разом, HTTP разом,
 * подтверждения разом, конец пачки. Возвращает число неудач. */
static int upload_all(item *its, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) k += its[i].put;
  if (!k) return 0;
  item **who = calloc((size_t)k, sizeof *who);
  int failed = 0;
  k = 0;
  for (int i = 0; i < n; i++) {
    item *it = &its[i];
    if (!it->put) continue;
    /* Читаем один раз и считаем sha по тому, что уходит: игра могла дописать файл после скана. */
    it->data = slurp(it->l->path, &it->n);
    if (!it->data) { warn("soju cloud: не прочитал %s\n", mask(it->l->path)); it->failed = true; failed++; continue; }
    sha1(it->data, it->n, it->sha);
    who[k++] = it;
  }
  if (!k) { free(who); return failed; }

  /* Пачка — как у клиента Steam: так облако видит одну запись о синхронизации. */
  uint64_t batch = 0;
  {
    bw req = {0};
    pb_u(&req, 1, g_app);
    pb_s(&req, 2, "soju");
    for (int j = 0; j < k; j++) pb_s(&req, 3, who[j]->name);
    uint8_t *resp; uint32_t rn;
    if (call("Cloud.BeginAppUploadBatch#1", &req, &resp, &rn) == 1) {
      const uint8_t *p = resp, *end = resp + rn;
      pbf f;
      while (pb_next(&p, end, &f)) if (f.field == 1) batch = f.v;
    }
    free(resp);
  }

  rpc *rs = calloc((size_t)k, sizeof *rs);
  for (int j = 0; j < k; j++) {
    item *it = who[j];
    rs[j].method = "Cloud.ClientBeginFileUpload#1";
    bw *r = &rs[j].req;
    pb_u(r, 1, g_app);
    pb_u(r, 2, it->n);
    pb_u(r, 3, it->n);
    pb_b(r, 4, it->sha, SHA_LEN);
    pb_u(r, 5, (uint64_t)it->l->mtime);
    pb_s(r, 6, it->name);
    pb_u(r, 10, 0);  /* can_encrypt = false: шифровать мы не умеем */
    if (batch) pb_u(r, 13, batch);
  }
  rpc_run(rs, k);

  /* Блоки всех файлов — в один список HTTP-запросов. */
  int cap = 0, nh = 0;
  hreq *hs = NULL;
  int *owner = NULL;
  bool *ok = calloc((size_t)k, sizeof *ok);
  for (int j = 0; j < k; j++) {
    item *it = who[j];
    if (rs[j].res != 1) {
      warn("soju cloud: ClientBeginFileUpload %s EResult %d\n", mask(it->name), rs[j].res);
      continue;
    }
    ok[j] = true;
    const uint8_t *p = rs[j].out, *end = rs[j].out + rs[j].outlen;
    pbf f;
    while (ok[j] && pb_next(&p, end, &f)) {
      if (f.field == 1 && f.v) { warn("soju cloud: Steam требует шифрование — не поддержано\n"); ok[j] = false; }
      if (f.field != 2 || f.type != 2) continue;
      if (nh == cap) {
        cap = cap ? cap * 2 : 8;
        hs = realloc(hs, (size_t)cap * sizeof *hs);
        owner = realloc(owner, (size_t)cap * sizeof *owner);
      }
      hreq *h = &hs[nh];
      memset(h, 0, sizeof *h);
      h->https = true;
      int method = 4;
      uint64_t off = 0, len = it->n;
      const uint8_t *explicit_body = NULL; size_t explicit_len = 0;
      const uint8_t *q = f.data, *qe = f.data + f.len;
      pbf g;
      while (pb_next(&q, qe, &g)) {
        if (g.field == 1 && g.type == 2) { free(h->host); h->host = pb_strdup(&g); }
        else if (g.field == 2 && g.type == 2) { free(h->path); h->path = pb_strdup(&g); }
        else if (g.field == 3) h->https = g.v;
        else if (g.field == 4) method = (int)g.v;
        else if (g.field == 6) off = g.v;
        else if (g.field == 7) len = g.v;
        else if (g.field == 8 && g.type == 2) { explicit_body = g.data; explicit_len = g.len; }
        else if (g.field == 5 && g.type == 2) pb_header(&g, h);
      }
      if (!h->host || !h->path || off > it->n || len > it->n - off) { hreq_free(h); ok[j] = false; break; }
      /* EHTTPMethod: 3 — POST, 4 — PUT. Тело с явным содержимым живёт в rs[j].out до конца. */
      h->method = method == 3 ? "POST" : "PUT";
      h->body = explicit_body ? explicit_body : it->data + off;
      h->blen = explicit_body ? explicit_len : len;
      owner[nh++] = j;
    }
  }
  http_run(hs, nh);
  for (int i = 0; i < nh; i++) {
    if (hs[i].code < 200 || hs[i].code >= 300) {
      warn("soju cloud: загрузка %s — HTTP %ld\n", mask(who[owner[i]]->name), hs[i].code);
      ok[owner[i]] = false;
    }
    hreq_free(&hs[i]);
  }
  free(hs);
  free(owner);

  /* Подтверждения: и неудачные — так Steam отпускает начатую загрузку. */
  rpc *cs = calloc((size_t)k, sizeof *cs);
  int nc = 0;
  int *cmap = calloc((size_t)k, sizeof *cmap);
  for (int j = 0; j < k; j++) {
    if (rs[j].res != 1) continue;
    cs[nc].method = "Cloud.ClientCommitFileUpload#1";
    pb_u(&cs[nc].req, 1, ok[j]);
    pb_u(&cs[nc].req, 2, g_app);
    pb_b(&cs[nc].req, 3, who[j]->sha, SHA_LEN);
    pb_s(&cs[nc].req, 4, who[j]->name);
    cmap[nc++] = j;
  }
  rpc_run(cs, nc);
  bool *committed = calloc((size_t)k, sizeof *committed);
  for (int c = 0; c < nc; c++) {
    const uint8_t *p = cs[c].out, *end = cs[c].out + cs[c].outlen;
    pbf f;
    while (cs[c].res == 1 && pb_next(&p, end, &f)) if (f.field == 1) committed[cmap[c]] = f.v;
    free(cs[c].out);
  }
  for (int j = 0; j < k; j++) {
    if (!(ok[j] && committed[j])) { who[j]->failed = true; failed++; }
    free(rs[j].out);
  }
  free(rs); free(cs); free(cmap); free(ok); free(committed);

  if (batch) {
    bw req = {0};
    pb_u(&req, 1, g_app);
    pb_u(&req, 2, batch);
    pb_u(&req, 3, failed ? 2 : 1);
    uint8_t *resp; uint32_t rn;
    call("Cloud.CompleteAppUploadBatchBlocking#1", &req, &resp, &rn);
    free(resp);
  }
  free(who);
  return failed;
}

/* Другая синхронизация этой же игры (выгрузка после прошлого запуска) ещё идёт — ждём её:
 * иначе pull увидел бы облако без только что выгруженных сейвов. */
static void lock_state(void) {
  char lp[4200];
  snprintf(lp, sizeof lp, "%s.lock", g_state_path);
  mkdirs(lp);
  int fd = open(lp, O_RDWR | O_CREAT, 0600);
  if (fd < 0) return;
  if (flock(fd, LOCK_EX | LOCK_NB) == 0) return;
  double t = now_s();
  warn("soju cloud: жду, пока закончится другая синхронизация этой игры\n");
  flock(fd, LOCK_EX);
  logf_("дождался другой синхронизации: %.2f с", now_s() - t);
  /* fd открыт до выхода: блокировку снимает сама система. */
}

/* Удаление по именам: Cloud.ClientDeleteFile, явное (is_explicit_delete) — как удаление из клиента. */
static int delete_files(int n, char **names) {
  int failed = 0;
  for (int i = 0; i < n; i++) {
    bw req = {0};
    pb_u(&req, 1, g_app);
    pb_s(&req, 2, names[i]);
    pb_u(&req, 3, 1);
    uint8_t *resp; uint32_t rn;
    int32_t r = call("Cloud.ClientDeleteFile#1", &req, &resp, &rn);
    free(resp);
    if (r == 1) say("  - %s\n", names[i]);
    else { warn("soju cloud: не удалил %s: EResult %d\n", names[i], r); failed++; }
  }
  return failed;
}

int cloud_main(int argc, char **argv) {
  if (argc < 3) die("cloud status|pull|push --state FILE --backup DIR --root N=DIR --rule R:P:PAT:REC [--prefer local|cloud] [--log FILE]\n"
                    "cloud delete NAME...");
  const char *mode = argv[2];
  if (!strcmp(mode, "delete")) {
    const char *app = getenv("SteamAppId");
    if (!app || argc < 4) die("cloud delete NAME... (нужен SteamAppId)");
    g_app = (uint32_t)strtoul(app, NULL, 10);
    quiet_steam();
    char err[1024] = {0};
    if (steam_init(NULL, err) != 0) { warn("soju cloud: Steam недоступен: %s\n", err); return 1; }
    void *client = SteamInternal_CreateInterface("SteamClient021");
    g_um = client ? SteamAPI_ISteamClient_GetISteamGenericInterface(client, SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(),
                                                                    "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001") : NULL;
    if (!g_um) { SteamAPI_Shutdown(); die("нет ISteamUnifiedMessages в клиенте Steam"); }
    int failed = delete_files(argc - 3, argv + 3);
    SteamAPI_Shutdown();
    return failed ? 1 : 0;
  }
  const char *log_path = NULL, *prefer = NULL;
  g_t0 = now_s();
  for (int i = 3; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--state")) g_state_path = argv[i + 1];
    else if (!strcmp(argv[i], "--backup")) g_backup = argv[i + 1];
    else if (!strcmp(argv[i], "--log")) log_path = argv[i + 1];
    else if (!strcmp(argv[i], "--prefer")) prefer = argv[i + 1];
    else if (!strcmp(argv[i], "--root") && g_nroots < 16) {
      char *eq = strchr(argv[i + 1], '=');
      if (!eq) die("--root NAME=DIR");
      *eq = 0;
      g_roots[g_nroots][0] = argv[i + 1];
      g_roots[g_nroots++][1] = eq + 1;
    } else if (!strcmp(argv[i], "--rule") && g_nrules < 16) {
      char *s = strdup(argv[i + 1]);
      rule *r = &g_rules[g_nrules++];
      r->root = strsep(&s, ":");
      r->path = strsep(&s, ":");
      r->pattern = strsep(&s, ":");
      r->recursive = s && !strcmp(s, "1");
      if (!r->root || !r->path || !r->pattern) die("--rule ROOT:PATH:PATTERN:RECURSIVE");
    }
  }
  bool status = !strcmp(mode, "status"), pulling = !strcmp(mode, "pull"), pushing = !strcmp(mode, "push");
  if (!status && !pulling && !pushing) die("режим: status, pull или push");
  if (prefer && strcmp(prefer, "local") && strcmp(prefer, "cloud")) die("--prefer local или --prefer cloud");
  const char *app = getenv("SteamAppId");
  if (!app || !g_state_path || !g_backup) die("нужны SteamAppId, --state, --backup");
  g_app = (uint32_t)strtoul(app, NULL, 10);
  g_serial = getenv("SOJU_CLOUD_SERIAL") && !strcmp(getenv("SOJU_CLOUD_SERIAL"), "1");
  if (log_path) log_open(log_path);
  logf_("--- cloud %s%s%s, app %u, pid %d%s", mode, prefer ? " --prefer " : "", prefer ? prefer : "", g_app, (int)getpid(),
        g_serial ? ", по одному" : "");
  /* status только читает: состояние не пишет и блокировку не ждёт. */
  if (!status) lock_state();
  quiet_steam();
  time_t now = time(NULL);
  strftime(g_stamp, sizeof g_stamp, "%Y%m%d-%H%M%S", localtime(&now));

  char err[1024] = {0};
  if (steam_init(NULL, err) != 0) {
    warn("soju cloud: Steam недоступен: %s\n", err);
    return 1;
  }
  void *client = SteamInternal_CreateInterface("SteamClient021");
  g_um = client ? SteamAPI_ISteamClient_GetISteamGenericInterface(client, SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(),
                                                                  "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001") : NULL;
  if (!g_um) { SteamAPI_Shutdown(); die("нет ISteamUnifiedMessages в клиенте Steam"); }
  logf_("Steam готов");
  curl_global_init(CURL_GLOBAL_DEFAULT);

  state_load();
  load_cloud();
  for (int i = 0; i < g_nrules; i++) {
    const char *rd = root_dir(g_rules[i].root);
    if (!rd) { warn("soju cloud: нет каталога для корня %s — пропускаю\n", g_rules[i].root); continue; }
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/%s", rd, g_rules[i].path);
    scan(&g_rules[i], dir, "");
  }
  logf_("в облаке %d файлов, на диске %d, в состоянии %d", g_ncloud, g_nlocal, g_nstate);

  /* Все имена: из облака и с диска. */
  static item its[MAXF * 2];
  int n = 0, conflicts = 0;
  for (int i = 0; i < g_ncloud; i++) its[n++].name = g_cloud[i].name;
  for (int i = 0; i < g_nlocal; i++) if (!find_cloud(g_local[i].name)) its[n++].name = g_local[i].name;
  for (int i = 0; i < n; i++) {
    item *it = &its[i];
    it->l = find_local(it->name);
    it->c = find_cloud(it->name);
    it->act = cloud_decide(it->l ? it->l->sha : NULL, it->c ? it->c->sha : NULL, state_get(it->name));
    conflicts += it->act == ACT_CONFLICT;
  }

  if (status || (conflicts && !prefer)) {
    for (int i = 0; i < n; i++) {
      const item *it = &its[i];
      const char *sn = mask(short_name(it->name));
      if (it->act == ACT_CONFLICT) say_conflict(it);
      else if (status && it->act == ACT_SYNCED) say("  = %s\n", sn);
      else if (status && it->act != ACT_NONE) say("  %s %s\n", it->act == ACT_PULL ? "↓" : "↑", sn);
    }
    curl_global_cleanup();
    SteamAPI_Shutdown();
    if (status) {
      logf_("итог: status, %.2f с", now_s() - g_t0);
      fflush(OUT);
      return 0;
    }
    warn("soju cloud: конфликт: файлы менялись и на диске, и в облаке — выбери --prefer local или --prefer cloud\n");
    logf_("итог: %s — конфликт (%d), ничего не перенесено, %.2f с", mode, conflicts, now_s() - g_t0);
    fflush(OUT);
    return 3;
  }

  /* План. Конфликт решён --prefer: проигравшая сторона уходит в backups. */
  for (int i = 0; i < n; i++) {
    item *it = &its[i];
    if (it->act == ACT_CONFLICT) {
      it->conflict = true;
      it->act = !strcmp(prefer, "local") ? ACT_PUSH : ACT_PULL;
      if (it->act == ACT_PUSH) it->get = true;           /* облачную — в backups */
    }
    if (it->act == ACT_PULL && (pulling || it->conflict)) it->get = it->write = true;
    if (it->act == ACT_PUSH && pushing) it->put = true;
  }

  fetch_all(its, n);
  int failed = 0;
  for (int i = 0; i < n; i++) {
    item *it = &its[i];
    const char *sn = mask(short_name(it->name));
    if (it->act == ACT_SYNCED) { state_set(it->name, it->c->sha); continue; }
    if (it->failed) {
      /* Облачную копию при «оставить локальные» не сохранили — не выгружаем поверх неё. */
      it->put = false;
      failed++;
      continue;
    }
    if (it->get && !it->write) {
      if (!backup(it->name, it->data, it->n, "cloud")) { it->put = false; failed++; continue; }
      /* pull: «оставить локальные» — запоминаем облачную как синхронизированную, тогда диск
       * выглядит изменённым и уедет в облако при следующем push. */
      if (!it->put) {
        state_set(it->name, it->c->sha);
        say("  ↑ %s (конфликт: оставлены локальные, облачные — в backups)\n", sn);
      }
      continue;
    }
    if (it->write) {
      char *path = it->l ? strdup(it->l->path) : local_path(it->name);
      if (!path) { warn("soju cloud: нет каталога для %s\n", mask(it->name)); failed++; continue; }
      bool saved = true;
      if (it->l) {
        size_t on = 0;
        uint8_t *old = slurp(it->l->path, &on);
        if (old) saved = backup(it->name, old, on, "local");
        free(old);
      }
      if (!saved) {
        /* Без копии локальный файл не трогаем. */
        failed++;
      } else if (write_file(path, it->data, it->n, (time_t)it->c->ts)) {
        state_set(it->name, it->c->sha);
        say("  ↓ %s%s\n", sn, it->conflict ? " (конфликт: взяты из облака, локальные — в backups)" : "");
      } else {
        warn("soju cloud: не записал %s: %s\n", mask(path), strerror(errno));
        failed++;
      }
      free(path);
    }
  }

  failed += upload_all(its, n);
  for (int i = 0; i < n; i++) {
    item *it = &its[i];
    if (!it->put || it->failed) continue;
    state_set(it->name, it->sha);
    say("  ↑ %s%s\n", mask(short_name(it->name)), it->conflict ? " (конфликт: оставлены локальные, облачные — в backups)" : "");
  }

  state_save();
  curl_global_cleanup();
  SteamAPI_Shutdown();
  logf_("итог: %s, ошибок %d, %.2f с", mode, failed, now_s() - g_t0);
  fflush(OUT);
  return failed ? 1 : 0;
}
