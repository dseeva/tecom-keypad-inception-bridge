// Small in-memory log shown on the web page (and printed on the USB serial port).
#include <time.h>
#include "shared.h"

static const int LOG_MAX = 60;
static String lines[LOG_MAX];
static int head = 0, count = 0;
static SemaphoreHandle_t logLock = nullptr;

void logf(const char *fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char stamp[16];
  time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &t);
  } else {
    snprintf(stamp, sizeof stamp, "+%lus", (unsigned long)(millis() / 1000));
  }
  String line = String(stamp) + " " + msg;
  Serial.println(line);
  if (!logLock) logLock = xSemaphoreCreateMutex();   // first call comes from setup(), before any task starts
  xSemaphoreTake(logLock, portMAX_DELAY);
  lines[head] = line;
  head = (head + 1) % LOG_MAX;
  if (count < LOG_MAX) count++;
  xSemaphoreGive(logLock);
}

void logLines(JsonArray out, int max) {
  if (!logLock) return;
  xSemaphoreTake(logLock, portMAX_DELAY);
  int n = count < max ? count : max;
  for (int i = n; i > 0; i--) out.add(lines[(head - i + LOG_MAX) % LOG_MAX]);
  xSemaphoreGive(logLock);
}
