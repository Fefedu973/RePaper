import {
  base64,
  unbase64,
  normalizeMoodle,
  HEX32,
  OPAQUE,
  PUBLIC_ORIGIN,
} from './protocol.ts';

type Statement = {
  bind(...values: unknown[]): Statement;
  first<T>(): Promise<T | null>;
  run(): Promise<unknown>;
};
export type RelayDb = { prepare(sql: string): Statement };
type Session = {
  id: string;
  code: string;
  device_hash: string;
  moodle_base: string;
  passport: string;
  site_id: string;
  public_key: string;
  expires_at: number;
  browser_hash: string | null;
  ciphertext: string | null;
  collected: number;
};
type Options = { now?: () => number; origin?: string; development?: boolean };
const TTL = 600_000;
const COOKIE = 'repaper_pairing';
const headers = {
  'Cache-Control': 'no-store, max-age=0',
  'Referrer-Policy': 'no-referrer',
  'X-Content-Type-Options': 'nosniff',
};
class Rejected extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}
const reply = (
  body: unknown,
  status = 200,
  extra: Record<string, string> = {},
) => Response.json(body, { status, headers: { ...headers, ...extra } });
const random = () =>
  base64(crypto.getRandomValues(new Uint8Array(32)))
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=/g, '');
const hash = async (v: string) =>
  base64(
    new Uint8Array(
      await crypto.subtle.digest('SHA-256', new TextEncoder().encode(v)),
    ),
  );

async function body(req: Request): Promise<Record<string, unknown>> {
  if (!(req.headers.get('content-type') || '').startsWith('application/json'))
    throw new Rejected(415, 'Requête JSON attendue.');
  if (Number(req.headers.get('content-length') || 0) > 4096)
    throw new Rejected(413, 'Requête trop longue.');
  const reader = req.body?.getReader();
  if (!reader) throw new Rejected(400, 'Requête vide.');
  let size = 0;
  const chunks: Uint8Array[] = [];
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > 4096) throw new Rejected(413, 'Requête trop longue.');
      chunks.push(value);
    }
  } finally {
    await reader.cancel();
  }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.length;
  }
  try {
    const value = JSON.parse(new TextDecoder().decode(bytes));
    if (!value || typeof value !== 'object' || Array.isArray(value))
      throw new Error();
    return value;
  } catch {
    throw new Rejected(400, 'Requête invalide.');
  }
}

export function createRelay(db: RelayDb, options: Options = {}) {
  const clock = options.now || Date.now;
  const origin = options.origin || PUBLIC_ORIGIN;
  async function rate(
    req: Request,
    scope: string,
    limit: number,
    windowMs: number,
  ) {
    const now = clock(),
      bucket = Math.floor(now / windowMs);
    const address = options.development
      ? 'development'
      : req.headers.get('cf-connecting-ip') || 'unknown';
    const key = await hash(`${scope}:${bucket}:${address}`);
    const result = await db
      .prepare(
        'INSERT INTO rates (key,hits,expires_at) VALUES (?,1,?) ON CONFLICT(key) DO UPDATE SET hits=hits+1 RETURNING hits',
      )
      .bind(key, (bucket + 1) * windowMs)
      .first<{ hits: number }>();
    if (!result || result.hits > limit)
      throw new Rejected(
        429,
        'Trop de tentatives. Patientez une minute avant de réessayer.',
      );
  }
  function sameOrigin(req: Request) {
    if (req.headers.get('origin') !== origin)
      throw new Rejected(
        403,
        'Ouvrez cette action depuis le portail reMoodle.',
      );
  }
  const publicSession = (s: Session) => ({
    sessionId: s.id,
    moodleBase: s.moodle_base,
    siteId: s.site_id,
    publicKey: s.public_key,
    profileUrl: s.moodle_base + '/user/profile.php',
    expiresAt: new Date(s.expires_at).toISOString(),
    status: s.collected ? 'collected' : s.ciphertext ? 'complete' : 'pending',
  });
  async function browserSession(req: Request) {
    const raw = (req.headers.get('cookie') || '')
      .split(';')
      .map((s) => s.trim())
      .find((s) => s.startsWith(COOKIE + '='))
      ?.slice(COOKIE.length + 1);
    if (!raw || !OPAQUE.test(raw))
      throw new Rejected(401, 'Reliez d’abord votre tablette avec son code.');
    const s = await db
      .prepare('SELECT * FROM sessions WHERE browser_hash=? AND expires_at>?')
      .bind(await hash(raw), clock())
      .first<Session>();
    if (!s)
      throw new Rejected(
        410,
        'Cette demande a expiré. Créez un nouveau code sur la tablette.',
      );
    return s;
  }
  return async function handle(req: Request): Promise<Response> {
    try {
      const path = new URL(req.url).pathname;
      const method = req.method;
      if (path === '/api/health' && method === 'GET')
        return reply({ status: 'ok', protocol: 1 });
      if (path === '/api/sessions' && method === 'POST') {
        if (req.headers.has('origin')) sameOrigin(req);
        await rate(req, 'create-minute', 6, 60_000);
        await rate(req, 'create-hour', 30, 3_600_000);
        await db
          .prepare('DELETE FROM sessions WHERE expires_at<=?')
          .bind(clock())
          .run();
        await db
          .prepare('DELETE FROM rates WHERE expires_at<=?')
          .bind(clock())
          .run();
        const count = await db
          .prepare('SELECT count(*) AS n FROM sessions')
          .first<{ n: number }>();
        if ((count?.n || 0) >= 5000)
          throw new Rejected(
            503,
            'Le service est occupé. Réessayez dans quelques minutes.',
          );
        const data = await body(req);
        let moodleBase: string;
        try {
          moodleBase = normalizeMoodle(data.moodleBase);
        } catch (e) {
          throw new Rejected(400, (e as Error).message);
        }
        const { passport, siteId, publicKey } = data;
        if (
          typeof passport !== 'string' ||
          !HEX32.test(passport) ||
          typeof siteId !== 'string' ||
          !HEX32.test(siteId) ||
          typeof publicKey !== 'string' ||
          publicKey.length > 600
        )
          throw new Rejected(400, 'Demande de tablette invalide.');
        try {
          const key = await crypto.subtle.importKey(
            'spki',
            unbase64(publicKey),
            { name: 'RSA-OAEP', hash: 'SHA-256' },
            false,
            ['encrypt'],
          );
          if ((key.algorithm as RsaHashedKeyAlgorithm).modulusLength !== 2048)
            throw new Error();
        } catch {
          throw new Rejected(400, 'Clé de tablette invalide.');
        }
        const sessionId = random(),
          deviceSecret = random(),
          expiresAt = clock() + TTL;
        let code = '';
        for (let attempt = 0; attempt < 8; attempt++) {
          // Rejection sampling keeps all eight-digit codes equally likely.
          let number: number;
          do {
            number = crypto.getRandomValues(new Uint32Array(1))[0];
          } while (number >= 4_200_000_000);
          code = (number % 100_000_000).toString().padStart(8, '0');
          const exists = await db
            .prepare('SELECT id FROM sessions WHERE code=?')
            .bind(code)
            .first();
          if (!exists) break;
          code = '';
        }
        if (!code) throw new Rejected(503, 'Réessayez dans un instant.');
        await db
          .prepare(
            'INSERT INTO sessions (id,code,device_hash,moodle_base,passport,site_id,public_key,expires_at,collected) VALUES (?,?,?,?,?,?,?,?,0)',
          )
          .bind(
            sessionId,
            code,
            await hash(deviceSecret),
            moodleBase,
            passport,
            siteId.toLowerCase(),
            publicKey,
            expiresAt,
          )
          .run();
        return reply(
          {
            sessionId,
            deviceSecret,
            code,
            verificationUrl: origin + '/#session=' + sessionId,
            portalBase: origin,
            expiresAt: new Date(expiresAt).toISOString(),
          },
          201,
        );
      }
      if (path === '/api/claim' && method === 'POST') {
        sameOrigin(req);
        await rate(req, 'claim', 10, 60_000);
        await rate(req, 'claim-window', 40, 600_000);
        const data = await body(req),
          code = data.code,
          id = data.sessionId;
        if (
          !(typeof code === 'string' && /^\d{8}$/.test(code)) &&
          !(typeof id === 'string' && OPAQUE.test(id))
        )
          throw new Rejected(
            400,
            'Saisissez les huit chiffres affichés sur la tablette.',
          );
        const browserSecret = random(),
          browserHash = await hash(browserSecret);
        const condition =
          typeof id === 'string' && OPAQUE.test(id) ? 'id' : 'code';
        const s = await db
          .prepare(
            `UPDATE sessions SET browser_hash=? WHERE ${condition}=? AND browser_hash IS NULL AND expires_at>? RETURNING *`,
          )
          .bind(browserHash, condition === 'id' ? id : code, clock())
          .first<Session>();
        if (!s)
          throw new Rejected(
            410,
            'Code expiré, déjà utilisé ou inconnu. Créez un nouveau code sur la tablette.',
          );
        return reply(publicSession(s), 200, {
          'Set-Cookie': `${COOKIE}=${browserSecret}; Path=/api; Max-Age=600; HttpOnly; SameSite=Lax${options.development ? '' : '; Secure'}`,
        });
      }
      if (path === '/api/browser' && method === 'GET')
        return reply(publicSession(await browserSession(req)));
      if (path === '/api/complete' && method === 'POST') {
        sameOrigin(req);
        await rate(req, 'complete', 15, 60_000);
        const s = await browserSession(req),
          data = await body(req);
        if (data.sessionId !== s.id)
          throw new Rejected(
            409,
            'Une autre tablette est ouverte dans ce navigateur. Utilisez un nouveau code pour reprendre.',
          );
        if (
          typeof data.ciphertext !== 'string' ||
          data.ciphertext.length !== 344
        )
          throw new Rejected(400, 'Retour chiffré invalide.');
        try {
          if (unbase64(data.ciphertext).length !== 256) throw new Error();
        } catch {
          throw new Rejected(400, 'Retour chiffré invalide.');
        }
        if (s.ciphertext) {
          if (s.ciphertext !== data.ciphertext)
            throw new Rejected(409, 'Cette demande a déjà reçu un retour.');
          return reply({ status: 'complete' });
        }
        const changed = await db
          .prepare(
            'UPDATE sessions SET ciphertext=? WHERE id=? AND ciphertext IS NULL AND expires_at>? RETURNING id',
          )
          .bind(data.ciphertext, s.id, clock())
          .first();
        if (!changed)
          throw new Rejected(409, 'Cette demande est déjà terminée.');
        return reply({ status: 'complete' });
      }
      const device = path.match(
        /^\/api\/sessions\/([A-Za-z0-9_-]{43})(\/poll)?$/,
      );
      if (
        device &&
        ((device[2] && method === 'POST') ||
          (!device[2] && method === 'DELETE'))
      ) {
        const secret = (req.headers.get('authorization') || '').replace(
          /^Bearer /,
          '',
        );
        if (!OPAQUE.test(secret)) throw new Rejected(401, 'Accès refusé.');
        const s = await db
          .prepare('SELECT * FROM sessions WHERE id=? AND device_hash=?')
          .bind(device[1], await hash(secret))
          .first<Session>();
        if (!s) throw new Rejected(401, 'Accès refusé.');
        if (s.expires_at <= clock()) {
          await db.prepare('DELETE FROM sessions WHERE id=?').bind(s.id).run();
          return reply({ status: 'expired' });
        }
        if (method === 'DELETE') {
          await db.prepare('DELETE FROM sessions WHERE id=?').bind(s.id).run();
          return reply({ status: 'cancelled' });
        }
        if (s.ciphertext) {
          await db
            .prepare('UPDATE sessions SET collected=1 WHERE id=?')
            .bind(s.id)
            .run();
          return reply({ status: 'complete', ciphertext: s.ciphertext });
        }
        return reply({ status: 'pending' });
      }
      return reply({ error: 'Action inconnue.' }, 404);
    } catch (error) {
      if (error instanceof Rejected)
        return reply(
          { error: error.message },
          error.status,
          error.status === 429 ? { 'Retry-After': '60' } : {},
        );
      // Never log request bodies, credentials, callback URLs, or database rows.
      return reply(
        { error: 'Le service est temporairement indisponible. Réessayez.' },
        503,
      );
    }
  };
}
