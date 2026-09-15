import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import {
  CHATGPT_URL, COOKIE_NAMES, EXPORT_FILENAME, MAX_CHUNKS, MAX_EXPORT_BYTES,
  SESSION_NAME, credentialFromCookies, readSessionForExport, serializeCredential,
} from "./session.mjs";

const now = 1_800_000_000;
const cookie = (changes = {}) => ({ name: SESSION_NAME, value: "fixture-session", domain: "chatgpt.com",
  hostOnly: true, path: "/", secure: true, httpOnly: true, storeId: "0", session: true, ...changes });
const part = (index, value = `part${index}`) => cookie({ name: `${SESSION_NAME}.${index}`, value });
const rejects = (cookies, code) => assert.throws(() => credentialFromCookies(cookies, now), error => error.code === code);

test("exports only the exact Credential v1 fields", () => {
  const result = credentialFromCookies([cookie()], now);
  assert.deepEqual(result, { version: 1, provider: "chatgpt-web", kind: "session_token", value: "fixture-session" });
  assert.deepEqual(JSON.parse(serializeCredential(result)), result);
  assert.equal(EXPORT_FILENAME, "rmchat-session.json");
});

test("joins a bounded contiguous sequence in numeric order", () => {
  const chunks = Array.from({ length: MAX_CHUNKS }, (_, index) => part(index));
  assert.equal(credentialFromCookies(chunks.toReversed(), now).value, chunks.map(item => item.value).join(""));
});

test("rejects mixed forms, gaps, duplicates, malformed and overflowing chunks", () => {
  rejects([cookie(), part(0)], "AMBIGUOUS_SESSION");
  rejects([cookie(), cookie()], "AMBIGUOUS_SESSION");
  rejects([part(0), part(0)], "AMBIGUOUS_SESSION");
  rejects([part(1)], "AMBIGUOUS_SESSION");
  rejects([part(0), part(2)], "AMBIGUOUS_SESSION");
  rejects([part(0), part(16)], "TOO_LARGE");
  for (const suffix of ["01", "-1", "17", "x"]) rejects([part(suffix)], "INVALID_SESSION");
});

test("accepts a root domain cookie and a future persistent cookie", () => {
  assert.equal(credentialFromCookies([cookie({ domain: ".chatgpt.com", hostOnly: false })], now).value, "fixture-session");
  assert.equal(credentialFromCookies([cookie({ session: false, expirationDate: now + 600 })], now).value, "fixture-session");
});

test("rejects every incompatible scope or expired cookie", () => {
  for (const changes of [
    { domain: "other.example" }, { domain: "sub.chatgpt.com" }, { domain: ".chatgpt.com" },
    { path: "/api" }, { secure: false }, { httpOnly: false }, { partitionKey: { topLevelSite: CHATGPT_URL } },
    { hostOnly: "true" }, { storeId: "" }, { session: "true" }, { session: false },
    { expirationDate: now }, { expirationDate: NaN }, { expirationDate: Infinity },
  ]) rejects([cookie(changes)], "INVALID_SESSION");
  rejects([part(0), cookie({ ...part(1), storeId: "1" })], "AMBIGUOUS_SESSION");
  rejects([part(0), cookie({ ...part(1), domain: ".chatgpt.com", hostOnly: false })], "AMBIGUOUS_SESSION");
});

test("rejects unrelated names and invalid values without disclosing them", () => {
  rejects([], "NO_SESSION");
  rejects([cookie({ name: "unrelated-fixture" })], "INVALID_SESSION");
  for (const value of ["", "fixture space", 'fixture"quote', "fixture,comma", "fixture;semicolon", "fixture\\slash", "fixture\nline", "fixture-é-secret"]) {
    assert.throws(() => credentialFromCookies([cookie({ value })], now), error => {
      assert.equal(error.code, "INVALID_SESSION");
      if (value) assert.equal(error.message.includes(value), false);
      return true;
    });
  }
});

test("bounds the entire exported file including JSON and Go-compatible escaping", () => {
  const empty = { version: 1, provider: "chatgpt-web", kind: "session_token", value: "" };
  const overhead = new TextEncoder().encode(serializeCredential(empty)).length;
  const exact = cookie({ value: "v".repeat(MAX_EXPORT_BYTES - overhead) });
  assert.equal(new TextEncoder().encode(serializeCredential(credentialFromCookies([exact], now))).length, MAX_EXPORT_BYTES);
  rejects([cookie({ value: exact.value + "v" })], "TOO_LARGE");
  rejects([cookie({ value: "<".repeat(6000) })], "TOO_LARGE");
  const escaped = credentialFromCookies([cookie({ value: "fixture<&>" })], now);
  assert.equal(JSON.parse(serializeCredential(escaped)).value, "fixture<&>");
});

test("reads only exact session names at the HTTPS root in the current store", async () => {
  const calls = [];
  const result = await readSessionForExport({ getAll: async details => {
    calls.push(details);
    return details.name === SESSION_NAME ? [cookie()] : [];
  } });
  assert.equal(result.value, "fixture-session");
  assert.equal(calls.length, COOKIE_NAMES.length * 2);
  for (const details of calls) {
    assert.deepEqual(details, { url: CHATGPT_URL, name: details.name, path: "/", secure: true });
    assert.ok(COOKIE_NAMES.includes(details.name));
  }
});

test("refuses a session that changes between the two reads", async () => {
  let calls = 0;
  await assert.rejects(readSessionForExport({ getAll: async details => {
    const first = calls++ < COOKIE_NAMES.length;
    return details.name === SESSION_NAME ? [cookie({ value: first ? "fixture-old" : "fixture-new" })] : [];
  } }), error => error.code === "SESSION_CHANGED");
});

test("does not expose raw API errors or accept unexpected query results", async () => {
  await assert.rejects(readSessionForExport({ getAll: async () => { throw new Error("fixture-private-error"); } }), error => {
    assert.equal(error.code, "READ_FAILED");
    assert.equal(error.message.includes("fixture-private-error"), false);
    return true;
  });
  await assert.rejects(readSessionForExport({ getAll: async () => [cookie({ name: "unrelated-fixture" })] }), error => error.code === "INVALID_SESSION");
  await assert.rejects(readSessionForExport({ getAll: async () => [cookie(), cookie(), cookie()] }), error => error.code === "AMBIGUOUS_SESSION");
});

test("manifest grants only cookies on the exact ChatGPT HTTPS host", async () => {
  const manifest = JSON.parse(await readFile(new URL("./manifest.json", import.meta.url)));
  assert.equal(manifest.manifest_version, 3);
  assert.deepEqual(manifest.permissions, ["cookies"]);
  assert.deepEqual(manifest.host_permissions, ["https://chatgpt.com/*"]);
  for (const key of ["background", "content_scripts", "externally_connectable", "optional_permissions", "optional_host_permissions"]) {
    assert.equal(manifest[key], undefined);
  }
  assert.ok(manifest.content_security_policy.extension_pages.includes("connect-src 'none'"));
});

test("popup reads nothing until a user click and exports a local Blob", async () => {
  const previous = { document: globalThis.document, window: globalThis.window, chrome: globalThis.chrome };
  const listeners = {};
  const windowListeners = {};
  const status = { textContent: "", dataset: {} };
  const button = { disabled: false, addEventListener: (name, handler) => { listeners[name] = handler; } };
  const link = { click() { this.clicked = true; }, remove() {} };
  let reads = 0;
  globalThis.document = { querySelector: selector => selector === "#export" ? button : status,
    createElement: () => link, body: { append() {} } };
  globalThis.window = { addEventListener: (name, handler) => { windowListeners[name] = handler; } };
  globalThis.chrome = { cookies: { getAll: async details => {
    reads++;
    return details.name === SESSION_NAME ? [cookie()] : [];
  } } };
  try {
    await import("./popup.mjs");
    assert.equal(reads, 0);
    await listeners.click();
    assert.equal(reads, COOKIE_NAMES.length * 2);
    assert.equal(link.download, EXPORT_FILENAME);
    assert.ok(link.href.startsWith("blob:"));
    assert.equal(link.clicked, true);
    assert.equal(status.textContent.includes("fixture-session"), false);
    assert.equal(button.disabled, false);
    windowListeners.pagehide();
  } finally {
    Object.assign(globalThis, previous);
  }
});
