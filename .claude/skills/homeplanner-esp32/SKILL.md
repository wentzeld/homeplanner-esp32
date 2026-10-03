---
name: homeplanner-esp32
description: Context and procedures for the HomePlanner firmware for the Elecrow CrowPanel Advance ESP32-P4 10.1" (ESP-IDF + LVGL). Use when changing, building, testing, flashing or releasing this project — over-the-air updates, GitHub releases, the settings web page, Google sign-in, iCal calendars, the LVGL UI, or panel hardware issues.
---

# HomePlanner for the CrowPanel (ESP32-P4)

A family wall calendar: Google Calendar week view with add/edit/delete, other calendars (Google or iCal),
weather, screen sleep, a settings page on the home network, and signed over-the-air updates. This file is
what a new session needs to make a change and ship it. The README covers end-user setup.

## Ground rules (from the project owner)
- Follow the owner's CLAUDE.md workflow for implementation work (plan mode → questions → PRD → design
  options → approval) and never commit or push: the owner does that (signed commits, HTTPS remote).
- Never weaken or delete tests to make them pass; fix the code. Test changes need the owner's approval.
- **Seizure risk:** the panel has flickered blue before. Before any test that could flicker (display,
  flash writes, updates), tell the owner to look away or watch from the side at low brightness.
- Never print or commit secrets: `secrets.defaults` (Google OAuth client ID/secret), `signing_key.pem`
  (firmware signing key; the owner keeps a backup), `sdkconfig`. All three are git-ignored.
- Never burn eFuses (no flash encryption, no hardware secure boot).
- Keep big structs off task stacks (a ~11 KB calendar list once crashed the web server task): heap/PSRAM.
- The repo is public: no personal data (emails, calendar IDs, home IP/Wi-Fi) in code, tests or docs.

## Hardware and toolchain
- CrowPanel Advance 10.1" **V1.2**: ESP32-P4 (chip rev v1.3 → `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`), 16 MB
  flash (Zbit, no flash auto-suspend), 32 MB hex PSRAM at 200 MHz (needs `CONFIG_IDF_EXPERIMENTAL_FEATURES`).
- Display: EK79007 MIPI-DSI 1024×600, 2 lanes @ 900 Mbps, DPI 51 MHz, LDO3 2.5 V (PHY) + LDO4 3.3 V,
  backlight PWM GPIO31. Touch GT911 (SDA 45, SCL 46, RST 40, INT 42). See `components/board/board.c`.
- Wi-Fi via the on-board **ESP32-C6** running ESP-Hosted slave **2.12.x**; host side esp_hosted 2.12.3 +
  esp_wifi_remote over SDIO (CLK 18, CMD 19, D0–D3 17/16/15/14, reset GPIO32 **active high**, 20 MHz).
  Versions must match; the C6 firmware is not updated by this project.
- ESP-IDF **5.5.4** at `~/esp/esp-idf`; activate with `. ~/esp/esp-idf/export.sh` (every new shell).
- USB serial (CH34x driver on macOS): `/dev/cu.wchusbserial*` (usually `wchusbserial10`).
- Mac tools: Apple-silicon Homebrew in `/opt/homebrew` (`git`, `gh`); `gh` must be logged in for releases.

## Layout
| Path | What |
|---|---|
| `main/app_main.c` | Start-up: setup hotspot or run mode; `supervise()` shows the QR "finish setup"/"sign in again" screen or the calendar; wires web/update hooks |
| `components/logic` | Pure calendar rules (events, add/edit form, repeats, HTML notes) — host-tested |
| `components/ical`, `components/tz` | iCal parser + repeat expansion; POSIX TZ engine + IANA table (`tools/gen_tz.py`) — host-tested |
| `components/settings` | Settings/calendar-list JSON (pure, tested) + NVS storage (`settings_store.c`) |
| `components/gauth` | Sign in with Google: `oauth.c` (pure PKCE/state/token parsing, tested), `gauth.c` (refresh tokens) |
| `components/gcal` | Google Calendar API calls (events, calendar list, edits) |
| `components/model` | Background sync (Family every 2 min, other calendars 30 min, weather 30 min), LittleFS cache (`/storage`), merge of all events |
| `components/web` | HTTP server: hotspot setup page + home-network settings page (`page.html`), one-time code + session cookies (`web_auth.c`, tested), JSON API |
| `components/ui` | LVGL screens: `ui_week.c` (week view, legend, details), `ui_form.c` (add/edit), `ui_menu.c` (⚙ menu, QR screens, software update), `ui_basic.c` |
| `components/update` | OTA: GitHub release check, install, upload, rollback; `update_logic.c` is host-tested |
| `components/net`, `components/weather`, `components/board` | Wi-Fi/HTTPS/downloads, Open-Meteo, hardware + LVGL port |
| `docs/` | GitHub Pages: `oauth.html` + `relay.js` (sign-in relay), `privacy.html`, `index.html`, `install.html` (browser installer, ESP Web Tools 10.4.0 vendored in `docs/vendor/esp-web-tools/`), `VERIFY.md` |
| `.github/workflows/pages.yml` | Deploys `docs/` + the newest release's installer files (`/firmware/`) to Pages; no build, no secrets |
| `tests/host/` | Host unit tests (Unity); `tests/web/relay_test.js` (node); `tests/tools/test_gen_manifest.py` (python3) |
| `tools/` | `ota.sh` (install over Wi-Fi), `release.sh` (GitHub release), `gen_manifest.py` (installer files), fixture/TZ generators |

## Build, test, install
```bash
cd ~/Documents/Github/homeplanner-esp32
. ~/esp/esp-idf/export.sh
idf.py build                      # signed automatically when signing_key.pem exists
# after changing sdkconfig.defaults / secrets.defaults / adding the key: rm sdkconfig, then build
cmake -S tests/host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
tools/ota.sh                      # install build/homeplanner.bin on the panel over Wi-Fi
```
- `tools/ota.sh [host] [file]` asks once for the 6-digit code (panel: ⚙ → Manage from phone or computer) and
  keeps the session in `~/.config/homeplanner/cookies` for 90 days; default host `homeplanner.local`.
- USB flashing is only needed for a bricked panel or a partition/bootloader change:
  `idf.py -p /dev/cu.wchusbserial10 -b 460800 flash`. Settings in NVS (0x9000) survive.
- Serial logs without resetting the panel: open the port with `dtr = rts = False` before `open()` (pyserial
  from the ESP-IDF python env), or `idf.py -p … monitor` (resets it). Free the port before flashing.
- The iCal test fixture comes from the Pi repo next door (`../homeplanner/.venv`, `tools/gen_ical_fixture.py`);
  without it `test_ical` is skipped. The TZ fixture comes from Python `zoneinfo`.
- Check stack use of new code with `-fstack-usage` (compile commands in `build/compile_commands.json`).
- The settings page can be tried without the panel against a small mock server serving `components/web/page.html`.

## Releasing (owner runs the commands)
1. Raise `version.txt` (semver; must be newer than every released or rolled-back version).
2. Owner commits and pushes (`git commit -S …`, `git push`).
3. Owner runs `. ~/esp/esp-idf/export.sh && tools/release.sh "What's new"`: it checks for a clean, pushed tree,
   builds, verifies the signature, refuses test/demo builds and builds without `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`,
   collects `build/release/` with `tools/gen_manifest.py` (from `build/flasher_args.json`), creates a signed tag
   `v<version>`, pushes it and publishes the GitHub release with five files: `homeplanner.bin` (OTA; panels pick the
   asset with exactly that name) plus the browser installer's `bootloader.bin` 0x2000, `partition-table.bin` 0x8000,
   `ota_data_initial.bin` 0x19000 and `manifest.json` (app at 0x20000). It resumes if the tag exists on HEAD: it
   creates a missing release, or uploads (`--clobber`) the files an existing release lacks and publishes a draft.
4. Publishing the release triggers `.github/workflows/pages.yml`, which copies `docs/` and the newest release that has
   all five files into the Pages site (`/firmware/`), so `docs/install.html` installs that version. Without such a
   release it deploys the site without `/firmware/` (the sign-in relay keeps working).
5. Panels check daily (⚙ → Software update → Check now to force). A version must get online within 5 minutes
   of its first start (`update_mark_good()`), otherwise the bootloader rolls back and the panel says so.
- Release files contain the Google client ID/secret (accepted by the owner).
- **Pages deploys via GitHub Actions** (one-time: *Settings → Pages → Source = GitHub Actions*; before that it was
  "deploy from branch main /docs"). After switching (or any Pages change), re-test Google sign-in through
  `docs/oauth.html` (the relay must stay at `wentzeld.github.io/homeplanner-esp32/oauth.html`).
- The installer offers erase or keep: keeping settings relies on `/storage` being formatted when it can't be mounted
  (`format_if_mount_failed = true` in `model.c`); keep that.

## Things that bit us (don't reintroduce)
- **Blue screen flashes:** the display reads its picture from PSRAM; anything that pauses PSRAM starves it.
  Fixed by `CONFIG_SPIRAM_XIP_FROM_PSRAM` (flash writes no longer pause the cache), two framebuffers
  (`num_fbs = 2`, `avoid_tearing`, `direct_mode`), cache files written only on change (`cache_write()` in
  `model.c`, weather at most every 6 h), and the backlight held off while an update is written
  (`board_backlight_hold`, `update_set_screen_hook`). Keep flash writes rare.
- **Stack overflows:** `hp_calendars_t` (~11 KB) and similar must be heap-allocated; the web server task has 16 KB.
- **OAuth:** Google can only redirect to https on a public domain, so `docs/oauth.html` forwards the code to
  `http://<panel>/oauth/done` (only private IPs/`.local`). The consent screen must be "In production"
  (Testing mode expires sign-ins after 7 days); the app is unverified (warning screen, ≤100 users) until
  `docs/VERIFY.md` is done. Calendar scopes don't work with Google's TV/device flow.
- **Signing:** RSA-3072 "signed apps without secure boot"; the panel trusts the key of the firmware it runs.
  A lost `signing_key.pem` means a USB flash with a new key.
- **Old Mac tools:** an Intel-only `/usr/local/bin/git`/`gh` once broke `release.sh` ("Bad CPU type");
  the scripts fall back to working binaries.
- LVGL calls from non-UI tasks must hold the LVGL lock (`UI(...)` macro in `app_main.c`).
- The Montserrat fonts have no accented letters; weather is text-only (no icon font) — open ideas.
