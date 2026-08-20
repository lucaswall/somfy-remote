#include "remotes.h"

#include <Arduino.h>

#include "log.h"

void Remotes::begin() {
  const uint8_t n = count();
  if (n == 0) {
    logLine("remotes   : none configured yet — waiting for Home Assistant");
    return;
  }
  logLine("remotes   : %u configured, next codes %lu..%lu", n, (unsigned long)counter(0),
          (unsigned long)counter((uint8_t)(n - 1)));
}

uint32_t Remotes::addressOf(uint8_t remote) const {
  const uint32_t override = _store.valueOr(rs::NS_ADDR, remote, rs::ADDR_NONE);
  if (override != rs::ADDR_NONE) {
    return override;
  }
  return _store.valueOr(rs::NS_SCALAR, rs::SCALAR_ADDRESS_BASE, 0) + remote;
}

bool Remotes::enabled(uint8_t remote) const {
  return (_store.valueOr(rs::NS_FLAGS, remote, 0) & rs::FLAG_ENABLED) != 0;
}

bool Remotes::operational(uint8_t remote) const {
  return (_store.valueOr(rs::NS_FLAGS, remote, 0) & rs::FLAG_OPERATIONAL) != 0;
}

bool Remotes::transmittable(uint8_t remote) const {
  return remote < count() && enabled(remote) && operational(remote) && hasCounter(remote) &&
         !adoptFailed(remote);
}

bool Remotes::observe(uint8_t remote, uint8_t command) {
  if (remote >= count() || !enabled(remote) || !operational(remote)) {
    return false;
  }
  _states[remote].observe(command, millis());
  return true;
}

void Remotes::queue(uint8_t remote, SomfyCommand command) {
  if (remote >= count()) {
    logError("remotes   : no remote %u, have %u", remote, count());
    return;
  }
  if (!_queue.push(remote, command)) {
    logError("remotes   : queue full, oldest command dropped");
  }
}

bool Remotes::adoptCounter(uint8_t remote, uint32_t value) {
  if (remote >= rs::MAX_REMOTES) {
    return false;
  }
  if (!_store.put(rs::NS_CODE, remote, value)) {
    logError("remotes   : could not persist adopted counter for remote %u", remote);
    _adoptFailed[remote] = true;
    return false;
  }
  _adoptFailed[remote] = false;
  return true;
}

void Remotes::loop() {
  // Before the early returns below: a shutter still travelling has to arrive whatever the
  // radio and the queue are doing.
  const uint32_t now = millis();
  for (uint8_t i = 0; i < count(); i++) {
    _states[i].tick(now);
  }

  // Leave commands queued while the radio is known down, or while reconciliation has not
  // decided whose counters win, rather than consuming them into nothing: a shutter that
  // moves late is better than one that never moves and reports that it did.
  if (_held || !_radio.ready() || _queue.empty()) {
    return;
  }

  QueuedCommand next;
  if (!_queue.pop(&next)) {
    return;
  }

  if (!transmittable(next.remote)) {
    logError("remotes   : remote %u not transmittable (%s), %s dropped", next.remote,
             !hasCounter(next.remote)  ? "no rolling code"
             : !enabled(next.remote)   ? "disabled"
             : !operational(next.remote) ? "not operational"
                                         : "adopted rolling code is not durable",
             somfyCommandName(next.command));
    markRejected(next.remote);
    return;
  }

  const uint32_t code = counter(next.remote);

  // Persisted before the frame goes out, never after, and the frame does not go out at all
  // if that fails. A code sent twice is a code the receiver rejects; a code burnt by a
  // reboot mid-transmission is one it skips over without complaint.
  if (!_store.put(rs::NS_CODE, next.remote, code + 1)) {
    logError("remotes   : remote %u %s not sent — rolling code is not durable",
             next.remote, somfyCommandName(next.command));
    markRejected(next.remote);
    return;
  }

  logLine("send      : remote %u %s (code %u)", next.remote,
          somfyCommandName(next.command), rs::transmitCode(code));

  if (_radio.send(next.command, addressOf(next.remote), rs::transmitCode(code))) {
    _states[next.remote].record(next.command, millis());
  } else {
    // Dropped, not re-queued: every retry would take a fresh rolling code, and a counter
    // run far past the last one the motor accepted costs a walk to every shutter. Losing
    // one unheard press is the cheaper failure — but it has to be on the record, because
    // nothing else names which command went missing.
    logError("send      : remote %u %s was not transmitted, command dropped", next.remote,
             somfyCommandName(next.command));
  }
}
