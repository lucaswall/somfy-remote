#include "remotes.h"

#include <EEPROM.h>

#include "log.h"

Remotes::Remotes(SomfyRadio &radio, uint32_t addressBase, uint8_t count)
    : _radio(radio), _base(addressBase),
      _count(count > ROLLING_CODE_MAX_REMOTES ? ROLLING_CODE_MAX_REMOTES : count) {}

void Remotes::begin() {
  EEPROM.begin(ROLLING_CODE_EEPROM_SIZE);
  if (_count == 0) {
    logError("remotes   : SOMFY_REMOTE_COUNT is 0, there is nothing to control");
    return;
  }
  logLine("remotes   : %u emulated, next codes %u..%u", _count, rollingCode(0),
          rollingCode((uint8_t)(_count - 1)));
}

void Remotes::queue(uint8_t remote, SomfyCommand command) {
  if (remote >= _count) {
    logError("remotes   : no remote %u, have %u", remote, _count);
    return;
  }
  if (!_queue.push(remote, command)) {
    logError("remotes   : queue full, oldest command dropped");
  }
}

void Remotes::loop() {
  // Leave commands queued while the radio is known down rather than consuming them into
  // nothing: a shutter that moves late is better than one that never moves and reports
  // that it did.
  if (!_radio.ready() || _queue.empty()) {
    return;
  }

  QueuedCommand next;
  if (!_queue.pop(&next)) {
    return;
  }

  const uint16_t code = takeRollingCode(next.remote);
  logLine("send      : remote %u %s (code %u)", next.remote,
          somfyCommandName(next.command), code);

  if (_radio.send(next.command, _base + next.remote, code)) {
    _states[next.remote].record(next.command);
  } else {
    // Dropped, not re-queued: every retry would take a fresh rolling code, and a counter
    // run far past the last one the motor accepted costs a walk to every shutter. Losing
    // one unheard press is the cheaper failure — but it has to be on the record, because
    // nothing else names which command went missing.
    logError("send      : remote %u %s was not transmitted, command dropped", next.remote,
             somfyCommandName(next.command));
  }
}

uint16_t Remotes::rollingCode(uint8_t remote) const {
  uint16_t code = 0;
  EEPROM.get(rollingCodeAddress(remote), code);
  return code;
}

// Persisted before the frame goes out, never after. A code sent twice is a code the
// receiver rejects; a code burnt by a reboot mid-transmission is one it skips over
// without complaint.
uint16_t Remotes::takeRollingCode(uint8_t remote) {
  const uint16_t code = rollingCode(remote);
  const uint16_t next = rollingCodeNext(code);
  EEPROM.put(rollingCodeAddress(remote), next);
  if (!EEPROM.commit()) {
    // Not fatal for this press, but the next boot will replay this code and the shutter
    // will ignore it.
    logError("remotes   : EEPROM write failed for remote %u", remote);
  }
  return code;
}
