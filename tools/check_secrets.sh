#!/usr/bin/env bash
# RULE 0 backstop. .gitignore is the actual control — git refuses to stage an ignored file
# without -f, so secrets.h, local/ and CLAUDE.local.md cannot reach a commit by accident.
# This only catches the mistake gitignore cannot: a real address or credential typed into a
# tracked file.
#
#   tools/check_secrets.sh

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
allow='192\.0\.2\.|198\.51\.100\.|203\.0\.113\.|0\.0\.0\.0|127\.0\.0\.1|255\.255\.255\.255|<[A-Z_]+>|your-|placeholder|xx:xx|change-me|"0x000000"'

fail=0
for pat in "$ipv4" "$mac" "$cred" "$rfid"; do
  # The allowlist is matched against the line's text only. git grep prefixes every hit with
  # "path:line:", and matching that too meant any path containing an allowed word — TODO.md,
  # include/secrets.h.example — was exempt from every pattern, whole file.
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
