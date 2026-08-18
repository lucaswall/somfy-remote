# local/ — never committed

Everything in this directory except this README is gitignored. **This repository is
destined to be public** (see RULE 0 in `CLAUDE.md`), so anything that came from the real
installation lives here and nowhere else.

Create `local/site.md` on your machine with whatever the work needs:

```markdown
# site notes (LOCAL ONLY)

## Network
bridge IP / hostname  : ...
DHCP reservation MAC  : ...
WiFi SSID             : ...            # password goes in include/secrets.h, not here

## MQTT
broker host / port    : ...
username              : ...            # password in include/secrets.h
device id / prefix    : ...

## Home Assistant
base URL              : ...
entity ids in use     : ...
areas                 : ...

## RF — the values that must never be lost
remote address base   : 0x......       # the key to real shutters — never commit
remotes emulated      : ...
which shutter is which: remote 0 = ..., remote 1 = ...
```

Rules of thumb:

- If a value came from a datasheet or an upstream project, it can be committed.
- If it came from *your* house, it belongs here.
- Never `git add -f` anything under `local/`.

The mapping from remote number to actual shutter is worth writing down properly. It exists
nowhere else — not in the firmware, which only knows indices, and not in Home Assistant,
which only knows whatever names somebody typed there.

## Bringing private context to agents

If you keep operational knowledge somewhere outside this repo — a personal wiki, notes, or
an agent workspace on your own machine — it can be made available to Claude Code without
ever reaching GitHub. The pattern this project uses:

1. **`CLAUDE.local.md`** at the repo root. Claude Code loads it automatically alongside
   `CLAUDE.md`, and it is gitignored. `CLAUDE.md` is the public half of the instructions,
   `CLAUDE.local.md` the private half. Put the pointers and the operational detail there.
2. **A mirror under `local/`** for anything worth reading offline, kept refreshed by a
   sync script that also lives under `local/` — so the script, not a tracked file, is what
   knows your host names.
3. **Read locally, execute remotely.** Mirror documentation, not credentials or scripts
   that depend on another machine's tokens and network position.

Nothing about this arrangement is committed except this description, which is deliberately
generic. A public reader learns the pattern and none of the contents.
