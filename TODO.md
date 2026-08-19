# TODO

Where this project is going. Nothing past the first section is committed to a design —
those are questions with a preferred direction, not decisions.

Site-specific detail for every step below (which shutter to test first, what to recover,
what to back up where) is in `local/site.md`, which is gitignored. Keep it that way.

## Done — the board

The firmware runs on the installed board. Migration from the 2023 sketch is complete: the
counters were carried across, verified, and the old EEPROM layout erased.

1. ~~Back up the running board.~~ Full 4 MB image kept, with its sha256 recorded.
2. ~~Recover the secret values.~~ Address base and every credential, off the serial console
   and out of the flash image.
3. ~~Put the credentials somewhere that is not this laptop.~~ In the agent workspace's
   secret store, audited by digest so the check is on values and not key names.
4. ~~Flash and verify.~~ Banner and pinmap, a live press by eye, the web UI, all Home
   Assistant entities, availability going `unavailable` on a last will, and OTA.

**One step of (4) was skipped: `make radio` has never been run on this board.** It replaces
the firmware with the standalone self-test, and by the time it mattered the boot banner
already reported the CC1101 answering and a real shutter had moved. Worth running the next
time the board is on a bench for another reason — it is the only thing that measures the
radio rather than inferring it from a working send.

## Next

5. **Full review, now that findings can be checked against hardware.** Earlier reviews
   covered the storage design (adversarially) and the Home Assistant layer. What none of
   them touched is the part that was unverifiable without a board and is now merely
   untested: **the CC1101 register set is asserted rather than measured**, and the
   **interrupts-stay-enabled decision is reasoned but unmeasured**. A shutter moving proves
   the waveform is close enough for one receiver at one distance; it does not prove margin.

6. ~~Add and remove remotes at runtime.~~ Done: the device's settings page allocates the
   next never-used index, publishes discovery, and on removal clears the retained config so
   Home Assistant drops the entity. Removal keeps the index and the rolling code, so
   restoring a remote resumes where it left off.

7. ~~Move the operating values off the device.~~ Done: configuration and a mirror of every
   rolling code live in retained MQTT, and a blank board recovers from them. The hazard
   this item named — a counter coming back from a retained topic after a command has
   already gone out — is handled exactly as it predicted, with a local high-water mark: the
   store is authoritative while running and the mirror is a floor that may only ever raise.
   See `docs/storage.md` and `docs/recovery.md`.

   **The prior-art check this item asked for was skipped.** It said to look at what ESPHome
   and Tasmota do before inventing anything, and nothing was invented with them in view.
   The design survived an adversarial review, but that is not the same as knowing whether
   somebody solved it better first. Worth an hour before the next storage change.

8. **Receive, not just transmit.** Unstarted, and now the item with the most leverage by
   some distance. The CC1101 can listen, and hearing the handheld remotes is the only way
   Home Assistant can know what actually happened to a shutter rather than what the bridge
   believes it caused — which is still, after all of the above, an inference.

   It also unlocks position: covers take a known time to travel and can be stopped
   part-way, so a timing model plus overheard presses could report a real position instead
   of open/closed. The handhelds transmit under their own addresses, so there is a learning
   step. Check what receiving costs while the transmit path is bit-banged on the same pin.

9. **Everything else.** Entity names, device classes and the entity set were reviewed and
   cleaned: names now derive from one place, covers carry `device_class: shutter`, Prog is
   no longer a Home Assistant entity because pairing is configuration rather than
   operation, and the rolling code is exposed as a diagnostic. What remains from this item:
   sun and wind interlocks, groups and scenes, and a real position slider once (8) lands.

## Watch

Not tasks, but things that should not be discovered by surprise.

- **The first compaction has never run on hardware.** The logic is covered by desktop tests
  including the torn-write and sector-full cases, but no real sector has been erased and
  rotated yet — roughly 350 presses away at the time of writing. It will announce itself in
  the log and by the store's active sector flipping from A to B, which is visible in Home
  Assistant.
- **The 16-bit rolling code space.** One remote is closer to the ceiling than the others and
  reaches it around 2030. The counter is stored as a monotonic 32-bit value and wraps only
  at the frame, which is where the wrap is pinned by a test — but no motor here has met a
  wrap.
- **The pre-migration flash image exists in one place.** It is the only copy of the counters
  as they were before the rework, and it is on one laptop by deliberate choice.
