#include "build_info.h"

// The only place __DATE__ and __TIME__ are expanded.
const char BUILD_STAMP[] = __DATE__ " " __TIME__;
