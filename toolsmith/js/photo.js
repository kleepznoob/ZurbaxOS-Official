/* ============================================================
   Photo — the Lightroom jobs people actually do.
   Adjustments run as one pass over the pixels: point operations
   and vignette together, then an unsharp mask if asked for.
   Preview is capped so dragging a slider stays instant; export
   runs the identical pipeline at full resolution.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h, L = TS.license;
  var PREVIEW_MAX = 1400;

  var DEFAULTS = {
    exposure:0, contrast:0, highlights:0, shadows:0,
    saturation:0, vibrance:0, temperature:0, tint:0,
    sharpen:0, vignette:0
  };

  var LOOKS = {
    "As shot":   {},
    "Punch":     { contrast:22, vibrance:30, shadows:12, sharpen:25 },
    "Golden":    { temperature:24, exposure:6, highlights:-14, vibrance:16, vignette:14 },
    "Cold steel":{ temperature:-22, saturation:-14, contrast:14, shadows:8 },
    "Faded":     { contrast:-16, shadows:26, saturation:-20, exposure:5 },
    "Mono":      { saturation:-100, contrast:20, sharpen:18 }
  };

  TS.register({
    id: "photo",
    name: "Photo",
    swaps: "Lightroom",
    blurb: "Fix exposure and colour, crop it, save it. No catalogue, no import step.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="5" width="18" height="14" rx="2"/><circle cx="12" cy="12" r="3.4"/><path d="M7.5 5 9 3h6l1.5 2"/></svg>',

    mount: function(stage, insp){
      var src = null, base = null, preview = null, previewData = null;
      var adj = Object.assign({}, DEFAULTS);
      var orient = { rot:0, flipH:false, flipV:false };
      var crop = null, cropping = false, drag = null, showOriginal = false;
      var fileName = "photo";
      var alive = true;

      /* ---------- stage ---------- */
      var display = h("canvas", { id: "photo-canvas" });
      var wrap = h("div", { class: "canvas-wrap plain" }, display);
      var dz = TS.dropzone({
        slim: true, multiple: false, title: "Open a different photo",
        onFiles: function(files){ openFile(files[0]); }
      });
      stage.appendChild(h("div", { class: "stage-pad" }, [dz, wrap]));

      /* ---------- inspector ---------- */
      var sliders = {};
      function sl(key, label, o){
        o = o || {};
        var s = TS.slider({
          id: "ph-" + key, label: label, min: o.min != null ? o.min : -100, max: o.max != null ? o.max : 100,
          value: adj[key], unit: o.unit || "", plus: o.plus !== false,
          onInput: function(v){ adj[key] = v; render(); }
        });
        sliders[key] = s;
        return s;
      }

      var lookSeg = TS.seg({
        label: "Look", value: "As shot",
        options: Object.keys(LOOKS).slice(0, 3).map(function(k){ return { value:k, label:k }; }),
        onChange: applyLook
      });
      var lookSeg2 = TS.seg({
        label: "Look", value: "",
        options: Object.keys(LOOKS).slice(3).map(function(k){ return { value:k, label:k }; }),
        onChange: applyLook
      });
      var lookSegs = [lookSeg, lookSeg2];
      function applyLook(name){
        adj = Object.assign({}, DEFAULTS, LOOKS[name] || {});
        Object.keys(sliders).forEach(function(k){ sliders[k].set(adj[k]); });
        lookSegs.forEach(function(sg){ sg.set(name); });
        render();
      }

      var histo = TS.fromHTML('<svg class="hist" viewBox="0 0 240 56" preserveAspectRatio="none" ' +
                              'role="img" aria-label="Brightness histogram"></svg>');

      var cropSeg = TS.seg({
        label: "Crop ratio", value: "free",
        options: [
          { value:"free", label:"Free" }, { value:"1", label:"1:1" },
          { value:"1.7778", label:"16:9" }, { value:"0.8", label:"4:5" }, { value:"1.5", label:"3:2" }
        ],
        onChange: function(v){ if(cropping) setAspect(v); }
      });
      var cropBtn = TS.btn("Crop", { kind: "ghost", onClick: toggleCrop });
      var cropApply = TS.btn("Apply crop", { onClick: applyCrop });
      var cropCancel = TS.btn("Cancel", { kind: "ghost", onClick: function(){ cropping = false; crop = null; syncCropUI(); render(); } });

      var fmtSeg = TS.seg({
        label: "Format", value: "image/jpeg",
        options: [{ value:"image/jpeg", label:"JPG" }, { value:"image/png", label:"PNG" }, { value:"image/webp", label:"WebP" }],
        onChange: function(v){ exportState.type = v; qWrap.hidden = (v === "image/png"); }
      });
      var exportState = { type:"image/jpeg", quality:88, long:0 };
      var qSlider = TS.slider({
        id:"ph-q", label:"Quality", min:40, max:100, value:88, plus:false,
        onInput:function(v){ exportState.quality = v; }
      });
      var qWrap = h("div", null, qSlider);
      var sizeInput = TS.num({ label:"Longest edge", min:64, max:12000, value:0,
        onInput:function(v){ exportState.long = v; } });

      insp.appendChild(TS.sec("Look", TS.stack([lookSeg, lookSeg2])));
      insp.appendChild(TS.sec("Light", TS.stack([
        sl("exposure","Exposure"), sl("contrast","Contrast"),
        sl("highlights","Highlights"), sl("shadows","Shadows")
      ]), function(){ ["exposure","contrast","highlights","shadows"].forEach(reset); }));
      insp.appendChild(TS.sec("Colour", TS.stack([
        sl("temperature","Temperature"), sl("tint","Tint"),
        sl("saturation","Saturation"), sl("vibrance","Vibrance")
      ]), function(){ ["temperature","tint","saturation","vibrance"].forEach(reset); }));
      insp.appendChild(TS.sec("Detail", TS.stack([
        sl("sharpen","Sharpen", { min:0, max:100 }),
        sl("vignette","Vignette", { min:-60, max:60 })
      ]), function(){ ["sharpen","vignette"].forEach(reset); }));
      insp.appendChild(TS.sec("Levels", histo));
      insp.appendChild(TS.sec("Frame", TS.stack([
        TS.row2([
          TS.btn("Rotate 90°", { kind:"ghost", sm:true, onClick:function(){ orient.rot = (orient.rot + 90) % 360; rebuild(); } }),
          TS.btn("Flip", { kind:"ghost", sm:true, onClick:function(){ orient.flipH = !orient.flipH; rebuild(); } })
        ]),
        cropSeg, cropBtn, cropApply, cropCancel
      ])));
      insp.appendChild(TS.sec("Export", TS.stack([
        fmtSeg, qWrap,
        TS.field("Longest edge (0 = original)", sizeInput),
        TS.btn("Save photo", { onClick: doExport }),
        h("p", { class:"note", id:"ph-dims" }, "")
      ])));

      function reset(k){ adj[k] = DEFAULTS[k]; sliders[k].set(adj[k]); render(); }

      /* ---------- loading ---------- */
      function openFile(file){
        if(!file || !/^image\//.test(file.type)) return;
        fileName = TS.baseName(file.name || "photo");
        TS.decode(file).then(function(bmp){
          if(!alive) return;
          src = bmp; orient = { rot:0, flipH:false, flipV:false };
          crop = null; cropping = false; syncCropUI();
          rebuild();
        }, function(){ TS.toast("Couldn't read that image."); });
      }

      // base = source with rotation/flip/crop baked in, at full resolution
      function rebuild(){
        if(!src) return;
        var sw = src.width, sh = src.height;
        var swap = orient.rot === 90 || orient.rot === 270;
        var bw = swap ? sh : sw, bh = swap ? sw : sh;
        var c = document.createElement("canvas");
        c.width = bw; c.height = bh;
        var x = c.getContext("2d");
        x.save();
        x.translate(bw/2, bh/2);
        x.rotate(orient.rot * Math.PI / 180);
        x.scale(orient.flipH ? -1 : 1, orient.flipV ? -1 : 1);
        x.drawImage(src, -sw/2, -sh/2);
        x.restore();
        base = c;

        var scale = Math.min(1, PREVIEW_MAX / Math.max(bw, bh));
        preview = scale < 1 ? TS.drawScaled(base, 0, 0, bw, bh, Math.round(bw*scale), Math.round(bh*scale)) : base;
        previewData = preview.getContext("2d").getImageData(0, 0, preview.width, preview.height);
        TS.$("ph-dims").textContent = bw + " x " + bh + " px original";
        TS.docName(fileName + ".jpg");
        TS.status({ doc: bw + " x " + bh + " px", hint: "Drag a slider, or pick a look. Hold the Compare button to see the original." });
        render();
      }

      /* ---------- the pixel pipeline ---------- */
      function process(srcData, a){
        var w = srcData.width, h2 = srcData.height;
        var out = new ImageData(w, h2);
        var s = srcData.data, d = out.data;
        var expo = Math.pow(2, a.exposure / 50);
        var con  = a.contrast / 100 * 0.9 + 1;
        var temp = a.temperature / 100 * 0.32;
        var tnt  = a.tint / 100 * 0.22;
        var sat  = a.saturation / 100 + 1;
        var vib  = a.vibrance / 100;
        var hi   = a.highlights / 100;
        var sh   = a.shadows / 100;
        var vig  = a.vignette / 100;
        var cx = w / 2, cy = h2 / 2;
        var maxR = Math.sqrt(cx*cx + cy*cy);

        for(var y = 0, i = 0; y < h2; y++){
          for(var x2 = 0; x2 < w; x2++, i += 4){
            var r = s[i] / 255, g = s[i+1] / 255, b = s[i+2] / 255;

            r *= expo; g *= expo; b *= expo;
            if(temp){ r *= 1 + temp; b *= 1 - temp; }
            if(tnt){ g *= 1 - tnt; r *= 1 + tnt * 0.5; b *= 1 + tnt * 0.5; }

            var lum = 0.2126*r + 0.7152*g + 0.0722*b;
            if(sh){ var lift = sh * (1 - lum) * (1 - lum) * 0.55; r += lift; g += lift; b += lift; }
            if(hi){ var cut = hi * lum * lum * 0.55; r += cut; g += cut; b += cut; }

            if(con !== 1){ r = (r-0.5)*con+0.5; g = (g-0.5)*con+0.5; b = (b-0.5)*con+0.5; }

            lum = 0.2126*r + 0.7152*g + 0.0722*b;
            if(sat !== 1){ r = lum + (r-lum)*sat; g = lum + (g-lum)*sat; b = lum + (b-lum)*sat; }
            if(vib){
              var mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
              var mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
              var amt = 1 + vib * (1 - (mx - mn));
              r = lum + (r-lum)*amt; g = lum + (g-lum)*amt; b = lum + (b-lum)*amt;
            }
            if(vig){
              var dx = x2-cx, dy = y-cy;
              var f = 1 - vig * Math.pow(Math.sqrt(dx*dx+dy*dy)/maxR, 2.1);
              r *= f; g *= f; b *= f;
            }

            d[i]   = r <= 0 ? 0 : r >= 1 ? 255 : r*255;
            d[i+1] = g <= 0 ? 0 : g >= 1 ? 255 : g*255;
            d[i+2] = b <= 0 ? 0 : b >= 1 ? 255 : b*255;
            d[i+3] = s[i+3];
          }
        }
        if(a.sharpen > 0) unsharp(out, a.sharpen / 100);
        return out;
      }

      // Unsharp mask: blur a copy, then push the original away from it.
      function unsharp(img, amount){
        var w = img.width, h2 = img.height, d = img.data;
        var blur = new Uint8ClampedArray(d);
        var k = 1;
        for(var y = 0; y < h2; y++){
          for(var x = 0; x < w; x++){
            for(var c = 0; c < 3; c++){
              var sum = 0, n = 0;
              for(var dy = -k; dy <= k; dy++){
                var yy = y+dy; if(yy < 0 || yy >= h2) continue;
                for(var dx = -k; dx <= k; dx++){
                  var xx = x+dx; if(xx < 0 || xx >= w) continue;
                  sum += d[(yy*w+xx)*4+c]; n++;
                }
              }
              blur[(y*w+x)*4+c] = sum / n;
            }
          }
        }
        for(var i = 0; i < d.length; i += 4){
          for(var c2 = 0; c2 < 3; c2++){
            var v = d[i+c2] + amount * 1.6 * (d[i+c2] - blur[i+c2]);
            d[i+c2] = v < 0 ? 0 : v > 255 ? 255 : v;
          }
        }
      }

      /* ---------- render ---------- */
      var render = TS.rafThrottle(function(){
        if(!preview || !alive) return;
        var result = showOriginal ? previewData : process(previewData, adj);
        display.width = preview.width;
        display.height = preview.height;
        var ctx = display.getContext("2d");
        ctx.putImageData(result, 0, 0);
        drawHistogram(result);
        if(cropping) drawCropOverlay(ctx);
        fitDisplay();
      });

      function fitDisplay(){
        display.style.maxWidth = "100%";
        display.style.maxHeight = "100%";
      }

      function drawHistogram(img){
        var bins = new Float32Array(64), d = img.data;
        for(var i = 0; i < d.length; i += 16){    // every 4th pixel is plenty
          var l = (0.2126*d[i] + 0.7152*d[i+1] + 0.0722*d[i+2]) / 255;
          bins[Math.min(63, (l*64)|0)]++;
        }
        var peak = 0;
        for(var b = 0; b < 64; b++) if(bins[b] > peak) peak = bins[b];
        if(!peak) peak = 1;
        var pts = ["M0,56"];
        for(var j = 0; j < 64; j++){
          pts.push("L" + (j*240/63).toFixed(1) + "," + (56 - Math.sqrt(bins[j]/peak)*52).toFixed(1));
        }
        pts.push("L240,56 Z");
        histo.innerHTML = '<path class="fill" d="' + pts.join(" ") + '"/>';
      }

      /* ---------- crop ---------- */
      function toggleCrop(){
        if(!preview) return;
        cropping = true;
        crop = { x:0.08, y:0.08, w:0.84, h:0.84 };
        setAspect(cropSeg.querySelector('[aria-pressed="true"]').dataset.v);
        syncCropUI();
        render();
      }
      function syncCropUI(){
        cropBtn.hidden = cropping;
        cropApply.hidden = !cropping;
        cropCancel.hidden = !cropping;
      }
      syncCropUI();

      function setAspect(v){
        if(!crop || v === "free") return;
        var ar = Number(v);
        var cw = crop.w * preview.width, ch = cw / ar;
        if(ch / preview.height > 0.96){ ch = preview.height * 0.96; cw = ch * ar; }
        crop.w = cw / preview.width; crop.h = ch / preview.height;
        crop.x = Math.min(crop.x, 1 - crop.w); crop.y = Math.min(crop.y, 1 - crop.h);
        render();
      }

      function drawCropOverlay(ctx){
        var W = display.width, H = display.height;
        var x = crop.x*W, y = crop.y*H, w = crop.w*W, hh = crop.h*H;
        ctx.save();
        ctx.fillStyle = "rgba(0,0,0,.55)";
        ctx.fillRect(0,0,W,y); ctx.fillRect(0,y+hh,W,H-y-hh);
        ctx.fillRect(0,y,x,hh); ctx.fillRect(x+w,y,W-x-w,hh);
        ctx.strokeStyle = "#fff"; ctx.lineWidth = 1.5;
        ctx.strokeRect(x+0.5, y+0.5, w, hh);
        ctx.strokeStyle = "rgba(255,255,255,.4)"; ctx.lineWidth = 1;
        for(var i = 1; i < 3; i++){
          ctx.beginPath(); ctx.moveTo(x+w*i/3, y); ctx.lineTo(x+w*i/3, y+hh); ctx.stroke();
          ctx.beginPath(); ctx.moveTo(x, y+hh*i/3); ctx.lineTo(x+w, y+hh*i/3); ctx.stroke();
        }
        ctx.fillStyle = "#fff";
        var s = 9;
        [[x,y],[x+w,y],[x,y+hh],[x+w,y+hh]].forEach(function(p){
          ctx.fillRect(p[0]-s/2, p[1]-s/2, s, s);
        });
        ctx.restore();
      }

      function canvasPoint(ev){
        var r = display.getBoundingClientRect();
        return { x: (ev.clientX - r.left) / r.width, y: (ev.clientY - r.top) / r.height };
      }
      display.addEventListener("pointerdown", function(ev){
        if(!cropping) return;
        var p = canvasPoint(ev);
        var near = 0.035;
        var corners = { nw:[crop.x,crop.y], ne:[crop.x+crop.w,crop.y],
                        sw:[crop.x,crop.y+crop.h], se:[crop.x+crop.w,crop.y+crop.h] };
        drag = null;
        for(var k in corners){
          if(Math.abs(p.x-corners[k][0]) < near && Math.abs(p.y-corners[k][1]) < near){
            drag = { mode:k }; break;
          }
        }
        if(!drag && p.x > crop.x && p.x < crop.x+crop.w && p.y > crop.y && p.y < crop.y+crop.h){
          drag = { mode:"move", ox: p.x-crop.x, oy: p.y-crop.y };
        }
        if(drag){ display.setPointerCapture(ev.pointerId); ev.preventDefault(); }
      });
      display.addEventListener("pointermove", function(ev){
        if(!drag || !cropping) return;
        var p = canvasPoint(ev);
        if(drag.mode === "move"){
          crop.x = TS.clamp(p.x - drag.ox, 0, 1 - crop.w);
          crop.y = TS.clamp(p.y - drag.oy, 0, 1 - crop.h);
        } else {
          var x1 = crop.x, y1 = crop.y, x2 = crop.x+crop.w, y2 = crop.y+crop.h;
          if(drag.mode.indexOf("w") >= 0) x1 = TS.clamp(p.x, 0, x2-0.05);
          if(drag.mode.indexOf("e") >= 0) x2 = TS.clamp(p.x, x1+0.05, 1);
          if(drag.mode.indexOf("n") >= 0) y1 = TS.clamp(p.y, 0, y2-0.05);
          if(drag.mode.indexOf("s") >= 0) y2 = TS.clamp(p.y, y1+0.05, 1);
          crop = { x:x1, y:y1, w:x2-x1, h:y2-y1 };
          var v = cropSeg.querySelector('[aria-pressed="true"]').dataset.v;
          if(v !== "free"){
            var ar = Number(v);
            var wpx = crop.w*preview.width, hpx = wpx/ar;
            crop.h = TS.clamp(hpx/preview.height, 0.05, 1 - crop.y);
          }
        }
        render();
      });
      display.addEventListener("pointerup", function(){ drag = null; });

      function applyCrop(){
        if(!crop || !base) return;
        var bw = base.width, bh = base.height;
        var sx = Math.round(crop.x*bw), sy = Math.round(crop.y*bh);
        var sw = Math.max(8, Math.round(crop.w*bw)), sh = Math.max(8, Math.round(crop.h*bh));
        var c = document.createElement("canvas");
        c.width = sw; c.height = sh;
        c.getContext("2d").drawImage(base, sx, sy, sw, sh, 0, 0, sw, sh);
        base = c;
        var scale = Math.min(1, PREVIEW_MAX / Math.max(sw, sh));
        preview = scale < 1 ? TS.drawScaled(base, 0, 0, sw, sh, Math.round(sw*scale), Math.round(sh*scale)) : base;
        previewData = preview.getContext("2d").getImageData(0, 0, preview.width, preview.height);
        cropping = false; crop = null; syncCropUI();
        TS.$("ph-dims").textContent = sw + " x " + sh + " px after crop";
        render();
        TS.toast("Cropped to " + sw + " x " + sh + ".");
      }

      /* ---------- export ---------- */
      function doExport(){
        if(!base) return;
        var bw = base.width, bh = base.height;
        var want = exportState.long > 0 ? exportState.long : Math.max(bw, bh);
        var cap = L.limit("exportPx");
        if(want > cap){
          if(!L.allow("exportPx", want, "Exporting above " + cap + " px")) return;
        }
        var scale = Math.min(1, want / Math.max(bw, bh));
        var ow = Math.max(1, Math.round(bw*scale)), oh = Math.max(1, Math.round(bh*scale));
        var work = scale < 1 ? TS.drawScaled(base, 0, 0, bw, bh, ow, oh) : base;
        var ctx = work.getContext("2d");
        var data = ctx.getImageData(0, 0, ow, oh);
        TS.toast("Rendering at " + ow + " x " + oh + "…");
        setTimeout(function(){
          var res = process(data, adj);
          var out = document.createElement("canvas");
          out.width = ow; out.height = oh;
          var octx = out.getContext("2d");
          if(exportState.type === "image/jpeg"){ octx.fillStyle = "#fff"; octx.fillRect(0,0,ow,oh); }
          octx.putImageData(res, 0, 0);
          var ext = exportState.type === "image/png" ? "png" : exportState.type === "image/webp" ? "webp" : "jpg";
          TS.saveCanvas(out, fileName + "-edited." + ext, exportState.type,
                        exportState.type === "image/png" ? undefined : exportState.quality/100);
        }, 30);
      }

      /* ---------- menus + options bar ---------- */
      TS.provide({
        open:   function(){ dz.openPicker(); },
        export: doExport,
        reset:  function(){ applyLook("As shot"); TS.toast("Adjustments reset."); },
        rotate: function(){ orient.rot = (orient.rot + 90) % 360; rebuild(); },
        flip:   function(){ orient.flipH = !orient.flipH; rebuild(); },
        crop:   function(){ if(!cropping) toggleCrop(); }
      });

      var compareBtn = TS.btn("Compare", { kind:"ghost" });
      compareBtn.title = "Hold to see the original";
      ["pointerdown","pointerenter"].forEach(function(ev){
        compareBtn.addEventListener(ev, function(e){
          if(ev === "pointerenter" && e.buttons !== 1) return;
          showOriginal = true; render();
        });
      });
      ["pointerup","pointerleave","pointercancel"].forEach(function(ev){
        compareBtn.addEventListener(ev, function(){ showOriginal = false; render(); });
      });

      var lookSegBar = TS.seg({
        label:"Look", value:"As shot",
        options: Object.keys(LOOKS).map(function(k){ return { value:k, label:k }; }),
        onChange: applyLook
      });
      lookSegs.push(lookSegBar);

      TS.options([
        TS.optLabel("Look"),
        lookSegBar,
        TS.optDivider(),
        compareBtn,
        TS.btn("Rotate", { kind:"ghost", onClick:function(){ orient.rot = (orient.rot + 90) % 360; rebuild(); }}),
        TS.btn("Crop", { kind:"ghost", onClick:function(){ if(!cropping) toggleCrop(); else applyCrop(); }}),
        TS.optDivider(),
        TS.btn("Export", { onClick: doExport })
      ]);

      /* ---------- go ---------- */
      TS.sampleImage().then(function(s){
        if(!alive || src) return;
        src = s.bitmap; fileName = "sample-valley";
        rebuild();
      });

      return function(){ alive = false; };
    }
  });
})();
