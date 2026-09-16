/* ============================================================
   Hearth core — DOM kit, storage, and safe markdown.

   Model output is untrusted text. It is rendered as markdown and
   then sanitised, because a model can be talked into emitting a
   <script> or a javascript: link, and "it came from the AI" is not
   a reason to hand it your page.
   ============================================================ */
window.H = (function(){
  "use strict";
  var H = {};

  /* ---------------- DOM ---------------- */
  function append(parent, kids){
    if(kids == null || kids === false) return;
    if(Array.isArray(kids)){ kids.forEach(function(k){ append(parent, k); }); return; }
    parent.appendChild(kids.nodeType ? kids : document.createTextNode(String(kids)));
  }
  function h(tag, props, kids){
    var e = document.createElement(tag);
    if(props) for(var k in props){
      var v = props[k];
      if(v == null || v === false) continue;
      if(k === "class") e.className = v;
      else if(k === "text") e.textContent = v;
      else if(k === "svg") e.innerHTML = v;
      else if(k.slice(0,2) === "on") e.addEventListener(k.slice(2).toLowerCase(), v);
      else if(v === true) e.setAttribute(k, "");
      else e.setAttribute(k, v);
    }
    append(e, kids);
    return e;
  }
  H.h = h;
  H.$ = function(id){ return document.getElementById(id); };
  H.clear = function(el){ while(el.firstChild) el.removeChild(el.firstChild); return el; };
  H.esc = function(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;"); };

  var toastTimer;
  H.toast = function(msg){
    var t = H.$("toast");
    t.textContent = msg;
    t.classList.add("on");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function(){ t.classList.remove("on"); }, 2800);
  };
  H.copy = function(text, what){
    if(navigator.clipboard && navigator.clipboard.writeText){
      navigator.clipboard.writeText(text).then(
        function(){ H.toast("Copied " + (what || "")); },
        function(){ H.toast("Couldn't copy — select it by hand."); });
    } else H.toast("Couldn't copy — select it by hand.");
  };

  /* ---------------- safe markdown ---------------- */
  if(window.marked) marked.setOptions({ breaks:true, gfm:true });
  if(window.DOMPurify){
    DOMPurify.addHook("afterSanitizeAttributes", function(node){
      if(node.tagName === "A"){
        node.setAttribute("target", "_blank");
        node.setAttribute("rel", "noopener noreferrer nofollow");
      }
    });
  }
  H.markdown = function(text){
    var raw = window.marked ? marked.parse(String(text || "")) : H.esc(text).replace(/\n/g, "<br>");
    if(window.DOMPurify){
      return DOMPurify.sanitize(raw, {
        ALLOWED_TAGS: ["p","br","strong","em","del","code","pre","blockquote","a","ul","ol","li",
                       "h1","h2","h3","h4","h5","h6","hr","table","thead","tbody","tr","th","td","span"],
        ALLOWED_ATTR: ["href","title","target","rel","class"],
        ALLOW_DATA_ATTR: false
      });
    }
    return H.esc(text);           // no sanitiser loaded: never render raw HTML
  };

  /* ---------------- storage ---------------- */
  var KEY = "hearth.v1";
  var DEFAULTS = {
    convos: [],
    active: null,
    settings: {
      engine: "none",           // "none" | "openrouter" | "local"
      model: "",
      modelLabel: "",
      key: "",
      system: "You are a helpful, concise assistant. Say plainly when you are unsure.",
      temperature: 0.7
    }
  };
  H.db = null;
  H.load = function(){
    try{
      var raw = localStorage.getItem(KEY);
      if(raw){
        var v = JSON.parse(raw);
        if(v && v.convos){
          v.settings = Object.assign({}, DEFAULTS.settings, v.settings || {});
          H.db = v;
          return H.db;
        }
      }
    }catch(e){}
    H.db = JSON.parse(JSON.stringify(DEFAULTS));
    return H.db;
  };
  H.save = function(){
    try{ localStorage.setItem(KEY, JSON.stringify(H.db)); }
    catch(e){ /* private window or full quota: the session still works */ }
  };

  H.newConvo = function(){
    var c = { id: "c" + Date.now().toString(36) + Math.random().toString(36).slice(2,5),
              title: "New chat", created: Date.now(), messages: [] };
    H.db.convos.unshift(c);
    H.db.active = c.id;
    if(H.db.convos.length > 60) H.db.convos.length = 60;
    H.save();
    return c;
  };
  H.convo = function(){
    var c = H.db.convos.filter(function(x){ return x.id === H.db.active; })[0];
    if(!c) c = H.db.convos[0] || H.newConvo();
    H.db.active = c.id;
    return c;
  };
  H.titleFrom = function(text){
    var t = String(text || "").replace(/\s+/g, " ").trim();
    return t.length > 42 ? t.slice(0, 42) + "…" : (t || "New chat");
  };

  /* ---------------- misc ---------------- */
  H.fmtNum = function(n){
    if(n >= 1e6) return (n/1e6).toFixed(n % 1e6 ? 1 : 0).replace(/\.0$/,"") + "M";
    if(n >= 1000) return Math.round(n/1000) + "k";
    return String(n);
  };
  H.fmtMB = function(mb){
    return mb >= 1024 ? (mb/1024).toFixed(1).replace(/\.0$/,"") + " GB" : Math.round(mb) + " MB";
  };
  H.rafThrottle = function(fn){
    var queued = false, args;
    return function(){
      args = arguments;
      if(queued) return;
      queued = true;
      requestAnimationFrame(function(){ queued = false; fn.apply(null, args); });
    };
  };

  return H;
})();
