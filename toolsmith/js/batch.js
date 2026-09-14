/* ============================================================
   Batch — resize, convert and compress a pile of images at once.
   This is the job Adobe makes you open Bridge, build a droplet or
   write an action for. Here it is a drop, a preset and a button.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h, L = TS.license;

  var PRESETS = [
    { id:"orig",   label:"Original size", sub:"convert only" },
    { id:"l1920",  label:"Long edge 1920", long:1920 },
    { id:"l1280",  label:"Long edge 1280", long:1280 },
    { id:"l800",   label:"Long edge 800",  long:800 },
    { id:"yt",     label:"YouTube thumb",  w:1280, h:720 },
    { id:"square", label:"Square post",    w:1080, h:1080 },
    { id:"story",  label:"Story / Reel",   w:1080, h:1920 },
    { id:"avatar", label:"Avatar",         w:512,  h:512 },
    { id:"custom", label:"Custom", sub:"set it yourself" }
  ];
  var EXT = { "image/jpeg":"jpg", "image/png":"png", "image/webp":"webp" };

  TS.register({
    id: "batch",
    name: "Batch",
    swaps: "Bridge",
    blurb: "Drop a hundred images, pick one size, get them all back in a zip.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="7" width="13" height="13" rx="2"/><path d="M7 7V5.5A1.5 1.5 0 0 1 8.5 4H19a1.5 1.5 0 0 1 1.5 1.5V16"/></svg>',

    mount: function(stage, insp){
      var items = [], seq = 0, busy = false, queued = false, alive = true;
      var cfg = { preset:"l1280", cw:1200, ch:800, fit:"cover", type:"image/jpeg", quality:82 };

      var listBox = h("div", { class:"list" });
      var dz = TS.dropzone({
        title:"Drop images here", sub:"JPG, PNG, WebP, GIF — as many as you like",
        onFiles: addFiles
      });
      stage.appendChild(h("div", { class:"stage-pad" }, [dz, listBox]));

      /* ---------- inspector ---------- */
      var presetSeg = h("div", { class:"stack" });
      var presetRows = [];
      for(var i = 0; i < PRESETS.length; i += 3){
        var group = PRESETS.slice(i, i+3);
        var seg = TS.seg({
          label:"Size preset", value: cfg.preset,
          options: group.map(function(p){
            return { value:p.id, label:p.label.replace("Long edge ","≤"), title:p.label };
          }),
          onChange: function(v){
            cfg.preset = v;
            presetRows.forEach(function(s){ s.set(v); });
            customRow.hidden = (v !== "custom");
            fitRow.hidden = !needsFit();
            reprocess();
          }
        });
        presetRows.push(seg);
        presetSeg.appendChild(seg);
      }
      var customRow = h("div", { class:"row2", hidden:true }, [
        TS.field("Width", TS.num({ label:"Width", min:8, max:12000, value:cfg.cw, onInput:function(v){ cfg.cw = v; reprocess(); } })),
        TS.field("Height", TS.num({ label:"Height", min:8, max:12000, value:cfg.ch, onInput:function(v){ cfg.ch = v; reprocess(); } }))
      ]);
      var fitRow = h("div", null, TS.seg({
        label:"Fit", value:cfg.fit,
        options:[{ value:"cover", label:"Crop to fill" }, { value:"contain", label:"Fit inside" }],
        onChange:function(v){ cfg.fit = v; reprocess(); }
      }));
      fitRow.hidden = !needsFit();

      var qSlider = TS.slider({
        id:"b-q", label:"Quality", min:40, max:100, value:cfg.quality, plus:false,
        onInput:function(v){ cfg.quality = v; }, onChange:reprocess
      });
      var qWrap = h("div", null, qSlider);
      var fmtSeg = TS.seg({
        label:"Format", value:cfg.type,
        options:[{ value:"image/jpeg", label:"JPG" }, { value:"image/webp", label:"WebP" }, { value:"image/png", label:"PNG" }],
        onChange:function(v){ cfg.type = v; qWrap.hidden = (v === "image/png"); reprocess(); }
      });

      var statBefore = h("b", { class:"num" }, "0 KB");
      var statAfter  = h("b", { class:"num" }, "0 KB");
      var statSaved  = h("b", { class:"num", style:"color:var(--good)" }, "0 KB");
      var zipBtn = TS.btn("Download all as .zip", { disabled:true, onClick:downloadZip });
      var clearBtn = TS.btn("Clear list", { kind:"ghost", onClick:function(){ items = []; paint(); } });
      var limitNote = h("div", null);

      insp.appendChild(TS.sec("Size", TS.stack([presetSeg, customRow, fitRow])));
      insp.appendChild(TS.sec("Format", TS.stack([fmtSeg, qWrap])));
      insp.appendChild(TS.sec("Result", TS.stack([
        h("div", { class:"item", style:"border:0;padding:0" }, [h("span", { class:"grow" }, "Before"), statBefore]),
        h("div", { class:"item", style:"border:0;padding:0" }, [h("span", { class:"grow" }, "After"),  statAfter]),
        h("div", { class:"item", style:"border:0;padding:0" }, [h("span", { class:"grow" }, "Saved"),  statSaved]),
        zipBtn, clearBtn, limitNote
      ])));

      function needsFit(){
        var p = byId(cfg.preset);
        return !!(p && (p.w || cfg.preset === "custom"));
      }
      function byId(id){ return PRESETS.filter(function(p){ return p.id === id; })[0]; }
      function target(){
        var p = byId(cfg.preset);
        if(!p || p.id === "orig") return null;
        if(p.id === "custom") return { w:cfg.cw, h:cfg.ch, exact:true };
        if(p.long) return { long:p.long };
        return { w:p.w, h:p.h, exact:true };
      }

      /* ---------- files ---------- */
      function addFiles(fileList){
        var files = Array.prototype.slice.call(fileList).filter(function(f){ return /^image\//.test(f.type); });
        if(!files.length) return;
        var cap = L.limit("batchFiles");
        var room = cap - items.length;
        if(room <= 0){
          L.unlockSheet("Batches over " + cap + " files");
          return;
        }
        if(files.length > room){
          TS.toast("Free plan takes " + cap + " at a time — added the first " + room + ".");
          files = files.slice(0, room);
        }
        Promise.all(files.map(function(file){
          return TS.decode(file).then(function(bmp){
            items.push({ id:++seq, name:file.name, bytes:file.size, bitmap:bmp, w:bmp.width, h:bmp.height });
          }).catch(function(){});
        })).then(reprocess);
      }

      function reprocess(){
        if(busy){ queued = true; return; }
        if(!items.length){ paint(); return; }
        busy = true;
        Promise.all(items.map(function(it){
          return renderOne(it).then(function(r){
            if(it.url) URL.revokeObjectURL(it.url);
            it.blob = r.blob; it.ow = r.w; it.oh = r.h;
            it.obytes = r.blob ? r.blob.size : 0;
            it.url = r.blob ? URL.createObjectURL(r.blob) : "";
          });
        })).then(function(){
          busy = false;
          paint();
          if(queued){ queued = false; reprocess(); }
        });
      }

      function renderOne(it){
        var t = target();
        var iw = it.w, ih = it.h;
        var sx = 0, sy = 0, sw = iw, sh = ih, dw = iw, dh = ih;
        if(t && t.long){
          var s = Math.min(1, t.long / Math.max(iw, ih));
          dw = Math.max(1, Math.round(iw*s)); dh = Math.max(1, Math.round(ih*s));
        } else if(t){
          if(cfg.fit === "cover"){
            dw = t.w; dh = t.h;
            var sc = Math.max(t.w/iw, t.h/ih);
            sw = Math.min(iw, Math.round(t.w/sc)); sh = Math.min(ih, Math.round(t.h/sc));
            sx = Math.round((iw-sw)/2); sy = Math.round((ih-sh)/2);
          } else {
            var f = Math.min(t.w/iw, t.h/ih);
            dw = Math.max(1, Math.round(iw*f)); dh = Math.max(1, Math.round(ih*f));
          }
        }
        var canvas = TS.drawScaled(it.bitmap, sx, sy, sw, sh, dw, dh,
                                   cfg.type === "image/jpeg" ? "#ffffff" : null);
        return new Promise(function(res){
          canvas.toBlob(function(blob){ res({ blob:blob, w:dw, h:dh }); },
                        cfg.type, cfg.type === "image/png" ? undefined : cfg.quality/100);
        });
      }

      /* ---------- painting ---------- */
      function paint(){
        TS.clear(listBox);
        var before = 0, after = 0, done = 0;
        if(!items.length){
          listBox.appendChild(h("p", { class:"empty" }, "Nothing queued yet. Drop images above."));
        }
        items.forEach(function(it){
          before += it.bytes;
          if(it.obytes){ after += it.obytes; done++; }
          var pct = it.obytes ? Math.round(100 - (it.obytes/it.bytes)*100) : 0;
          var thumb = h("img", { src: it.url || "", alt:"", width:40, height:40,
            style:"width:40px;height:40px;object-fit:cover;border-radius:5px;background:var(--sunk);flex:none" });
          listBox.appendChild(h("div", { class:"item" }, [
            thumb,
            h("div", { class:"grow" }, [
              h("div", { style:"font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" },
                TS.baseName(it.name) + "." + EXT[cfg.type]),
              h("div", { class:"num" }, it.w + "x" + it.h + (it.ow ? "  ->  " + it.ow + "x" + it.oh : ""))
            ]),
            h("span", { class:"num", style: it.obytes && it.obytes > it.bytes ? "color:var(--accent)" : "color:var(--good)" },
              it.obytes ? (it.obytes > it.bytes ? "+" + Math.abs(pct) + "%" : pct + "% smaller") : "…"),
            h("button", { type:"button", title:"Save this one", onClick:function(){
              if(it.blob) TS.saveBlob(it.blob, TS.baseName(it.name) + "." + EXT[cfg.type]);
            }}, "save"),
            h("button", { type:"button", title:"Remove", onClick:function(){
              items = items.filter(function(x){ return x.id !== it.id; });
              paint();
            }}, "x")
          ]));
        });

        statBefore.textContent = TS.fmtBytes(before);
        statAfter.textContent = TS.fmtBytes(after);
        var saved = before - after;
        statSaved.textContent = (saved < 0 ? "+" : "") + TS.fmtBytes(Math.abs(saved)) +
          (before ? "  (" + Math.round(saved/before*100) + "%)" : "");
        statSaved.style.color = saved < 0 ? "var(--accent)" : "var(--good)";
        zipBtn.disabled = !done;
        dz.classList.toggle("slim", items.length > 0);

        TS.clear(limitNote);
        var cap = L.limit("batchFiles");
        if(cap !== Infinity){
          var note = L.lockNote(items.length + " of " + cap + " free slots used.", "Bigger batches");
          if(note) limitNote.appendChild(note);
        }
      }

      function downloadZip(){
        var ready = items.filter(function(it){ return it.blob; });
        if(!ready.length) return;
        zipBtn.disabled = true; zipBtn.textContent = "Zipping…";
        TS.loadScript("vendor/jszip.min.js").then(function(){
          var zip = new JSZip();
          ready.forEach(function(it){ zip.file(TS.baseName(it.name) + "." + EXT[cfg.type], it.blob); });
          return zip.generateAsync({ type:"blob" });
        }).then(function(blob){
          return TS.saveBlob(blob, "toolsmith-batch.zip");
        }).catch(function(){
          TS.toast("Zipping failed — saving them one by one instead.");
          ready.forEach(function(it){ TS.saveBlob(it.blob, TS.baseName(it.name) + "." + EXT[cfg.type]); });
        }).then(function(){
          zipBtn.disabled = false; zipBtn.textContent = "Download all as .zip";
        });
      }

      TS.sampleImage().then(function(s){
        if(!alive || items.length) return;
        items.push({ id:++seq, name:s.name, bytes:s.size, bitmap:s.bitmap, w:s.bitmap.width, h:s.bitmap.height });
        reprocess();
      });

      paint();
      return function(){
        alive = false;
        items.forEach(function(it){ if(it.url) URL.revokeObjectURL(it.url); });
      };
    }
  });
})();
