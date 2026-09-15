import assert from 'node:assert/strict';
import { base64, encryptAuthorization, unbase64 } from '../lib/protocol.ts';

// Uses only fictional credentials and removes the session it creates.
const origin = process.argv[2] || 'http://localhost:3000';
const keys = await crypto.subtle.generateKey(
  {
    name: 'RSA-OAEP',
    modulusLength: 2048,
    publicExponent: new Uint8Array([1, 0, 1]),
    hash: 'SHA-256',
  },
  true,
  ['encrypt', 'decrypt'],
);
const publicKey = base64(
  new Uint8Array(await crypto.subtle.exportKey('spki', keys.publicKey)),
);
const jsonHeaders = { 'Content-Type': 'application/json', Origin: origin };
const created = await fetch(origin + '/api/sessions', {
  method: 'POST',
  headers: jsonHeaders,
  body: JSON.stringify({
    moodleBase: 'https://school.example',
    passport: 'a'.repeat(32),
    siteId: 'b'.repeat(32),
    publicKey,
  }),
});
assert.equal(created.status, 201, `creation status ${created.status}`);
const s = (await created.json()) as Record<string, string>,
  authorization = 'Bearer ' + s.deviceSecret;
try {
  const claim = await fetch(origin + '/api/claim', {
    method: 'POST',
    headers: jsonHeaders,
    body: JSON.stringify({ code: s.code }),
  });
  assert.equal(claim.status, 200);
  const cookie = claim.headers.get('set-cookie')!.split(';')[0];
  const payload = {
    kind: 'qr' as const,
    userid: 12,
    qrloginkey: 'c'.repeat(32),
  };
  const ciphertext = await encryptAuthorization(publicKey, payload);
  const completed = await fetch(origin + '/api/complete', {
    method: 'POST',
    headers: { ...jsonHeaders, Cookie: cookie },
    body: JSON.stringify({ sessionId: s.sessionId, ciphertext }),
  });
  assert.equal(completed.status, 200);
  const polled = await fetch(
    origin + '/api/sessions/' + s.sessionId + '/poll',
    { method: 'POST', headers: { Authorization: authorization } },
  );
  assert.equal(polled.status, 200);
  const result = (await polled.json()) as Record<string, string>;
  assert.equal(result.status, 'complete');
  assert.equal(polled.headers.get('cache-control'), 'no-store, max-age=0');
  const plaintext = await crypto.subtle.decrypt(
    { name: 'RSA-OAEP' },
    keys.privateKey,
    unbase64(result.ciphertext),
  );
  assert.deepEqual(JSON.parse(new TextDecoder().decode(plaintext)), payload);
  console.log(
    'HTTP round trip, D1 state, cookie, RSA and tablet polling passed (fictional data).',
  );
} finally {
  const removed = await fetch(origin + '/api/sessions/' + s.sessionId, {
    method: 'DELETE',
    headers: { Authorization: authorization },
  });
  assert.equal(removed.status, 200);
}
