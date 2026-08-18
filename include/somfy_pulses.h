#pragma once

#include <stddef.h>
#include <stdint.h>

#include "somfy_frame.h"

// The RTS waveform: a frame turned into the on/off pulses an OOK transmitter plays out.
// Built here, in a pure header, so the timings are unit tested rather than eyeballed on a
// scope — getting one of them wrong produces a signal a shutter simply ignores, with no
// error anywhere to point at it.

// Half a bit period. Every duration below is either this, a multiple of it, or one of the
// three odd values Somfy's own remotes send, which receivers key on.
#define SOMFY_SYMBOL_US 640

#define SOMFY_WAKEUP_HIGH_US 9415
#define SOMFY_WAKEUP_LOW_US 9565
#define SOMFY_WAKEUP_GAP_MS 80

#define SOMFY_HW_SYNC_US (4 * SOMFY_SYMBOL_US)
#define SOMFY_SW_SYNC_HIGH_US 4550
#define SOMFY_SILENCE_US 415
#define SOMFY_INTERFRAME_GAP_MS 30

// A press is one frame with two hardware sync pulses followed by repeats with seven. The
// receiver needs the repeats: it samples rather than listening continuously, and a single
// copy often lands while it is not looking.
#define SOMFY_FIRST_SYNC 2
#define SOMFY_REPEAT_SYNC 7
#define SOMFY_REPEATS 4

// Worst case, which is a repeat frame: sync pairs + software sync + two pulses per bit +
// the closing silence.
#define SOMFY_MAX_PULSES (2 * SOMFY_REPEAT_SYNC + 2 + 2 * SOMFY_FRAME_BITS + 1)

struct SomfyPulse {
  uint16_t microseconds;
  bool high;
};

// Returns the number of pulses written, or 0 if they would not fit. Consecutive pulses at
// the same level are left unmerged: a Manchester 1 following the software sync is two
// adjacent lows, and emitting them as two entries costs nothing while keeping the mapping
// from bit to pulse pair obvious.
inline size_t somfyBuildPulses(const uint8_t *frame, uint8_t syncCount, SomfyPulse *out,
                               size_t max) {
  const size_t needed = (size_t)syncCount * 2 + 2 + SOMFY_FRAME_BITS * 2 + 1;
  if (needed > max) {
    return 0;
  }

  size_t n = 0;

  // Hardware sync: the burst that tells a receiver a frame is starting.
  for (uint8_t i = 0; i < syncCount; i++) {
    out[n++] = {SOMFY_HW_SYNC_US, true};
    out[n++] = {SOMFY_HW_SYNC_US, false};
  }

  // Software sync: the marker the data is measured from.
  out[n++] = {SOMFY_SW_SYNC_HIGH_US, true};
  out[n++] = {SOMFY_SYMBOL_US, false};

  // Manchester, MSB first, and inverted from the usual convention: a 1 is a rising edge
  // in the middle of the bit, so it starts low.
  for (uint8_t bit = 0; bit < SOMFY_FRAME_BITS; bit++) {
    const bool one = (frame[bit / 8] >> (7 - (bit % 8))) & 1;
    out[n++] = {SOMFY_SYMBOL_US, !one};
    out[n++] = {SOMFY_SYMBOL_US, one};
  }

  out[n++] = {SOMFY_SILENCE_US, false};
  return n;
}
