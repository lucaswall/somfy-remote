# Code standards

Binding on everyone working in this repository, human or agent. RULE 0 in `CLAUDE.md`
outranks everything here.

## Minimal by default

- Write the smallest thing that does the job. No abstraction, flag, or layer for a use
  case that does not exist yet.
- Comments explain **why**, not what. A comment restating the code is noise; a comment
  giving the reason for a pin choice, a timing constant, or a workaround is required.
  One line where one line does. A file over ~20% comment lines is usually explaining
  itself twice — a build flag needs a clause, not a paragraph.
- No commented-out code, ever. Git remembers it.
- Delete dead things in the same commit that makes them dead — unused files, headers
  nothing includes, `make` targets nothing runs, obsolete instructions in docs. A future
  agent cannot tell a deliberate leftover from an oversight, and will preserve both.

## Testing

Logic that does not touch hardware is tested on the desktop, where a failure costs a
second instead of a flash cycle.

```bash
make test          # pio test -e native
```

**Write the test first.** For anything with a definable input and output — the frame
codec, the pulse train, topic construction, state transitions — add the failing test,
then the code.

The split that makes this work:

| Where | What | Tested by |
|---|---|---|
| `include/*.h` | Pure logic, no Arduino headers, header-only | `pio test -e native` |
| `lib/CC1101/` | The radio driver: SPI, registers | On hardware, by `make radio` |
| `src/*.cpp` | Pins, peripherals, `Serial`, `WiFi`, MQTT | On hardware, by looking |

`pio test` does not build `src/` for the native environment, so anything under test must
live in a header under `include/`. This is a constraint worth keeping: it forces the
hardware-independent half of the codebase to stay hardware-independent.

Some things cannot be unit tested and should not be faked: RTS is one-way and the motor
reports nothing, so the only proof a command worked is that the shutter moved. Those
checks are human.

### The tests that are not really tests

`test_topics` and `test_rolling_code` pin strings and addresses that a live installation
depends on — Home Assistant entity ids and EEPROM offsets. They are not describing the
code, they are describing what must not change. Read `docs/somfy-rts.md` and the header
comments before "fixing" one of them.

## Hardware code

- Reason in **GPIO numbers**, never silkscreen labels. See the pin map in `CLAUDE.md`.
- Every pin choice and timing constant gets a one-line reason.
- Use `elapsed()` from `include/timing.h` for periodic work, never `delay()` in `loop()`
  and never `now >= last + interval` — that breaks on the `millis()` rollover. Inside a
  transmission is the one exception: the waveform is timed against absolute deadlines and
  the gaps between frames are the protocol's, not ours.

## Workflow

- **Never leave the repository dirty.** Commit and push a completed set of changes before
  moving on. When a new file appears, decide immediately whether it is tracked or
  gitignored, and act — an untracked file sitting in `git status` is a decision not made.
- **`.gitignore` is the real protection.** Git refuses to stage an ignored file without
  `-f`, and every secret lives in one of three ignored paths. `make check` only catches
  what gitignore cannot: a real address or credential typed into a tracked file. CI runs
  it on every push. Keep it that small, and there is no pre-commit hook: while the
  repository is private a bad commit is still fixable, and the gate that matters is a full
  `make check` immediately before making it public.
- No git tags. Use the commit log to find a working state.

## Serial

Never run `pio device monitor` from a script, a `make` target, or an agent tool call: it
requires a TTY on stdin and orphans a process holding the port without one. Use
`make log` / `tools/serial_log.py`, which always exits and always closes the port.
