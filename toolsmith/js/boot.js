/* ============================================================
   Boot — build the tool rail, wire the chrome, open a tool.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h;

  /* ---------------- tool rail ---------------- */
  var rail = TS.$("rail");
  TS.tools.forEach(function(t){
    rail.appendChild(h("button", {
      type:"button", "data-tool": t.id, "aria-current":"false",
      title: t.name + " — replaces " + t.swaps,
      onClick: function(){ TS.activate(t.id); }
    }, [h("span", { svg: t.icon }), h("b", null, t.name)]));
  });

  /* ---------------- theme ---------------- */
  function applyTheme(mode){
    document.documentElement.setAttribute("data-theme", mode);
    TS.$("theme-toggle").textContent = mode === "dark" ? "Light" : "Dark";
    try{ localStorage.setItem("toolsmith.theme", mode); }catch(e){}
  }
  var startTheme = "dark";
  try{
    var saved = localStorage.getItem("toolsmith.theme");
    if(saved === "light" || saved === "dark") startTheme = saved;
  }catch(e){}
  applyTheme(startTheme);
  TS.$("theme-toggle").addEventListener("click", function(){
    applyTheme(document.documentElement.getAttribute("data-theme") === "dark" ? "light" : "dark");
  });

  /* ---------------- sheet plumbing ---------------- */
  TS.$("sheet-close").addEventListener("click", TS.closeSheet);
  TS.$("sheet-wrap").addEventListener("click", function(e){
    if(e.target === TS.$("sheet-wrap")) TS.closeSheet();
  });
  document.addEventListener("keydown", function(e){
    if(e.key === "Escape" && !TS.$("sheet-wrap").hidden) TS.closeSheet();
  });

  /* ---------------- about ---------------- */
  TS.$("about-btn").addEventListener("click", function(){
    TS.sheet("About Toolsmith", function(b){
      b.appendChild(h("p", null, "Six tools for the creative jobs people actually do every day, in one browser tab. No install, no account, no subscription."));
      b.appendChild(h("h3", null, "Your files never leave this device"));
      b.appendChild(h("p", null, "There is no server to upload them to. Every pixel is processed by your own browser, which is also why it keeps working with the wifi off."));
      b.appendChild(h("h3", null, "What replaces what"));
      var t = h("table", null, [h("tr", null, [h("th", null, "Toolsmith"), h("th", null, "Instead of")])]);
      TS.tools.forEach(function(x){
        t.appendChild(h("tr", null, [h("td", null, x.name), h("td", null, x.swaps)]));
      });
      b.appendChild(t);
      b.appendChild(h("h3", null, "What it is not"));
      b.appendChild(h("p", null, "It is not Photoshop. It has no layer masks, no CMYK, no print colour management, and Cutout reads colour rather than understanding what a person is. For those jobs, the big apps are still the right tool — and this is honest about it rather than finding out halfway through your homework."));
      b.appendChild(h("p", { class:"note" }, "Built with Claude Code. Source and licence on GitHub."));
    });
  });

  /* ---------------- open ---------------- */
  TS.license.paint();
  TS.license.refresh();
  var start = location.hash.slice(1);
  TS.activate(TS.tools.some(function(t){ return t.id === start; }) ? start : "photo");

  window.addEventListener("hashchange", function(){
    var id = location.hash.slice(1);
    if(TS.tools.some(function(t){ return t.id === id; })) TS.activate(id);
  });

  if("serviceWorker" in navigator && location.protocol === "https:"){
    navigator.serviceWorker.register("sw.js").catch(function(){});
  }
})();
