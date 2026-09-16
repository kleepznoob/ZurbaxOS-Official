/* ============================================================
   UI — conversations, the chat surface, setup, and the honest
   explanation of what this can and cannot do.
   ============================================================ */
(function(){
  "use strict";
  var H = window.H, h = H.h;
  var U = {};
  var busy = false, abort = null, view = "chat";

  /* ================= views ================= */
  U.show = function(name){
    view = name;
    ["chat","models","setup","about"].forEach(function(v){
      H.$("view-" + v).hidden = (v !== name);
    });
    Array.prototype.forEach.call(H.$("side-nav").children, function(b){
      b.setAttribute("aria-current", String(b.dataset.view === name));
    });
    if(name === "models") H.models.render(H.$("view-models"));
    if(name === "setup") renderSetup();
    if(name === "about") renderAbout();
    if(name === "chat") H.$("topbar-title").textContent = H.convo().title;
    else H.$("topbar-title").textContent = ({ models:"What's free", setup:"Set up", about:"How this works" })[name];
    closeSide();
  };

  /* ================= sidebar ================= */
  U.renderConvos = function(){
    var box = H.clear(H.$("convos"));
    if(!H.db.convos.length){
      box.appendChild(h("p", { class:"note", style:"padding:6px 10px" }, "No conversations yet."));
      return;
    }
    H.db.convos.forEach(function(c){
      var row = h("button", {
        class:"convo", type:"button", "aria-current": String(c.id === H.db.active),
        onClick:function(){ H.db.active = c.id; H.save(); U.renderConvos(); U.renderMessages(); U.show("chat"); }
      }, [
        h("span", { class:"grow" }, c.title),
        h("span", { class:"x", role:"button", tabindex:"0", title:"Delete",
          onClick:function(ev){
            ev.stopPropagation();
            H.db.convos = H.db.convos.filter(function(x){ return x.id !== c.id; });
            if(H.db.active === c.id) H.db.active = H.db.convos[0] ? H.db.convos[0].id : null;
            if(!H.db.convos.length) H.newConvo();
            H.save(); U.renderConvos(); U.renderMessages();
          }}, "×")
      ]);
      box.appendChild(row);
    });
  };

  U.refreshEngine = function(){
    var s = H.db.settings;
    var dot = H.$("engine-dot"), name = H.$("engine-name");
    var badge = H.$("privacy-badge");
    if(s.engine === "local" && s.model){
      dot.className = "dot on";
      name.textContent = shortModel(s.model);
      badge.hidden = false;
      badge.className = "badge";
      badge.textContent = "Runs on your machine";
    } else if(s.engine === "openrouter" && s.model){
      dot.className = "dot " + (s.key ? "on" : "busy");
      name.textContent = s.modelLabel || shortModel(s.model);
      badge.hidden = false;
      badge.className = "badge warn";
      badge.textContent = s.key ? "Sent to the provider" : "Key needed";
    } else {
      dot.className = "dot";
      name.textContent = "No model yet";
      badge.hidden = true;
    }
    updateComposerNote();
  };
  function shortModel(id){
    var s = String(id).split("/").pop();
    return s.length > 30 ? s.slice(0, 30) + "…" : s;
  }

  function updateComposerNote(){
    var s = H.db.settings, n = H.$("composer-note");
    if(s.engine === "local" && s.model){
      n.textContent = "Running on your own computer. Nothing you type leaves this device.";
    } else if(s.engine === "openrouter" && s.model && s.key){
      n.textContent = "Sent to openrouter.ai using your key. Free models are rate limited and may log prompts.";
    } else if(s.engine === "openrouter" && s.model){
      n.textContent = "Add your free OpenRouter key in Set up before sending.";
    } else {
      n.textContent = "No model chosen yet — open Set up to pick one.";
    }
  }

  /* ================= messages ================= */
  var STARTERS = [
    ["Explain something", "Explain how a rainbow works, for a 12-year-old"],
    ["Fix my writing", "Make this paragraph clearer without changing what it says:"],
    ["Plan it out", "Give me a one-week revision plan for a science test on Friday"],
    ["Understand code", "What does this bit of JavaScript do, line by line?"]
  ];

  U.renderMessages = function(){
    var box = H.clear(H.$("messages"));
    var c = H.convo();
    H.$("topbar-title").textContent = c.title;
    box.classList.toggle("empty", !c.messages.length);

    if(!c.messages.length){
      var op = h("div", { class:"opener" }, [
        h("h1", null, "Free AI, honestly"),
        h("p", null, "Either run a model on your own computer, or use one that is genuinely free. " +
                     "No account with us, because there is no server here to hold one."),
        h("div", { class:"starters" }, STARTERS.map(function(s){
          return h("button", { class:"starter", type:"button", onClick:function(){
            H.$("prompt").value = s[1];
            H.$("prompt").focus();
            grow();
          }}, [h("b", null, s[0]), s[1]]);
        }))
      ]);
      box.appendChild(op);
      return;
    }
    c.messages.forEach(function(m, i){ box.appendChild(bubble(m, i)); });
    box.scrollTop = box.scrollHeight;
  };

  function bubble(m, index){
    var body = h("div", { class:"body" }, [
      h("div", { class:"name" }, m.role === "user" ? "You" : (H.db.settings.modelLabel || "Assistant"))
    ]);
    var prose = h("div", { class:"prose" });
    if(m.role === "user") prose.textContent = m.content;
    else prose.innerHTML = H.markdown(m.content);
    body.appendChild(prose);

    var tools = h("div", { class:"msg-tools" }, [
      h("button", { type:"button", onClick:function(){ H.copy(m.content, "message"); }}, "Copy"),
      m.role === "assistant" && index === H.convo().messages.length - 1
        ? h("button", { type:"button", onClick:regenerate }, "Try again")
        : null
    ]);
    body.appendChild(tools);

    return h("div", { class:"msg " + (m.role === "user" ? "user" : "bot") }, [
      h("div", { class:"who" }, m.role === "user" ? "You" : "AI"),
      body
    ]);
  }

  /* ================= sending ================= */
  function setBusy(on){
    busy = on;
    H.$("send").hidden = on;
    H.$("stop").hidden = !on;
    H.$("prompt").disabled = on;
  }

  U.send = function(text){
    var s = H.db.settings;
    if(!s.engine || s.engine === "none" || !s.model){
      H.toast("Pick a model first.");
      U.show("setup");
      return;
    }
    if(s.engine === "openrouter" && !s.key){
      H.toast("Add your OpenRouter key first.");
      U.show("setup");
      return;
    }
    var c = H.convo();
    c.messages.push({ role:"user", content:text });
    if(c.messages.length === 1) c.title = H.titleFrom(text);
    H.save();
    U.renderConvos();
    U.renderMessages();

    var box = H.$("messages");
    var prose = h("div", { class:"prose" }, h("span", { class:"caret" }));
    var wrap = h("div", { class:"msg bot" }, [
      h("div", { class:"who" }, "AI"),
      h("div", { class:"body" }, [h("div", { class:"name" }, s.modelLabel || "Assistant"), prose])
    ]);
    box.appendChild(wrap);
    box.scrollTop = box.scrollHeight;

    var payload = [];
    if(s.system) payload.push({ role:"system", content:s.system });
    c.messages.forEach(function(m){ payload.push({ role:m.role, content:m.content }); });

    var acc = "";
    var paint = H.rafThrottle(function(){
      prose.innerHTML = H.markdown(acc) + '<span class="caret"></span>';
      box.scrollTop = box.scrollHeight;
    });

    abort = ("AbortController" in window) ? new AbortController() : null;
    setBusy(true);

    H.engines.send(s.engine, payload, {
      model: s.model,
      key: s.key,
      temperature: s.temperature,
      signal: abort ? abort.signal : undefined,
      onToken: function(t){ acc += t; paint(); },
      onProgress: function(text, frac){
        prose.innerHTML = "";
        prose.appendChild(h("p", { class:"note" },
          "Downloading the model to your computer — this happens once. " +
          (frac ? Math.round(frac * 100) + "%  " : "") + (text || "")));
        box.scrollTop = box.scrollHeight;
      }
    }).then(function(){
      finish(acc || "(no answer came back)");
    }, function(err){
      var msg = (err && err.name === "AbortError") ? (acc || "(stopped)")
              : (acc ? acc + "\n\n---\n\n**Stopped:** " + err.message
                     : "**That didn't work.** " + (err && err.message ? err.message : "Unknown error."));
      finish(msg);
    });

    function finish(content){
      setBusy(false);
      abort = null;
      c.messages.push({ role:"assistant", content:content });
      H.save();
      U.renderMessages();
    }
  };

  function regenerate(){
    var c = H.convo();
    if(busy || !c.messages.length) return;
    var last = c.messages[c.messages.length - 1];
    if(last.role !== "assistant") return;
    c.messages.pop();
    var prev = c.messages.pop();
    H.save();
    U.renderMessages();
    if(prev && prev.role === "user") U.send(prev.content);
  }

  /* ================= setup ================= */
  function renderSetup(){
    var root = H.clear(H.$("view-setup"));
    var body = h("div", { class:"panel-in" });
    root.appendChild(h("div", { class:"panel" }, body));
    var s = H.db.settings;

    body.appendChild(h("h1", null, "Set up"));
    body.appendChild(h("p", { class:"lede" },
      "Two ways to get answers. The first costs nothing and never sends your words anywhere. " +
      "The second borrows a provider's free tier using a key you make yourself."));

    /* --- local --- */
    var localCard = h("div", { class:"card" });
    localCard.appendChild(h("h3", null, ["1 · Run it on this computer", h("span", { class:"tag" }, "no key, no bill")]));
    localCard.appendChild(h("p", null,
      "The model is downloaded once and then runs on your graphics card. After that it works with the " +
      "wifi off, and the conversation physically cannot leave the machine."));
    var gpuLine = h("p", { class:"note" }, "Checking what this browser can do…");
    localCard.appendChild(gpuLine);
    var localList = h("div");
    localCard.appendChild(localList);
    body.appendChild(localCard);

    H.engines.webgpu().then(function(r){
      if(!r.ok){
        gpuLine.className = "note warn";
        gpuLine.textContent = r.why + " Local models need it, so use option 2 below.";
        return;
      }
      gpuLine.className = "note";
      gpuLine.textContent = r.why + " Models are listed smallest first — smaller ones download faster and need less memory.";
      var btn = h("button", { class:"btn ghost sm", type:"button", onClick:function(){
        btn.disabled = true; btn.textContent = "Loading the list…";
        H.engines.local.list().then(function(list){
          H.clear(localList);
          list.slice(0, 14).forEach(function(m){
            localList.appendChild(h("div", { class:"model-row" }, [
              h("div", null, [
                h("div", { class:"id" }, m.id),
                h("div", { class:"meta" }, [
                  h("span", null, "needs about " + H.fmtMB(m.vram) + " of video memory"),
                  m.lowRes ? h("span", { class:"tag" }, "runs on modest hardware") : null
                ])
              ]),
              h("div", { class:"acts" }, [
                h("button", { class:"btn sm", type:"button", onClick:function(){
                  s.engine = "local"; s.model = m.id; s.modelLabel = m.id.split("-")[0];
                  H.save(); U.refreshEngine();
                  H.toast("Set. The download starts with your first message.");
                  U.show("chat");
                }}, "Use this")
              ])
            ]));
          });
        }, function(err){
          H.clear(localList);
          localList.appendChild(h("p", { class:"note warn" },
            "Could not load the local-model library: " + err.message +
            ". It is fetched from a CDN, which a sandboxed preview blocks; on the real site it loads."));
        });
      }}, "Show models this machine can run");
      localList.appendChild(btn);
    });

    /* --- openrouter --- */
    var orCard = h("div", { class:"card" });
    orCard.appendChild(h("h3", null, ["2 · Use a genuinely free model online", h("span", { class:"tag warn" }, "needs a free key")]));
    orCard.appendChild(h("p", null,
      "OpenRouter gives one key access to many models, some priced at zero. Make an account, create a key, " +
      "paste it here. It is stored in this browser only and sent to openrouter.ai and nowhere else — there " +
      "is no server here that could collect it."));

    var keyIn = h("input", { class:"inp mono", type:"password", value:s.key || "",
      placeholder:"sk-or-v1-…", "aria-label":"OpenRouter API key" });
    orCard.appendChild(h("label", { class:"lbl", for:"" }, "Your OpenRouter key"));
    orCard.appendChild(keyIn);
    orCard.appendChild(h("div", { class:"row", style:"margin-top:9px" }, [
      h("button", { class:"btn sm", type:"button", onClick:function(){
        s.key = keyIn.value.trim(); s.engine = s.model ? "openrouter" : s.engine;
        H.save(); U.refreshEngine();
        H.toast(s.key ? "Key saved in this browser." : "Key cleared.");
      }}, "Save key"),
      h("button", { class:"btn ghost sm", type:"button", onClick:function(){
        keyIn.value = ""; s.key = ""; H.save(); U.refreshEngine(); H.toast("Key removed.");
      }}, "Remove key"),
      h("a", { class:"btn ghost sm", href:"https://openrouter.ai/keys", target:"_blank",
               rel:"noopener noreferrer", style:"text-decoration:none" }, "Get a key ↗")
    ]));
    orCard.appendChild(h("p", { class:"note warn" },
      "Never put a key into a website you did not build, and never commit one to GitHub. " +
      "Anyone who gets it can spend on your account."));
    orCard.appendChild(h("p", { class:"note" },
      "Current model: " + (s.engine === "openrouter" && s.model ? s.model : "none chosen — pick one under What's free.")));
    body.appendChild(orCard);

    /* --- behaviour --- */
    var beh = h("div", { class:"card" });
    beh.appendChild(h("h3", null, "3 · How it should answer"));
    var sys = h("textarea", { class:"inp", rows:3, "aria-label":"System prompt" });
    sys.value = s.system || "";
    beh.appendChild(h("label", { class:"lbl" }, "Standing instructions"));
    beh.appendChild(sys);
    var temp = h("input", { class:"inp", type:"range", min:"0", max:"1.4", step:"0.1", value:String(s.temperature) });
    var tempOut = h("span", { class:"note", style:"margin:0" }, String(s.temperature));
    temp.addEventListener("input", function(){ tempOut.textContent = temp.value; });
    beh.appendChild(h("label", { class:"lbl", style:"margin-top:10px" }, "Imagination (0 = predictable, 1.4 = wild)"));
    beh.appendChild(temp);
    beh.appendChild(tempOut);
    beh.appendChild(h("div", { class:"row", style:"margin-top:10px" },
      h("button", { class:"btn sm", type:"button", onClick:function(){
        s.system = sys.value; s.temperature = Number(temp.value);
        H.save(); H.toast("Saved.");
      }}, "Save")));
    body.appendChild(beh);

    /* --- data --- */
    var data = h("div", { class:"card" });
    data.appendChild(h("h3", null, "4 · Your data"));
    data.appendChild(h("p", null,
      "Conversations, settings and your key live in this browser's local storage. Clearing your browser data " +
      "clears them. Nothing is backed up anywhere, because there is nowhere to back it up to."));
    data.appendChild(h("div", { class:"row" }, [
      h("button", { class:"btn ghost sm", type:"button", onClick:function(){
        var copy = JSON.parse(JSON.stringify(H.db));
        copy.settings.key = "";      // never write a key into a file the person might share
        var blob = new Blob([JSON.stringify(copy, null, 2)], { type:"application/json" });
        saveBlob(blob, "hearth-chats.json");
      }}, "Download my chats"),
      h("button", { class:"btn ghost sm", type:"button", onClick:function(){
        if(!window.confirm("Delete every conversation and setting on this device?")) return;
        try{ localStorage.removeItem("hearth.v1"); }catch(e){}
        location.reload();
      }}, "Delete everything")
    ]));
    body.appendChild(data);
  }

  function saveBlob(blob, name){
    if(window.claude && typeof window.claude.use === "function"){
      window.claude.use("downloads").then(function(dl){
        if(!dl) return link(blob, name);
        dl.save({ filename:name, data:blob }).then(function(){ H.toast("Saved " + name); },
          function(err){ if(!err || err.code !== "declined") link(blob, name); });
      }, function(){ link(blob, name); });
      return;
    }
    link(blob, name);
  }
  function link(blob, name){
    var url = URL.createObjectURL(blob);
    var a = h("a", { href:url, download:name });
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    setTimeout(function(){ URL.revokeObjectURL(url); }, 5000);
    H.toast("Saved " + name);
  }

  /* ================= about ================= */
  function renderAbout(){
    var root = H.clear(H.$("view-about"));
    var body = h("div", { class:"panel-in" });
    root.appendChild(h("div", { class:"panel" }, body));

    body.appendChild(h("h1", null, "How this works, and what it is not"));
    body.appendChild(h("p", { class:"lede" },
      "There are sites promising the newest paid model, free, right now. This is not one of them, " +
      "and it is worth knowing why they can say it and this cannot."));

    body.appendChild(h("h2", null, "Why nobody can give away the flagship models"));
    body.appendChild(h("p", null,
      "The big models do not run on your computer. They run on racks of expensive machines owned by " +
      "the company that made them, and every message burns real electricity and real rented hardware. " +
      "There is no clever trick that makes that free."));
    body.appendChild(h("p", null, "So when a site offers one free, it is doing one of these:"));
    body.appendChild(h("ul", null, [
      h("li", null, [h("strong", null, "Spending stolen keys. "),
        "Someone's leaked credentials are paying for it. That is theft, and using the site knowingly drags you into it."]),
      h("li", null, [h("strong", null, "Serving something cheaper and lying. "),
        "You type to what you think is a famous model; you get a small open one with a new name painted on."]),
      h("li", null, [h("strong", null, "Selling you. "),
        "Ads, a card number for a “free trial”, your prompts, or malware in a download."])
    ]));
    body.appendChild(h("p", null,
      "A good tell is an impressive version number you cannot find anywhere else. Invented names are cheap; " +
      "running a frontier model is not."));

    body.appendChild(h("h2", null, "What is real"));
    body.appendChild(h("ul", null, [
      h("li", null, [h("strong", null, "Open-weight models you can download. "),
        "Genuinely yours, genuinely free, and they run on your own graphics card. Smaller and less capable " +
        "than the flagships — but private, offline, and unlimited."]),
      h("li", null, [h("strong", null, "Real free tiers. "),
        "Providers do publish models at zero cost. The list in What's free is read live from OpenRouter's " +
        "public endpoint, so it is checkable rather than a promise."]),
      h("li", null, [h("strong", null, "The official free apps. "),
        "The companies themselves offer limited free access to their own assistants. That is the honest route " +
        "to the big models, and it does not need this site at all."])
    ]));

    body.appendChild(h("h2", null, "What this site can see"));
    body.appendChild(h("p", null,
      "Nothing. It is a folder of static files with no server behind it. Conversations and your key stay in " +
      "your browser's storage. When you use a local model, nothing goes over the network at all; when you use " +
      "a free online model, your message goes to that provider and no one else."));

    body.appendChild(h("h2", null, "Honest limitations"));
    body.appendChild(h("ul", null, [
      h("li", null, "Local models need WebGPU — a recent Chrome, Edge or Safari on a computer, not most phones."),
      h("li", null, "The first run downloads several hundred megabytes. Once. On school wifi, be patient."),
      h("li", null, "Small models get things wrong more often than big ones. Check anything that matters."),
      h("li", null, "Free online models are rate limited and can disappear without notice.")
    ]));
    body.appendChild(h("p", { class:"note" },
      "Built with Claude Code. Source on GitHub — you can read every line that touches your key."));
  }

  /* ================= composer plumbing ================= */
  function grow(){
    var t = H.$("prompt");
    t.style.height = "auto";
    t.style.height = Math.min(190, t.scrollHeight) + "px";
  }
  U.init = function(){
    var form = H.$("composer"), ta = H.$("prompt");
    ta.addEventListener("input", grow);
    ta.addEventListener("keydown", function(e){
      if(e.key === "Enter" && !e.shiftKey){ e.preventDefault(); form.requestSubmit(); }
    });
    form.addEventListener("submit", function(e){
      e.preventDefault();
      var text = ta.value.trim();
      if(!text || busy) return;
      ta.value = ""; grow();
      U.send(text);
    });
    H.$("stop").addEventListener("click", function(){
      if(abort) abort.abort();
      setBusy(false);
    });
    H.$("new-chat").addEventListener("click", function(){
      H.newConvo(); U.renderConvos(); U.renderMessages(); U.show("chat");
    });
    Array.prototype.forEach.call(H.$("side-nav").children, function(b){
      b.addEventListener("click", function(){ U.show(b.dataset.view); });
    });
    H.$("engine-chip").addEventListener("click", function(){ U.show("setup"); });
    H.$("menu-btn").addEventListener("click", openSide);
    H.$("scrim").addEventListener("click", closeSide);
  };
  function openSide(){ H.$("side").classList.add("open"); H.$("scrim").hidden = false; }
  function closeSide(){ H.$("side").classList.remove("open"); H.$("scrim").hidden = true; }

  H.ui = U;
})();
