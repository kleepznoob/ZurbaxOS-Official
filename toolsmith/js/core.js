/* ============================================================
   Toolsmith core — shell, shared UI kit, file plumbing.
   Every tool is built from the same handful of controls below,
   which is what keeps six different tools feeling like one app.
   ============================================================ */
window.TS = (function(){
  "use strict";
  var TS = {};

  /* ---------------- tiny DOM helper ---------------- */
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
      else if(k === "svg") e.innerHTML = v;                 // our own inline icons only
      else if(k.slice(0,2) === "on") e.addEventListener(k.slice(2).toLowerCase(), v);
      else if(v === true) e.setAttribute(k, "");
      else e.setAttribute(k, v);
    }
    append(e, kids);
    return e;
  }
  TS.h = h;
  TS.$ = function(id){ return document.getElementById(id); };
  // document.createElement("path") builds an HTML element that an <svg> will
  // not render. Parsing markup lets the browser assign the SVG namespace.
  TS.fromHTML = function(markup){
    var box = document.createElement("div");
    box.innerHTML = markup;
    return box.firstElementChild;
  };
  TS.clear = function(el){ while(el.firstChild) el.removeChild(el.firstChild); return el; };

  /* ---------------- numbers & strings ---------------- */
  TS.clamp = function(v, lo, hi){ return v < lo ? lo : v > hi ? hi : v; };
  TS.fmtBytes = function(n){
    if(n < 1024) return n + " B";
    if(n < 1048576) return (n/1024).toFixed(n < 10240 ? 1 : 0) + " KB";
    return (n/1048576).toFixed(2) + " MB";
  };
  TS.baseName = function(name){
    var i = String(name).lastIndexOf(".");
    return i > 0 ? name.slice(0, i) : name;
  };
  TS.rafThrottle = function(fn){
    var queued = false, lastArgs;
    return function(){
      lastArgs = arguments;
      if(queued) return;
      queued = true;
      requestAnimationFrame(function(){ queued = false; fn.apply(null, lastArgs); });
    };
  };

  /* ---------------- toast & sheet ---------------- */
  var toastTimer;
  TS.toast = function(msg){
    var t = TS.$("toast");
    t.textContent = msg;
    t.classList.add("on");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function(){ t.classList.remove("on"); }, 2800);
  };
  TS.sheet = function(title, build){
    TS.$("sheet-title").textContent = title;
    var body = TS.clear(TS.$("sheet-body"));
    build(body);
    TS.$("sheet-wrap").hidden = false;
  };
  TS.closeSheet = function(){ TS.$("sheet-wrap").hidden = true; };

  /* ---------------- saving files ---------------- */
  // A plain <a download> works on the real site. Inside the claude.ai viewer a
  // frame cannot download on its own, so ask the host to hand the file over.
  function linkSave(blob, filename){
    var url = URL.createObjectURL(blob);
    var a = h("a", { href: url, download: filename });
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    setTimeout(function(){ URL.revokeObjectURL(url); }, 5000);
  }
  TS.saveBlob = function(blob, filename){
    if(window.claude && typeof window.claude.use === "function"){
      return window.claude.use("downloads").then(function(dl){
        if(!dl){ linkSave(blob, filename); return; }
        return dl.save({ filename: filename, data: blob }).then(function(){
          TS.toast("Saved " + filename);
        }, function(err){
          if(err && err.code === "declined") return;
          linkSave(blob, filename);
        });
      }, function(){ linkSave(blob, filename); });
    }
    linkSave(blob, filename);
    TS.toast("Saved " + filename);
    return Promise.resolve();
  };
  TS.saveCanvas = function(canvas, filename, type, quality){
    return new Promise(function(res){
      canvas.toBlob(function(blob){
        if(!blob){ TS.toast("Could not build that file."); return res(); }
        TS.saveBlob(blob, filename).then(res);
      }, type || "image/png", quality);
    });
  };
  TS.saveText = function(text, filename, mime){
    return TS.saveBlob(new Blob([text], { type: mime || "text/plain" }), filename);
  };

  /* ---------------- lazy vendor loading ---------------- */
  var scripts = {};
  TS.loadScript = function(src){
    if(scripts[src]) return scripts[src];
    scripts[src] = new Promise(function(res, rej){
      var s = h("script", { src: src, onload: function(){ res(); }, onerror: function(){ rej(new Error(src)); } });
      document.head.appendChild(s);
    });
    return scripts[src];
  };

  /* ---------------- images ---------------- */
  TS.decode = function(file){
    if(window.createImageBitmap){
      return createImageBitmap(file).catch(function(){ return decodeViaImg(file); });
    }
    return decodeViaImg(file);
  };
  function decodeViaImg(file){
    return new Promise(function(res, rej){
      var url = URL.createObjectURL(file);
      var img = new Image();
      img.onload = function(){ URL.revokeObjectURL(url); res(img); };
      img.onerror = function(){ URL.revokeObjectURL(url); rej(new Error("unreadable")); };
      img.src = url;
    });
  }
  // Halve repeatedly before the final draw — a single big downscale looks mushy.
  TS.drawScaled = function(src, sx, sy, sw, sh, dw, dh, background){
    var cw = sw, ch = sh;
    var cur = document.createElement("canvas");
    cur.width = cw; cur.height = ch;
    cur.getContext("2d").drawImage(src, sx, sy, sw, sh, 0, 0, cw, ch);
    while(cw > dw * 2 && ch > dh * 2){
      var nw = Math.max(dw, Math.round(cw/2)), nh = Math.max(dh, Math.round(ch/2));
      var nxt = document.createElement("canvas");
      nxt.width = nw; nxt.height = nh;
      var nc = nxt.getContext("2d");
      nc.imageSmoothingQuality = "high";
      nc.drawImage(cur, 0, 0, cw, ch, 0, 0, nw, nh);
      cur = nxt; cw = nw; ch = nh;
    }
    var out = document.createElement("canvas");
    out.width = dw; out.height = dh;
    var ctx = out.getContext("2d");
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    if(background){ ctx.fillStyle = background; ctx.fillRect(0, 0, dw, dh); }
    ctx.drawImage(cur, 0, 0, cw, ch, 0, 0, dw, dh);
    return out;
  };

  /* ---------------- UI kit ---------------- */
  TS.sec = function(title, kids, onReset){
    var head = h("h2", null, [
      h("span", { class: "chev", "aria-hidden": "true" }, "\u25BC"),
      h("span", { class: "sect" }, title)
    ]);
    if(onReset){
      head.appendChild(h("button", {
        class: "reset", type: "button", title: "Reset this panel",
        onClick: function(ev){ ev.stopPropagation(); onReset(); }
      }, "Reset"));
    }
    var sec = h("div", { class: "sec" }, [head, h("div", { class: "sec-body" }, kids)]);
    head.addEventListener("click", function(){ sec.classList.toggle("shut"); });
    return sec;
  };
  TS.stack = function(kids){ return h("div", { class: "stack" }, kids); };
  TS.row2  = function(kids){ return h("div", { class: "row2" }, kids); };

  TS.slider = function(o){
    // The number is an input, not a readout: in a real editor you type an exact
    // value as often as you drag for one.
    var out = h("input", {
      class: "sl-val", value: fmtVal(o.value, o), "aria-label": o.label + " value",
      onChange: function(){
        var v = parseFloat(String(out.value).replace(/[^-\d.]/g, ""));
        if(isNaN(v)) v = Number(input.value);
        v = TS.clamp(v, Number(o.min), Number(o.max));
        input.value = v;
        out.value = fmtVal(v, o);
        if(o.onInput) o.onInput(v);
        if(o.onChange) o.onChange(v);
      }
    });
    var input = h("input", {
      type: "range", min: o.min, max: o.max, step: o.step || 1, value: o.value,
      id: o.id, "aria-label": o.label,
      onInput: function(){
        var v = Number(input.value);
        out.value = fmtVal(v, o);
        if(o.onInput) o.onInput(v);
      },
      onChange: function(){ if(o.onChange) o.onChange(Number(input.value)); }
    });
    var wrap = h("div", { class: "sl" }, [
      h("div", { class: "sl-top" }, [h("label", { for: o.id }, o.label), out]),
      input
    ]);
    wrap.set = function(v){ input.value = v; out.value = fmtVal(Number(v), o); };
    wrap.input = input;
    return wrap;
  };
  function fmtVal(v, o){
    var s = (o.plus && v > 0 ? "+" : "") + (o.decimals ? v.toFixed(o.decimals) : v);
    return s + (o.unit || "");
  }

  TS.seg = function(o){
    var box = h("div", { class: "seg", role: "group", "aria-label": o.label || "" });
    o.options.forEach(function(opt){
      box.appendChild(h("button", {
        type: "button", "aria-pressed": String(opt.value === o.value), "data-v": opt.value,
        title: opt.title || opt.label,
        onClick: function(){
          o.value = opt.value;
          Array.prototype.forEach.call(box.children, function(c){
            c.setAttribute("aria-pressed", String(c.dataset.v === String(opt.value)));
          });
          if(o.onChange) o.onChange(opt.value);
        }
      }, opt.label));
    });
    box.set = function(v){
      Array.prototype.forEach.call(box.children, function(c){
        c.setAttribute("aria-pressed", String(c.dataset.v === String(v)));
      });
    };
    return box;
  };

  TS.btn = function(label, o){
    o = o || {};
    return h("button", {
      type: "button",
      class: "btn" + (o.kind ? " " + o.kind : "") + (o.sm ? " sm" : ""),
      disabled: o.disabled, id: o.id, onClick: o.onClick
    }, label);
  };
  TS.field = function(label, input){
    return h("div", null, [label ? h("span", { class: "lbl" }, label) : null, input]);
  };
  TS.num = function(o){
    return h("input", {
      class: "inp mono", type: "number", min: o.min, max: o.max, step: o.step || 1,
      value: o.value, "aria-label": o.label, onInput: function(e){ o.onInput(Number(e.target.value)); }
    });
  };

  TS.dropzone = function(o){
    var input = h("input", {
      type: "file", accept: o.accept || "image/*", multiple: o.multiple !== false, hidden: true,
      onChange: function(){ if(input.files.length) o.onFiles(input.files); input.value = ""; }
    });
    var zone = h("label", { class: "drop" + (o.slim ? " slim" : ""), tabindex: "0" }, [
      h("strong", null, o.title || "Drop files here"),
      o.sub ? h("span", null, o.sub) : null
    ]);
    zone.appendChild(input);
    zone.addEventListener("click", function(e){ if(e.target !== input) input.click(); });
    zone.addEventListener("keydown", function(e){
      if(e.key === "Enter" || e.key === " "){ e.preventDefault(); input.click(); }
    });
    ["dragenter","dragover"].forEach(function(ev){
      zone.addEventListener(ev, function(e){ e.preventDefault(); zone.classList.add("hot"); });
    });
    ["dragleave","drop"].forEach(function(ev){
      zone.addEventListener(ev, function(e){ e.preventDefault(); zone.classList.remove("hot"); });
    });
    zone.addEventListener("drop", function(e){
      if(e.dataTransfer && e.dataTransfer.files.length) o.onFiles(e.dataTransfer.files);
    });
    zone.openPicker = function(){ input.click(); };   // so File > Open… can reach it
    return zone;
  };

  /* ---------------- options bar / status bar / doc tab ---------------- */
  TS.options = function(nodes){
    var bar = TS.clear(TS.$("options"));
    (Array.isArray(nodes) ? nodes : [nodes]).forEach(function(n){ if(n) bar.appendChild(n); });
  };
  TS.optLabel = function(text){ return h("label", { class: "olabel" }, text); };
  TS.optDivider = function(){ return h("span", { class: "divider" }); };

  TS.status = function(o){
    if(o.zoom != null) TS.$("status-zoom").textContent = o.zoom;
    if(o.doc != null) TS.$("status-doc").textContent = o.doc;
    if(o.hint != null) TS.$("status-hint").textContent = o.hint;
  };
  TS.docName = function(name){ TS.$("doctab").textContent = name; };

  // Tools publish the actions the menu bar can call. Anything a tool does not
  // publish shows greyed out rather than missing — the same way a real editor
  // greys Save when there is nothing to save.
  TS.cmds = {};
  TS.onCommands = null;
  TS.provide = function(cmds){
    TS.cmds = cmds || {};
    if(TS.onCommands) TS.onCommands();
  };
  TS.run = function(name){
    var fn = TS.cmds[name];
    if(typeof fn === "function") fn();
  };

  /* ---------------- the sample image ---------------- */
  // Every tool opens on a real picture rather than an empty box. Flat sky on
  // purpose: it gives Cutout something it can actually cut.
  var samplePromise = null;
  TS.sampleImage = function(){
    if(samplePromise) return samplePromise;
    samplePromise = new Promise(function(res){
      var W = 1500, H = 1000;
      var c = document.createElement("canvas");
      c.width = W; c.height = H;
      var x = c.getContext("2d");

      var sky = x.createLinearGradient(0, 0, 0, H * 0.66);
      sky.addColorStop(0, "#2E6FA8");
      sky.addColorStop(0.62, "#79B4D8");
      sky.addColorStop(1, "#BFDCEC");
      x.fillStyle = sky; x.fillRect(0, 0, W, H);

      x.fillStyle = "#FFF3D4";
      x.beginPath(); x.arc(W * 0.74, H * 0.24, 52, 0, 6.284); x.fill();

      [[0.62,"#3E5A55",1],[0.70,"#2C4441",1],[0.79,"#1D2E2C",1]].forEach(function(r, idx){
        x.fillStyle = r[1];
        x.beginPath(); x.moveTo(0, H); x.lineTo(0, H * r[0]);
        var peaks = 4 + idx * 2;
        for(var i = 0; i <= peaks; i++){
          var px = (W / peaks) * i;
          var py = H * r[0] - Math.sin(i * 1.9 + idx * 1.3) * H * 0.075 - (i % 2 ? H * 0.045 : 0);
          x.lineTo(px, py);
        }
        x.lineTo(W, H); x.closePath(); x.fill();
      });

      x.fillStyle = "#8E6A3F";
      x.fillRect(W * 0.16, H * 0.66, 16, H * 0.2);
      x.fillStyle = "#2F6B3C";
      x.beginPath();
      x.moveTo(W * 0.168, H * 0.60); x.lineTo(W * 0.115, H * 0.73);
      x.lineTo(W * 0.225, H * 0.73); x.closePath(); x.fill();

      var g = x.getImageData(0, 0, W, H), d = g.data;
      for(var i = 0; i < d.length; i += 4){
        var n = (Math.random() - 0.5) * 11;
        d[i] += n; d[i+1] += n; d[i+2] += n;
      }
      x.putImageData(g, 0, 0);

      c.toBlob(function(blob){
        var f = new File([blob], "sample-valley.jpg", { type: "image/jpeg" });
        TS.decode(f).then(function(bmp){ res({ bitmap: bmp, file: f, name: f.name, size: blob.size }); });
      }, "image/jpeg", 0.92);
    });
    return samplePromise;
  };

  /* ---------------- tool registry ---------------- */
  TS.tools = [];
  TS.register = function(def){ TS.tools.push(def); };
  var teardown = null;
  TS.active = null;

  TS.activate = function(id){
    var def = TS.tools.filter(function(t){ return t.id === id; })[0] || TS.tools[0];
    if(TS.active === def.id) return;
    if(teardown){ try{ teardown(); }catch(e){} teardown = null; }
    TS.active = def.id;

    Array.prototype.forEach.call(TS.$("rail").children, function(b){
      if(b.dataset.tool) b.setAttribute("aria-current", String(b.dataset.tool === def.id));
    });
    var badge = TS.clear(TS.$("tool-badge"));
    badge.appendChild(h("b", null, def.name));
    badge.appendChild(h("i", null, "replaces " + def.swaps));
    TS.options([]);
    TS.provide({});
    TS.docName("untitled");
    TS.status({ zoom: "Fit", doc: "", hint: def.blurb });

    var stage = TS.clear(TS.$("stage"));
    var insp = TS.clear(TS.$("inspector"));
    insp.appendChild(h("div", { class: "insp-head" }, [
      h("h1", null, [def.name, h("span", { class: "swap" }, def.swaps)]),
      h("p", null, def.blurb)
    ]));
    try{
      teardown = def.mount(stage, insp) || null;
    }catch(err){
      stage.appendChild(h("div", { class: "stage-pad" },
        h("p", { class: "empty" }, "This tool failed to start: " + err.message)));
    }
    if(location.hash.slice(1) !== def.id) history.replaceState(null, "", "#" + def.id);
  };

  return TS;
})();
