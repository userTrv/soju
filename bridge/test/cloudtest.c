/* Проверка решения облака без Steam: clang -I bridge/helper bridge/test/cloudtest.c && ./a.out */
#include <stdlib.h>

#include "cloud_decide.h"

static int g_fail;
#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      fprintf(stderr, "%s:%d: не выполнено: %s\n", __FILE__, __LINE__, #cond); \
      g_fail++;                                                   \
    }                                                             \
  } while (0)

static void sha(uint8_t out[CLOUD_SHA_LEN], uint8_t v) { memset(out, v, CLOUD_SHA_LEN); }

int main(void) {
  uint8_t a[CLOUD_SHA_LEN], b[CLOUD_SHA_LEN], c[CLOUD_SHA_LEN];
  char ha[41], hb[41], hc[41];
  sha(a, 0xaa); sha(b, 0xbb); sha(c, 0xcc);
  cloud_hex(a, ha); cloud_hex(b, hb); cloud_hex(c, hc);
  EXPECT(!strcmp(ha, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

  /* Одинаково — синхронизировано при любом состоянии. */
  EXPECT(cloud_decide(a, a, NULL) == ACT_SYNCED);
  EXPECT(cloud_decide(a, a, hb) == ACT_SYNCED);
  /* Поменялось только облако: на диске то, что синхронизировали. */
  EXPECT(cloud_decide(a, b, ha) == ACT_PULL);
  /* Поменялся только диск: в облаке то, что синхронизировали. */
  EXPECT(cloud_decide(b, a, ha) == ACT_PUSH);
  /* Поменялись оба — конфликт, а не «новее побеждает». */
  EXPECT(cloud_decide(b, c, ha) == ACT_CONFLICT);
  /* Первая синхронизация с разными файлами — тоже конфликт: неизвестно, что новее по смыслу. */
  EXPECT(cloud_decide(a, b, NULL) == ACT_CONFLICT);
  /* Файл только на одной стороне — едет на другую. */
  EXPECT(cloud_decide(a, NULL, NULL) == ACT_PUSH);
  EXPECT(cloud_decide(a, NULL, ha) == ACT_PUSH);
  EXPECT(cloud_decide(NULL, a, NULL) == ACT_PULL);
  EXPECT(cloud_decide(NULL, a, hb) == ACT_PULL);
  EXPECT(cloud_decide(NULL, NULL, ha) == ACT_NONE);

  /* ISO 8601 с двоеточием в поясе: 2026-10-30T13:42:59+03:00. */
  setenv("TZ", "Europe/Moscow", 1);
  tzset();
  char iso[32];
  cloud_iso(1793357379, iso);
  EXPECT(!strcmp(iso, "2026-10-30T13:49:39+03:00"));
  setenv("TZ", "UTC", 1);
  tzset();
  cloud_iso(0, iso);
  EXPECT(!strcmp(iso, "1970-01-01T00:00:00+00:00"));

  if (g_fail) return 1;
  puts("cloudtest: ок");
  return 0;
}
