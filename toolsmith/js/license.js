/* ============================================================
   Toolsmith licensing.

   HOW TO SWITCH THE PAYWALL ON
   ----------------------------
   Until STORE.checkoutUrl and STORE.productId are filled in, NOTHING is
   locked — the app ships as a complete free tool. The moment a grown-up
   creates the Gumroad product and pastes those two values in, the Pro
   limits below start applying and the unlock dialog starts working.

   That order is deliberate: a buy button that leads nowhere is worse than
   no buy button, so the paywall stays off until there is something to buy.

   NOT YET TESTED END-TO-END: verification talks to Gumroad's real API, and
   there is no product to test against yet. Make one test purchase with a
   free (100%-off) discount code before telling anyone the store is open.
   ============================================================ */
(function(){
  "use strict";
  var h = TS.h;

  var STORE = {
    checkoutUrl: "",     // e.g. "https://yourname.gumroad.com/l/toolsmith"
    productId:   "",     // Gumroad product ID, from the product's Settings tab
    price:       "£9",
    terms:       "one payment, yours forever"
  };

  var KEY = "toolsmith.license";
  var REVERIFY_AFTER = 30 * 24 * 3600 * 1000;   // re-check monthly, work offline between

  // Free is generous on purpose. People who hit these limits are doing real
  // volume work — they are the ones for whom £9 is obviously worth it.
  var LIMITS = {
    free: { batchFiles: 5,      exportPx: 2400,   pdfPages: 20,       presets: false },
    pro:  { batchFiles: Infinity, exportPx: Infinity, pdfPages: Infinity, presets: true }
  };

  var state = load();

  function load(){
    try{
      var raw = localStorage.getItem(KEY);
      if(raw){
        var v = JSON.parse(raw);
        if(v && v.key) return v;
      }
    }catch(e){}
    return null;
  }
  function store(v){
    state = v;
    try{ v ? localStorage.setItem(KEY, JSON.stringify(v)) : localStorage.removeItem(KEY); }catch(e){}
    paint();
  }

  var L = {};
  L.STORE = STORE;

  /** Is the paywall switched on at all? */
  L.enforced = function(){ return !!(STORE.checkoutUrl && STORE.productId); };

  /** Pro if the paywall is off (everything free) or a key checked out. */
  L.isPro = function(){ return !L.enforced() || !!(state && state.ok); };

  L.limit = function(name){
    return (L.isPro() ? LIMITS.pro : LIMITS.free)[name];
  };

  /** True when the action is allowed; otherwise nudges and returns false. */
  L.allow = function(name, wanted, label){
    var cap = L.limit(name);
    if(wanted == null || wanted <= cap) return true;
    L.unlockSheet(label || "That");
    return false;
  };

  /* ---------------- verification ---------------- */
  L.verify = function(rawKey){
    var key = String(rawKey || "").trim();
    if(!key) return Promise.reject(new Error("Enter the licence key from your receipt."));
    if(!L.enforced()) return Promise.reject(new Error("The store is not set up yet."));

    var body = new URLSearchParams();
    body.set("product_id", STORE.productId);
    body.set("license_key", key);
    body.set("increment_uses_count", "false");

    return fetch("https://api.gumroad.com/v2/licenses/verify", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: body.toString()
    }).then(function(r){ return r.json().catch(function(){ return {}; }); })
      .then(function(data){
        if(!data || !data.success) throw new Error("That key didn't match a purchase. Check for typos, or paste it straight from the receipt email.");
        var p = data.purchase || {};
        if(p.refunded || p.chargebacked) throw new Error("That purchase was refunded, so the key no longer works.");
        store({ key: key, ok: true, at: Date.now(), email: p.email || "" });
        return true;
      }, function(err){
        // Offline or blocked: a key that checked out before keeps working.
        if(state && state.ok && state.key === key){
          store({ key: key, ok: true, at: state.at, email: state.email });
          return true;
        }
        throw new Error(err.message || "Couldn't reach the store. Try again when you're online.");
      });
  };

  L.deactivate = function(){
    store(null);
    TS.toast("Licence removed from this browser.");
  };

  // Quiet monthly re-check. Failure never downgrades anyone — being offline
  // is not the same as not having paid.
  L.refresh = function(){
    if(!L.enforced() || !state || !state.ok) return;
    if(Date.now() - (state.at || 0) < REVERIFY_AFTER) return;
    L.verify(state.key).catch(function(){});
  };

  /* ---------------- the unlock dialog ---------------- */
  L.unlockSheet = function(what){
    TS.sheet("Unlock Toolsmith Pro", function(body){
      body.appendChild(h("p", null,
        what ? what + " is a Pro feature. Everything else in Toolsmith stays free, forever." :
               "Pro lifts the limits. Everything else in Toolsmith stays free, forever."));

      var t = h("table", null, [
        h("tr", null, [h("th", null, ""), h("th", null, "Free"), h("th", null, "Pro")]),
        h("tr", null, [h("td", null, "Batch at once"), h("td", null, "5 files"), h("td", null, "no limit")]),
        h("tr", null, [h("td", null, "Export size"), h("td", null, "2400 px"), h("td", null, "full resolution")]),
        h("tr", null, [h("td", null, "PDF pages"), h("td", null, "20"), h("td", null, "no limit")]),
        h("tr", null, [h("td", null, "Saved presets"), h("td", null, "—"), h("td", null, "yes")]),
        h("tr", null, [h("td", null, "Every tool, no watermark"), h("td", null, "yes"), h("td", null, "yes")])
      ]);
      body.appendChild(t);

      body.appendChild(h("p", null, [
        h("strong", null, STORE.price + " — " + STORE.terms + "."),
        " No subscription. Compare that with £20 a month, every month, forever."
      ]));

      if(!L.enforced()){
        body.appendChild(h("p", { class: "note warn" },
          "The shop isn't open yet, so nothing is locked — you already have everything."));
        return;
      }

      body.appendChild(TS.btn("Buy Toolsmith Pro — " + STORE.price, { onClick: function(){
        window.open(STORE.checkoutUrl, "_blank", "noopener");
      }}));

      var input = h("input", { class: "inp mono", placeholder: "Paste licence key from your receipt", "aria-label": "Licence key" });
      var msg = h("p", { class: "note" }, "Already bought it? Paste the key from your receipt email.");
      var go = TS.btn("Unlock", { kind: "ghost", onClick: function(){
        go.disabled = true; go.textContent = "Checking…";
        L.verify(input.value).then(function(){
          TS.closeSheet();
          TS.toast("Pro unlocked. Thank you.");
        }, function(err){
          msg.textContent = err.message;
          msg.className = "note warn";
        }).then(function(){ go.disabled = false; go.textContent = "Unlock"; });
      }});
      body.appendChild(h("div", { class: "stack", style: "margin-top:14px" }, [input, go, msg]));
    });
  };

  /* ---------------- the badge ---------------- */
  function paint(){
    var badge = TS.$("plan-badge");
    if(!badge) return;
    if(!L.enforced()){ badge.hidden = true; return; }
    badge.hidden = false;
    var pro = L.isPro();
    badge.textContent = pro ? "Pro" : "Free";
    badge.className = "plan-badge" + (pro ? " pro" : "");
    badge.style.cursor = "pointer";
    badge.onclick = function(){ L.unlockSheet(null); };
  }
  L.paint = paint;

  /** A standard "this is Pro" strip to drop under a locked control. */
  L.lockNote = function(text, what){
    if(L.isPro()) return null;
    return h("div", { class: "lock-note" }, [
      h("span", null, text),
      h("button", { type: "button", onClick: function(){ L.unlockSheet(what); } }, "Unlock")
    ]);
  };

  TS.license = L;
})();
