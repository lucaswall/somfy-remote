#pragma once

#include "log_buffer.h"

// Writes to the serial console and to a ring buffer the web UI can serve. Use this rather
// than Serial directly in the firmware: the board lives in a case with no USB, so a line
// that only reaches the UART is a line nobody will ever read.
//
// Do not pass a trailing newline; one is added.
// Not named logf: that is <math.h>'s natural logarithm, and the collision only
// surfaces in translation units that happen to pull maths in.
void logLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Faults go to both rings. The main one fills with routine traffic, so an error from
// hours ago would otherwise be long gone by the time anyone looks.
void logError(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

const LogBuffer &logBuffer();
const ErrorBuffer &errorBuffer();
