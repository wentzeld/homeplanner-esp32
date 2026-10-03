// node tests/web/relay_test.js — the sign-in relay page only forwards to the panel on the home network.
"use strict";
const assert = require("assert");
const { decodeState, isPanelHost, relayTarget } = require("../../docs/relay.js");

const b64 = (s) => Buffer.from(s).toString("base64url");
const NONCE = "0123456789abcdef0123456789abcdef";

// The same state the panel's C code makes (tests/host/test_oauth.c).
assert.deepStrictEqual(decodeState("MTkyLjE2OC4xLjUwfDAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2Nzg5YWJjZGVm"),
  { host: "192.168.1.50", nonce: NONCE });

for (const h of ["192.168.1.50", "10.0.0.5", "172.16.0.1", "172.31.255.254", "homeplanner.local", "HomePlanner.local"])
  assert.ok(isPanelHost(h), h);
for (const h of ["8.8.8.8", "172.32.0.1", "192.169.1.1", "192.168.1.256", "evil.com", "evil.com/.local",
                 "a.b.local", "-x.local", "localhost", "", "192.168.1.1:8080", "x@192.168.1.1"])
  assert.ok(!isPanelHost(h), h);

let t = relayTarget(`?state=${b64("192.168.1.50|" + NONCE)}&code=4/0Ab%2Bc&scope=email`);
assert.strictEqual(t.url, `http://192.168.1.50/oauth/done?state=${b64("192.168.1.50|" + NONCE)}&code=4%2F0Ab%2Bc`);

t = relayTarget(`?state=${b64("homeplanner.local|" + NONCE)}&error=access_denied`);
assert.ok(t.url.startsWith("http://homeplanner.local/oauth/done?") && t.url.endsWith("&error=access_denied"));

for (const bad of [`?state=${b64("evil.com|" + NONCE)}&code=x`, `?state=${b64("192.168.1.2|nonce")}&code=x`,
                   `?state=${b64("192.168.1.2")}&code=x`, "?code=x", "", "?state=%%%&code=x",
                   `?state=${b64("192.168.1.2/x|" + NONCE)}&code=x`])
  assert.ok(relayTarget(bad).error, bad);

console.log("relay page: all tests passed");
