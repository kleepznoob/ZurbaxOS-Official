/* ============================================================
   Boot — build the chrome, wire the menus, open a tool.

   The menu bar is real: every item either runs something or shows
   greyed out because the current tool does not publish that command.
   A decorative menu bar would be worse than none.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h;
  var zoom = "fit", panelsHidden = false;

  /* ---------------- tool rail ---------------- */
  var rail = TS.$("rail");
  var GROUPS = [["photo","cutout","design"], ["batch","color"], ["pdf"]];
  GROUPS.forEach(function(group, gi){
    if(gi) rail.appendChild(h("span", { class:"rule" }));
    group.forEach(function(id){
      var t = TS.tools.filter(function(x){ return x.id === id; })[0];
      if(!t) return;
      rail.appendChild(h("button", {
        type:"button", "data-tool": t.id, "aria-current":"false",
        title: t.name + " — replaces " + t.swaps,
        "aria-label": t.name,
        onClick: function(){ TS.activate(t.id); }
      }, h("span", { svg: t.icon })));
    });
  });

  /* ---------------- theme ---------------- */
  function theme(){ return document.documentElement.getAttribute("data-theme") || "dark"; }
  function applyTheme(mode){
    document.documentElement.setAttribute("data-theme", mode);
    try{ localStorage.setItem("toolsmith.theme", mode); }catch(e){}
  }
  var startTheme = "dark";
  try{
    var saved = localStorage.getItem("toolsmith.theme");
    if(saved === "light" || saved === "dark") startTheme = saved;
  }catch(e){}
  applyTheme(startTheme);

  /* ---------------- zoom + panels ---------------- */
  function setZoom(mode){
    zoom = mode;
    Array.prototype.forEach.call(document.querySelectorAll(".canvas-wrap"), function(w){
      w.classList.toggle("actual", mode === "actual");
    });
    TS.status({ zoom: mode === "actual" ? "Actual pixels" : "Fit" });
  }
  TS.setZoom = setZoom;
  TS.$("status-zoom").addEventListener("click", function(){
    setZoom(zoom === "fit" ? "actual" : "fit");
  });
  function togglePanels(){
    panelsHidden = !panelsHidden;
    TS.$("app").classList.toggle("no-dock", panelsHidden);
  }

  /* ---------------- menu definitions ---------------- */
  var MENUS = [
    { label:"File", items:[
      { label:"Open…",            cmd:"open",   key:"Ctrl O" },
      { label:"Save / Export…",   cmd:"export", key:"Ctrl S" },
      "-",
      { label:"Clear this document",   cmd:"clear" }
    ]},
    { label:"Edit", items:[
      { label:"Undo",                  cmd:"undo",   key:"Ctrl Z" },
      { label:"Start over",            cmd:"clear" },
      "-",
      { label:"Reset all adjustments", cmd:"reset" }
    ]},
    { label:"Image", items:[
      { label:"Rotate 90° clockwise", cmd:"rotate" },
      { label:"Flip horizontal",           cmd:"flip" },
      { label:"Crop…",                cmd:"crop" }
    ]},
    { label:"View", items:[
      { label:"Fit on screen",  act:function(){ setZoom("fit"); },    tick:function(){ return zoom === "fit"; } },
      { label:"Actual pixels",  act:function(){ setZoom("actual"); }, tick:function(){ return zoom === "actual"; } },
      "-",
      { label:"Dark interface",  act:function(){ applyTheme("dark"); },  tick:function(){ return theme() === "dark"; } },
      { label:"Light interface", act:function(){ applyTheme("light"); }, tick:function(){ return theme() === "light"; } },
      "-",
      { label:"Hide panels", act:togglePanels, tick:function(){ return panelsHidden; }, key:"Tab" }
    ]},
    { label:"Window", items: TS.tools.map(function(t){
        return { label: t.name + "  —  replaces " + t.swaps,
                 act: function(){ TS.activate(t.id); },
                 tick: function(){ return TS.active === t.id; } };
      })
    },
    { label:"Help", items:[
      { label:"About Toolsmith",    act:about },
      { label:"What this is not",   act:limits },
      "-",
      { label:"Unlock Pro…",   act:function(){ TS.license.unlockSheet(null); } }
    ]}
  ];

  /* ---------------- menu machinery ---------------- */
  var pop = TS.$("menupop"), openIdx = -1;

  function closeMenu(){
    pop.hidden = true;
    openIdx = -1;
    Array.prototype.forEach.call(TS.$("menus").children, function(b){
      b.setAttribute("aria-expanded", "false");
    });
  }
  function openMenu(i){
    var def = MENUS[i], btn = TS.$("menus").children[i];
    TS.clear(pop);
    def.items.forEach(function(item){
      if(item === "-"){ pop.appendChild(h("hr")); return; }
      var enabled = item.act ? true : typeof TS.cmds[item.cmd] === "function";
      var ticked = item.tick ? item.tick() : false;
      pop.appendChild(h("button", {
        type:"button", disabled: !enabled,
        onClick: function(){
          closeMenu();
          if(item.act) item.act(); else TS.run(item.cmd);
        }
      }, [
        h("span", { class:"tick" }, ticked ? "✓" : ""),
        h("span", null, item.label),
        item.key ? h("span", { class:"key" }, item.key) : null
      ]));
    });
    var r = btn.getBoundingClientRect();
    pop.hidden = false;
    pop.style.left = Math.min(r.left, window.innerWidth - pop.offsetWidth - 8) + "px";
    pop.style.top = r.bottom + "px";
    openIdx = i;
    btn.setAttribute("aria-expanded", "true");
  }

  var menus = TS.$("menus");
  MENUS.forEach(function(def, i){
    menus.appendChild(h("button", {
      type:"button", "aria-expanded":"false", "aria-haspopup":"true",
      onClick: function(ev){
        ev.stopPropagation();
        if(openIdx === i) closeMenu(); else { closeMenu(); openMenu(i); }
      },
      onMouseEnter: function(){ if(openIdx >= 0 && openIdx !== i){ closeMenu(); openMenu(i); } }
    }, def.label));
  });
  document.addEventListener("click", function(){ if(openIdx >= 0) closeMenu(); });
  document.addEventListener("keydown", function(e){
    if(e.key === "Escape"){
      if(openIdx >= 0) return closeMenu();
      if(!TS.$("sheet-wrap").hidden) TS.closeSheet();
    }
    var tag = (e.target.tagName || "").toLowerCase();
    if(tag === "input" || tag === "textarea" || tag === "select") return;
    if(e.key === "Tab" && !e.ctrlKey && !e.metaKey && !e.altKey){ e.preventDefault(); togglePanels(); return; }
    if(e.ctrlKey || e.metaKey){
      var k = e.key.toLowerCase();
      if(k === "o" && TS.cmds.open){ e.preventDefault(); TS.run("open"); }
      else if(k === "s" && TS.cmds.export){ e.preventDefault(); TS.run("export"); }
      else if(k === "z" && TS.cmds.undo){ e.preventDefault(); TS.run("undo"); }
    }
  });
  // Menus re-read command availability whenever a tool publishes its own.
  TS.onCommands = function(){ if(openIdx >= 0){ var i = openIdx; closeMenu(); openMenu(i); } };

  /* ---------------- help sheets ---------------- */
  function about(){
    TS.sheet("About Toolsmith", function(b){
      b.appendChild(h("p", null, "Six tools for the creative jobs people actually do every day, in one browser tab. No install, no account, no subscription."));
      b.appendChild(h("h3", null, "Your files never leave this device"));
      b.appendChild(h("p", null, "There is no server to upload them to. Every pixel is processed by your own browser, which is also why it keeps working with the wifi off."));
      b.appendChild(h("h3", null, "What replaces what"));
      var t = h("table", null, [h("tr", null, [h("th", null, "Toolsmith"), h("th", null, "Instead of")])]);
      TS.tools.forEach(function(x){ t.appendChild(h("tr", null, [h("td", null, x.name), h("td", null, x.swaps)])); });
      b.appendChild(t);
      b.appendChild(h("p", { class:"note" }, "Not affiliated with Adobe. Product names are their owners' trademarks and are used here only to say what each tool is for."));
    });
  }
  function limits(){
    TS.sheet("What this is not", function(b){
      b.appendChild(h("p", null, "Toolsmith is deliberately smaller than the apps it stands in for, and it is better to know where the edges are now than halfway through a deadline."));
      b.appendChild(h("h3", null, "It does not have"));
      b.appendChild(h("p", null, "Layer masks and blend modes. CMYK or print colour management. RAW developing. Type on a path. Multi-page layout with linked text. Video of any kind."));
      b.appendChild(h("h3", null, "Cutout reads colour, not meaning"));
      b.appendChild(h("p", null, "It finds pixels close in colour to the one you clicked. Flat or evenly-lit backgrounds come out clean. Hair against a forest will not, because nothing here knows what hair is."));
      b.appendChild(h("h3", null, "Big files will feel it"));
      b.appendChild(h("p", null, "Everything runs in one browser tab on your own machine. A 60-megapixel photo works, but it will not be instant."));
      b.appendChild(h("p", null, "For those jobs the big applications are still the right tool. For resizing 200 images, lifting a palette, or cutting a product shot out of a white background, this is faster and costs nothing."));
    });
  }

  /* ---------------- open ---------------- */
  TS.license.paint();
  TS.license.refresh();
  var start = location.hash.slice(1);
  TS.activate(TS.tools.some(function(t){ return t.id === start; }) ? start : "photo");
  setZoom("fit");

  window.addEventListener("hashchange", function(){
    var id = location.hash.slice(1);
    if(TS.tools.some(function(t){ return t.id === id; })) TS.activate(id);
  });

  if("serviceWorker" in navigator && location.protocol === "https:"){
    navigator.serviceWorker.register("sw.js").catch(function(){});
  }
})();
