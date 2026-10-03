// Hands a Google sign-in back to the HomePlanner panel on the home network.
// Google can only return to a public https page (this one); the panel's address and a one-time
// nonce travel in `state`. This page forwards the answer to http://<panel>/oauth/done and nowhere
// else: only private network addresses (10.x, 172.16-31.x, 192.168.x) and *.local names.
"use strict";

function decodeState(state) {
  if (typeof state !== "string" || !/^[A-Za-z0-9_-]{1,136}$/.test(state)) return null;
  let text;
  try {
    text = atob(state.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((state.length + 3) % 4));
  } catch {
    return null;
  }
  const bar = text.indexOf("|");
  if (bar < 0) return null;
  return { host: text.slice(0, bar), nonce: text.slice(bar + 1) };
}

function isPanelHost(host) {
  const ip = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  if (ip) {
    const [a, b, c, d] = ip.slice(1).map(Number);
    if ([a, b, c, d].some((n) => n > 255)) return false;
    return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168);
  }
  return /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.local$/i.test(host);
}

// -> { url } to continue to, or { error } to show.
function relayTarget(search) {
  const q = new URLSearchParams(search);
  const state = decodeState(q.get("state"));
  if (!state || !isPanelHost(state.host) || !/^[0-9a-f]{32}$/.test(state.nonce)) {
    return { error: "This page only finishes a HomePlanner sign-in. Start again from the panel's settings page." };
  }
  const out = new URLSearchParams({ state: q.get("state") });
  if (q.get("code")) out.set("code", q.get("code"));
  else out.set("error", q.get("error") || "no_code");
  return { url: `http://${state.host}/oauth/done?${out}` };
}

if (typeof module !== "undefined") module.exports = { decodeState, isPanelHost, relayTarget };
