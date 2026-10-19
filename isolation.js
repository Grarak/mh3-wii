// Cross-origin isolation where the host sends no headers of its own (GitHub
// Pages and other static hosts): this service worker gives every response the
// headers scripts/serve-web.py sends. mh3-web.js registers it and reloads
// once under it.
'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
  const request = event.request;

  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin')
    return;

  event.respondWith(fetch(request).then((response) => {
    // Opaque responses (another origin's, without CORS) keep theirs.
    if (response.status === 0)
      return response;

    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
