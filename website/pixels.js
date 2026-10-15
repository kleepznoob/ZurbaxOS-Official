// zurbax.com: the pixel style on every page, as Zurbax moves. Pictures come
// out of the dark block by block in the ordered pattern windows open in;
// the pointer is drawn in pixels; a bar of blocks fills as you read; buttons
// throw pixel sparks; small headings type themselves out with a block for a
// cursor. Nothing moves when the system asks for less motion.
(() => {
  const root = document.documentElement;
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const fine = matchMedia('(pointer: fine)').matches;
  const BAYER = [0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21];
  const turn = (x, y) => (BAYER[(y & 7) * 8 + (x & 7)] + 0.5) / 64;
  const css = name => getComputedStyle(root).getPropertyValue(name).trim();

  // ---- the pointer, in pixels: an arrow, and a hand over what can be pressed
  const ARROW = ['X..........', 'XX.........', 'XWX........', 'XWWX.......', 'XWWWX......', 'XWWWWX.....', 'XWWWWWX....', 'XWWWWWWX...',
    'XWWWWWWWX..', 'XWWWWWWWWX.', 'XWWWWWXXXXX', 'XWWXWWX....', 'XWX.XWWX...', 'XX..XWWX...', 'X....XWWX..', '.....XXX...'];
  const HAND = ['....XX........', '...XWWX.......', '...XWWX.......', '...XWWX.......', '...XWWXXX.....', '...XWWXWWXXX..',
    '.XXXWWXWWXWWXX', 'XWWXWWWWWWWWWX', 'XWWWWWWWWWWWWX', '.XWWWWWWWWWWWX', '..XWWWWWWWWWWX', '..XWWWWWWWWWX.',
    '...XWWWWWWWWX.', '...XWWWWWWWX..', '....XWWWWWWX..', '....XXXXXXXX..'];
  const cursor = (rows, hx, hy, fill) => {
    const s = 2;
    let rects = '';
    rows.forEach((r, y) => [...r].forEach((c, x) => {
      if (c !== '.') rects += `<rect x="${x * s}" y="${y * s}" width="${s}" height="${s}" fill="${c === 'X' ? '#0b0b0b' : fill}"/>`;
    }));
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${rows[0].length * s}" height="${rows.length * s}" shape-rendering="crispEdges">${rects}</svg>`;
    return `url("data:image/svg+xml,${encodeURIComponent(svg)}") ${hx * s} ${hy * s}`;
  };
  if (fine) {
    root.style.setProperty('--cur-arrow', cursor(ARROW, 0, 0, '#f2f2f2') + ', auto');
    root.style.setProperty('--cur-hand', cursor(HAND, 4, 0, '#f2f2f2') + ', pointer');
    root.classList.add('px-cursor');
  }

  // ---- pictures come out of the dark in blocks, when they're first seen
  const B = 14; /* a block, in CSS pixels */
  const veils = [];
  const veil = img => {
    const host = document.createElement('span');
    host.className = 'px-host';
    img.replaceWith(host);
    host.append(img);
    const cv = document.createElement('canvas');
    cv.className = 'px-veil';
    cv.setAttribute('aria-hidden', 'true');
    host.append(cv);
    const v = { host, cv, cx: cv.getContext('2d'), w: 0, h: 0, start: 0, done: false };
    v.size = () => {
      const r = host.getBoundingClientRect();
      v.w = cv.width = Math.max(1, Math.ceil(r.width / B));
      v.h = cv.height = Math.max(1, Math.ceil(r.height / B));
      v.paint(0);
    };
    v.paint = k => {
      const { cx, w, h } = v, dark = css('--deep') || '#0b0b0b', edge = css('--brand') || '#e68e0d', edge2 = css('--b1') || '#fff';
      cx.clearRect(0, 0, w, h);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const t = turn(x, y);
        if (t >= k) cx.fillStyle = dark;
        else if (t > k - 0.04) cx.fillStyle = edge2;
        else if (t > k - 0.1) cx.fillStyle = edge;
        else continue;
        cx.fillRect(x, y, 1, 1);
      }
    };
    v.play = () => {
      if (v.start) return;
      v.start = performance.now();
      const step = now => {
        /* in steps, sixteen of them, like the system's mosaic */
        const k = Math.floor(((now - v.start) / 700) * 16) / 16 * 1.18;
        if (k >= 1.18) { cv.remove(); v.done = true; return; }
        v.paint(k);
        requestAnimationFrame(step);
      };
      requestAnimationFrame(step);
    };
    veils.push(v);
    return v;
  };
  if (!still) {
    const targets = document.querySelectorAll('.screen img, .card img, .g-figure img, .theme-grid img');
    const seen = new IntersectionObserver(entries => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        const v = veils.find(v => v.host === e.target);
        seen.unobserve(e.target);
        if (!v) continue;
        const img = v.host.querySelector('img');
        const go = () => setTimeout(v.play, 120);
        if (img.complete) go(); else img.addEventListener('load', go, { once: true });
      }
    }, { threshold: 0.2 });
    targets.forEach(img => { const v = veil(img); v.size(); seen.observe(v.host); });
    addEventListener('resize', () => veils.forEach(v => { if (!v.done && !v.start) v.size(); }));
  }

  // ---- a bar of blocks under the top bar that fills as you read
  const bar = document.querySelector('.bar');
  if (bar) {
    const prog = document.createElement('div');
    prog.className = 'px-progress';
    prog.setAttribute('aria-hidden', 'true');
    bar.append(prog);
    const STEP = 14;
    const sync = () => {
      const max = root.scrollHeight - innerHeight;
      const f = max > 0 ? Math.min(1, scrollY / max) : 0;
      prog.style.width = `${Math.round((f * innerWidth) / STEP) * STEP}px`;
    };
    addEventListener('scroll', sync, { passive: true });
    addEventListener('resize', sync);
    sync();
  }

  // ---- sparks: pressing a button throws pixels, as Zurbax's tick does
  if (!still) {
    const cv = document.createElement('canvas');
    cv.className = 'px-sparks';
    cv.setAttribute('aria-hidden', 'true');
    document.body.append(cv);
    const cx = cv.getContext('2d');
    let sparks = [], raf = 0;
    const P = 5;
    const resize = () => { cv.width = Math.ceil(innerWidth / P); cv.height = Math.ceil(innerHeight / P); };
    resize();
    addEventListener('resize', resize);
    const frame = now => {
      cx.clearRect(0, 0, cv.width, cv.height);
      sparks = sparks.filter(s => now - s.t0 < s.life);
      for (const s of sparks) {
        const u = (now - s.t0) / 1000;
        /* positions in whole blocks, and the time in steps of a twelfth of a second */
        const q = Math.floor(u * 12) / 12;
        const x = Math.round((s.x + s.vx * q) / P), y = Math.round((s.y + s.vy * q + 260 * q * q) / P);
        cx.globalAlpha = 1 - Math.floor(((now - s.t0) / s.life) * 4) / 4;
        cx.fillStyle = s.c;
        cx.fillRect(x, y, s.big ? 2 : 1, s.big ? 2 : 1);
      }
      cx.globalAlpha = 1;
      raf = sparks.length ? requestAnimationFrame(frame) : 0;
    };
    const burst = (x, y, n) => {
      const cols = ['--b1', '--b2', '--b3', '--b4', '--brand-2'].map(css).filter(Boolean);
      const t0 = performance.now();
      for (let i = 0; i < n; i++) {
        const a = (i / n) * Math.PI * 2 + Math.random() * 0.5, sp = 120 + Math.random() * 220;
        sparks.push({ x, y, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp - 120, c: cols[i % cols.length], t0, life: 600 + Math.random() * 400, big: i % 3 === 0 });
      }
      if (!raf) raf = requestAnimationFrame(frame);
    };
    document.addEventListener('click', e => {
      const b = e.target.closest('.btn, .theme-grid button, .picker-list button');
      if (!b) return;
      const r = b.getBoundingClientRect();
      const x = e.clientX || r.left + r.width / 2, y = e.clientY || r.top + r.height / 2;
      burst(x, y, b.matches('[data-get]') ? 40 : 18);
    });
  }

  // ---- small headings type themselves out, with a block for a cursor
  if (!still) {
    const typed = new IntersectionObserver(entries => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        typed.unobserve(e.target);
        const el = e.target, chars = [...el.querySelectorAll('.tw')];
        let i = 0;
        el.classList.add('typing');
        const tick = () => {
          if (i > 0) chars[i - 1].classList.remove('tw-last');
          if (i < chars.length) {
            chars[i].classList.add('on', 'tw-last');
            i++;
            setTimeout(tick, chars[i - 1].textContent === ' ' ? 20 : 34);
          } else {
            chars[chars.length - 1].classList.add('tw-last');
            setTimeout(() => { el.classList.remove('typing'); chars[chars.length - 1].classList.remove('tw-last'); }, 1600);
          }
        };
        setTimeout(tick, 150);
      }
    }, { threshold: 0.6 });
    document.querySelectorAll('.kicker, .g-hero .version').forEach(el => {
      const text = el.textContent;
      el.classList.add('tw-ready');
      el.setAttribute('aria-label', text);
      el.innerHTML = [...text].map(c => `<span class="tw" aria-hidden="true">${c === ' ' ? ' ' : c.replace(/[&<>]/g, m => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[m])}</span>`).join('');
      typed.observe(el);
    });
  }
})();
