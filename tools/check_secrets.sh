#!/usr/bin/env bash
# RULE 0 backstop. .gitignore is the actual control — git refuses to stage an ignored file
# without -f, so secrets.h, local/ and CLAUDE.local.md cannot reach a commit by accident.
# This only catches the mistake gitignore cannot: a real address or credential typed into a
# tracked file.
#
# **It reads tracked files in the working tree, and nothing else.** Not commit messages, not
# history, not staged-but-unwritten content. A value already committed is past this gate, and
# the only remedy then is rewriting history before the repository is published. Read your own
# diff; this catches shapes, not intent.
#
#   tools/check_secrets.sh
#
# Example addresses in tracked files start 00 — that is the convention the patterns below
# rely on to tell a placeholder from a real one, and it is why docs use control/000000.

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

content=$(git grep -nI '' -- . ':!tools/check_secrets.sh' 2>/dev/null)

# No \b: git's regex engine matches nothing on it, which would silently disable the check.
ipv4='[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}'
mac='[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}'
# The separator has to allow bare whitespace as well as = and :, because the shape every
# value in secrets.h has is `#define WIFI_PASSWORD "..."` — with only [=:], the one form
# these credentials actually take was the one form that got through.
cred='(PASSWORD|PASSWD|SECRET|TOKEN|API_?KEY|PSK|SSID)[[:space:]"]*([=:]|[[:space:]]+")[[:space:]]*"?[A-Za-z0-9._/+-]{6,}'
# The remote address is the RF credential CLAUDE.md singles out, and no generic pattern can
# recognise a bare hex number. What is recognisable is the shape it lives in.
#
# It no longer lives in a macro. The address moved out of the firmware and into the
# retained configuration document, so the old `SOMFY_ADDRESS_BASE 0x…` pattern would now
# match nothing at all — a scanner that cannot fire is worse than no scanner, because it
# still reads like protection. The shape to catch is the document's own: a "base" or
# "addr" key carrying hex, which is exactly what a worked example pasted into a doc looks
# like.
rfid='"(base|addr)"[[:space:]]*:[[:space:]]*"0[xX][0-9a-fA-F]'
# The learned controls put an address in a *topic path* rather than in a JSON value, so the
# pattern above cannot see one — and a scanner that cannot fire is worse than no scanner,
# because it still reads like protection. These are the addresses of the handhelds in this
# house and its neighbours', which is the same class of secret as our own base.
# The convention is the same one the configuration document already uses: an example
# address is obviously synthetic. Here that means it starts 00, which no address in this
# installation does — so a real one pasted into a doc, a test or a commit message fires.
ctl='/control/([1-9a-fA-F][0-9a-fA-F]|[0-9a-fA-F][1-9a-fA-F])[0-9a-fA-F]{4}'
# The same address written the other three ways it actually appears: the decimal form the
# save endpoint parses and /api/heard emits, an "address" JSON key, and a bare macro of the
# kind the 2023 firmware used. Only the topic shape was covered, which is one of four.
ctldec='address=[0-9]{6,8}|"a":[0-9]{6,8}'
addrkey='"(address|remote|base|addr)"[[:space:]]*:[[:space:]]*"?0[xX][0-9a-fA-F]{4,6}'
addrmacro='#define[[:space:]]+[A-Z_]*(ADDRESS|REMOTE)[A-Z_]*[[:space:]]+0[xX][0-9a-fA-F]{4,6}'
# **A bare 24-bit literal in C, which is how an address reaches a test.** Every pattern above
# needs the address to sit in a recognisable wrapper — a JSON key, a topic path, a macro — and
# a captured address pasted into a unit test has none of them. One got through exactly that
# way: a real wall switch's address, in an assertion, with the whole suite green.
#
# Six hex digits that do not begin 00, terminated by a non-hex character so a longer literal
# like 0xDEADBEEF or 0xFFFFFFFF is not matched on its prefix.
addrbare='0[xX](0[1-9a-fA-F]|[1-9a-fA-F][0-9a-fA-F])[0-9a-fA-F]{4}([^0-9a-fA-F]|$)'
allow='192\.0\.2\.|198\.51\.100\.|203\.0\.113\.|0\.0\.0\.0|127\.0\.0\.1|255\.255\.255\.255|<[A-Z_]+>|your-|placeholder|xx:xx|change-me|"0x000000"|/control/00[0-9a-fA-F]{4}|/control/[+<{$]|0[xX]0{4,6}|address=0|"a":0'

# Six-digit hex that is not an address at all, plus the synthetic addresses the tests already
# use. **A new address must start 00; adding to this list instead is a decision, not a
# formality.** The flash offsets and the all-ones masks are here because they are genuinely
# not addresses; the rest are test constants that predate the 00 convention and are left
# alone because the golden frames are deliberately computed from an independent
# implementation and must not be regenerated from the code under test.
synthetic='0[xX](3FA000|3FB000|FFFFFF|ABCDEF|123456|123457|AABBCC|765432|0A0A0A|0B0B0B|0C0C0C|010203|100000|200000|223344|654321|0A1B2C|0C0D0E)'
allow="$allow|$synthetic"

fail=0
for pat in "$ipv4" "$mac" "$cred" "$rfid" "$ctl" "$ctldec" "$addrkey" "$addrmacro" "$addrbare"; do
  # The allowlist is matched against the line's text only. git grep prefixes every hit with
  # "path:line:", and matching that too would exempt whole files whose *path* happens to
  # contain an allowed word.
  hits=$(printf '%s\n' "$content" | grep -E "$pat" \
         | awk -v allow="$allow" '{ text = $0; sub(/^[^:]*:[0-9]+:/, "", text);
                                    if (text !~ allow) print }')
  if [ -n "$hits" ]; then
    printf 'SUSPECT /%s/\n%s\n' "$pat" "$hits" | sed 's/^/        /'
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "RULE 0: use <HUB_IP> or an RFC 5737 address (192.0.2.x). Real values go in local/." >&2
  exit 1
fi
echo clean
