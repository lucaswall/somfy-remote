#pragma once

#include <stdint.h>

#include "somfy_frame.h"

// Commands waiting for the radio. Sending one takes the better part of a second — five
// frames plus the gaps between them — and nothing else runs while it does, so commands
// are queued here and played out one per loop rather than inside the MQTT callback that
// asked for them.
//
// Sixteen covers a "close everything" scene across a dozen shutters and costs 32 bytes.
#define COMMAND_QUEUE_LEN 16

struct QueuedCommand {
  uint8_t remote;
  SomfyCommand command;
};

class CommandQueue {
 public:
  // False means the queue was full and the oldest command was dropped to make room. The
  // newest is the one somebody just asked for, so it is the one that survives.
  bool push(uint8_t remote, SomfyCommand command) {
    _slots[_head] = {remote, command};
    _head = next(_head);
    if (_count == COMMAND_QUEUE_LEN) {
      _tail = next(_tail);
      return false;
    }
    _count++;
    return true;
  }

  bool pop(QueuedCommand *out) {
    if (_count == 0) {
      return false;
    }
    *out = _slots[_tail];
    _tail = next(_tail);
    _count--;
    return true;
  }

  bool empty() const { return _count == 0; }
  uint8_t count() const { return _count; }

 private:
  static uint8_t next(uint8_t index) { return (uint8_t)((index + 1) % COMMAND_QUEUE_LEN); }

  QueuedCommand _slots[COMMAND_QUEUE_LEN] = {};
  uint8_t _head = 0;
  uint8_t _tail = 0;
  uint8_t _count = 0;
};
