// Toolsmith offline cache. The heavy PDF libraries are deliberately left out
// of the precache — they are big, most sessions never open the PDF tool, and
// the runtime handler below caches them the first time someone does.
var CACHE = "toolsmith-v1";
var SHELL = [
  "./", "./index.html", "./manifest.webmanifest", "./icon.svg",
  "./css/app.css",
  "./js/core.js", "./js/license.js", "./js/photo.js", "./js/cutout.js",
  "./js/design.js", "./js/batch.js", "./js/color.js", "./js/pdfkit.js", "./js/boot.js",
  "./fonts/fonts.css",
  "./fonts/Archivo-latin.woff2",
  "./fonts/AtkinsonHyperlegible-400-latin.woff2",
  "./fonts/AtkinsonHyperlegible-700-latin.woff2",
  "./fonts/IBMPlexMono-400-latin.woff2",
  "./fonts/IBMPlexMono-600-latin.woff2"
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
  if(new URL(e.request.url).origin !== location.origin) return;
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
