/* ============================================================
   Colour — pull a palette out of a photo, build harmonies from it,
   and check the pairs are actually readable before you ship them.
   Extraction is median cut: repeatedly split the colour box with the
   widest spread along its longest axis. It beats "most frequent
   pixel" because it keeps the accents instead of ten shades of sky.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h;

  /* ---------------- colour maths ---------------- */
  function hex(r,g,b){
    return "#" + [r,g,b].map(function(v){
      return ("0" + Math.round(TS.clamp(v,0,255)).toString(16)).slice(-2);
    }).join("").toUpperCase();
  }
  function parseHex(s){
    s = String(s).replace("#","").trim();
    if(s.length === 3) s = s[0]+s[0]+s[1]+s[1]+s[2]+s[2];
    if(!/^[0-9a-f]{6}$/i.test(s)) return null;
    return [parseInt(s.slice(0,2),16), parseInt(s.slice(2,4),16), parseInt(s.slice(4,6),16)];
  }
  function rgbToHsl(r,g,b){
    r/=255; g/=255; b/=255;
    var mx = Math.max(r,g,b), mn = Math.min(r,g,b), d = mx-mn;
    var l = (mx+mn)/2, s = 0, hh = 0;
    if(d){
      s = l > 0.5 ? d/(2-mx-mn) : d/(mx+mn);
      if(mx === r) hh = ((g-b)/d + (g < b ? 6 : 0));
      else if(mx === g) hh = (b-r)/d + 2;
      else hh = (r-g)/d + 4;
      hh *= 60;
    }
    return [hh, s*100, l*100];
  }
  function hslToRgb(hh,s,l){
    hh = ((hh % 360) + 360) % 360; s/=100; l/=100;
    var c = (1-Math.abs(2*l-1))*s, x = c*(1-Math.abs((hh/60)%2-1)), m = l-c/2;
    var t = hh < 60 ? [c,x,0] : hh < 120 ? [x,c,0] : hh < 180 ? [0,c,x]
          : hh < 240 ? [0,x,c] : hh < 300 ? [x,0,c] : [c,0,x];
    return [(t[0]+m)*255, (t[1]+m)*255, (t[2]+m)*255];
  }
  function relLum(rgb){
    var a = rgb.map(function(v){
      v /= 255;
      return v <= 0.03928 ? v/12.92 : Math.pow((v+0.055)/1.055, 2.4);
    });
    return 0.2126*a[0] + 0.7152*a[1] + 0.0722*a[2];
  }
  function contrast(a, b){
    var l1 = relLum(a), l2 = relLum(b);
    return (Math.max(l1,l2) + 0.05) / (Math.min(l1,l2) + 0.05);
  }

  /* ---------------- median cut ---------------- */
  function extract(bitmap, count){
    var S = 160;
    var scale = Math.min(1, S / Math.max(bitmap.width, bitmap.height));
    var w = Math.max(1, Math.round(bitmap.width*scale)), hh = Math.max(1, Math.round(bitmap.height*scale));
    var c = document.createElement("canvas");
    c.width = w; c.height = hh;
    c.getContext("2d").drawImage(bitmap, 0, 0, w, hh);
    var d = c.getContext("2d").getImageData(0,0,w,hh).data;

    var px = [];
    for(var i = 0; i < d.length; i += 4){
      if(d[i+3] < 128) continue;
      px.push([d[i], d[i+1], d[i+2]]);
    }
    if(!px.length) return [];

    // Over-split first: a big smooth region like sky has a wide range, so plain
    // median cut spends every box on it and hands back six shades of the same
    // blue. Cut finer than needed, then pick a spread-out subset below.
    var boxes = [px];
    var fine = Math.min(count * 5, 60);
    while(boxes.length < fine){
      var bi = -1, best = -1;
      boxes.forEach(function(box, idx){
        if(box.length < 2) return;
        var r = ranges(box), spread = Math.max(r[0], r[1], r[2]);
        if(spread > best){ best = spread; bi = idx; }
      });
      if(bi < 0) break;
      var box = boxes[bi];
      var r2 = ranges(box);
      var ch = r2.indexOf(Math.max(r2[0], r2[1], r2[2]));
      box.sort(function(a,b){ return a[ch] - b[ch]; });
      var mid = box.length >> 1;
      boxes.splice(bi, 1, box.slice(0, mid), box.slice(mid));
    }

    var cand = boxes.filter(function(b){ return b.length; }).map(function(box){
      var s = [0,0,0];
      box.forEach(function(p){ s[0]+=p[0]; s[1]+=p[1]; s[2]+=p[2]; });
      return { rgb:[s[0]/box.length, s[1]/box.length, s[2]/box.length], n:box.length };
    });
    if(!cand.length) return [];

    // Farthest-point selection: start from the colour covering the most of the
    // picture, then each next pick is the one furthest from everything chosen,
    // nudged towards colours that actually cover some area.
    cand.sort(function(a,b){ return b.n - a.n; });
    var picked = [cand.shift()];
    while(picked.length < count && cand.length){
      var bestI = 0, bestScore = -1;
      cand.forEach(function(c, i){
        var nearest = Infinity;
        picked.forEach(function(p){
          var dr = c.rgb[0]-p.rgb[0], dg = c.rgb[1]-p.rgb[1], db = c.rgb[2]-p.rgb[2];
          var d = dr*dr + dg*dg + db*db;
          if(d < nearest) nearest = d;
        });
        var score = nearest * Math.pow(c.n, 0.22);
        if(score > bestScore){ bestScore = score; bestI = i; }
      });
      picked.push(cand.splice(bestI, 1)[0]);
    }
    return picked.map(function(p){ return p.rgb; })
                 .sort(function(a,b){ return relLum(b) - relLum(a); });
  }
  function ranges(box){
    var lo = [255,255,255], hi = [0,0,0];
    box.forEach(function(p){
      for(var i = 0; i < 3; i++){
        if(p[i] < lo[i]) lo[i] = p[i];
        if(p[i] > hi[i]) hi[i] = p[i];
      }
    });
    return [hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]];
  }

  var HARMONIES = {
    complementary: [0, 180],
    analogous:     [-30, 0, 30],
    triad:         [0, 120, 240],
    split:         [0, 150, 210],
    square:        [0, 90, 180, 270]
  };

  TS.register({
    id: "color",
    name: "Colour",
    swaps: "Adobe Color",
    blurb: "Lift a palette out of any photo, build harmonies, and check the pairs are readable.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><circle cx="9" cy="9" r="1.4" fill="currentColor" stroke="none"/><circle cx="15.5" cy="10" r="1.4" fill="currentColor" stroke="none"/><circle cx="12" cy="15.5" r="1.4" fill="currentColor" stroke="none"/></svg>',

    mount: function(stage, insp){
      var bitmap = null, palette = [], count = 6, alive = true;
      var pickA = "#FFFFFF", pickB = "#111418";

      var preview = h("canvas", { style:"max-height:280px" });
      var strip = h("div", { class:"grid-sw", style:"margin-top:2px" });
      var harmonyBox = h("div", { class:"grid-sw" });
      var dz = TS.dropzone({
        slim:true, multiple:false, title:"Pull a palette from another image",
        onFiles:function(f){ open(f[0]); }
      });
      stage.appendChild(h("div", { class:"stage-pad" }, [
        dz,
        h("div", { class:"canvas-wrap plain", style:"flex:none;padding:12px" }, preview),
        h("h2", { style:"font:700 11px var(--body);letter-spacing:.11em;text-transform:uppercase;color:var(--muted);margin:4px 0 0" },
          "Palette — click a swatch to copy"),
        strip
      ]));

      /* ---------- inspector ---------- */
      var countSlider = TS.slider({
        id:"col-n", label:"Colours", min:3, max:10, value:count, plus:false,
        onInput:function(v){ count = v; },
        onChange:function(){ recompute(); }
      });

      var baseInput = h("input", { class:"inp mono", value:"#EFA940", "aria-label":"Base colour",
        onInput:function(){ buildHarmony(); } });
      var harmonySeg = TS.seg({
        label:"Harmony", value:"analogous",
        options:[{ value:"complementary", label:"Comp" }, { value:"analogous", label:"Analog" },
                 { value:"triad", label:"Triad" }, { value:"split", label:"Split" }],
        onChange:function(){ buildHarmony(); }
      });

      var aIn = h("input", { class:"inp mono", value:pickA, "aria-label":"Foreground colour", onInput:function(){ pickA = aIn.value; checkContrast(); } });
      var bIn = h("input", { class:"inp mono", value:pickB, "aria-label":"Background colour", onInput:function(){ pickB = bIn.value; checkContrast(); } });
      var ratioOut = h("div", { style:"font:600 22px var(--mono);font-variant-numeric:tabular-nums" }, "—");
      var verdict = h("div", { class:"note" }, "");
      var sampleText = h("div", {
        style:"border-radius:8px;padding:12px;text-align:center;font-weight:700;border:1px solid var(--line)"
      }, "Sample text");

      insp.appendChild(TS.sec("From the image", TS.stack([countSlider,
        TS.btn("Copy all as CSS variables", { kind:"ghost", sm:true, onClick:copyCss })
      ])));
      insp.appendChild(TS.sec("Harmony", TS.stack([
        TS.field("Base colour", baseInput), harmonySeg, harmonyBox
      ])));
      insp.appendChild(TS.sec("Readable?", TS.stack([
        TS.row2([TS.field("Text", aIn), TS.field("Behind", bIn)]),
        sampleText, ratioOut, verdict
      ])));
      insp.appendChild(TS.sec("Export", TS.stack([
        TS.btn("Save palette.css", { kind:"ghost", onClick:function(){
          TS.saveText(cssText(), "palette.css", "text/css");
        }}),
        TS.btn("Save palette.json", { kind:"ghost", onClick:function(){
          TS.saveText(JSON.stringify({ colors: palette.map(function(c){ return hex(c[0],c[1],c[2]); }) }, null, 2),
                      "palette.json", "application/json");
        }}),
        TS.btn("Save swatches.svg", { kind:"ghost", onClick:saveSvg })
      ])));

      /* ---------- behaviour ---------- */
      function open(file){
        if(!file || !/^image\//.test(file.type)) return;
        TS.decode(file).then(function(bmp){
          if(!alive) return;
          bitmap = bmp; drawPreview(); recompute();
        }, function(){ TS.toast("Couldn't read that image."); });
      }
      function drawPreview(){
        var maxW = 560;
        var s = Math.min(1, maxW / bitmap.width);
        preview.width = Math.round(bitmap.width*s);
        preview.height = Math.round(bitmap.height*s);
        preview.getContext("2d").drawImage(bitmap, 0, 0, preview.width, preview.height);
      }
      function recompute(){
        if(!bitmap) return;
        palette = extract(bitmap, count);
        TS.clear(strip);
        palette.forEach(function(c){
          strip.appendChild(swatch(hex(c[0],c[1],c[2])));
        });
        if(palette.length){
          baseInput.value = hex(palette[Math.floor(palette.length/2)][0],
                                palette[Math.floor(palette.length/2)][1],
                                palette[Math.floor(palette.length/2)][2]);
          pickA = hex(palette[0][0], palette[0][1], palette[0][2]);
          pickB = hex(palette[palette.length-1][0], palette[palette.length-1][1], palette[palette.length-1][2]);
          aIn.value = pickA; bIn.value = pickB;
          buildHarmony(); checkContrast();
        }
      }
      function swatch(hx){
        var rgb = parseHex(hx) || [0,0,0];
        return h("button", {
          class: "sw" + (relLum(rgb) > 0.45 ? " lightbg" : ""),
          type: "button", "data-hex": hx, title: "Copy " + hx,
          style: "background:" + hx,
          onClick: function(){ copy(hx); }
        });
      }
      function copy(text){
        if(navigator.clipboard && navigator.clipboard.writeText){
          navigator.clipboard.writeText(text).then(function(){ TS.toast("Copied " + text); },
                                                   function(){ TS.toast(text); });
        } else TS.toast(text);
      }
      function buildHarmony(){
        var rgb = parseHex(baseInput.value);
        TS.clear(harmonyBox);
        if(!rgb) return;
        var hsl = rgbToHsl(rgb[0], rgb[1], rgb[2]);
        var which = harmonySeg.querySelector('[aria-pressed="true"]').dataset.v;
        HARMONIES[which].forEach(function(shift){
          var c = hslToRgb(hsl[0] + shift, hsl[1], hsl[2]);
          harmonyBox.appendChild(swatch(hex(c[0], c[1], c[2])));
        });
      }
      function checkContrast(){
        var a = parseHex(pickA), b = parseHex(pickB);
        if(!a || !b){ ratioOut.textContent = "—"; verdict.textContent = "Enter two hex colours."; return; }
        var r = contrast(a, b);
        ratioOut.textContent = r.toFixed(2) + " : 1";
        sampleText.style.color = pickA;
        sampleText.style.background = pickB;
        var parts = [];
        parts.push(r >= 4.5 ? "Body text: passes AA" : r >= 3 ? "Body text: fails AA (large text only)" : "Body text: fails");
        parts.push(r >= 7 ? "and AAA" : "");
        verdict.textContent = parts.filter(Boolean).join(" ") + ".";
        verdict.className = "note" + (r >= 4.5 ? "" : " warn");
      }
      function cssText(){
        return ":root{\n" + palette.map(function(c, i){
          return "  --colour-" + (i+1) + ": " + hex(c[0],c[1],c[2]) + ";";
        }).join("\n") + "\n}\n";
      }
      function copyCss(){
        if(!palette.length) return;
        copy(cssText());
      }
      function saveSvg(){
        if(!palette.length) return;
        var w = 120, hh = 160;
        var parts = palette.map(function(c, i){
          var hx = hex(c[0],c[1],c[2]);
          var light = relLum(c) > 0.45;
          return '<rect x="' + (i*w) + '" y="0" width="' + w + '" height="' + hh + '" fill="' + hx + '"/>' +
                 '<text x="' + (i*w + w/2) + '" y="' + (hh-14) + '" text-anchor="middle" ' +
                 'font-family="monospace" font-size="13" fill="' + (light ? "#000" : "#fff") + '">' + hx + '</text>';
        }).join("");
        var svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + (palette.length*w) + '" height="' + hh +
                  '" viewBox="0 0 ' + (palette.length*w) + ' ' + hh + '">' + parts + '</svg>';
        TS.saveText(svg, "swatches.svg", "image/svg+xml");
      }

      TS.sampleImage().then(function(s){
        if(!alive || bitmap) return;
        bitmap = s.bitmap; drawPreview(); recompute();
      });

      return function(){ alive = false; };
    }
  });
})();
