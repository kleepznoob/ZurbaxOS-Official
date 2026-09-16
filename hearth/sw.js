// Hearth offline shell. Only the app itself is cached — model weights and
// provider calls are deliberately left to the network.
var CACHE = "hearth-v1";
var SHELL = [
  "./", "./index.html", "./manifest.webmanifest", "./icon.svg",
  "./css/app.css",
  "./js/core.js", "./js/engines.js", "./js/models.js", "./js/ui.js", "./js/boot.js",
  "./vendor/marked.min.js", "./vendor/purify.min.js",
  "./fonts/fonts.css",
  "./fonts/BricolageGrotesque-latin.woff2",
  "./fonts/Karla-latin.woff2",
  "./fonts/JetBrainsMono-latin.woff2"
];

self.addEventListener("install", function(e){
  e.waitUntil(caches.open(CACHE).then(function(c){
    return Promise.all(SHELL.map(function(u){ return c.add(u).catch(function(){}); }));
  }).then(function(){ return self.skipWaiting(); }));
});
self.addEventListener("activate", function(e){
  e.waitUntil(caches.keys().then(function(keys){
    return Promise.all(keys.map(function(k){ return k === CACHE ? null : caches.delete(k); }));
  }).then(function(){ return self.clients.claim(); }));
});
self.addEventListener("fetch", function(e){
  if(e.request.method !== "GET") return;
  if(new URL(e.request.url).origin !== location.origin) return;   // never cache provider traffic
  e.respondWith(caches.match(e.request).then(function(hit){
    if(hit) return hit;
    return fetch(e.request).then(function(res){
      if(res && res.ok){
        var copy = res.clone();
        caches.open(CACHE).then(function(c){ c.put(e.request, copy).catch(function(){}); });
      }
      return res;
    }).catch(function(){
      if(e.request.mode === "navigate") return caches.match("./index.html");
      throw new Error("offline");
    });
  }));
});
