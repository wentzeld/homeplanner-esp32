#!/usr/bin/env bash
# Install a firmware build on the panel over Wi-Fi (instead of USB).
#   tools/ota.sh [panel address] [firmware file]
# The first time it asks for the 6-digit code the panel shows (gear > Manage from phone or computer);
# the sign-in is then kept for 90 days in ~/.config/homeplanner/cookies.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST="${1:-homeplanner.local}"
BIN="${2:-build/homeplanner.bin}"
[ -f "$BIN" ] || { echo "No $BIN yet: run idf.py build first." >&2; exit 1; }

JAR="$HOME/.config/homeplanner/cookies"
mkdir -p "$(dirname "$JAR")" && touch "$JAR" && chmod 600 "$JAR"
HDR=(-H "X-HomePlanner: 1")

if [ "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "http://$HOST/api/update")" != "200" ]; then
  echo "On the panel: tap the gear > Manage from phone or computer."
  read -r -p "6-digit code: " CODE
  CODE="${CODE//[^0-9]/}"
  RESULT=$(curl -sS -c "$JAR" "${HDR[@]}" -H "Content-Type: application/json" --data "{\"code\":\"$CODE\"}" "http://$HOST/api/login")
  echo "$RESULT" | grep -Eq '"ok": ?true' || { echo "Sign-in failed: $RESULT" >&2; exit 1; }
fi

echo "Sending $BIN ($(( $(wc -c < "$BIN") / 1024 )) KB) to $HOST..."
RESULT=$(curl -sS -b "$JAR" "${HDR[@]}" -H "Content-Type: application/octet-stream" --data-binary @"$BIN" "http://$HOST/api/update/upload")
echo "$RESULT"
echo "$RESULT" | grep -Eq '"ok": ?true'
