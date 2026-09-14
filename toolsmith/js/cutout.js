/* ============================================================
   Cutout — click a background, get a transparent PNG.

   Removals are recorded as normalised operations rather than baked
   into pixels, so the same clicks replay exactly at preview size and
   at full export resolution, and undo is just "replay one fewer".

   Honest about its limits: this reads colour, not meaning. Flat or
   evenly-lit backgrounds come out clean; a person's hair against a
   forest will not. The UI says so rather than letting you find out.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h, L = TS.license;
  var WORK_MAX = 1500;

  TS.register({
    id: "cutout",
    name: "Cutout",
    swaps: "Photoshop",
    blurb: "Click the background to delete it. Best on flat or evenly-lit backdrops.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="6" cy="6" r="2.6"/><circle cx="6" cy="18" r="2.6"/><path d="M8.1 7.9 20 19M20 5 8.1 16.1"/></svg>',

    mount: function(stage, insp){
      var src = null, work = null, workData = null, baseData = null;
      var ops = [], fileName = "cutout", alive = true;
      var settings = { tolerance: 28, contiguous: true, feather: 1, backdrop: "checker" };

      var display = h("canvas", { id: "cut-canvas", style: "cursor:crosshair" });
      var wrap = h("div", { class: "canvas-wrap" }, display);
      var dz = TS.dropzone({
        slim: true, multiple: false, title: "Open an image to cut out",
        onFiles: function(f){ openFile(f[0]); }
      });
      var hint = h("p", { class: "note", style: "text-align:center;margin:0" },
        "Click any background area to delete it. Click again on what's left over.");
      stage.appendChild(h("div", { class: "stage-pad" }, [dz, wrap, hint]));

      var tolSlider = TS.slider({
        id:"cut-tol", label:"Tolerance", min:2, max:90, value:settings.tolerance, plus:false,
        onInput:function(v){ settings.tolerance = v; },
        onChange:function(){ if(ops.length) replay(); }
      });
      var featherSlider = TS.slider({
        id:"cut-feather", label:"Soften edge", min:0, max:6, value:settings.feather, unit:" px", plus:false,
        onInput:function(v){ settings.feather = v; },
        onChange:function(){ replay(); }
      });
      var modeSeg = TS.seg({
        label:"Mode", value:"touching",
        options:[{ value:"touching", label:"Touching", title:"Only the connected area you click" },
                 { value:"similar", label:"All similar", title:"Every pixel of that colour in the image" }],
        onChange:function(v){ settings.contiguous = (v === "touching"); if(ops.length) replay(); }
      });
      var backSeg = TS.seg({
        label:"Preview behind", value:"checker",
        options:[{ value:"checker", label:"Checker" }, { value:"#ffffff", label:"White" }, { value:"#111418", label:"Black" }],
        onChange:function(v){
          settings.backdrop = v;
          wrap.classList.toggle("plain", v !== "checker");
          wrap.style.background = v === "checker" ? "" : v;
          render();
        }
      });

      var undoBtn = TS.btn("Undo last click", { kind:"ghost", disabled:true, onClick:function(){
        ops.pop(); replay();
      }});
      var clearBtn = TS.btn("Start over", { kind:"ghost", onClick:function(){ ops = []; replay(); }});
      var countNote = h("p", { class:"note" }, "No cuts yet.");

      var trimBox = h("input", { type:"checkbox", id:"cut-trim", checked:true });

      insp.appendChild(TS.sec("How much to take", TS.stack([modeSeg, tolSlider, featherSlider])));
      insp.appendChild(TS.sec("Cuts", TS.stack([undoBtn, clearBtn, countNote])));
      insp.appendChild(TS.sec("Preview", backSeg));
      insp.appendChild(TS.sec("Export", TS.stack([
        h("label", { class:"item", style:"border:1px solid var(--line);border-radius:8px" },
          [trimBox, h("span", { class:"grow" }, "Trim empty edges")]),
        TS.btn("Save transparent PNG", { onClick: doExport }),
        h("p", { class:"note" }, "PNG keeps transparency. JPG cannot — it would fill the gap with white.")
      ])));

      function openFile(file){
        if(!file || !/^image\//.test(file.type)) return;
        fileName = TS.baseName(file.name || "cutout");
        TS.decode(file).then(function(bmp){
          if(!alive) return;
          src = bmp; ops = []; rebuild();
        }, function(){ TS.toast("Couldn't read that image."); });
      }

      function rebuild(){
        var sw = src.width, sh = src.height;
        var scale = Math.min(1, WORK_MAX / Math.max(sw, sh));
        var w = Math.max(1, Math.round(sw*scale)), hh = Math.max(1, Math.round(sh*scale));
        work = scale < 1 ? TS.drawScaled(src, 0, 0, sw, sh, w, hh) : (function(){
          var c = document.createElement("canvas");
          c.width = sw; c.height = sh;
          c.getContext("2d").drawImage(src, 0, 0);
          return c;
        })();
        baseData = work.getContext("2d").getImageData(0, 0, work.width, work.height);
        replay();
      }

      /* ---------- the cutting ---------- */
      function removeAt(img, px, py, tol, contiguous){
        var w = img.width, hh = img.height, d = img.data;
        var sx = TS.clamp(Math.round(px * w), 0, w-1);
        var sy = TS.clamp(Math.round(py * hh), 0, hh-1);
        var si = (sy*w + sx) * 4;
        var tr = d[si], tg = d[si+1], tb = d[si+2];
        var limit = Math.pow(tol / 100 * 200, 2);

        function close(i){
          var dr = d[i]-tr, dg = d[i+1]-tg, db = d[i+2]-tb;
          return (dr*dr + dg*dg + db*db) <= limit;
        }
        if(!contiguous){
          for(var i = 0; i < d.length; i += 4) if(d[i+3] && close(i)) d[i+3] = 0;
          return;
        }
        var seen = new Uint8Array(w*hh);
        var stack = new Int32Array(w*hh);
        var top = 0;
        stack[top++] = sy*w + sx;
        seen[sy*w + sx] = 1;
        while(top > 0){
          var p = stack[--top];
          var idx = p*4;
          if(!close(idx)) continue;
          d[idx+3] = 0;
          var x = p % w, y = (p / w) | 0;
          if(x > 0   && !seen[p-1]){ seen[p-1] = 1; stack[top++] = p-1; }
          if(x < w-1 && !seen[p+1]){ seen[p+1] = 1; stack[top++] = p+1; }
          if(y > 0   && !seen[p-w]){ seen[p-w] = 1; stack[top++] = p-w; }
          if(y < hh-1&& !seen[p+w]){ seen[p+w] = 1; stack[top++] = p+w; }
        }
      }

      function featherAlpha(img, radius){
        if(radius < 1) return;
        var w = img.width, hh = img.height, d = img.data;
        var a = new Uint8ClampedArray(w*hh);
        for(var i = 0, p = 0; i < d.length; i += 4, p++) a[p] = d[i+3];
        var out = new Uint8ClampedArray(w*hh);
        var r = Math.round(radius);
        for(var y = 0; y < hh; y++){
          for(var x = 0; x < w; x++){
            var sum = 0, n = 0;
            for(var dy = -r; dy <= r; dy++){
              var yy = y+dy; if(yy < 0 || yy >= hh) continue;
              for(var dx = -r; dx <= r; dx++){
                var xx = x+dx; if(xx < 0 || xx >= w) continue;
                sum += a[yy*w+xx]; n++;
              }
            }
            out[y*w+x] = sum/n;
          }
        }
        for(var i2 = 0, p2 = 0; i2 < d.length; i2 += 4, p2++) d[i2+3] = out[p2];
      }

      function applyOps(img, opList, feather){
        opList.forEach(function(o){ removeAt(img, o.x, o.y, o.tol, o.contiguous); });
        featherAlpha(img, feather);
      }

      function replay(){
        if(!baseData) return;
        workData = new ImageData(new Uint8ClampedArray(baseData.data), baseData.width, baseData.height);
        applyOps(workData, ops, settings.feather);
        undoBtn.disabled = !ops.length;
        countNote.textContent = ops.length
          ? ops.length + " cut" + (ops.length === 1 ? "" : "s") + " applied."
          : "No cuts yet.";
        render();
      }

      function render(){
        if(!workData) return;
        display.width = workData.width;
        display.height = workData.height;
        display.getContext("2d").putImageData(workData, 0, 0);
      }

      display.addEventListener("click", function(ev){
        if(!workData) return;
        var r = display.getBoundingClientRect();
        ops.push({
          x: (ev.clientX - r.left) / r.width,
          y: (ev.clientY - r.top) / r.height,
          tol: settings.tolerance,
          contiguous: settings.contiguous
        });
        replay();
      });

      /* ---------- export ---------- */
      function doExport(){
        if(!src) return;
        var sw = src.width, sh = src.height;
        var cap = L.limit("exportPx");
        var scale = Math.min(1, cap / Math.max(sw, sh));
        var ow = Math.max(1, Math.round(sw*scale)), oh = Math.max(1, Math.round(sh*scale));
        if(scale < 1) TS.toast("Free export caps at " + cap + " px — rendering at " + ow + " x " + oh + ".");

        var c = document.createElement("canvas");
        c.width = ow; c.height = oh;
        var ctx = c.getContext("2d");
        ctx.drawImage(src, 0, 0, ow, oh);
        var img = ctx.getImageData(0, 0, ow, oh);
        applyOps(img, ops, settings.feather * scale);
        ctx.putImageData(img, 0, 0);

        var finalCanvas = trimBox.checked ? trim(c) : c;
        TS.saveCanvas(finalCanvas, fileName + "-cutout.png", "image/png");
      }

      function trim(canvas){
        var w = canvas.width, hh = canvas.height;
        var d = canvas.getContext("2d").getImageData(0, 0, w, hh).data;
        var minX = w, minY = hh, maxX = -1, maxY = -1;
        for(var y = 0; y < hh; y++){
          for(var x = 0; x < w; x++){
            if(d[(y*w+x)*4+3] > 8){
              if(x < minX) minX = x;
              if(x > maxX) maxX = x;
              if(y < minY) minY = y;
              if(y > maxY) maxY = y;
            }
          }
        }
        if(maxX < 0) return canvas;
        var nw = maxX-minX+1, nh = maxY-minY+1;
        if(nw === w && nh === hh) return canvas;
        var out = document.createElement("canvas");
        out.width = nw; out.height = nh;
        out.getContext("2d").drawImage(canvas, minX, minY, nw, nh, 0, 0, nw, nh);
        return out;
      }

      TS.sampleImage().then(function(s){
        if(!alive || src) return;
        src = s.bitmap; fileName = "sample-valley";
        rebuild();
      });

      return function(){ alive = false; };
    }
  });
})();
