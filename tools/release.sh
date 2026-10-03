#!/usr/bin/env bash
# Publish a firmware release on GitHub; panels offer it under gear > Software update.
#   1. Put the new version in version.txt (e.g. 1.2.0) and commit everything.
#   2. . ~/esp/esp-idf/export.sh && tools/release.sh ["What's new"]
# Builds the signed firmware, tags v<version> (signed tag), pushes the tag and creates the release
# with build/homeplanner.bin attached. Without notes, GitHub writes them from the commits.
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
command -v gh >/dev/null || die "the GitHub CLI (gh) is needed"
CHANGES=$(git status --porcelain) || die "git status failed"
[ -z "$CHANGES" ] || die "commit your changes first"
[ -z "$(git log --oneline @{u}..HEAD 2>/dev/null)" ] || die "push your commits first (git push)"
! git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null || die "v$VERSION exists already: raise version.txt"

idf.py build
espsecure.py verify_signature --version 2 --keyfile signing_key.pem build/homeplanner.bin | grep -q "verification successful" \
  || die "build/homeplanner.bin isn't signed with signing_key.pem"
grep -q "CONFIG_HP_OTA_TEST_NEVER_GOOD=y" sdkconfig && die "this is a rollback-test build; don't release it"

git tag -s "v$VERSION" -m "HomePlanner $VERSION"
git push origin "v$VERSION"
if [ $# -gt 0 ]; then NOTES=(--notes "$1"); else NOTES=(--generate-notes); fi
gh release create "v$VERSION" build/homeplanner.bin --title "HomePlanner $VERSION" "${NOTES[@]}"
echo "Released $VERSION. Panels find it within a day, or right away with Check now."
