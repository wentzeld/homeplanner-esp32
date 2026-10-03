#!/usr/bin/env bash
# Save what the panel shows as a PNG (demo builds only: tools/demo.sh install).
#   tools/screenshot.sh [panel address] [file]     default: homeplanner.local docs/screenshot.png
# Uses the sign-in tools/ota.sh keeps in ~/.config/homeplanner/cookies (it asks for the code if needed).
set -euo pipefail
cd "$(dirname "$0")/.."
HOST="${1:-homeplanner.local}"
OUT="${2:-docs/screenshot.png}"
JAR="$HOME/.config/homeplanner/cookies"
mkdir -p "$(dirname "$JAR")" && touch "$JAR" && chmod 600 "$JAR"

if [ "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "http://$HOST/api/update")" != "200" ]; then
  echo "On the panel: tap the gear > Manage from phone or computer."
  read -r -p "6-digit code: " CODE
  CODE="${CODE//[^0-9]/}"
  RESULT=$(curl -sS -c "$JAR" -H "X-HomePlanner: 1" -H "Content-Type: application/json" --data "{\"code\":\"$CODE\"}" "http://$HOST/api/login")
  echo "$RESULT" | grep -Eq '"ok": ?true' || { echo "Sign-in failed: $RESULT" >&2; exit 1; }
fi

TMP=$(mktemp -t homeplanner).bmp
trap 'rm -f "$TMP"' EXIT
STATUS=$(curl -sS -b "$JAR" -o "$TMP" -w '%{http_code}' "http://$HOST/api/screenshot")
[ "$STATUS" = 200 ] || { echo "No screenshot (HTTP $STATUS): is the demo build installed? (tools/demo.sh install)" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
sips -s format png "$TMP" --out "$OUT" >/dev/null
echo "Saved $OUT ($(sips -g pixelWidth -g pixelHeight "$OUT" | awk '/pixel/ {printf "%s ", $2}')pixels)"
