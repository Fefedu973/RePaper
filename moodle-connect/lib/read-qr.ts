import jsQR from 'jsqr';

export function readPixels(canvas: HTMLCanvasElement): string | null {
  const context = canvas.getContext('2d', { willReadFrequently: true });
  if (!context)
    throw new Error(
      'La lecture d’image n’est pas disponible dans ce navigateur.',
    );
  const pixels = context.getImageData(0, 0, canvas.width, canvas.height);
  return (
    jsQR(pixels.data, pixels.width, pixels.height, {
      inversionAttempts: 'dontInvert',
    })?.data || null
  );
}
export async function readQrFile(file: File): Promise<string> {
  if (file.size > 12 * 1024 * 1024)
    throw new Error('Choisissez une capture de moins de 12 Mo.');
  if (!['image/png', 'image/jpeg', 'image/webp'].includes(file.type))
    throw new Error('Choisissez une capture PNG, JPG ou WebP.');
  const url = URL.createObjectURL(file),
    image = new Image();
  try {
    await new Promise<void>((resolve, reject) => {
      image.onload = () => resolve();
      image.onerror = () =>
        reject(new Error('Impossible de lire cette image.'));
      image.src = url;
    });
    if (image.naturalWidth * image.naturalHeight > 40_000_000)
      throw new Error(
        'Cette image est trop grande. Recadrez la capture autour du QR code.',
      );
    for (const maximum of [1400, 2400]) {
      const scale = Math.min(
        1,
        maximum / Math.max(image.naturalWidth, image.naturalHeight),
      );
      const canvas = document.createElement('canvas');
      canvas.width = Math.round(image.naturalWidth * scale);
      canvas.height = Math.round(image.naturalHeight * scale);
      const context = canvas.getContext('2d');
      if (!context) throw new Error('Lecture d’image indisponible.');
      context.drawImage(image, 0, 0, canvas.width, canvas.height);
      const data = readPixels(canvas);
      canvas.width = 0;
      canvas.height = 0;
      if (data) return data;
      if (scale === 1) break;
    }
    throw new Error(
      'QR non trouvé. Recadrez la capture autour du QR Moodle en gardant sa bordure blanche.',
    );
  } finally {
    URL.revokeObjectURL(url);
    image.src = '';
  }
}
