/* ============================================================
   What's free — read from OpenRouter's public model list.

   This is fetched live rather than typed in by hand, because a
   hand-written list of "free AI models" is wrong within weeks and
   wrong lists are exactly what the scam sites run on. No key is
   needed to read it; anyone can check the same URL.
   ============================================================ */
(function(){
  "use strict";
  var H = window.H, h = H.h;
  var URL_MODELS = "https://openrouter.ai/api/v1/models";
  var CACHE_KEY = "hearth.models";
  var CACHE_MS = 6 * 3600 * 1000;

  var M = { all: [], free: [], fetchedAt: 0, error: "" };

  function isFree(m){
    var p = m.pricing || {};
    var z = function(v){ return v === "0" || v === "0.0" || Number(v) === 0; };
    return z(p.prompt) && z(p.completion);
  }

  M.load = function(force){
    try{
      var c = JSON.parse(localStorage.getItem(CACHE_KEY) || "null");
      if(!force && c && Date.now() - c.at < CACHE_MS && c.models){
        apply(c.models, c.at);
        return Promise.resolve(M);
      }
    }catch(e){}
    return fetch(URL_MODELS, { headers:{ "Accept":"application/json" } })
      .then(function(r){
        if(!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      })
      .then(function(j){
        var models = (j.data || []).map(function(m){
          return {
            id: m.id,
            name: m.name || m.id,
            ctx: m.context_length || 0,
            modality: (m.architecture && m.architecture.modality) || "",
            free: isFree(m),
            prompt: (m.pricing || {}).prompt
          };
        });
        apply(models, Date.now());
        try{ localStorage.setItem(CACHE_KEY, JSON.stringify({ at: M.fetchedAt, models: models })); }catch(e){}
        return M;
      })
      .catch(function(err){
        M.error = err.message || "could not reach openrouter.ai";
        // Stale cache still beats nothing.
        try{
          var c2 = JSON.parse(localStorage.getItem(CACHE_KEY) || "null");
          if(c2 && c2.models) apply(c2.models, c2.at);
        }catch(e){}
        return M;
      });
  };

  function apply(models, at){
    M.all = models;
    M.free = models.filter(function(m){ return m.free; })
                   .sort(function(a,b){ return b.ctx - a.ctx; });
    M.fetchedAt = at;
    M.error = "";
  }

  /* ---------------- view ---------------- */
  M.render = function(root){
    H.clear(root);
    var body = h("div", { class:"panel-in" });
    root.appendChild(h("div", { class:"panel" }, body));

    body.appendChild(h("h1", null, "What is actually free"));
    body.appendChild(h("p", { class:"lede" },
      "Read live from OpenRouter's public model list — the same URL anyone can open, no account needed. " +
      "If a model appears here, its listed price for input and output is genuinely zero right now."));

    var status = h("p", { class:"note" }, "Loading…");
    var filters = h("div", { class:"filters" });
    var list = h("div", null);
    body.appendChild(status);
    body.appendChild(filters);
    body.appendChild(list);

    body.appendChild(h("h2", null, "Why anyone gives a model away"));
    body.appendChild(h("ul", null, [
      h("li", null, "A lab wants people to try a new model, so it is free while it is new."),
      h("li", null, "A small model is cheap enough to run that giving it away buys goodwill."),
      h("li", null, "Some free tiers are paid for with your prompts — the provider may keep them to train on.")
    ]));
    body.appendChild(h("p", null, [
      h("strong", null, "So treat free models as public. "),
      "Do not paste anything you would mind a stranger reading: passwords, anyone's address, " +
      "private family things. That advice holds for the paid ones too, but it matters more here."
    ]));

    body.appendChild(h("h2", null, "What free does not mean"));
    body.appendChild(h("p", null,
      "It does not mean unlimited. Free models are rate limited, usually a handful of messages a minute, " +
      "and they can be withdrawn without notice. It also does not mean the biggest models — the flagship " +
      "ones cost real money to run, and nobody hands those out."));

    var search = h("input", { class:"inp", type:"search", placeholder:"Filter by name or id…",
      "aria-label":"Filter models", style:"max-width:280px", onInput:function(){ paint(); } });
    var onlyBig = h("button", { class:"btn ghost sm", type:"button", "data-on":"0",
      onClick:function(){
        onlyBig.dataset.on = onlyBig.dataset.on === "1" ? "0" : "1";
        onlyBig.textContent = onlyBig.dataset.on === "1" ? "Large context only ✓" : "Large context only";
        paint();
      }}, "Large context only");
    var refresh = h("button", { class:"btn ghost sm", type:"button", onClick:function(){
      status.textContent = "Refreshing…";
      M.load(true).then(function(){ paint(); });
    }}, "Refresh");
    filters.appendChild(search);
    filters.appendChild(onlyBig);
    filters.appendChild(refresh);

    function paint(){
      H.clear(list);
      if(M.error && !M.free.length){
        status.className = "note warn";
        status.textContent = "Could not reach openrouter.ai (" + M.error + "). " +
          "That is expected inside a sandboxed preview, and on the real site it works. " +
          "You can always open the URL yourself: openrouter.ai/api/v1/models";
        return;
      }
      var q = search.value.trim().toLowerCase();
      var big = onlyBig.dataset.on === "1";
      var rows = M.free.filter(function(m){
        if(big && m.ctx < 128000) return false;
        return !q || (m.id + " " + m.name).toLowerCase().indexOf(q) >= 0;
      });

      var when = M.fetchedAt ? new Date(M.fetchedAt).toLocaleString() : "just now";
      status.className = "note";
      status.textContent = M.free.length + " free of " + M.all.length + " models listed · checked " + when +
                           (M.error ? " · refresh failed, showing the last copy" : "");

      if(!rows.length){
        list.appendChild(h("p", { class:"note" }, "Nothing matches that filter."));
        return;
      }
      rows.forEach(function(m){
        var preview = m.id.indexOf(":free") < 0;
        list.appendChild(h("div", { class:"model-row" }, [
          h("div", null, [
            h("p", { class:"nm" }, m.name),
            h("div", { class:"id" }, m.id),
            h("div", { class:"meta" }, [
              h("span", null, H.fmtNum(m.ctx) + " token context"),
              m.modality ? h("span", null, m.modality) : null,
              preview ? h("span", { class:"tag warn" }, "preview — may log prompts") : h("span", { class:"tag" }, "free tier")
            ])
          ]),
          h("div", { class:"acts" }, [
            h("button", { class:"btn sm", type:"button", onClick:function(){ M.use(m); }}, "Use this"),
            h("button", { class:"btn ghost sm", type:"button", onClick:function(){ H.copy(m.id, "model id"); }}, "Copy id")
          ])
        ]));
      });
    }

    M.paint = paint;
    M.load().then(paint);
  };

  // Chosen from the list: point the chat at it and say what is still needed.
  M.use = function(m){
    H.db.settings.engine = "openrouter";
    H.db.settings.model = m.id;
    H.db.settings.modelLabel = m.name;
    H.save();
    if(H.ui) H.ui.refreshEngine();
    if(!H.db.settings.key){
      H.toast("Model set. Add a free OpenRouter key in Set up to start chatting.");
      if(H.ui) H.ui.show("setup");
    } else {
      H.toast("Now using " + m.name);
      if(H.ui) H.ui.show("chat");
    }
  };

  H.models = M;
})();
