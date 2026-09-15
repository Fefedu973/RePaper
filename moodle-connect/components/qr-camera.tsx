'use client';
import { useEffect, useRef } from 'react';
import { readPixels } from '@/lib/read-qr';
import { Button } from '@/components/ui/button';
export function QrCamera({
  onRead,
  onError,
  onClose,
}: {
  onRead: (value: string) => void;
  onError: (error: string) => void;
  onClose: () => void;
}) {
  const video = useRef<HTMLVideoElement>(null),
    callbacks = useRef({ onRead, onError });
  callbacks.current = { onRead, onError };
  useEffect(() => {
    let stopped = false,
      stream: MediaStream | undefined,
      timer: ReturnType<typeof setTimeout>;
    const canvas = document.createElement('canvas');
    const stop = () => {
      stream?.getTracks().forEach((t) => t.stop());
      clearTimeout(timer);
    };
    const scan = () => {
      if (stopped) return;
      const v = video.current;
      if (v && v.readyState >= 2 && v.videoWidth) {
        const scale = Math.min(1, 960 / v.videoWidth);
        canvas.width = Math.round(v.videoWidth * scale);
        canvas.height = Math.round(v.videoHeight * scale);
        canvas
          .getContext('2d')
          ?.drawImage(v, 0, 0, canvas.width, canvas.height);
        const text = readPixels(canvas);
        if (text) {
          stop();
          callbacks.current.onRead(text);
          return;
        }
      }
      timer = setTimeout(scan, 350);
    };
    navigator.mediaDevices
      ?.getUserMedia({
        video: { facingMode: { ideal: 'environment' } },
        audio: false,
      })
      .then(async (media) => {
        stream = media;
        if (stopped) {
          stop();
          return;
        }
        if (video.current) {
          video.current.srcObject = media;
          await video.current.play();
        }
        scan();
      })
      .catch(() => {
        stop();
        if (!stopped)
          callbacks.current.onError(
            'Caméra indisponible ou refusée. Vous pouvez choisir une capture du QR.',
          );
      });
    return () => {
      stopped = true;
      stop();
      canvas.width = 0;
      canvas.height = 0;
    };
  }, []);
  return (
    <div className="camera-view">
      <video
        ref={video}
        muted
        playsInline
        aria-label="Caméra pour lire le QR Moodle"
      />
      <Button variant="outline" onClick={onClose} className="secondary-action">
        Arrêter la caméra
      </Button>
    </div>
  );
}
