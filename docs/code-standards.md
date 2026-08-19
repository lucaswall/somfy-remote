# Code standards

Binding on everyone working in this repository, human or agent. RULE 0 in `CLAUDE.md`
outranks everything here.

## Minimal by default

- Write the smallest thing that does the job. No abstraction, flag, or layer for a use
  case that does not exist yet.
- No commented-out code, ever. Git remembers it.

### Comments earn their place or they go

A comment is worth writing for exactly four reasons:

1. an edge case the code cannot show on its own,
2. something counter-intuitive, where the obvious reading is wrong,
3. a warning that something must not be changed without knowing why,
4. a constant or a pin choice whose value came from somewhere outside the file.

Everything else is bloat, and **bloat costs the same as a stale document or a dead
function**: it is one more thing a reader has to check against the code, and one more thing
that silently stops being true.

Three kinds show up again and again and are always wrong:

- **History.** "An earlier version did X", "this used to be Y", "the correction to a bug
  that…". `git log` and `git blame` hold this, and hold it accurately. A comment describing
  a bug that no longer exists describes nothing.
- **The obvious.** Anything a reader gets from the line below it. If the code says
  `_edgeRate = 0`, it does not need a sentence saying the rate is cleared.
- **Over-fitting a fix.** A paragraph justifying a change, written while making it, at the
  point where it was made. The change needs a commit message, not a monument. Ask what a
  reader who has never seen the bug needs — usually one clause, often nothing.

One line where one line does. **A file over ~20 % comment lines is explaining itself
twice**; measure it (`grep -cE '^\s*(//|/\*|\*)'`) rather than guessing, and cut rather
than justify.
- Delete dead things in the same commit that makes them dead — unused files, headers
  nothing includes, `make` targets nothing runs, obsolete instructions in docs. A future
  agent cannot tell a deliberate leftover from an oversight, and will preserve both.

## The web UI is load-bearing

`/`, `/settings` and `/controls` are not debug output. They are opened standing in front of
a shutter, one-handed, on a phone, sometimes to decide whether the bridge or the motor is the
problem — and `/controls` is the only way the learned map is ever built. Treat their design
with the same seriousness as the send path.

**Constraints belong at the input.** A field must not accept what the device will refuse.
`maxlength` on every text input, `min`/`max` on every number, and the limit visible *before*
it is reached — a counter, not an error afterwards. A server-side check is the backstop, never
the only guard.

**Never silently alter what somebody typed.** Truncating a name to fit is worse than refusing
it: the result still looks like a name, so nobody notices. Three controls were once cut to the
same 23 characters and became indistinguishable. Refuse, say why, and say what the limit is.

**Never redraw over an edit in progress.** A polled page must leave alone any field being
typed into and any control being changed. Replace rows when the *set* of rows changes, not on
every tick.

**Design for the list being long.** Every screen here grows: twelve remotes, thirty-odd
controls. One compact line per item, detail on demand, and never a form per row rendered all
at once. If a page is unusable at the size the installation will actually reach, it does not
work.

**Order by what the person is doing.** During a naming walk the control just pressed is the
one that matters, so it sorts first. Check the sort direction against real data — ages and
timestamps sort opposite ways, and getting it backwards puts the wanted row last.

**Confirm the irreversible, and nothing else.** Prog and Forget ask. Up, Down and Stop do not.
A confirmation on a reversible action is noise that trains people to click through the ones
that matter.

**Say what to do, not what failed.** "The broker did not accept it — nothing was saved" beats
a status code. If an action did not take effect, the page must not look as though it did.

**Escape everything that came from somewhere else.** Names arrive from Home Assistant and from
the form; both reach `innerHTML` and hand-built JSON. Escape at the emitter.

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
depends on — Home Assistant entity ids and the record format. They are not describing the
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
