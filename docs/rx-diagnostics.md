# Receive diagnostics

Read `GET /status`; no arming, transmission or reset is needed.

- **rx dwell** reports the last complete 60-second window and the current partial
  window, with receive-enabled, muted/backoff, suspended and inactive milliseconds.
  Duty percentages are receive-enabled / muted. A zero-span last window means the
  first minute has not finished. Windows start at receiver initialization; timestamps
  are boot-relative milliseconds and wrap with `millis()`.
- Receive-enabled is software dwell, **not** proof of continuous MARCSTATE RX or
  decoded traffic. Frames/presses and instantaneous MARCSTATE remain separate evidence.
  Mute time includes receive-entry failures and chip-left-RX backoff, not just overload.
- **rx config** freezes the first sustained-overload mute's active register state,
  after the ISR is detached but before release changes IOCFG0. It stays pending if
  no overload mute occurs, and resets only with the firmware's RAM on reboot.
- Each IOCFG0, PKTCTRL0, MDMCFG, AGCCTRL, FSCTRL and FREQ byte is read twice.
  Stable unequal readback is `MISMATCH`; failed or disagreeing reads are `INVALID`.
  Masks use the printed register order. A match is evidence for that instant only,
  not proof against electrical interference or a transient configuration fault.

`readConfig()` accepts only configuration addresses 0x00–0x2E and a non-null output
pointer. Failed reads leave the output unchanged. Snapshot collection sends no
strobes or writes, changes no gain/frequency/power, and adds no recovery/reset path.
Existing protective mute and retry behavior is unchanged.

Before firmware-only OTA, preserve the running image and verify its build stamp.
Rollback uses that firmware image through the same ArduinoOTA transport, **never**
a historical full-flash/store backup: rolling codes must not move backwards.
After reboot, verify the new build, configuration, remote readiness and non-decreasing
codes, then a complete dwell window. Never induce RF traffic or move a shutter to
test these diagnostics; a pending snapshot is a valid result.
