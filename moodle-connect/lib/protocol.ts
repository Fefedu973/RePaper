export const PUBLIC_ORIGIN =
  'https://repaper-moodle-connect.fefe-du-973.chatgpt.site';
export const HEX32 = /^[a-f0-9]{32}$/i;
export const OPAQUE = /^[A-Za-z0-9_-]{43}$/;

export function base64(bytes: Uint8Array): string {
  return btoa(String.fromCharCode(...bytes));
}
export function unbase64(text: string): Uint8Array<ArrayBuffer> {
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(text) || text.length % 4 !== 0)
    throw new Error('Encodage invalide.');
  return Uint8Array.from(atob(text), (c) => c.charCodeAt(0));
}
export function normalizeMoodle(value: unknown): string {
  if (typeof value !== 'string' || value.length > 2048)
    throw new Error('Adresse Moodle invalide.');
  const url = new URL(value);
  if (
    url.protocol !== 'https:' ||
    url.username ||
    url.password ||
    url.search ||
    url.hash ||
    url.port ||
    !/^[a-z0-9.-]+$/i.test(url.hostname) ||
    !url.hostname.includes('.') ||
    /(^|\.)(localhost|local|internal)$/.test(url.hostname) ||
    /^[\d.]+$/.test(url.hostname)
  )
    throw new Error('Indiquez une adresse Moodle publique en HTTPS.');
  return url.href.replace(/\/+$/, '');
}
export type QrAuthorization = {
  kind: 'qr';
  userid: number;
  qrloginkey: string;
};
export function parseMoodleQr(
  input: string,
  expectedBase: string,
): QrAuthorization {
  if (input.length > 4096) throw new Error('QR Moodle invalide.');
  const match = input.match(/^[a-z][a-z0-9+.-]*:\/\/(https:\/\/.+)$/i);
  if (!match)
    throw new Error(
      'Ce QR n’est pas un QR de connexion Moodle. Utilisez « Afficher le code QR » dans votre profil Moodle.',
    );
  const url = new URL(match[1]);
  const values = [...url.searchParams.keys()];
  const userid = url.searchParams.get('userid') || '',
    qrloginkey = url.searchParams.get('qrlogin') || '';
  if (
    values.length !== 2 ||
    !values.includes('userid') ||
    !values.includes('qrlogin') ||
    !HEX32.test(qrloginkey) ||
    !/^[1-9][0-9]{0,9}$/.test(userid) ||
    Number(userid) > 2147483647
  )
    throw new Error(
      'Ce QR ne contient pas d’autorisation Moodle. Affichez un nouveau QR depuis votre profil.',
    );
  url.search = '';
  if (normalizeMoodle(url.href) !== normalizeMoodle(expectedBase))
    throw new Error(
      'Ce QR appartient à un autre site Moodle que celui indiqué sur la tablette.',
    );
  return { kind: 'qr', userid: Number(userid), qrloginkey };
}
export async function encryptAuthorization(
  publicKey: string,
  value: QrAuthorization,
): Promise<string> {
  const key = await crypto.subtle.importKey(
    'spki',
    unbase64(publicKey),
    { name: 'RSA-OAEP', hash: 'SHA-256' },
    false,
    ['encrypt'],
  );
  if ((key.algorithm as RsaHashedKeyAlgorithm).modulusLength !== 2048)
    throw new Error('Clé de tablette invalide.');
  return base64(
    new Uint8Array(
      await crypto.subtle.encrypt(
        { name: 'RSA-OAEP' },
        key,
        new TextEncoder().encode(JSON.stringify(value)),
      ),
    ),
  );
}
