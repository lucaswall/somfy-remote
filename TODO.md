# TODO

Where this project is going. Nothing here is committed to a design yet — the items past
the first section are questions with a preferred direction, not decisions.

Site-specific detail for every step below (which shutter to test first, what to recover,
what to back up where) is in `local/site.md`, which is gitignored. Keep it that way.

## Next session — the board

In order. Nothing after this section can be verified until this is done, because no board
has ever run this firmware.

1. **Back up the running board before touching it.** Connect USB and dump the flash:
   `esptool.py -p <port> read_flash 0 0x400000 old-firmware.bin`. Keep it forever — it is
   the rollback image, and the only copy of the rolling code counters at `0x3FB000`.
2. **Recover the secret values.** The remotes' address base is the one that cannot be
   guessed. The old firmware prints every address on its serial console at boot, so
   capture that in the same session: `tools/serial_log.py --seconds 12 --out`. See the
   migration section of `docs/hardware.md` for the fallbacks.
3. **Put the credentials somewhere that is not this laptop.** Address base, MQTT
   host/user/password, OTA password, WiFi. `include/secrets.h` is gitignored by design,
   which also means it is backed up by nothing. Store them in the agent workspace's secret
   store alongside the other projects' — see `CLAUDE.local.md` for where and under what
   key names.
4. **Flash and verify, in this order.** `make radio` first — it proves the CC1101 is wired
   before any protocol code runs. Then `make run`, read the banner, and check the pinmap
   line. Then, one at a time — every command that moves a shutter is sent by a human, not
   by the agent (RULE 1 in `CLAUDE.md`):
   - one remote, one Up, watched by eye;
   - the web UI: state, the console panel, `/status`, `/errors`;
   - the Home Assistant entities — all of them still present, cover state now reported,
     the My switch returning to off, availability going offline when the board is;
   - **OTA**, last and deliberately: `make ota OTA_HOST=<name-or-ip>`. Until this works
     the board is a USB-only device, and it lives in a case.

## Then — design work

Roughly in the order they are worth doing, not in the order they were raised.

5. **Full review of everything** (raised as #6). Architecture, code, project layout, docs.
   An ultracode deep review once there is a board to check the findings against — several
   of the riskiest things in here are unverifiable without one: the CC1101 register set is
   asserted rather than measured, the waveform timing has never met a real receiver, and
   the interrupts-stay-enabled decision is reasoned but untested.

6. **Add and remove remotes at runtime** (#7). Today the count is a compile-time constant
   and adding a shutter means a reflash, which is wrong. It should be a thing the device's
   own web UI does: allocate the next address, publish discovery, and on removal publish an
   empty retained config so Home Assistant drops the entity cleanly. Needs somewhere to
   keep the list — see the next item, they are the same question.

7. **Move the operating values off the device** (#4). Base address, per-remote rolling
   codes, the remote list. If they lived in Home Assistant or in retained MQTT, the device
   would become replaceable: flash a blank board, let it learn who it is, done.
   The hazard to design around is the rolling code. It has to increase on every press and
   never repeat — a counter that comes back from a retained topic *after* the first command
   has already been sent, or that two devices both believe they own, desynchronises the
   motor and costs a walk to every shutter. Whatever the design, the device probably still
   needs a local high-water mark. Worth checking what ESPHome and Tasmota do here before
   inventing anything.

8. **Receive, not just transmit** (#5). The CC1101 can listen, and this is the item with
   the most leverage: hearing the handheld remotes is the only way Home Assistant can ever
   know what actually happened to a shutter. It also unlocks position: covers take a known
   time to travel and can be stopped part-way, so a timing model plus overheard presses
   could report a real position instead of open/closed. Note the handhelds transmit under
   their own addresses, not ours, so there is a learning step. Check what receiving costs
   while the transmit path is bit-banged on the same pin.

9. **Everything else** (#8). Entity names, device classes and types are all still the 2023
   sketch's choices and are open for improvement — the constraint is only that a rename
   must not orphan an existing entity. Beyond that: sun and wind interlocks, groups and
   scenes, a real position slider once #8 lands, and whatever the review turns up.
