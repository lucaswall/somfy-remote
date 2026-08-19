#pragma once

// One build stamp for the whole firmware.
//
// __DATE__ and __TIME__ expand per translation unit, so after a partial rebuild the boot
// banner and /status can report different times for the same image — they were 29 minutes
// apart once. Defining it in one header that only this file's translation unit stamps
// gives every caller the same answer.
extern const char BUILD_STAMP[];
