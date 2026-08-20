#!/usr/bin/env python3
"""Write the retained configuration document for a somfy-remote bridge.

The firmware carries no addresses and no remote count: it reads both from
`<device id>/config`, which is what lets a replacement board recover instead of needing a
rebuild. This writes that topic.

    tools/seed_config.py --host <BROKER_IP> --user <U> --password <P> \\
                         --base 0x000000 --count 8 --not-operational 6,7

RULE 0: the address is the RF credential of a real installation. Pass it on the command
line or via SOMFY_ADDRESS_BASE in the environment — never commit it.

Reads the current document first and takes epoch + 1, so seeding twice does not go
backwards and a device that already has a newer one is not overwritten.

Re-running preserves every field of an existing entry and overwrites only `enabled` and
`operational` — per-remote `addr` overrides and `travel` times set from the web UI survive.
`--travel N` additionally sets the same travel time on every remote.
A `--count` lower than the highest existing index is refused, because an index that falls
out of the array is cleared rather than left alone; pass --allow-shrink to mean it.
"""

import argparse
import json
import os
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("error: no paho-mqtt. fix: pip install paho-mqtt")


def parse_indices(text):
    if not text:
        return set()
    return {int(part) for part in text.split(",") if part.strip() != ""}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--device-id", default="wemos_somfy_remote")
    ap.add_argument("--base", default=os.environ.get("SOMFY_ADDRESS_BASE"),
                    help="address of remote 0, e.g. 0x000000")
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--not-operational", default="",
                    help="comma-separated indices the firmware must refuse to transmit for")
    ap.add_argument("--disabled", default="",
                    help="comma-separated indices with no Home Assistant entities")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--allow-shrink", action="store_true",
                    help="permit a --count that drops existing indices and their overrides")
    ap.add_argument("--travel", type=int,
                    help="end-to-end travel seconds, applied to every remote; omit to "
                         "leave each remote's own value alone")
    args = ap.parse_args()

    if not args.base:
        sys.exit("error: --base is required (or set SOMFY_ADDRESS_BASE)")
    base = int(args.base, 16) if args.base.lower().startswith("0x") else int(args.base, 16)

    blocked = parse_indices(args.not_operational)
    disabled = parse_indices(args.disabled)

    topic = f"{args.device_id}/config"
    existing = {"epoch": 0}

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.user:
        client.username_pw_set(args.user, args.password)

    received = {}

    def on_message(_client, _userdata, msg):
        try:
            received.update(json.loads(msg.payload))
        except ValueError:
            pass

    client.on_message = on_message
    client.connect(args.host, args.port, 10)
    client.subscribe(topic)
    client.loop_start()
    time.sleep(2)
    client.loop_stop()
    if received:
        existing = received
        print(f"existing document: epoch {existing.get('epoch')} "
              f"by {existing.get('writer')}", file=sys.stderr)

    # Carry the existing entries forward whole. Copying the entire dict rather than named
    # fields is deliberate: a field a future firmware adds must not be destroyed by an older
    # copy of this tool.
    by_index = {
        int(e["i"]): dict(e)
        for e in existing.get("remotes", [])
        if isinstance(e, dict) and "i" in e
    }

    # An index that falls out of the array is not left alone — cfg::project() clears its
    # address and flags — so a shrink silently destroys that index's override.
    dropped = sorted(i for i in by_index if i >= args.count)
    if dropped and not args.allow_shrink:
        sys.exit(
            f"error: --count {args.count} would drop remote(s) {dropped} and any address "
            f"override or travel time they carry. Re-run with --allow-shrink to mean it."
        )

    remotes = []
    for i in range(args.count):
        entry = dict(by_index.get(i, {"i": i}))
        entry["i"] = i
        entry["enabled"] = i not in disabled
        # A shutter known not to work is refused in the send path, not merely left out of
        # automations.
        entry["operational"] = i not in blocked
        # Only when asked. Omitting the flag leaves whatever each remote already carries,
        # which is the point of preserving entries in the first place.
        if args.travel is not None:
            entry["travel"] = args.travel
        remotes.append(entry)

    doc = {
        "v": 1,
        "epoch": int(existing.get("epoch", 0)) + 1,
        "writer": "seed",
        "hash": 0,
        "base": f"0x{base:06X}",
        "remotes": remotes,
    }

    if args.dry_run:
        for i in range(args.count):
            before, after = by_index.get(i), remotes[i]
            if before is None:
                print(f"  remote {i}: new", file=sys.stderr)
            elif before != after:
                changed = {k: (before.get(k), after.get(k))
                           for k in set(before) | set(after)
                           if before.get(k) != after.get(k)}
                print(f"  remote {i}: {changed}", file=sys.stderr)
        for i in dropped:
            print(f"  remote {i}: DROPPED (was {by_index[i]})", file=sys.stderr)

    payload = json.dumps(doc, separators=(",", ":"))
    print(f"{topic} ({len(payload)} bytes, epoch {doc['epoch']})", file=sys.stderr)
    if args.dry_run:
        print(payload)
        return

    client.publish(topic, payload, qos=0, retain=True)
    client.loop_start()
    time.sleep(1)
    client.loop_stop()
    client.disconnect()
    print("published retained", file=sys.stderr)


if __name__ == "__main__":
    main()
