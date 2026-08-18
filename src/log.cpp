#include "log.h"

#include <Arduino.h>
#include <stdarg.h>

static LogBuffer buffer;
static ErrorBuffer errors;

void logLine(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  Serial.println(line);
  buffer.push(millis() / 1000, line);
}

void logError(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  const uint32_t seconds = millis() / 1000;
  Serial.println(line);
  buffer.push(seconds, line);
  errors.push(seconds, line);
}

const LogBuffer &logBuffer() { return buffer; }

const ErrorBuffer &errorBuffer() { return errors; }
