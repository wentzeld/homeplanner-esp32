# Google app verification checklist (later)

Until Google verifies the app, people signing in see **"Google hasn't verified this app"** and must tap
*Advanced → Go to HomePlanner (unsafe)*, and the app can have at most **100 users** (counted for its
lifetime). Everything works without verification. When you want to remove the warning:

1. **Domain.** Google wants an *authorized domain* you own. `wentzeld.github.io` counts as your own
   subdomain of a public suffix; verify it in [Google Search Console](https://search.google.com/search-console)
   (URL-prefix property `https://wentzeld.github.io/homeplanner-esp32/`, HTML-file method: add the file to
   `docs/`). A custom domain works too.
2. **Pages** (already in `docs/`, published by GitHub Pages):
   - Home page: `https://wentzeld.github.io/homeplanner-esp32/`
   - Privacy policy: `https://wentzeld.github.io/homeplanner-esp32/privacy.html`
3. **OAuth consent screen** (Google Cloud console → Google Auth Platform → Branding): app name *HomePlanner*,
   logo (optional, triggers extra review), support email, home page and privacy-policy links, authorized
   domain `wentzeld.github.io`.
4. **Scopes** (Data access): exactly `openid`, `.../auth/userinfo.email`,
   `.../auth/calendar.events`, `.../auth/calendar.calendarlist.readonly`. The two calendar scopes are
   *sensitive* (not *restricted*), so no security assessment is needed.
5. **Justification** for each sensitive scope, e.g.:
   - *calendar.events:* "Shows the family's calendar events on a wall panel and lets the family add, edit and
     delete events from the panel. Optional extra calendars are only read."
   - *calendar.calendarlist.readonly:* "Lets the user pick which of their calendars the panel shows."
6. **Demo video** (unlisted YouTube): the whole sign-in on a phone — panel QR → settings page → *Sign in with
   Google* → consent screen with the scopes visible (show the browser address bar with the client ID) →
   choosing the Family calendar → the panel showing events → adding an event on the panel and it appearing
   in Google Calendar.
7. Submit (Verification center). Google usually answers within a few days to weeks; reply to their emails
   from the support address.
