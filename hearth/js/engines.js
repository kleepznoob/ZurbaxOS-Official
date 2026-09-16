/* ============================================================
   Engines — the two honest ways to get a model to answer.

   openrouter : the provider's own free tier, using a key the person
                creates themselves. The key lives in their browser and
                is sent to openrouter.ai and nowhere else. There is no
                server here that could collect it.
   local      : the model is downloaded once and runs on their GPU
                through WebLLM. No key, no bill, no network after the
                download, and the conversation never leaves the machine.
   ============================================================ */
(function(){
  "use strict";
  var H = window.H;

  var WEBLLM_URL = "https://cdn.jsdelivr.net/npm/@mlc-ai/web-llm@0.2.79/+esm";
  var OR_URL = "https://openrouter.ai/api/v1/chat/completions";

  var E = { local: {}, openrouter: {} };

  /* ---------------- capability probe ---------------- */
  var gpuProbe = null;
  E.webgpu = function(){
    if(gpuProbe) return gpuProbe;
    gpuProbe = (async function(){
      if(!("gpu" in navigator)) return { ok:false, why:"This browser has no WebGPU. Chrome, Edge or Arc on a desktop is the usual fix; Safari needs version 18 or newer." };
      try{
        var a = await navigator.gpu.requestAdapter();
        if(!a) return { ok:false, why:"WebGPU is present but no graphics adapter was offered, which usually means the GPU is unavailable to the browser." };
        return { ok:true, why:"WebGPU ready." };
      }catch(err){
        return { ok:false, why:"WebGPU check failed: " + err.message };
      }
    })();
    return gpuProbe;
  };

  /* ================= OpenRouter ================= */
  E.openrouter.send = function(messages, o){
    var body = {
      model: o.model,
      messages: messages,
      stream: true,
      temperature: typeof o.temperature === "number" ? o.temperature : 0.7
    };
    return fetch(OR_URL, {
      method: "POST",
      signal: o.signal,
      headers: {
        "Content-Type": "application/json",
        "Authorization": "Bearer " + o.key,
        // OpenRouter uses these to attribute traffic; they are not tracking.
        "HTTP-Referer": location.origin,
        "X-Title": "Hearth"
      },
      body: JSON.stringify(body)
    }).then(function(res){
      if(!res.ok){
        return res.text().then(function(t){
          var msg = "";
          try{ msg = (JSON.parse(t).error || {}).message || ""; }catch(e){}
          throw new Error(explain(res.status, msg));
        });
      }
      if(!res.body) throw new Error("This browser cannot stream responses.");
      return pump(res.body.getReader(), o.onToken);
    }, function(err){
      // fetch() rejects before any status when the connection is refused outright.
      if(err && err.name === "AbortError") throw err;
      throw new Error(offlineNote("openrouter.ai", err));
    });
  };

  function offlineNote(host, err){
    var base = "Could not reach " + host + ".";
    if(!navigator.onLine) return base + " This device looks offline.";
    return base + " If you are viewing this inside an embedded preview, outside connections are blocked " +
           "there by design — open the real site instead. Otherwise check the connection. (" +
           ((err && err.message) || "network error") + ")";
  }

  function explain(status, msg){
    if(status === 401) return "That key was rejected. Check you pasted the whole thing, including the sk-or- prefix.";
    if(status === 402) return "This model needs credit on the account. Pick one marked free instead.";
    if(status === 403) return "The provider refused this request" + (msg ? ": " + msg : ".");
    if(status === 404) return "That model id no longer exists. Refresh the free list and pick another.";
    if(status === 429) return "Rate limited — free models cap how often you can ask. Wait a minute, or pick another free model.";
    if(status >= 500) return "The provider is having trouble right now. Try again shortly.";
    return msg || ("Request failed (HTTP " + status + ").");
  }

  function pump(reader, onToken){
    var dec = new TextDecoder(), buf = "", full = "";
    function step(){
      return reader.read().then(function(r){
        if(r.done) return full;
        buf += dec.decode(r.value, { stream:true });
        var i;
        while((i = buf.indexOf("\n")) >= 0){
          var line = buf.slice(0, i).trim();
          buf = buf.slice(i + 1);
          if(!line || line.charAt(0) === ":") continue;      // keep-alive comment
          if(line.indexOf("data:") !== 0) continue;
          var data = line.slice(5).trim();
          if(data === "[DONE]") return full;
          try{
            var j = JSON.parse(data);
            var d = j.choices && j.choices[0] && j.choices[0].delta;
            if(d && d.content){ full += d.content; onToken(d.content); }
          }catch(e){ /* a partial frame: the next read completes it */ }
        }
        return step();
      });
    }
    return step();
  }

  /* ================= Local (WebLLM) ================= */
  var lib = null, engine = null, loadedModel = "";

  E.local.library = function(){
    if(lib) return Promise.resolve(lib);
    return import(/* webpackIgnore: true */ WEBLLM_URL).then(function(m){ lib = m; return m; },
      function(err){
        throw new Error(offlineNote("the WebLLM library on jsdelivr", err) +
          " Local models need that download the first time.");
      });
  };

  E.local.list = function(){
    return E.local.library().then(function(m){
      var all = (m.prebuiltAppConfig && m.prebuiltAppConfig.model_list) || [];
      return all.map(function(x){
        return {
          id: x.model_id,
          vram: x.vram_required_MB || 0,
          lowRes: !!x.low_resource_required
        };
      }).filter(function(x){ return x.vram > 0; })
        .sort(function(a,b){ return a.vram - b.vram; });
    });
  };

  E.local.loaded = function(){ return loadedModel; };

  E.local.prepare = function(modelId, onProgress){
    if(engine && loadedModel === modelId) return Promise.resolve(engine);
    return E.local.library().then(function(m){
      return m.CreateMLCEngine(modelId, {
        initProgressCallback: function(p){
          if(onProgress) onProgress(p.text || "", typeof p.progress === "number" ? p.progress : 0);
        }
      });
    }).then(function(eng){
      engine = eng;
      loadedModel = modelId;
      return eng;
    });
  };

  E.local.send = function(messages, o){
    return E.local.prepare(o.model, o.onProgress).then(function(eng){
      return eng.chat.completions.create({
        messages: messages,
        stream: true,
        temperature: typeof o.temperature === "number" ? o.temperature : 0.7
      });
    }).then(function(stream){
      var full = "";
      function next(it){
        return it.next().then(function(step){
          if(step.done) return full;
          if(o.signal && o.signal.aborted) return full;
          var c = step.value.choices && step.value.choices[0];
          var d = c && c.delta && c.delta.content;
          if(d){ full += d; o.onToken(d); }
          return next(it);
        });
      }
      var iter = stream[Symbol.asyncIterator] ? stream[Symbol.asyncIterator]() : stream;
      return next(iter);
    });
  };

  /* ---------------- dispatch ---------------- */
  E.send = function(kind, messages, o){
    if(kind === "openrouter") return E.openrouter.send(messages, o);
    if(kind === "local") return E.local.send(messages, o);
    return Promise.reject(new Error("Pick a model first — open Set up in the sidebar."));
  };

  H.engines = E;
})();
