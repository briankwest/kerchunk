const CACHE_NAME = 'kerchunk-v3';
const CORE_ASSETS = [
  '/',
  'capture-processor.js',
  'stream-processor.js',
  'manifest.json'
];

/* Cache core assets on install */
self.addEventListener('install', (e) => {
  e.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(CORE_ASSETS))
      .then(() => self.skipWaiting())
  );
});

/* Clean old caches on activate */
self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(
        keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k))
      ))
      .then(() => self.clients.claim())
  );
});

/* Network-first strategy; skip everything that isn't a static asset.
 *
 * Pass-through (no caching, no respondWith) for:
 *   - both API surfaces (/api/ and /admin/api/) — responses are
 *     per-request truth, and /{,admin/}api/events is an SSE stream
 *     that must not be cloned into CacheStorage
 *   - WebSocket upgrades (/api/audio, /admin/api/audio)
 *   - any request explicitly asking for event-stream
 *   - authenticated admin HTML (/admin/...) so a cached authed page
 *     can't later leak to an unauthenticated client on the same origin
 */
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  const path = url.pathname;

  const isApi        = path.startsWith('/api/') || path.startsWith('/admin/api/');
  const isAdminPage  = path.startsWith('/admin/') && !path.startsWith('/admin/api/');
  const isWsUpgrade  = e.request.headers.get('upgrade') === 'websocket';
  const isSse        = (e.request.headers.get('accept') || '').includes('text/event-stream');

  if (isApi || isAdminPage || isWsUpgrade || isSse) {
    return;  /* let the browser handle it normally — no SW caching */
  }

  e.respondWith(
    fetch(e.request)
      .then((response) => {
        if (e.request.method === 'GET' && response.status === 200) {
          const clone = response.clone();
          caches.open(CACHE_NAME).then((cache) => cache.put(e.request, clone));
        }
        return response;
      })
      .catch(() => caches.match(e.request))
  );
});
