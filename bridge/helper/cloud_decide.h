/* Решение по файлу облака — трёхстороннее сравнение: диск, облако и последняя синхронизация.
 * Отдельно от cloud.c, чтобы проверять без Steam (bridge/test/cloudtest.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLOUD_SHA_LEN 20

enum cloud_act {
  ACT_NONE,
  ACT_SYNCED,   /* одинаково на диске и в облаке */
  ACT_PULL,     /* поменялось только облако (или файла нет на диске) */
  ACT_PUSH,     /* поменялся только диск (или файла нет в облаке) */
  ACT_CONFLICT, /* поменялись обе стороны — или прошлой синхронизации нет, а файлы разные */
};

static inline void cloud_hex(const uint8_t sha[CLOUD_SHA_LEN], char out[CLOUD_SHA_LEN * 2 + 1]) {
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < CLOUD_SHA_LEN; i++) {
    out[2 * i] = d[sha[i] >> 4];
    out[2 * i + 1] = d[sha[i] & 15];
  }
  out[CLOUD_SHA_LEN * 2] = 0;
}

/* local, cloud — sha1 или NULL, если файла нет; state — sha1 последней синхронизации (hex) или NULL.
 * Удаление не переносится: файл, которого нет на одной стороне, приезжает с другой. */
static inline enum cloud_act cloud_decide(const uint8_t *local, const uint8_t *cloud, const char *state) {
  if (!local && !cloud) return ACT_NONE;
  if (!cloud) return ACT_PUSH;
  if (!local) return ACT_PULL;
  if (!memcmp(local, cloud, CLOUD_SHA_LEN)) return ACT_SYNCED;
  if (state) {
    char lh[CLOUD_SHA_LEN * 2 + 1], ch[CLOUD_SHA_LEN * 2 + 1];
    cloud_hex(local, lh);
    cloud_hex(cloud, ch);
    if (!strcmp(state, lh)) return ACT_PULL;
    if (!strcmp(state, ch)) return ACT_PUSH;
  }
  return ACT_CONFLICT;
}

/* Время для отчёта: ISO 8601 с часовым поясом «+03:00» — его понимают и человек, и Date в JS. */
static inline void cloud_iso(time_t t, char out[32]) {
  struct tm tm;
  localtime_r(&t, &tm);
  char z[8];
  strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &tm);
  strftime(z, sizeof z, "%z", &tm); /* +0300 */
  size_t n = strlen(out);
  if (strlen(z) == 5) snprintf(out + n, 32 - n, "%.3s:%.2s", z, z + 3);
}
