/* ============================================================
   Boot — load what was saved, wire the chrome, open the chat.
   ============================================================ */
(function(){
  "use strict";
  var H = window.H;

  H.load();
  if(!H.db.convos.length) H.newConvo();

  function applyTheme(mode){
    if(mode) document.documentElement.setAttribute("data-theme", mode);
    else document.documentElement.removeAttribute("data-theme");
    var dark = mode === "dark" ||
      (!mode && window.matchMedia("(prefers-color-scheme: dark)").matches);
    H.$("theme-btn").textContent = dark ? "Light" : "Dark";
    try{ if(mode) localStorage.setItem("hearth.theme", mode); }catch(e){}
  }
  try{
    var saved = localStorage.getItem("hearth.theme");
    applyTheme(saved === "light" || saved === "dark" ? saved : null);
  }catch(e){ applyTheme(null); }
  H.$("theme-btn").addEventListener("click", function(){
    var dark = document.documentElement.getAttribute("data-theme") === "dark" ||
      (!document.documentElement.hasAttribute("data-theme") &&
       window.matchMedia("(prefers-color-scheme: dark)").matches);
    applyTheme(dark ? "light" : "dark");
  });

  H.ui.init();
  H.ui.renderConvos();
  H.ui.renderMessages();
  H.ui.refreshEngine();

  var start = location.hash.slice(1);
  H.ui.show(["chat","models","setup","about"].indexOf(start) >= 0 ? start : "chat");

  if("serviceWorker" in navigator && location.protocol === "https:"){
    navigator.serviceWorker.register("sw.js").catch(function(){});
  }
})();
