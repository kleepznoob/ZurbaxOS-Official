/* ============================================================
   Design — an artboard with text, shapes and images on it.
   Immediate-mode canvas: objects live in one ordered array and the
   whole board redraws each frame. That keeps hit-testing, z-order,
   PNG export and SVG export all reading from the same list, so they
   can never disagree about what the design looks like.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h, L = TS.license;

  var BOARDS = [
    { id:"square", label:"Square post", w:1080, h:1080 },
    { id:"story",  label:"Story",       w:1080, h:1920 },
    { id:"thumb",  label:"YT thumbnail",w:1280, h:720 },
    { id:"poster", label:"Poster A4",   w:1240, h:1754 },
    { id:"slide",  label:"Slide",       w:1920, h:1080 }
  ];
  var FONTS = [
    { v:'"Archivo", sans-serif', label:"Archivo" },
    { v:'"Atkinson Hyperlegible", sans-serif', label:"Atkinson" },
    { v:'"IBM Plex Mono", monospace', label:"Plex Mono" },
    { v:'Georgia, serif', label:"Georgia" }
  ];

  TS.register({
    id: "design",
    name: "Design",
    swaps: "Illustrator",
    blurb: "Put text, shapes and photos on an artboard. Export a crisp PNG or a real SVG.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M4 4h16v16H4z"/><path d="M4 9h16M9 9v11"/></svg>',

    mount: function(stage, insp){
      var board = { w:1080, h:1350, bg:"#14161A" };
      var objects = [], sel = null, seq = 0, alive = true;
      var drag = null;

      var canvas = h("canvas", { id:"design-canvas", tabindex:"0", style:"cursor:default" });
      var wrap = h("div", { class:"canvas-wrap plain" }, canvas);
      stage.appendChild(h("div", { class:"stage-pad" }, [
        wrap,
        h("p", { class:"note", style:"text-align:center;margin:0" },
          "Click to select · drag to move · drag a corner to resize · Delete removes · Ctrl+D duplicates")
      ]));

      /* ---------- object factory ---------- */
      function add(type, extra){
        var o = Object.assign({
          id: ++seq, type: type,
          x: board.w*0.15, y: board.h*0.35,
          w: board.w*0.6, h: board.h*0.16,
          fill:"#EFA940", stroke:"", strokeW:4, radius:14, opacity:100,
          text:"Your text here", fontSize: Math.round(board.w/14), fontWeight:700,
          fontFamily: FONTS[0].v, align:"center", color:"#FFFFFF"
        }, extra || {});
        objects.push(o);
        select(o);
        draw(); paintLayers();
        return o;
      }

      /* ---------- drawing ---------- */
      function roundRect(ctx, x, y, w, hh, r){
        r = Math.max(0, Math.min(r, Math.abs(w)/2, Math.abs(hh)/2));
        ctx.beginPath();
        ctx.moveTo(x+r, y);
        ctx.arcTo(x+w, y, x+w, y+hh, r);
        ctx.arcTo(x+w, y+hh, x, y+hh, r);
        ctx.arcTo(x, y+hh, x, y, r);
        ctx.arcTo(x, y, x+w, y, r);
        ctx.closePath();
      }
      function wrapLines(ctx, text, maxW){
        var out = [];
        String(text).split("\n").forEach(function(para){
          var words = para.split(/\s+/), line = "";
          words.forEach(function(word){
            var test = line ? line + " " + word : word;
            if(ctx.measureText(test).width > maxW && line){ out.push(line); line = word; }
            else line = test;
          });
          out.push(line);
        });
        return out;
      }
      function fontStr(o){ return o.fontWeight + " " + o.fontSize + "px " + o.fontFamily; }

      function drawObject(ctx, o){
        ctx.save();
        ctx.globalAlpha = o.opacity / 100;
        if(o.type === "rect"){
          roundRect(ctx, o.x, o.y, o.w, o.h, o.radius);
          if(o.fill){ ctx.fillStyle = o.fill; ctx.fill(); }
          if(o.stroke){ ctx.strokeStyle = o.stroke; ctx.lineWidth = o.strokeW; ctx.stroke(); }
        } else if(o.type === "ellipse"){
          ctx.beginPath();
          ctx.ellipse(o.x+o.w/2, o.y+o.h/2, Math.abs(o.w)/2, Math.abs(o.h)/2, 0, 0, 6.2832);
          if(o.fill){ ctx.fillStyle = o.fill; ctx.fill(); }
          if(o.stroke){ ctx.strokeStyle = o.stroke; ctx.lineWidth = o.strokeW; ctx.stroke(); }
        } else if(o.type === "line"){
          ctx.beginPath();
          ctx.moveTo(o.x, o.y + o.h/2);
          ctx.lineTo(o.x + o.w, o.y + o.h/2);
          ctx.strokeStyle = o.fill || "#fff";
          ctx.lineWidth = o.strokeW;
          ctx.lineCap = "round";
          ctx.stroke();
        } else if(o.type === "image" && o.bitmap){
          var s = Math.max(o.w/o.bitmap.width, o.h/o.bitmap.height);
          var sw = o.w/s, sh = o.h/s;
          var sx = (o.bitmap.width-sw)/2, sy = (o.bitmap.height-sh)/2;
          ctx.save();
          roundRect(ctx, o.x, o.y, o.w, o.h, o.radius);
          ctx.clip();
          ctx.drawImage(o.bitmap, sx, sy, sw, sh, o.x, o.y, o.w, o.h);
          ctx.restore();
        } else if(o.type === "text"){
          ctx.font = fontStr(o);
          ctx.fillStyle = o.color;
          ctx.textBaseline = "top";
          ctx.textAlign = o.align;
          var lines = wrapLines(ctx, o.text, o.w);
          var lh = o.fontSize * 1.22;
          var tx = o.align === "center" ? o.x + o.w/2 : o.align === "right" ? o.x + o.w : o.x;
          lines.forEach(function(ln, i){ ctx.fillText(ln, tx, o.y + i*lh); });
          o.h = Math.max(o.fontSize, lines.length * lh);
        }
        ctx.restore();
      }

      function draw(){
        var maxW = 760, maxH = 520;
        var s = Math.min(maxW/board.w, maxH/board.h, 1);
        canvas.width = Math.round(board.w * s);
        canvas.height = Math.round(board.h * s);
        canvas.dataset.scale = s;
        var ctx = canvas.getContext("2d");
        ctx.save();
        ctx.scale(s, s);
        ctx.fillStyle = board.bg;
        ctx.fillRect(0, 0, board.w, board.h);
        objects.forEach(function(o){ drawObject(ctx, o); });
        if(sel){
          ctx.strokeStyle = "#EFA940";
          ctx.lineWidth = 2/s;
          ctx.setLineDash([6/s, 4/s]);
          ctx.strokeRect(sel.x, sel.y, sel.w, sel.h);
          ctx.setLineDash([]);
          ctx.fillStyle = "#EFA940";
          var hs = 9/s;
          corners(sel).forEach(function(p){ ctx.fillRect(p[0]-hs/2, p[1]-hs/2, hs, hs); });
        }
        ctx.restore();
      }
      function corners(o){
        return [[o.x,o.y],[o.x+o.w,o.y],[o.x,o.y+o.h],[o.x+o.w,o.y+o.h]];
      }

      /* ---------- interaction ---------- */
      function toBoard(ev){
        var r = canvas.getBoundingClientRect();
        return {
          x: (ev.clientX - r.left) / r.width * board.w,
          y: (ev.clientY - r.top) / r.height * board.h
        };
      }
      canvas.addEventListener("pointerdown", function(ev){
        var p = toBoard(ev);
        var near = board.w * 0.022;
        if(sel){
          var cs = corners(sel);
          for(var i = 0; i < 4; i++){
            if(Math.abs(p.x-cs[i][0]) < near && Math.abs(p.y-cs[i][1]) < near){
              drag = { mode:"resize", corner:i, ox:sel.x, oy:sel.y, ow:sel.w, oh:sel.h };
              canvas.setPointerCapture(ev.pointerId);
              return;
            }
          }
        }
        var hit = null;
        for(var j = objects.length-1; j >= 0; j--){
          var o = objects[j];
          if(p.x >= o.x && p.x <= o.x+o.w && p.y >= o.y && p.y <= o.y+o.h){ hit = o; break; }
        }
        select(hit);
        if(hit){
          drag = { mode:"move", dx: p.x - hit.x, dy: p.y - hit.y };
          canvas.setPointerCapture(ev.pointerId);
        }
        draw();
      });
      canvas.addEventListener("pointermove", function(ev){
        if(!drag || !sel) return;
        var p = toBoard(ev);
        if(drag.mode === "move"){
          sel.x = Math.round(p.x - drag.dx);
          sel.y = Math.round(p.y - drag.dy);
        } else {
          var x1 = drag.ox, y1 = drag.oy, x2 = drag.ox+drag.ow, y2 = drag.oy+drag.oh;
          if(drag.corner === 0){ x1 = p.x; y1 = p.y; }
          if(drag.corner === 1){ x2 = p.x; y1 = p.y; }
          if(drag.corner === 2){ x1 = p.x; y2 = p.y; }
          if(drag.corner === 3){ x2 = p.x; y2 = p.y; }
          sel.x = Math.round(Math.min(x1,x2)); sel.y = Math.round(Math.min(y1,y2));
          sel.w = Math.max(16, Math.round(Math.abs(x2-x1)));
          sel.h = Math.max(16, Math.round(Math.abs(y2-y1)));
        }
        draw();
      });
      canvas.addEventListener("pointerup", function(){ drag = null; syncProps(); });
      canvas.addEventListener("keydown", function(ev){
        if(!sel) return;
        if(ev.key === "Delete" || ev.key === "Backspace"){
          ev.preventDefault(); removeSel();
        } else if((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === "d"){
          ev.preventDefault();
          var copy = Object.assign({}, sel, { id: ++seq, x: sel.x + 24, y: sel.y + 24 });
          objects.push(copy); select(copy); draw(); paintLayers();
        }
      });

      function select(o){ sel = o; syncProps(); paintLayers(); }
      function removeSel(){
        objects = objects.filter(function(o){ return o !== sel; });
        select(null); draw(); paintLayers();
      }

      /* ---------- inspector ---------- */
      var boardSeg = TS.seg({
        label:"Artboard", value:"square",
        options: BOARDS.slice(0,3).map(function(b){ return { value:b.id, label:b.label.split(" ")[0] }; }),
        onChange: setBoard
      });
      var boardSeg2 = TS.seg({
        label:"Artboard", value:"",
        options: BOARDS.slice(3).map(function(b){ return { value:b.id, label:b.label.split(" ")[0] }; }),
        onChange: setBoard
      });
      function setBoard(id){
        var b = BOARDS.filter(function(x){ return x.id === id; })[0];
        if(!b) return;
        board.w = b.w; board.h = b.h;
        boardSeg.set(id); boardSeg2.set(id);
        draw();
      }
      var bgInput = h("input", { type:"color", value:board.bg, class:"inp", style:"height:36px;padding:3px",
        "aria-label":"Artboard background", onInput:function(e){ board.bg = e.target.value; draw(); } });

      var imgInput = h("input", { type:"file", accept:"image/*", hidden:true, onChange:function(){
        var f = imgInput.files[0]; imgInput.value = "";
        if(!f) return;
        TS.decode(f).then(function(bmp){
          add("image", { bitmap:bmp, x:board.w*0.1, y:board.h*0.1, w:board.w*0.8,
                         h: Math.round(board.w*0.8 * bmp.height/bmp.width), radius:0 });
        });
      }});

      var props = h("div", { class:"stack" });
      var layers = h("div", { class:"list" });

      insp.appendChild(TS.sec("Artboard", TS.stack([boardSeg, boardSeg2, TS.field("Background", bgInput)])));
      insp.appendChild(TS.sec("Add", TS.stack([
        TS.row2([
          TS.btn("Text", { kind:"ghost", sm:true, onClick:function(){ add("text", { fill:"" }); }}),
          TS.btn("Rectangle", { kind:"ghost", sm:true, onClick:function(){ add("rect"); }})
        ]),
        TS.row2([
          TS.btn("Ellipse", { kind:"ghost", sm:true, onClick:function(){ add("ellipse"); }}),
          TS.btn("Line", { kind:"ghost", sm:true, onClick:function(){ add("line", { h:8, strokeW:8 }); }})
        ]),
        TS.btn("Image…", { kind:"ghost", sm:true, onClick:function(){ imgInput.click(); }}),
        imgInput
      ])));
      insp.appendChild(TS.sec("Selected", props));
      insp.appendChild(TS.sec("Layers", layers));
      insp.appendChild(TS.sec("Export", TS.stack([
        TS.btn("Save PNG", { onClick:function(){ exportPng(1); }}),
        TS.btn("Save PNG at 2x", { kind:"ghost", onClick:function(){ exportPng(2); }}),
        TS.btn("Save SVG", { kind:"ghost", onClick:exportSvg }),
        h("p", { class:"note" }, "PNG is exactly what you see. SVG stays editable and infinitely sharp, but its text uses whichever fonts the opener has.")
      ])));

      function syncProps(){
        TS.clear(props);
        if(!sel){
          props.appendChild(h("p", { class:"note", style:"margin:0" }, "Nothing selected. Click something on the artboard."));
          return;
        }
        var o = sel;
        if(o.type === "text"){
          var ta = h("textarea", { class:"inp", rows:3, value:o.text, "aria-label":"Text",
            onInput:function(e){ o.text = e.target.value; draw(); } });
          ta.value = o.text;
          props.appendChild(TS.field("Words", ta));
          props.appendChild(TS.field("Typeface", (function(){
            var s = h("select", { class:"inp", "aria-label":"Typeface", onChange:function(e){ o.fontFamily = e.target.value; draw(); } });
            FONTS.forEach(function(f){
              s.appendChild(h("option", { value:f.v, selected:f.v === o.fontFamily }, f.label));
            });
            return s;
          })()));
          props.appendChild(TS.slider({ id:"d-fs", label:"Size", min:10, max:Math.round(board.w/3), value:o.fontSize, unit:" px", plus:false,
            onInput:function(v){ o.fontSize = v; draw(); } }));
          props.appendChild(TS.seg({ label:"Weight", value:String(o.fontWeight),
            options:[{ value:"400", label:"Regular" }, { value:"700", label:"Bold" }],
            onChange:function(v){ o.fontWeight = Number(v); draw(); } }));
          props.appendChild(TS.seg({ label:"Align", value:o.align,
            options:[{ value:"left", label:"Left" }, { value:"center", label:"Centre" }, { value:"right", label:"Right" }],
            onChange:function(v){ o.align = v; draw(); } }));
          props.appendChild(TS.field("Colour", h("input", { type:"color", class:"inp", style:"height:36px;padding:3px",
            value:o.color, "aria-label":"Text colour", onInput:function(e){ o.color = e.target.value; draw(); } })));
        } else {
          if(o.type !== "image"){
            props.appendChild(TS.field("Fill", h("input", { type:"color", class:"inp", style:"height:36px;padding:3px",
              value:o.fill || "#EFA940", "aria-label":"Fill", onInput:function(e){ o.fill = e.target.value; draw(); } })));
          }
          if(o.type === "rect" || o.type === "ellipse" || o.type === "image"){
            props.appendChild(TS.slider({ id:"d-rad", label:"Corner", min:0, max:200, value:o.radius, unit:" px", plus:false,
              onInput:function(v){ o.radius = v; draw(); } }));
          }
          if(o.type === "line"){
            props.appendChild(TS.slider({ id:"d-sw", label:"Thickness", min:1, max:80, value:o.strokeW, unit:" px", plus:false,
              onInput:function(v){ o.strokeW = v; draw(); } }));
          }
        }
        props.appendChild(TS.slider({ id:"d-op", label:"Opacity", min:5, max:100, value:o.opacity, unit:"%", plus:false,
          onInput:function(v){ o.opacity = v; draw(); } }));
        props.appendChild(TS.row2([
          TS.btn("Bring forward", { kind:"ghost", sm:true, onClick:function(){ move(1); }}),
          TS.btn("Send back", { kind:"ghost", sm:true, onClick:function(){ move(-1); }})
        ]));
        props.appendChild(TS.btn("Delete", { kind:"danger", sm:true, onClick:removeSel }));
      }
      function move(dir){
        var i = objects.indexOf(sel);
        var j = i + dir;
        if(i < 0 || j < 0 || j >= objects.length) return;
        objects.splice(i, 1); objects.splice(j, 0, sel);
        draw(); paintLayers();
      }
      function paintLayers(){
        TS.clear(layers);
        if(!objects.length){
          layers.appendChild(h("p", { class:"empty" }, "Empty artboard."));
          return;
        }
        objects.slice().reverse().forEach(function(o){
          var label = o.type === "text" ? (o.text.slice(0,22) || "Text") : o.type;
          layers.appendChild(h("div", {
            class:"item", "aria-selected": String(o === sel),
            onClick:function(){ select(o); draw(); }
          }, [
            h("span", { class:"num" }, o.type[0].toUpperCase()),
            h("span", { class:"grow" }, label)
          ]));
        });
      }

      /* ---------- export ---------- */
      function exportPng(mult){
        var w = board.w*mult, hh = board.h*mult;
        var cap = L.limit("exportPx");
        if(Math.max(w, hh) > cap){
          if(!L.allow("exportPx", Math.max(w,hh), "Exporting above " + cap + " px")) return;
        }
        var c = document.createElement("canvas");
        c.width = w; c.height = hh;
        var ctx = c.getContext("2d");
        ctx.scale(mult, mult);
        ctx.fillStyle = board.bg;
        ctx.fillRect(0, 0, board.w, board.h);
        objects.forEach(function(o){ drawObject(ctx, o); });
        TS.saveCanvas(c, "design-" + board.w + "x" + board.h + (mult > 1 ? "@" + mult + "x" : "") + ".png", "image/png");
      }

      function esc(s){
        return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
      }
      function exportSvg(){
        var meas = document.createElement("canvas").getContext("2d");
        var parts = ['<rect width="' + board.w + '" height="' + board.h + '" fill="' + board.bg + '"/>'];
        objects.forEach(function(o){
          var op = o.opacity < 100 ? ' opacity="' + (o.opacity/100).toFixed(2) + '"' : "";
          if(o.type === "rect"){
            parts.push('<rect x="'+o.x+'" y="'+o.y+'" width="'+o.w+'" height="'+o.h+'" rx="'+o.radius+'" fill="'+(o.fill||"none")+'"'+op+'/>');
          } else if(o.type === "ellipse"){
            parts.push('<ellipse cx="'+(o.x+o.w/2)+'" cy="'+(o.y+o.h/2)+'" rx="'+Math.abs(o.w/2)+'" ry="'+Math.abs(o.h/2)+'" fill="'+(o.fill||"none")+'"'+op+'/>');
          } else if(o.type === "line"){
            parts.push('<line x1="'+o.x+'" y1="'+(o.y+o.h/2)+'" x2="'+(o.x+o.w)+'" y2="'+(o.y+o.h/2)+'" stroke="'+(o.fill||"#fff")+'" stroke-width="'+o.strokeW+'" stroke-linecap="round"'+op+'/>');
          } else if(o.type === "text"){
            meas.font = fontStr(o);
            var lines = wrapLines(meas, o.text, o.w);
            var lh = o.fontSize*1.22;
            var tx = o.align === "center" ? o.x+o.w/2 : o.align === "right" ? o.x+o.w : o.x;
            var anchor = o.align === "center" ? "middle" : o.align === "right" ? "end" : "start";
            var tspans = lines.map(function(ln, i){
              return '<tspan x="'+tx+'" y="'+(o.y + i*lh + o.fontSize*0.82)+'">'+esc(ln)+'</tspan>';
            }).join("");
            parts.push('<text font-family="'+esc(o.fontFamily.replace(/"/g,""))+'" font-size="'+o.fontSize+
                       '" font-weight="'+o.fontWeight+'" fill="'+o.color+'" text-anchor="'+anchor+'"'+op+'>'+tspans+'</text>');
          } else if(o.type === "image" && o.bitmap){
            var c = document.createElement("canvas");
            c.width = o.bitmap.width; c.height = o.bitmap.height;
            c.getContext("2d").drawImage(o.bitmap, 0, 0);
            parts.push('<image x="'+o.x+'" y="'+o.y+'" width="'+o.w+'" height="'+o.h+
                       '" preserveAspectRatio="xMidYMid slice" href="'+c.toDataURL("image/jpeg",0.88)+'"'+op+'/>');
          }
        });
        var svg = '<svg xmlns="http://www.w3.org/2000/svg" width="'+board.w+'" height="'+board.h+
                  '" viewBox="0 0 '+board.w+' '+board.h+'">'+parts.join("")+'</svg>';
        TS.saveText(svg, "design.svg", "image/svg+xml");
      }

      /* ---------- open on something real ---------- */
      setBoard("square");
      TS.sampleImage().then(function(s){
        if(!alive || objects.length) return;
        objects.push({ id:++seq, type:"image", bitmap:s.bitmap, x:0, y:0, w:1080, h:720,
                       radius:0, opacity:100, fill:"", strokeW:0 });
        objects.push({ id:++seq, type:"rect", x:0, y:620, w:1080, h:460,
                       fill:"#14161A", radius:0, opacity:100, strokeW:0 });
        objects.push({ id:++seq, type:"text", x:80, y:700, w:920, h:120, text:"FIELD NOTES",
                       fontSize:118, fontWeight:700, fontFamily:FONTS[0].v, align:"left",
                       color:"#EFA940", opacity:100 });
        objects.push({ id:++seq, type:"text", x:80, y:850, w:920, h:80,
                       text:"Six tools. One tab. No subscription.",
                       fontSize:44, fontWeight:400, fontFamily:FONTS[1].v, align:"left",
                       color:"#E8ECF1", opacity:100 });
        objects.push({ id:++seq, type:"line", x:80, y:970, w:260, h:10, fill:"#EFA940", strokeW:10, opacity:100 });
        draw(); paintLayers(); syncProps();
      });

      syncProps(); paintLayers(); draw();
      return function(){ alive = false; };
    }
  });
})();
