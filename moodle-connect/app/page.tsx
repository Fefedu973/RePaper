'use client';
import { useEffect, useRef, useState } from 'react';
import {
  InputOTP,
  InputOTPGroup,
  InputOTPSlot,
} from '@/components/ui/input-otp';
import { Button } from '@/components/ui/button';
import { QrCamera } from '@/components/qr-camera';
import { encryptAuthorization, parseMoodleQr, OPAQUE } from '@/lib/protocol';
import { readQrFile } from '@/lib/read-qr';

type Pairing = {
  sessionId: string;
  moodleBase: string;
  publicKey: string;
  profileUrl: string;
  expiresAt: string;
  status: 'pending' | 'complete' | 'collected';
};
async function api<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, {
    ...init,
    cache: 'no-store',
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json', ...init?.headers },
  });
  const data = (await response.json()) as T & { error?: string };
  if (!response.ok)
    throw new Error(data.error || 'Impossible de joindre le service.');
  return data;
}
export default function Home() {
  const [code, setCode] = useState(''),
    [pairing, setPairing] = useState<Pairing | null>(null),
    [busy, setBusy] = useState(false),
    [error, setError] = useState(''),
    [camera, setCamera] = useState(false),
    [ready, setReady] = useState(false),
    [now, setNow] = useState(Date.now());
  const initialized = useRef(false),
    sending = useRef(false),
    pendingCiphertext = useRef<string | null>(null);
  useEffect(() => {
    if (initialized.current) return;
    initialized.current = true;
    const fragment = new URLSearchParams(location.hash.slice(1)),
      id = fragment.get('session');
    if (location.hash) history.replaceState(null, '', location.pathname);
    const restore = async () => {
      try {
        const existing = await api<Pairing>('/api/browser').catch(() => null);
        if (id && OPAQUE.test(id)) {
          if (existing?.sessionId === id) setPairing(existing);
          else
            setPairing(
              await api<Pairing>('/api/claim', {
                method: 'POST',
                body: JSON.stringify({ sessionId: id }),
              }),
            );
        } else if (existing) setPairing(existing);
      } catch (e) {
        setError((e as Error).message);
      } finally {
        setReady(true);
      }
    };
    void restore();
  }, []);
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, []);
  useEffect(() => {
    if (!pairing || pairing.status === 'pending') return;
    let cancelled = false;
    const timer = setInterval(() => {
      void api<Pairing>('/api/browser')
        .then((p) => {
          if (!cancelled) setPairing(p);
        })
        .catch(() => {});
    }, 3000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [pairing?.sessionId, pairing?.status]);
  async function claim() {
    setBusy(true);
    setError('');
    try {
      setPairing(
        await api<Pairing>('/api/claim', {
          method: 'POST',
          body: JSON.stringify({ code }),
        }),
      );
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  async function sendQr(text: string) {
    setCamera(false);
    if (!pairing || sending.current || pendingCiphertext.current) return;
    sending.current = true;
    setBusy(true);
    setError('');
    try {
      const authorization = parseMoodleQr(text, pairing.moodleBase);
      const ciphertext = await encryptAuthorization(
        pairing.publicKey,
        authorization,
      );
      pendingCiphertext.current = ciphertext;
      await api('/api/complete', {
        method: 'POST',
        body: JSON.stringify({ sessionId: pairing.sessionId, ciphertext }),
      });
      pendingCiphertext.current = null;
      setPairing({ ...pairing, status: 'complete' });
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
      sending.current = false;
    }
  }
  async function retrySend() {
    if (!pendingCiphertext.current || !pairing) return;
    setBusy(true);
    setError('');
    try {
      await api('/api/complete', {
        method: 'POST',
        body: JSON.stringify({
          sessionId: pairing.sessionId,
          ciphertext: pendingCiphertext.current,
        }),
      });
      pendingCiphertext.current = null;
      setPairing({ ...pairing, status: 'complete' });
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  const expired = pairing && now >= Date.parse(pairing.expiresAt),
    remaining = pairing
      ? Math.max(0, Math.ceil((Date.parse(pairing.expiresAt) - now) / 60000))
      : 0;
  const finished =
    pairing &&
    (pairing.status === 'collected' ||
      (pairing.status === 'complete' && !expired));
  return (
    <main className="connect-shell">
      <header>
        <span className="wordmark">
          reMoodle<span>connexion</span>
        </span>
      </header>
      <section
        className="connect-card"
        aria-labelledby="title"
        aria-busy={busy}
      >
        <span className="step">
          {finished
            ? '03 / TRANSMISSION'
            : pairing
              ? '02 / AUTORISER MOODLE'
              : '01 / RELIER LA TABLETTE'}
        </span>
        {!pairing ? (
          <>
            <h1 id="title">
              Votre Moodle,
              <br />
              sur votre reMarkable.
            </h1>
            <p>
              Scannez le QR affiché dans reMoodle, ou saisissez son code
              temporaire.
            </p>
            <form
              onSubmit={(e) => {
                e.preventDefault();
                void claim();
              }}
            >
              <label htmlFor="pairing-code">Code affiché sur la tablette</label>
              <InputOTP
                id="pairing-code"
                maxLength={8}
                value={code}
                onChange={setCode}
                pattern="[0-9]*"
                inputMode="numeric"
                autoComplete="one-time-code"
                disabled={!ready || busy}
              >
                <InputOTPGroup>
                  {Array.from({ length: 8 }, (_, i) => (
                    <InputOTPSlot key={i} index={i} className="otp-cell" />
                  ))}
                </InputOTPGroup>
              </InputOTP>
              <Button
                type="submit"
                className="primary-action"
                disabled={!ready || busy || code.length !== 8}
              >
                {busy ? 'Recherche de la tablette…' : 'Relier cette tablette'}
              </Button>
            </form>
            <p className="fineprint">
              Ouvrez d’abord reMoodle sur votre tablette. Son code est valable
              10 minutes.
            </p>
          </>
        ) : finished ? (
          <>
            <span className="success-mark" aria-hidden="true">
              ✓
            </span>
            <h1 id="title">
              {pairing.status === 'collected'
                ? 'Autorisation transmise.'
                : 'Autorisation prête.'}
            </h1>
            <p>
              {pairing.status === 'collected'
                ? 'Revenez sur la tablette pour terminer la connexion.'
                : 'Gardez reMoodle ouvert sur la tablette pendant la transmission.'}
            </p>
            <p className="fineprint">
              La tablette échange elle-même l’autorisation auprès de Moodle.
              Vérifiez le résultat dans reMoodle.
            </p>
          </>
        ) : expired ? (
          <>
            <h1 id="title">Le code a expiré.</h1>
            <p>
              Créez une nouvelle demande depuis reMoodle sur votre tablette,
              puis scannez son QR.
            </p>
          </>
        ) : (
          <>
            <h1 id="title">Autorisez votre tablette.</h1>
            <p className="site-name">{new URL(pairing.moodleBase).hostname}</p>
            <p className="notice">
              Utilisez le même Wi-Fi pour la tablette et l’appareil qui affiche
              le QR Moodle. Évitez un VPN sur un seul des deux appareils.
            </p>
            <ol className="instructions">
              <li>
                Ouvrez votre profil Moodle et connectez-vous avec votre compte
                habituel.
              </li>
              <li>
                Dans <strong>Application mobile</strong>, choisissez{' '}
                <strong>Afficher le code QR</strong>, puis faites une capture
                d’écran.
              </li>
              <li>
                Revenez ici pour choisir cette capture. Vous pouvez aussi
                scanner le QR affiché sur un autre écran.
              </li>
            </ol>
            <a
              href={pairing.profileUrl}
              target="_blank"
              rel="noopener noreferrer"
              className="profile-link"
            >
              Ouvrir mon profil Moodle ↗
            </a>
            <label className="file-label">
              {busy
                ? 'Lecture et transmission…'
                : 'Choisir la capture du QR Moodle'}
              <input
                type="file"
                accept="image/png,image/jpeg,image/webp"
                disabled={busy || !!pendingCiphertext.current}
                onChange={async (e) => {
                  const file = e.target.files?.[0];
                  e.target.value = '';
                  if (!file) return;
                  setError('');
                  setBusy(true);
                  try {
                    const qr = await readQrFile(file);
                    await sendQr(qr);
                  } catch (err) {
                    setError((err as Error).message);
                  } finally {
                    setBusy(false);
                  }
                }}
              />
            </label>
            {!camera ? (
              <Button
                variant="outline"
                className="secondary-action"
                disabled={busy || !!pendingCiphertext.current}
                onClick={() => {
                  setError('');
                  if (!navigator.mediaDevices?.getUserMedia) {
                    setError(
                      'La caméra n’est pas disponible. Choisissez une capture du QR.',
                    );
                    return;
                  }
                  setCamera(true);
                }}
              >
                Scanner avec la caméra
              </Button>
            ) : (
              <QrCamera
                onRead={(value) => void sendQr(value)}
                onError={(message) => {
                  setError(message);
                  setCamera(false);
                }}
                onClose={() => setCamera(false)}
              />
            )}
            <p className="fineprint">
              Votre capture reste dans ce navigateur. Seule l’autorisation
              chiffrée pour votre tablette est transmise. Code valable encore{' '}
              {remaining} min.
            </p>
            <details>
              <summary>Je ne trouve pas le QR Moodle</summary>
              <p>
                Ce parcours nécessite le QR de connexion proposé par Moodle. Il
                est annoncé comme activé sur le Moodle de CPE, mais doit encore
                être vérifié avec votre compte. Si seule l’adresse du site
                apparaît dans le QR, votre établissement n’a pas activé
                l’autorisation par QR.
              </p>
            </details>
          </>
        )}
        {error && (
          <p role="alert" className="error">
            {error}
          </p>
        )}
        {pendingCiphertext.current && error && (
          <Button
            className="primary-action"
            disabled={busy}
            onClick={() => void retrySend()}
          >
            Réessayer la transmission
          </Button>
        )}
        {pairing && (
          <Button
            variant="ghost"
            className="secondary-action"
            disabled={busy}
            onClick={() => {
              setPairing(null);
              setCamera(false);
              setCode('');
              setError('');
              pendingCiphertext.current = null;
            }}
          >
            Utiliser un nouveau code
          </Button>
        )}
      </section>
      <footer>reMoodle · Vos identifiants restent sur le site Moodle</footer>
    </main>
  );
}
