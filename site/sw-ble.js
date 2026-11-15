/* Service worker of the Bluetooth page (ble.html on GitHub Pages). The page talks to the board over Bluetooth and needs nothing from the network once
 * it is loaded, so the app shell is kept and the installed app also starts offline. "Stale while revalidate": the cached copy answers at once and is
 * refreshed behind (a new version of the page is there at the next start). Change VERSION to flush the cache. */
const VERSION = 'slink-ble-v1';
const SHELL = ['./ble.html', './manifest-ble.webmanifest', './icon.svg', './icon-192.png', './icon-512.png', './apple-touch-icon.png', './help.html'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(VERSION).then(c => Promise.all(SHELL.map(u => c.add(new Request(u, { cache: 'reload' })).catch(() => {})))).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== VERSION).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const r = e.request, u = new URL(r.url);
  if (r.method !== 'GET' || u.origin !== location.origin || !u.pathname.startsWith(new URL('./', location).pathname)) return;      /* only this site's own files */
  if (u.pathname.endsWith('/sw-ble.js')) return;
  const key = u.origin + u.pathname;
  const fromNet = () => fetch(r, { cache: 'no-cache' }).then(res => { if (res.ok) { const copy = res.clone(); e.waitUntil(caches.open(VERSION).then(c => c.put(key, copy))); } return res; });
  e.respondWith(caches.match(key).then(m => { if (m) { e.waitUntil(fromNet().catch(() => {})); return m; } return fromNet().catch(() => caches.match(new URL('./ble.html', location).href)); }));
});
