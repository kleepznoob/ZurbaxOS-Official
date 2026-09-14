/* ============================================================
   PDF — merge, reorder, rotate, delete, split, and build a PDF
   out of images. Two libraries, loaded only when this tool opens:
   pdf-lib does the surgery, pdf.js draws the thumbnails so you can
   see which page you are about to throw away.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h, L = TS.license;

  TS.register({
    id: "pdf",
    name: "PDF",
    swaps: "Acrobat",
    blurb: "Merge, reorder, rotate and delete pages. Or turn a folder of images into one PDF.",
    icon: '<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><path d="M9 13h6M9 17h4"/></svg>',

    mount: function(stage, insp){
      var docs = [];            // loaded source PDFs (pdf-lib documents + raw bytes)
      var pages = [];           // the working page list, in output order
      var alive = true, ready = false, loading = false;

      var grid = h("div", { class:"thumbs" });
      var status = h("p", { class:"note", style:"margin:0" }, "Loading the PDF engine…");
      var dz = TS.dropzone({
        accept:"application/pdf,image/*", title:"Drop PDFs or images here",
        sub:"PDFs get merged in the order you drop them · images become one page each",
        onFiles: addFiles
      });
      stage.appendChild(h("div", { class:"stage-pad" }, [dz, status, grid]));

      var countNote = h("p", { class:"note" }, "No pages yet.");
      var selNote = h("p", { class:"note" }, "");
      var limitNote = h("div", null);

      insp.appendChild(TS.sec("Selection", TS.stack([
        TS.row2([
          TS.btn("Select all", { kind:"ghost", sm:true, onClick:function(){ pages.forEach(function(p){ p.sel = true; }); paint(); }}),
          TS.btn("Select none", { kind:"ghost", sm:true, onClick:function(){ pages.forEach(function(p){ p.sel = false; }); paint(); }})
        ]),
        selNote
      ])));
      insp.appendChild(TS.sec("Do this to the selected pages", TS.stack([
        TS.row2([
          TS.btn("Rotate left", { kind:"ghost", sm:true, onClick:function(){ rotate(-90); }}),
          TS.btn("Rotate right", { kind:"ghost", sm:true, onClick:function(){ rotate(90); }})
        ]),
        TS.row2([
          TS.btn("Move earlier", { kind:"ghost", sm:true, onClick:function(){ shift(-1); }}),
          TS.btn("Move later", { kind:"ghost", sm:true, onClick:function(){ shift(1); }})
        ]),
        TS.btn("Delete pages", { kind:"danger", sm:true, onClick:deleteSel })
      ])));
      insp.appendChild(TS.sec("Save", TS.stack([
        TS.btn("Save as one PDF", { onClick:function(){ save(false); }}),
        TS.btn("Save only the selected pages", { kind:"ghost", onClick:function(){ save(true); }}),
        countNote, limitNote
      ])));

      /* ---------- engine ---------- */
      var engine = Promise.all([
        TS.loadScript("vendor/pdf-lib.min.js"),
        TS.loadScript("vendor/pdf.min.js")
      ]).then(function(){
        if(window.pdfjsLib) pdfjsLib.GlobalWorkerOptions.workerSrc = "vendor/pdf.worker.min.js";
        ready = true;
        status.textContent = "Ready. Drop a PDF, or a pile of images to bind into one.";
      }).catch(function(){
        status.textContent = "The PDF engine could not load, so this tool is unavailable.";
        status.className = "note warn";
      });

      /* ---------- loading ---------- */
      function addFiles(fileList){
        engine.then(function(){
          if(!ready) return;
          var files = Array.prototype.slice.call(fileList);
          if(loading) return;
          loading = true;
          status.textContent = "Reading " + files.length + " file" + (files.length === 1 ? "" : "s") + "…";
          return files.reduce(function(chain, file){
            return chain.then(function(){
              return /pdf/i.test(file.type) ? addPdf(file) : addImage(file);
            });
          }, Promise.resolve()).then(function(){
            loading = false;
            status.textContent = pages.length + " page" + (pages.length === 1 ? "" : "s") + " loaded.";
            paint();
          }, function(err){
            loading = false;
            status.textContent = "Couldn't read that: " + (err && err.message ? err.message : "unsupported file");
            status.className = "note warn";
          });
        });
      }

      function addPdf(file){
        return file.arrayBuffer().then(function(buf){
          var forLib = buf.slice(0), forJs = buf.slice(0);
          return PDFLib.PDFDocument.load(forLib, { ignoreEncryption:true }).then(function(doc){
            var docIndex = docs.push({ doc: doc, name: file.name }) - 1;
            var n = doc.getPageCount();
            for(var i = 0; i < n; i++){
              pages.push({ doc: docIndex, page: i, rotate: 0, sel: false,
                           label: TS.baseName(file.name).slice(0,10) + " p" + (i+1), thumb: null });
            }
            paint();
            return renderThumbs(forJs, docIndex);
          });
        });
      }

      function renderThumbs(bytes, docIndex){
        if(!window.pdfjsLib) return Promise.resolve();
        return pdfjsLib.getDocument({ data: bytes }).promise.then(function(pdf){
          var mine = pages.filter(function(p){ return p.doc === docIndex; });
          return mine.reduce(function(chain, p){
            return chain.then(function(){
              return pdf.getPage(p.page + 1).then(function(page){
                var vp = page.getViewport({ scale: 1 });
                var scale = 150 / Math.max(vp.width, vp.height);
                var v2 = page.getViewport({ scale: scale });
                var c = document.createElement("canvas");
                c.width = Math.ceil(v2.width); c.height = Math.ceil(v2.height);
                return page.render({ canvasContext: c.getContext("2d"), viewport: v2 }).promise
                  .then(function(){ p.thumb = c; paint(); });
              });
            });
          }, Promise.resolve());
        }).catch(function(){ /* thumbnails are a nicety, not a requirement */ });
      }

      function addImage(file){
        return TS.decode(file).then(function(bmp){
          var c = document.createElement("canvas");
          c.width = bmp.width; c.height = bmp.height;
          c.getContext("2d").drawImage(bmp, 0, 0);
          var thumb = TS.drawScaled(bmp, 0, 0, bmp.width, bmp.height,
            Math.max(1, Math.round(150 * bmp.width / Math.max(bmp.width, bmp.height))),
            Math.max(1, Math.round(150 * bmp.height / Math.max(bmp.width, bmp.height))));
          pages.push({ image: c, rotate: 0, sel: false,
                       label: TS.baseName(file.name).slice(0,12), thumb: thumb });
        });
      }

      /* ---------- editing ---------- */
      function selected(){ return pages.filter(function(p){ return p.sel; }); }
      function rotate(d){
        var s = selected();
        if(!s.length){ TS.toast("Select some pages first."); return; }
        s.forEach(function(p){ p.rotate = ((p.rotate + d) % 360 + 360) % 360; });
        paint();
      }
      function deleteSel(){
        if(!selected().length){ TS.toast("Select some pages first."); return; }
        pages = pages.filter(function(p){ return !p.sel; });
        paint();
      }
      function shift(dir){
        var idxs = pages.map(function(p,i){ return p.sel ? i : -1; }).filter(function(i){ return i >= 0; });
        if(!idxs.length){ TS.toast("Select some pages first."); return; }
        if(dir > 0) idxs.reverse();
        idxs.forEach(function(i){
          var j = i + dir;
          if(j < 0 || j >= pages.length) return;
          var t = pages[i]; pages[i] = pages[j]; pages[j] = t;
        });
        paint();
      }

      /* ---------- painting ---------- */
      function paint(){
        TS.clear(grid);
        pages.forEach(function(p, i){
          var btn = h("button", {
            class:"thumb", type:"button", "aria-pressed": String(!!p.sel),
            title: p.label + (p.rotate ? " · rotated " + p.rotate + "°" : ""),
            onClick: function(){ p.sel = !p.sel; paint(); }
          });
          if(p.thumb){
            var img = h("canvas");
            img.width = p.thumb.width; img.height = p.thumb.height;
            var cx = img.getContext("2d");
            if(p.rotate % 180 !== 0){
              img.width = p.thumb.height; img.height = p.thumb.width;
            }
            cx = img.getContext("2d");
            cx.save();
            cx.translate(img.width/2, img.height/2);
            cx.rotate(p.rotate * Math.PI/180);
            cx.drawImage(p.thumb, -p.thumb.width/2, -p.thumb.height/2);
            cx.restore();
            btn.appendChild(img);
          } else {
            btn.appendChild(h("div", { style:"display:grid;place-items:center;height:100%;color:var(--muted);font-size:11px" }, "page " + (i+1)));
          }
          btn.appendChild(h("span", { class:"tag" }, (i+1) + (p.rotate ? " ↻" : "")));
          btn.appendChild(h("button", { class:"kill", type:"button", title:"Remove this page",
            onClick:function(ev){ ev.stopPropagation(); pages.splice(i,1); paint(); } }, "×"));
          grid.appendChild(btn);
        });
        if(!pages.length) grid.appendChild(h("p", { class:"empty" }, "No pages loaded yet."));

        var n = selected().length;
        selNote.textContent = n ? n + " page" + (n === 1 ? "" : "s") + " selected." : "Click page thumbnails to select them.";
        countNote.textContent = pages.length + " page" + (pages.length === 1 ? "" : "s") + " in the output.";
        dz.classList.toggle("slim", pages.length > 0);

        TS.clear(limitNote);
        var cap = L.limit("pdfPages");
        if(cap !== Infinity && pages.length > cap){
          var note = L.lockNote("Free saves the first " + cap + " pages.", "PDFs over " + cap + " pages");
          if(note) limitNote.appendChild(note);
        }
      }

      /* ---------- saving ---------- */
      function save(onlySelected){
        var list = onlySelected ? selected() : pages;
        if(!list.length){ TS.toast(onlySelected ? "Select some pages first." : "Nothing to save."); return; }
        var cap = L.limit("pdfPages");
        if(list.length > cap){
          TS.toast("Free plan saves " + cap + " pages — saving the first " + cap + ".");
          list = list.slice(0, cap);
        }
        TS.toast("Building the PDF…");
        PDFLib.PDFDocument.create().then(function(out){
          return list.reduce(function(chain, p){
            return chain.then(function(){
              if(p.image){
                return new Promise(function(res){ p.image.toBlob(res, "image/jpeg", 0.9); })
                  .then(function(blob){ return blob.arrayBuffer(); })
                  .then(function(buf){ return out.embedJpg(buf); })
                  .then(function(img){
                    var swap = p.rotate % 180 !== 0;
                    var pw = swap ? img.height : img.width, ph = swap ? img.width : img.height;
                    var page = out.addPage([pw, ph]);
                    page.drawImage(img, { x:0, y:0, width:img.width, height:img.height,
                                          rotate: PDFLib.degrees(p.rotate) });
                  });
              }
              return out.copyPages(docs[p.doc].doc, [p.page]).then(function(copied){
                var page = copied[0];
                if(p.rotate){
                  var cur = page.getRotation().angle || 0;
                  page.setRotation(PDFLib.degrees((cur + p.rotate) % 360));
                }
                out.addPage(page);
              });
            });
          }, Promise.resolve()).then(function(){ return out.save(); });
        }).then(function(bytes){
          TS.saveBlob(new Blob([bytes], { type:"application/pdf" }),
                      onlySelected ? "selected-pages.pdf" : "merged.pdf");
        }).catch(function(err){
          TS.toast("Couldn't build that PDF: " + (err && err.message ? err.message : "unknown error"));
        });
      }

      paint();
      return function(){ alive = false; };
    }
  });
})();
