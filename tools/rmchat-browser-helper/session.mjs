// Pure validation and a narrowly scoped cookie reader. No browser access at import time.
export const CHATGPT_URL = "https://chatgpt.com/";
export const SESSION_NAME = "__Secure-next-auth.session-token";
export const MAX_CHUNKS = 16;
export const MAX_EXPORT_BYTES = 32 * 1024;
export const EXPORT_FILENAME = "rmchat-session.json";

// Index 16 is queried only as an overflow guard; it can never be exported.
export const COOKIE_NAMES = Object.freeze([
  SESSION_NAME,
  ...Array.from({ length: MAX_CHUNKS + 1 }, (_, index) => `${SESSION_NAME}.${index}`),
]);

const messages = Object.freeze({
  NO_SESSION: "Aucune session compatible. Connectez-vous à ChatGPT dans ce profil du navigateur, puis réessayez.",
  INVALID_SESSION: "Cette session ne peut pas être exportée. Reconnectez-vous à ChatGPT, puis réessayez.",
  AMBIGUOUS_SESSION: "Plusieurs cookies de session incompatibles sont présents. Reconnectez-vous à ChatGPT avant l’export.",
  SESSION_CHANGED: "La session a changé pendant l’export. Réessayez.",
  TOO_LARGE: "Cette session dépasse la taille acceptée par RMChat.",
  READ_FAILED: "Lecture de session impossible. Vérifiez que l’extension peut accéder à chatgpt.com.",
});

export class SessionExportError extends Error {
  constructor(code) {
    super(messages[code] ?? messages.INVALID_SESSION);
    this.name = "SessionExportError";
    this.code = code in messages ? code : "INVALID_SESSION";
  }
}

function fail(code = "INVALID_SESSION") {
  throw new SessionExportError(code);
}

function validValue(value) {
  return typeof value === "string" && value.length > 0 && value.length <= MAX_EXPORT_BYTES
    && /^[\x21-\x7e]+$/.test(value) && !/[",;\\]/.test(value);
}

export function serializeCredential(credential) {
  // Match Go encoding/json's HTML escaping when checking the envelope size.
  const text = JSON.stringify(credential).replace(/[<>&]/g, character => ({
    "<": "\\u003c", ">": "\\u003e", "&": "\\u0026",
  })[character]) + "\n";
  if (new TextEncoder().encode(text).length > MAX_EXPORT_BYTES) fail("TOO_LARGE");
  return text;
}

export function credentialFromCookies(cookies, now = Date.now() / 1000) {
  if (!Array.isArray(cookies) || !Number.isFinite(now)) fail();
  if (cookies.length === 0) fail("NO_SESSION");
  if (cookies.length > COOKIE_NAMES.length) fail("AMBIGUOUS_SESSION");
  const byName = new Map();
  let scope;
  for (const cookie of cookies) {
    if (!cookie || !COOKIE_NAMES.includes(cookie.name)) fail();
    if (byName.has(cookie.name)) fail("AMBIGUOUS_SESSION");
    if (!["chatgpt.com", ".chatgpt.com"].includes(cookie.domain) || cookie.path !== "/"
        || cookie.secure !== true || cookie.httpOnly !== true || cookie.partitionKey !== undefined
        || typeof cookie.hostOnly !== "boolean" || (cookie.hostOnly && cookie.domain !== "chatgpt.com")
        || typeof cookie.storeId !== "string" || !cookie.storeId
        || typeof cookie.session !== "boolean") fail();
    if (cookie.expirationDate !== undefined) {
      if (!Number.isFinite(cookie.expirationDate) || cookie.expirationDate <= now) fail();
    } else if (!cookie.session) {
      fail();
    }
    if (!validValue(cookie.value)) fail();
    const cookieScope = JSON.stringify([cookie.domain, cookie.hostOnly, cookie.storeId]);
    if (scope !== undefined && scope !== cookieScope) fail("AMBIGUOUS_SESSION");
    scope = cookieScope;
    byName.set(cookie.name, cookie.value);
  }
  if (byName.has(`${SESSION_NAME}.${MAX_CHUNKS}`)) fail("TOO_LARGE");
  let value;
  if (byName.has(SESSION_NAME)) {
    if (byName.size !== 1) fail("AMBIGUOUS_SESSION");
    value = byName.get(SESSION_NAME);
  } else {
    const parts = [];
    for (let index = 0; index < byName.size; index++) {
      const part = byName.get(`${SESSION_NAME}.${index}`);
      if (part === undefined) fail("AMBIGUOUS_SESSION");
      parts.push(part);
    }
    value = parts.join("");
  }
  const credential = { version: 1, provider: "chatgpt-web", kind: "session_token", value };
  serializeCredential(credential);
  return credential;
}

function signature(cookies) {
  return JSON.stringify(cookies.map(cookie => [cookie.name, cookie.value, cookie.domain,
    cookie.hostOnly, cookie.storeId, cookie.session, cookie.expirationDate ?? null])
    .sort((left, right) => left[0].localeCompare(right[0])));
}

export async function readSessionForExport(cookieApi) {
  async function read() {
    let groups;
    try {
      groups = await Promise.all(COOKIE_NAMES.map(name => cookieApi.getAll({
        url: CHATGPT_URL, name, path: "/", secure: true,
      })));
    } catch {
      fail("READ_FAILED");
    }
    if (groups.some(group => !Array.isArray(group) || group.length > 2)) fail("AMBIGUOUS_SESSION");
    for (let index = 0; index < groups.length; index++) {
      if (groups[index].some(cookie => cookie?.name !== COOKIE_NAMES[index])) fail();
    }
    return groups.flat();
  }
  const first = await read();
  credentialFromCookies(first);
  const second = await read();
  const credential = credentialFromCookies(second);
  if (signature(first) !== signature(second)) fail("SESSION_CHANGED");
  return credential;
}
