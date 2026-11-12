/* Service worker of the S-Link portal: keeps the app shell so the installed app opens at once (and shows the "disconnected" screen instead of a
 * browser error when the device is off). The live data (WebSocket /ws, /api/*) is never cached. Browsers register service workers only on a secure
 * context (https:// or localhost); on plain http://<device> the page still works, it just is not cached. Change VERSION to flush the cache. */
const VERSION = 'slink-v1';
const SHELL = ['/', '/manifest.webmanifest', '/icon.svg', '/icon-192.png', '/icon-512.png', '/apple-touch-icon.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(VERSION).then(c => Promise.all(SHELL.map(u => c.add(new Request(u, { cache: 'reload' })).catch(() => {})))).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== VERSION).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const r = e.request, u = new URL(r.url);
  if (r.method !== 'GET' || u.origin !== location.origin || u.pathname.startsWith('/api/') || u.pathname === '/ws' || u.pathname === '/sw.js') return;
  /* network first (the firmware may have been updated), the cached copy when the device does not answer */
  e.respondWith(fetch(r).then(res => {
    if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(r, copy)); }
    return res;
  }).catch(() => caches.match(r, { ignoreSearch: true }).then(m => m || (r.mode === 'navigate' ? caches.match('/') : Response.error()))));
});
