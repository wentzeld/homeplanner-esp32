# HomePlanner for the CrowPanel (ESP32-P4)

**A family wall calendar on a 10.1" touch panel, no computer needed.** It shows your family's Google
Calendar for the week, lets anyone at home add and change events right on the screen, and shows the
weather. Setup is done with a phone: join the panel's Wi-Fi, scan a QR code, *Sign in with Google*, pick
your family calendar.

This is the standalone firmware version of [HomePlanner for the Raspberry Pi](https://github.com/wentzeld/homeplanner).

## Features

- **Week at a glance:** Sunday to Saturday, today highlighted, past days dimmed; swipe or tap ◀ ▶ to change weeks.
- **Google Calendar:** shows your family calendar and syncs every 2 minutes; changes from phones appear by themselves.
- **Add, edit, delete** events on the panel (on-screen keyboard, date and time pickers), including repeating
  events (*This event* / *All events*). Events appear in Google as made by you.
- **A color per person** (Google's event colors, so the panel matches everyone's phones). Tap a name in the
  legend to highlight just their events.
- **Other calendars,** read-only in their own color: any of your Google calendars (for example a school
  calendar you subscribed to) or any iCal link (`.ics` / `webcal://`), with repeating events expanded on the panel.
- **Weather** from [Open-Meteo](https://open-meteo.com/) for your city or ZIP code (no account needed).
- **Screen sleep** at night on a schedule; a touch wakes it for a few minutes.
- **Works offline** with the last synced events, and recovers by itself after power cuts and Wi-Fi drops.
- **Settings from your phone or computer** at `http://homeplanner.local`, unlocked with a code from the panel.
- **Updates over Wi-Fi:** new versions from GitHub install with one tap; a version that doesn't work is undone
  automatically. Only firmware signed with the project's key is accepted.

## What you need

- An **Elecrow CrowPanel Advance ESP32-P4 10.1"** (version **1.2**, with the ESP32-C6 Wi-Fi chip) and its USB-C power.
- A **Google account** with a calendar for the family (in Google Calendar: *Other calendars → + → Create new
  calendar*, then share it with everyone in the family).
- Home **Wi-Fi** (2.4 GHz).

## Setting it up at home

1. **Power the panel.** It shows a QR code and its own Wi-Fi name and password.
2. **Scan that QR code with your phone** to join the panel's Wi-Fi. The setup page opens by itself (if not,
   open `http://192.168.4.1`). Choose your home Wi-Fi, type its password, and tap **Save and connect**.
3. The panel restarts, joins your Wi-Fi and shows **Finish setup on your phone** with a new QR code. Put your
   phone back on your home Wi-Fi and **scan it**.
4. On the page that opens: **Sign in with Google** (Google may warn that the app isn't verified yet: tap
   *Advanced → Go to HomePlanner*), choose your **Family calendar**, add the **family members** with their
   colors, your **city or ZIP code**, and tap **Save and start**.

The panel restarts and shows your week. Done.

## Using it

- **Add** an event with **+ Add**; tap an event to see it, **Edit** or **Delete** it.
- **Who's it for:** pick a family member when adding; the event gets their color in Google too.
- **Settings:** tap **⚙** on the panel:
  - **Manage from phone or computer** shows a QR code and a 6-digit code. Scan it, or open
    `http://homeplanner.local` (or the address shown) on a computer and type the code. That device stays
    signed in for 90 days.
  - **Change settings (hotspot)** starts the setup Wi-Fi again (to change the home Wi-Fi network).
  - **Sign out all computers** forgets every phone and computer that was signed in.
  - **Software update** shows the version and whether a newer one is available; **Install** downloads it and
    restarts. The panel checks once a day; the settings page has the same button.
- **Other calendars:** on the settings page, tick any of your Google calendars, or paste an iCal link
  (for a Google calendar that isn't yours: its *Secret address in iCal format*). They refresh every 30 minutes.
- **Disconnect Google** on the settings page removes the panel's access (it also asks Google to withdraw it).

## Building and flashing (developers)

1. **ESP-IDF 5.5.4:**
   ```bash
   git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
   ~/esp/esp-idf/install.sh esp32p4
   . ~/esp/esp-idf/export.sh
   ```
2. **USB driver (macOS):** the panel's USB-serial chip is a CH340/CH343. Install WCH's
   [CH34x driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html) and allow it in *System Settings →
   Privacy & Security*. The port appears as `/dev/cu.wchusbserial*`.
3. **Google sign-in client:** copy `secrets.defaults.example` to `secrets.defaults` and fill in the client ID and
   secret (see the next section). Without it, everything works except *Sign in with Google*.
4. **Signing key** (once): only firmware signed with this key can be installed over the air.
   ```bash
   espsecure.py generate_signing_key --version 2 --scheme rsa3072 signing_key.pem
   ```
   It's git-ignored. **Back it up** (a password manager's secure note, or an encrypted disk image): without it,
   your panels only accept new firmware by USB again. Builds without the key aren't signed, and panels running
   signed firmware refuse them.
5. **Build and flash** by USB the first time:
   ```bash
   idf.py build
   idf.py -p /dev/cu.wchusbserial10 -b 460800 flash   # bootloader, partition table and firmware
   idf.py -p /dev/cu.wchusbserial10 monitor           # logs (Ctrl-] to quit)
   ```
   After changing `secrets.defaults` or adding the key, delete `sdkconfig` once so the new settings are picked up.
6. **After that, over Wi-Fi:** `idf.py build && tools/ota.sh` (asks once for the panel's code; or use *Install a
   firmware file* on the settings page). The panel shows the progress and restarts into the new version.

**Updating a panel from before over-the-air updates** (versions before 1.1.0): flash it once more by USB with
`idf.py … flash` as above. The flash is divided differently now (two firmware slots); settings, Wi-Fi and the Google
sign-in are kept, the calendar cache is rebuilt.

### Releasing a new version

1. Raise the version in `version.txt` (e.g. `1.2.0`) and commit.
2. `. ~/esp/esp-idf/export.sh && tools/release.sh "What's new in this version"`

The script builds the signed firmware, tags `v1.2.0`, pushes the tag and publishes a GitHub release with
`homeplanner.bin`. Panels see it within a day (or at once with *Check now*). A new version has 5 minutes after
its first start to get online; otherwise the panel goes back to the previous version and says so.

Release files contain the Google client ID and secret from `secrets.defaults`, like any installed app; that alone
gives no access to anyone's calendar.

**Wi-Fi chip:** the panel's ESP32-C6 must run the *ESP-Hosted* slave firmware **2.12.x** (version 1.2 panels ship
with it). This firmware uses esp_hosted 2.12.3 over SDIO; other versions don't talk to each other.

### One-time setup for the project owner: Google sign-in

The panel uses Google's normal *Sign in with Google* (OAuth 2.0 with PKCE). Google only sends a sign-in back to
an `https` page on a public domain, so a tiny static page (`docs/oauth.html`, published with GitHub Pages)
passes the one-time code on to the panel in the home. It stores nothing and only forwards to private
network addresses (`192.168.x.x`, `10.x.x.x`, `172.16–31.x.x`, `*.local`).

1. **GitHub Pages:** in the repository's *Settings → Pages*, publish from the `main` branch, folder `/docs`.
   Check that `https://<you>.github.io/homeplanner-esp32/oauth.html` opens.
2. **Google Cloud project** ([console.cloud.google.com](https://console.cloud.google.com)): create one (or reuse
   one) and enable the **Google Calendar API** (*APIs & Services → Library*).
3. **Consent screen** (*Google Auth Platform*): audience **External**; app name *HomePlanner*; support email;
   home page `https://<you>.github.io/homeplanner-esp32/` and privacy policy `…/privacy.html`; data access
   (scopes): `openid`, `…/auth/userinfo.email`, `…/auth/calendar.events`, `…/auth/calendar.calendarlist.readonly`.
4. **Publish the app: Audience → Publish app ("In production").** In *Testing* mode Google ends every sign-in
   after 7 days. In production but unverified, users see a warning screen once and there's a limit of 100 users;
   see [docs/VERIFY.md](docs/VERIFY.md) to get verified.
5. **OAuth client** (*Clients → Create client*): type **Web application**; authorized redirect URI
   `https://<you>.github.io/homeplanner-esp32/oauth.html`. Put the client ID and secret into `secrets.defaults`,
   and set `CONFIG_HP_GOOGLE_REDIRECT_URI` if your page's address differs from the default.

The client secret ends up inside the firmware, as with any app installed on a device. It doesn't give access to
anyone's calendar: that needs the user's own consent, and each sign-in is bound to the panel that started it (PKCE).
Keep `secrets.defaults` out of git anyway (it's in `.gitignore`).

## How it works

```
 Google Calendar API ◄──┐   Open-Meteo ◄──┐   iCal feeds ◄──┐
                        │                  │                 │
              ┌─────────┴──────────────────┴─────────────────┴──┐
              │  ESP32-P4 firmware (ESP-IDF, LVGL)               │
              │  week view · sync · settings page (port 80)      │
              │  Wi-Fi via the ESP32-C6 (esp_hosted over SDIO)   │
              └─────────┬───────────────────────────────────────┘
                        │ home Wi-Fi
              phones / computers: http://homeplanner.local
```

- `components/logic` — calendar rules (pure C, host-tested): events, the add/edit form, repeats, notes.
- `components/ical` + `components/tz` — iCal parser and repeat expansion, and a POSIX timezone engine
  (no timezone database on the device).
- `components/gauth` — Google sign-in (OAuth + PKCE) and access tokens; `components/gcal` — Calendar API calls.
- `components/web` — the settings page: setup hotspot (captive portal) and the home-network page with one-time
  codes and session cookies. `docs/` — the sign-in relay page, home page and privacy policy (GitHub Pages).
- `components/model` — background sync and the flash cache (LittleFS); `components/ui` — the LVGL screens.
- `components/update` — over-the-air updates (GitHub releases, uploads, rollback); `tools/ota.sh`, `tools/release.sh`.

## Tests

The pure C parts are unit-tested on your computer (Unity, cJSON and mbedTLS from ESP-IDF):

```bash
cmake -S tests/host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
```

Reference data is generated, not committed: timezone offsets from Python's `zoneinfo`, and iCal expansion from the
Raspberry Pi version (`recurring_ical_events`; needs that repo with its `.venv` next to this one, otherwise
`test_ical` is skipped). The relay page is tested with Node when it's installed.

## Troubleshooting

- **The panel shows "Can't join …":** check the Wi-Fi name and password with **Change settings**. The panel needs 2.4 GHz Wi-Fi.
- **"Wi-Fi chip not responding":** unplug the panel for 10 seconds. If it persists, the C6 firmware version doesn't
  match (see *Wi-Fi chip* above).
- **`homeplanner.local` doesn't open:** some networks or Android versions don't support `.local` names; use the
  address the panel shows (like `http://192.168.1.50`).
- **Google sign-in returns "This site can't be reached":** the phone or computer must be on the same Wi-Fi as the
  panel. Some browsers don't jump from the Google page back to a home address on their own; tap *Return to HomePlanner*.
- **"Google hasn't verified this app":** expected until verification; tap *Advanced → Go to HomePlanner*.
- **The panel shows "Sign in to Google again":** the sign-in was withdrawn (for example in your Google account
  settings, or after 7 days if the app is still in *Testing* mode). Scan the QR code and sign in again.
- **An iCal link fails with HTTP 401/403/404:** use the calendar's *secret*/private iCal address, not a web page
  link. For Google calendars, use *Settings and sharing → Secret address in iCal format*.
- **An update is refused with "isn't signed with this panel's key":** build with the same `signing_key.pem` that
  signed the firmware on the panel (check that `signing_key.pem` is in the project folder and delete `sdkconfig` once).
- **Logs:** `idf.py -p /dev/cu.wchusbserial10 monitor`.

## Security and privacy

- The panel stores its settings, your Wi-Fi password and its Google sign-in (a refresh token) in its flash
  memory, **not encrypted**. Someone who takes the panel could read them: use **Disconnect Google** (or remove
  access at [myaccount.google.com/permissions](https://myaccount.google.com/permissions)) if it's lost or given away.
- The settings page on the home network is plain `http`. It needs a code shown on the panel to sign in (6 digits,
  10 minutes, 5 tries); signed-in devices get a random 90-day session (only a hash is stored). It refuses requests
  from other web sites and from other host names.
- Updates only install when signed with the project's key (RSA-3072, checked by the panel before it switches); this
  doesn't use hardware secure boot, so someone with the panel and a USB cable can still flash anything.
- There is no HomePlanner server: the panel talks directly to Google, Open-Meteo, your iCal links and GitHub
  (to check for updates).
  See the [privacy policy](https://wentzeld.github.io/homeplanner-esp32/privacy.html).

## License

[MIT](LICENSE)
