/* Service worker of the S-Link portal. Browsers register service workers only on a secure context (https:// or localhost): on plain http://<device> the
 * page still works, it just is not cached. With HTTPS (see the README) this keeps the app shell so the installed app opens at once and, when the device
 * does not answer, still opens and shows the "disconnected" screen instead of a browser error.
 *   - the shell (page, manifest, icons, help) is served from the cache AT ONCE and refreshed in the background ("stale while revalidate"): the next
 *     start has the version the device served last time. The page itself asks the device which build it serves (/api/ota) and, when the cached copy
 *     is an old one, reloads once with ?r=<build>; a request with ?r= always goes to the network first.
 *   - the live data (WebSocket /ws, /api/*), the manifest and the CA download are never cached.
 * Change VERSION to flush the cache. */
const VERSION = 'slink-v3';
const SHELL = ['/', '/icon.svg', '/icon-192.png', '/icon-512.png', '/apple-touch-icon.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(VERSION).then(c => Promise.all(SHELL.map(u => c.add(new Request(u, { cache: 'reload' })).catch(() => {})))).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== VERSION).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const r = e.request, u = new URL(r.url);
  if (r.method !== 'GET' || u.origin !== location.origin || u.pathname.startsWith('/api/') || u.pathname === '/ws' || u.pathname === '/sw.js' || u.pathname === '/ca.crt' || u.pathname === '/manifest.webmanifest') return;   /* the manifest carries the app name (Settings): always from the device, never from the cache */
  const key = u.origin + u.pathname;                                    /* one copy per path: the query string (?tab=, ?r=) is not part of the key */
  const fromNet = () => fetch(r, { cache: 'no-cache' }).then(res => {
    if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(key, copy)); }
    return res;
  });
  const offline = () => caches.match(key).then(m => m || (r.mode === 'navigate' ? caches.match(u.origin + '/') : Response.error()));
  if (u.searchParams.has('r')) { e.respondWith(fromNet().catch(offline)); return; }      /* the page asked for a fresh copy */
  e.respondWith(caches.match(key).then(m => {
    const net = fromNet();
    if (m) { e.waitUntil(net.catch(() => {})); return m; }             /* cached: answer now, refresh behind */
    return net.catch(offline);
  }));
});
