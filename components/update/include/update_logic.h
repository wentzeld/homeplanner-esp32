// Over-the-air updates: the pure parts (host-tested) — comparing versions and reading GitHub's
// "latest release" answer.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define UPDATE_ASSET_NAME "homeplanner.bin"

// <0, 0, >0 like strcmp, numerically per part: "v1.2.10" > "1.2.9", "1.2" == "1.2.0".
// Anything after the numbers ("-beta") is ignored.
int update_version_cmp(const char *a, const char *b);

typedef struct {
    char version[32];   // "1.2.0" (tag without a leading "v")
    char notes[640];    // release notes, trimmed
    char url[512];      // download address of homeplanner.bin
    long size;          // bytes, 0 if unknown
} update_release_t;

// GitHub's /releases/latest JSON. False when it isn't a usable release (draft, prerelease, no
// homeplanner.bin, bad tag).
bool update_parse_release(const char *json, update_release_t *out);
