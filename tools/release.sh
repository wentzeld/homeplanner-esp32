#!/usr/bin/env bash
# Publish a firmware release on GitHub; panels offer it under gear > Software update.
#   1. Put the new version in version.txt (e.g. 1.2.0) and commit everything.
#   2. . ~/esp/esp-idf/export.sh && tools/release.sh ["What's new"]
# Builds the signed firmware, tags v<version> (signed tag), pushes the tag and creates the release with
# homeplanner.bin (over-the-air updates) and the browser installer's files (bootloader.bin, partition-table.bin,
# ota_data_initial.bin, manifest.json; see docs/install.html) attached, all from build/release/.
# Without notes, GitHub writes them from the commits.
set -euo pipefail
cd "$(dirname "$0")/.."
die() { echo "release: $*" >&2; exit 1; }

# A git that runs on this Mac (an old Intel-only git earlier in PATH fails with "Bad CPU type").
GIT=$(command -v git || true)
if ! "$GIT" --version >/dev/null 2>&1; then GIT=/usr/bin/git; fi
"$GIT" --version >/dev/null 2>&1 || die "no working git found"
git() { "$GIT" "$@"; }

VERSION=$(tr -d ' \n' < version.txt)
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+)*$ ]] || die "version.txt must hold a version like 1.2.0"
[ -f signing_key.pem ] || die "signing_key.pem is missing (restore it from your backup)"
[ -f secrets.defaults ] || die "secrets.defaults is missing (the Google sign-in client)"
command -v idf.py >/dev/null || die "run '. ~/esp/esp-idf/export.sh' first"
GH=$(command -v gh || true)
for candidate in "$GH" /opt/homebrew/bin/gh; do
  if [ -n "$candidate" ] && "$candidate" --version >/dev/null 2>&1; then GH=$candidate; break; fi
  GH=""
done
[ -n "$GH" ] || die "a working GitHub CLI is needed (brew install gh), or publish on github.com/…/releases/new"
CHANGES=$(git status --porcelain) || die "git status failed"
[ -z "$CHANGES" ] || die "commit your changes first"
[ -z "$(git log --oneline @{u}..HEAD 2>/dev/null)" ] || die "push your commits first (git push)"
ASSETS=(homeplanner.bin bootloader.bin partition-table.bin ota_data_initial.bin manifest.json)
RESUME=""   # the tag may exist from an interrupted run: then the release, or some of its files, are missing
MISSING=()  # files to add to an existing release of this tag
DRAFT=""    # that release is still a draft (an upload was interrupted): publish it after adding the files
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
  [ "$(git rev-list -n1 "v$VERSION")" = "$(git rev-parse HEAD)" ] || die "v$VERSION exists already: raise version.txt"
  RESUME=1
  if "$GH" release view "v$VERSION" >/dev/null 2>&1; then
    HAVE=$("$GH" release view "v$VERSION" --json assets --jq '.assets[].name') || die "can't list the files of release v$VERSION"
    for f in "${ASSETS[@]}"; do
      grep -qxF "$f" <<<"$HAVE" || MISSING+=("$f")
    done
    DRAFT=$("$GH" release view "v$VERSION" --json isDraft --jq 'select(.isDraft) | "yes"') || die "can't read release v$VERSION"
    [ ${#MISSING[@]} -gt 0 ] || [ -n "$DRAFT" ] || die "release v$VERSION is already published: raise version.txt"
    echo "release v$VERSION exists; finishing it (missing files: ${MISSING[*]:-none}${DRAFT:+; still a draft})"
  fi
fi

idf.py build
espsecure.py verify_signature --version 2 --keyfile signing_key.pem build/homeplanner.bin | grep -q "verification successful" \
  || die "build/homeplanner.bin isn't signed with signing_key.pem"
grep -q "CONFIG_HP_OTA_TEST_NEVER_GOOD=y" sdkconfig && die "this is a rollback-test build; don't release it"
grep -q "CONFIG_HP_UPDATE_TEST_SCREEN_ON=y" sdkconfig && die "this test build keeps the screen on during updates; don't release it"
grep -q "CONFIG_HP_DEMO=y" sdkconfig && die "this is a demo build (made-up calendar); don't release it"
grep -qx "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y" sdkconfig \
  || die "sdkconfig must have CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y (the panel's ESP32-P4 is revision v1.x); delete sdkconfig and build again"

# Everything the release carries, in one folder: the browser installer flashes the four parts at the offsets
# in manifest.json; panels update over the air from homeplanner.bin.
PYTHON=$(command -v python3 || command -v python || true)
[ -n "$PYTHON" ] || die "python3 is needed for tools/gen_manifest.py"
rm -rf build/release
"$PYTHON" tools/gen_manifest.py build build/release "$VERSION" || die "couldn't prepare the installer files"
for f in "${ASSETS[@]}"; do [ -s "build/release/$f" ] || die "build/release/$f is missing"; done

if [ -z "$RESUME" ]; then
  git tag -s "v$VERSION" -m "HomePlanner $VERSION"
  git push origin "v$VERSION"
fi
if [ ${#MISSING[@]} -gt 0 ] || [ -n "$DRAFT" ]; then
  [ ${#MISSING[@]} -eq 0 ] || "$GH" release upload "v$VERSION" "${MISSING[@]/#/build/release/}" --clobber
  [ -z "$DRAFT" ] || "$GH" release edit "v$VERSION" --draft=false
  # Adding files to a published release doesn't trigger the Pages workflow; start it so the installer gets them.
  "$GH" workflow run pages.yml >/dev/null 2>&1 \
    || echo "release: start the Pages workflow yourself (Actions > Pages > Run workflow) to update the installer" >&2
else
  if [ $# -gt 0 ]; then NOTES=(--notes "$1"); else NOTES=(--generate-notes); fi
  "$GH" release create "v$VERSION" "${ASSETS[@]/#/build/release/}" --title "HomePlanner $VERSION" "${NOTES[@]}"
fi
echo "Released $VERSION. Panels find it within a day, or right away with Check now."
