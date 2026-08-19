#pragma once

#include <stdint.h>

#include "command_queue.h"
#include "radio.h"
#include "record_store.h"
#include "remote_state.h"
#include "store.h"

// The emulated remotes: one per shutter, each with its own address, its own rolling code
// counter, and the state we have inferred from what we sent it.
//
// This is the layer that knows nothing about MQTT or HTTP. Both of those queue commands
// here and read state back from here, so a press in Home Assistant and a press on the web
// page follow exactly the same path.
//
// Nothing here is compiled in any more. How many remotes exist, what address each one
// carries and whether it may be driven all come from the store, which Home Assistant
// fills. A board with an empty store controls nothing until it is told what to control —
// which is the point: a replacement board recovers instead of needing a rebuild.
class Remotes {
 public:
  Remotes(SomfyRadio &radio, Store &store) : _radio(radio), _store(store) {}

  void begin();

  // Queued rather than sent: a press takes most of a second on the air, and the caller is
  // usually an MQTT callback. An unknown remote is logged as a fault and dropped here.
  void queue(uint8_t remote, SomfyCommand command);

  // Sends at most one queued command, so the loop keeps serving MQTT and OTA between
  // presses.
  void loop();

  // Commands are held, not dropped, while the boot reconciliation is still deciding whose
  // counters win. Dropping would be the wrong verb: the queue evicts its *oldest* entry
  // when full, so a dropped command is silently the one somebody asked for first.
  void hold(bool holding) { _held = holding; }
  bool held() const { return _held; }

  uint8_t count() const {
    return (uint8_t)_store.valueOr(rs::NS_SCALAR, rs::SCALAR_REMOTE_COUNT, 0);
  }
  uint8_t pending() const { return _queue.count(); }
  const RemoteState &state(uint8_t remote) const { return _states[remote]; }
  void restoreState(uint8_t remote, CoverPosition position) {
    if (remote < rs::MAX_REMOTES) {
      _states[remote].restore(position);
    }
  }

  uint32_t addressOf(uint8_t remote) const;
  bool enabled(uint8_t remote) const;
  bool operational(uint8_t remote) const;
  bool hasCounter(uint8_t remote) const { return _store.has(rs::NS_CODE, remote); }

  // Every reason a press might not reach the air, in one place, so the web UI and Home
  // Assistant give the same answer as the send path.
  bool transmittable(uint8_t remote) const;

  // The counter the next press will use. Monotonic; the transmitted code is its low 16
  // bits. A counter that has stopped moving while presses are still logged is the
  // signature of a flash sector that has stopped accepting writes.
  uint32_t counter(uint8_t remote) const {
    return _store.valueOr(rs::NS_CODE, remote, 0);
  }

  // Raises a counter to a value the mirror supplied, persisting it before returning.
  // False means it could not be made durable and the remote must not transmit.
  bool adoptCounter(uint8_t remote, uint32_t value);

 private:
  SomfyRadio &_radio;
  Store &_store;
  CommandQueue _queue;
  bool _held = false;
  RemoteState _states[rs::MAX_REMOTES];
};
