const CACHE_NAME = 'kerchunk-ptt-v1';
const CORE_ASSETS = [
  'ptt.html',
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

/* Network-first strategy; skip API calls and WebSocket upgrades */
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);

  /* Don't cache API or WebSocket requests */
  if (url.pathname.startsWith('/api/') || e.request.headers.get('upgrade') === 'websocket') {
    return;
  }

  e.respondWith(
    fetch(e.request)
      .then((response) => {
        /* Cache successful GET responses */
        if (e.request.method === 'GET' && response.status === 200) {
          const clone = response.clone();
          caches.open(CACHE_NAME).then((cache) => cache.put(e.request, clone));
        }
        return response;
      })
      .catch(() => caches.match(e.request))
  );
});
