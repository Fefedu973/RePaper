import { env } from 'cloudflare:workers';
import { createRelay } from '@/lib/relay';
import { PUBLIC_ORIGIN } from '@/lib/protocol';
export const dynamic = 'force-dynamic';
async function handle(request: Request) {
  const url = new URL(request.url);
  const development = import.meta.env.DEV;
  return createRelay(env.DB, {
    origin: development ? url.origin : PUBLIC_ORIGIN,
    development,
  })(request);
}
export const GET = handle;
export const POST = handle;
export const DELETE = handle;
