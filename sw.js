// Studylark offline cache.
// The whole app is one HTML file, so "works on the bus" is just a matter of
// keeping that file plus the one library it loads in the cache.
var CACHE = "studylark-v2";
var ASSETS = [
  "./",
  "./index.html",
  "./manifest.webmanifest",
  "./icon.svg",
  "./vendor/lz-string.min.js",
  "./fonts/fonts.css",
  "./fonts/Fraunces-latin.woff2",
  "./fonts/PublicSans-latin.woff2",
  "./fonts/IBMPlexMono-400-latin.woff2",
  "./fonts/IBMPlexMono-600-latin.woff2"
];

self.addEventListener("install", function(e){
  e.waitUntil(
    caches.open(CACHE).then(function(cache){
      // One bad URL must not sink the whole install, so cache them one by one.
      return Promise.all(ASSETS.map(function(url){
        return cache.add(new Request(url, { mode: "no-cors" })).catch(function(){});
      }));
    }).then(function(){ return self.skipWaiting(); })
  );
});

self.addEventListener("activate", function(e){
  e.waitUntil(
    caches.keys().then(function(keys){
      return Promise.all(keys.map(function(k){
        return k === CACHE ? null : caches.delete(k);
      }));
    }).then(function(){ return self.clients.claim(); })
  );
});

self.addEventListener("fetch", function(e){
  if(e.request.method !== "GET") return;
  e.respondWith(
    caches.match(e.request).then(function(hit){
      if(hit) return hit;
      return fetch(e.request).then(function(res){
        if(res && (res.ok || res.type === "opaque")){
          var copy = res.clone();
          caches.open(CACHE).then(function(c){ c.put(e.request, copy).catch(function(){}); });
        }
        return res;
      }).catch(function(){
        // Offline and not cached: fall back to the app shell for page loads.
        if(e.request.mode === "navigate") return caches.match("./index.html");
        throw new Error("offline");
      });
    })
  );
});
