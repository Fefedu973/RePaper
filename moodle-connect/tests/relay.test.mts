import test from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import { createRelay, type RelayDb } from '../lib/relay.ts';
import {
  base64,
  unbase64,
  parseMoodleQr,
  encryptAuthorization,
  normalizeMoodle,
} from '../lib/protocol.ts';

const origin = 'https://portal.example';
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
const creation = {
  moodleBase: 'https://school.example/moodle',
  passport: 'a'.repeat(32),
  siteId: 'b'.repeat(32),
  publicKey,
};
function fixture() {
  const sqlite = new DatabaseSync(':memory:');
  sqlite.exec(
    readFileSync(
      new URL('../drizzle/0000_odd_ted_forrester.sql', import.meta.url),
      'utf8',
    ),
  );
  const db: RelayDb = {
    prepare(sql) {
      let args: unknown[] = [];
      return {
        bind(...values) {
          args = values;
          return this;
        },
        async first<T>() {
          return (sqlite.prepare(sql).get(...(args as never[])) ||
            null) as T | null;
        },
        async run() {
          return sqlite.prepare(sql).run(...(args as never[]));
        },
      };
    },
  };
  let now = 1_800_000_000_000;
  const handle = createRelay(db, { origin, now: () => now, development: true });
  const req = (
    path: string,
    method = 'GET',
    data?: unknown,
    extra: Record<string, string> = {},
  ) =>
    handle(
      new Request(origin + path, {
        method,
        headers: {
          Origin: origin,
          'Content-Type': 'application/json',
          ...extra,
        },
        body: data === undefined ? undefined : JSON.stringify(data),
      }),
    ).then(
      (r) =>
        r as Omit<Response, 'json'> & {
          json(): Promise<Record<string, string>>;
        },
    );
  return {
    sqlite,
    req,
    advance: (ms: number) => {
      now += ms;
    },
  };
}
test('pairing → local QR encryption → authenticated tablet polling, without server plaintext', async () => {
  const f = fixture();
  const created = await f.req('/api/sessions', 'POST', creation);
  assert.equal(created.status, 201);
  const session = await created.json();
  assert.match(session.code, /^\d{8}$/);
  assert.equal(
    session.verificationUrl,
    origin + '/#session=' + session.sessionId,
  );
  const claim = await f.req('/api/claim', 'POST', { code: session.code });
  assert.equal(claim.status, 200);
  const browser = await claim.json();
  const cookie = claim.headers.get('set-cookie')!.split(';')[0];
  assert.match(claim.headers.get('set-cookie')!, /HttpOnly; SameSite=Lax/);
  const qr = parseMoodleQr(
    'moodlemobile://https://school.example/moodle?qrlogin=' +
      'c'.repeat(32) +
      '&userid=123',
    creation.moodleBase,
  );
  const ciphertext = await encryptAuthorization(browser.publicKey, qr);
  const encrypted = await f.req(
    '/api/complete',
    'POST',
    { sessionId: session.sessionId, ciphertext },
    { Cookie: cookie },
  );
  assert.equal(encrypted.status, 200);
  assert.equal(
    (
      await f.req(
        '/api/complete',
        'POST',
        { sessionId: session.sessionId, ciphertext },
        { Cookie: cookie },
      )
    ).status,
    200,
    'network retry is idempotent',
  );
  const deviceHeaders = { Authorization: 'Bearer ' + session.deviceSecret };
  assert.equal(
    (
      await f.req(
        '/api/sessions/' + session.sessionId + '/poll',
        'POST',
        undefined,
        { Authorization: 'Bearer ' + 'x'.repeat(43) },
      )
    ).status,
    401,
  );
  const result = await (
    await f.req(
      '/api/sessions/' + session.sessionId + '/poll',
      'POST',
      undefined,
      deviceHeaders,
    )
  ).json();
  assert.equal(result.status, 'complete');
  const plain = await crypto.subtle.decrypt(
    { name: 'RSA-OAEP' },
    keys.privateKey,
    unbase64(result.ciphertext),
  );
  assert.deepEqual(JSON.parse(new TextDecoder().decode(plain)), qr);
  const row = JSON.stringify(f.sqlite.prepare('SELECT * FROM sessions').get());
  assert.ok(!row.includes(qr.qrloginkey));
  assert.ok(!row.includes(session.deviceSecret));
  assert.equal(
    (
      await f.req(
        '/api/sessions/' + session.sessionId,
        'DELETE',
        undefined,
        deviceHeaders,
      )
    ).status,
    200,
  );
  assert.equal(
    f.sqlite.prepare('SELECT count(*) AS n FROM sessions').get()!.n,
    0,
  );
  f.sqlite.close();
});
test('a code can be claimed only once, including concurrent claims', async () => {
  const f = fixture(),
    s = await (await f.req('/api/sessions', 'POST', creation)).json();
  const results = await Promise.all([
    f.req('/api/claim', 'POST', { code: s.code }),
    f.req('/api/claim', 'POST', { sessionId: s.sessionId }),
  ]);
  assert.deepEqual(results.map((r) => r.status).sort(), [200, 410]);
  f.sqlite.close();
});
test('expiry cannot be extended by polling or browser activity', async () => {
  const f = fixture(),
    s = await (await f.req('/api/sessions', 'POST', creation)).json();
  const claim = await f.req('/api/claim', 'POST', { code: s.code });
  const cookie = claim.headers.get('set-cookie')!.split(';')[0];
  f.advance(600_000);
  assert.equal(
    (await f.req('/api/browser', 'GET', undefined, { Cookie: cookie })).status,
    410,
  );
  assert.equal(
    (
      await (
        await f.req(
          '/api/sessions/' + s.sessionId + '/poll',
          'POST',
          undefined,
          { Authorization: 'Bearer ' + s.deviceSecret },
        )
      ).json()
    ).status,
    'expired',
  );
  f.sqlite.close();
});
test('origin protection, invalid keys, bounded bodies, and code guessing limits', async () => {
  const f = fixture();
  assert.equal(
    (
      await f.req(
        '/api/claim',
        'POST',
        { code: '00000000' },
        { Origin: 'https://evil.example' },
      )
    ).status,
    403,
  );
  assert.equal(
    (await f.req('/api/sessions', 'POST', { ...creation, publicKey: 'AAAA' }))
      .status,
    400,
  );
  assert.equal(
    (
      await f.req('/api/sessions', 'POST', {
        ...creation,
        padding: 'x'.repeat(5000),
      })
    ).status,
    413,
  );
  for (let i = 0; i < 10; i++)
    assert.equal(
      (await f.req('/api/claim', 'POST', { code: '00000000' })).status,
      410,
    );
  const limited = await f.req('/api/claim', 'POST', { code: '00000000' });
  assert.equal(limited.status, 429);
  assert.equal(limited.headers.get('retry-after'), '60');
  f.sqlite.close();
});
test('completion requires a claimed browser; ciphertext has exact bounds', async () => {
  const f = fixture(),
    s = await (await f.req('/api/sessions', 'POST', creation)).json();
  assert.equal(
    (await f.req('/api/complete', 'POST', { ciphertext: 'x'.repeat(344) }))
      .status,
    401,
  );
  const claim = await f.req('/api/claim', 'POST', { code: s.code }),
    cookie = claim.headers.get('set-cookie')!.split(';')[0];
  assert.equal(
    (
      await f.req(
        '/api/complete',
        'POST',
        { sessionId: s.sessionId, ciphertext: 'AAAA' },
        { Cookie: cookie },
      )
    ).status,
    400,
  );
  f.sqlite.close();
});
test('two browser tabs cannot complete each other’s tablet session', async () => {
  const f = fixture(),
    a = await (await f.req('/api/sessions', 'POST', creation)).json(),
    b = await (await f.req('/api/sessions', 'POST', creation)).json();
  await f.req('/api/claim', 'POST', { code: a.code });
  const claimB = await f.req('/api/claim', 'POST', { code: b.code }),
    cookieB = claimB.headers.get('set-cookie')!.split(';')[0];
  const ciphertext = await encryptAuthorization(publicKey, {
    kind: 'qr',
    userid: 2,
    qrloginkey: 'd'.repeat(32),
  });
  assert.equal(
    (
      await f.req(
        '/api/complete',
        'POST',
        { sessionId: a.sessionId, ciphertext },
        { Cookie: cookieB },
      )
    ).status,
    409,
  );
  assert.equal(
    f.sqlite
      .prepare(
        'SELECT count(*) AS n FROM sessions WHERE ciphertext IS NOT NULL',
      )
      .get()!.n,
    0,
  );
  f.sqlite.close();
});
test('QR parsing pins the exact Moodle path and rejects URLs, tokens and ambiguous inputs', () => {
  const good =
    'moodlemobile://https://school.example/moodle?qrlogin=' +
    'a'.repeat(32) +
    '&userid=7';
  assert.equal(parseMoodleQr(good, creation.moodleBase).userid, 7);
  for (const bad of [
    good.replace('school.example', 'evil.example'),
    good.replace('/moodle?', '/?'),
    good + '&userid=8',
    good.replace('userid=7', 'userid=0'),
    good.replace('userid=7', 'userid=99999999999999999'),
    good.replace('qrlogin=', 'other='),
    'moodlemobile://token=abc',
    'https://school.example',
  ])
    assert.throws(() => parseMoodleQr(bad, creation.moodleBase));
  for (const bad of [
    'http://school.example',
    'https://user:pass@school.example',
    'https://127.0.0.1',
    'https://school.example/?token=abc',
    'https://school.example/#x',
  ])
    assert.throws(() => normalizeMoodle(bad));
});
test('RSA ciphertext cannot be opened by another tablet', async () => {
  const ciphertext = await encryptAuthorization(publicKey, {
    kind: 'qr',
    userid: 2,
    qrloginkey: 'a'.repeat(32),
  });
  const other = await crypto.subtle.generateKey(
    {
      name: 'RSA-OAEP',
      modulusLength: 2048,
      publicExponent: new Uint8Array([1, 0, 1]),
      hash: 'SHA-256',
    },
    true,
    ['encrypt', 'decrypt'],
  );
  await assert.rejects(
    crypto.subtle.decrypt(
      { name: 'RSA-OAEP' },
      other.privateKey,
      unbase64(ciphertext),
    ),
  );
});
