// zurbax.com: the page in Hive's themes, its moving pixels, and the download
(() => {
  const root = document.documentElement;
  root.classList.replace('no-js', 'js');
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;

  // Hive's eight themes, as the system has them: id, name, background, text, the two accents
  const THEMES = [
    ['tokyo-night', 'Tokyo Night', '#1a1b26', '#c0caf5', '#7aa2f7', '#bb9af7'],
    ['catppuccin', 'Catppuccin', '#1e1e2e', '#cdd6f4', '#cba6f7', '#89b4fa'],
    ['gruvbox', 'Gruvbox', '#282828', '#ebdbb2', '#fe8019', '#fabd2f'],
    ['nord', 'Nord', '#2e3440', '#eceff4', '#88c0d0', '#b48ead'],
    ['everforest', 'Everforest', '#2d353b', '#d3c6aa', '#a7c080', '#83c092'],
    ['kanagawa', 'Kanagawa', '#1f1f28', '#dcd7ba', '#7e9cd8', '#d27e99'],
    ['rose-pine', 'Rosé Pine', '#191724', '#e0def4', '#ebbcba', '#c4a7e7'],
    ['matte-black', 'Matte Black', '#121212', '#e6e6e6', '#e68e0d', '#d35f5f'],
  ];
  const byId = id => THEMES.find(t => t[0] === id) || THEMES[0];
  const swatch = t => `<span class="sw" aria-hidden="true"><i style="background:${t[4]}"></i><i style="background:${t[5]}"></i><i style="background:${t[3]}"></i></span>`;

  const picker = document.querySelector('.picker');
  const pickBtn = picker.querySelector('.picker-btn');
  const pickList = picker.querySelector('.picker-list');
  const grid = document.querySelector('.theme-grid');
  const shot = document.querySelector('.screen .shot');

  pickList.innerHTML = THEMES.map(t => `<li><button type="button" role="menuitemradio" data-theme="${t[0]}">${swatch(t)}${t[1]}</button></li>`).join('');
  grid.innerHTML = THEMES.map(t => `<li><button type="button" data-theme="${t[0]}" aria-pressed="false">
    <img src="/images/hive/bit-${t[0]}.png" alt="${t[1]}: its pixel-art wallpaper" loading="lazy" width="1600" height="900">
    <span class="meta">${t[1]}${swatch(t)}</span></button></li>`).join('');

  // a theme for the whole page: colours, the hero's wallpaper and screen, the tab's colour
  const apply = (id, keep) => {
    const t = byId(id);
    root.dataset.theme = t[0];
    root.style.setProperty('--wall', `url("/images/hive/bit-${t[0]}.png")`);
    document.querySelector('meta[name=theme-color]').content = t[2];
    picker.querySelector('.picker-name').textContent = t[1];
    pickList.querySelectorAll('button').forEach(b => b.setAttribute('aria-checked', String(b.dataset.theme === t[0])));
    grid.querySelectorAll('button').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.theme === t[0])));
    const src = `/images/hive/hive-${t[0]}.jpg`;
    if (!shot.src.endsWith(src)) {
      shot.classList.add('fading');
      const img = new Image();
      img.onload = () => { shot.src = src; shot.classList.remove('fading'); };
      img.src = src;
    }
    if (keep) try { localStorage.setItem('zurbax-theme', t[0]); } catch (e) {}
    stars.recolour();
  };

  const closeList = () => { pickList.hidden = true; pickBtn.setAttribute('aria-expanded', 'false'); };
  pickBtn.addEventListener('click', e => {
    e.stopPropagation();
    pickList.hidden = !pickList.hidden;
    pickBtn.setAttribute('aria-expanded', String(!pickList.hidden));
  });
  document.addEventListener('click', closeList);
  document.addEventListener('keydown', e => { if (e.key === 'Escape') closeList(); });
  pickList.addEventListener('click', e => { const b = e.target.closest('button'); if (b) { apply(b.dataset.theme, true); closeList(); } });
  grid.addEventListener('click', e => { const b = e.target.closest('button'); if (b) apply(b.dataset.theme, true); });

  // the hero's stars: big square pixels that twinkle, as on the wallpapers
  const stars = (() => {
    const cv = document.querySelector('.stars');
    const cx = cv.getContext('2d');
    let w = 0, h = 0, px = 4, list = [], colour = '#fff', raf = 0, shooting = null;
    const size = () => {
      const r = cv.getBoundingClientRect();
      w = cv.width = Math.round(r.width / px); h = cv.height = Math.round(r.height / px);
      list = Array.from({ length: Math.round(w * h / 260) }, () => ({
        x: Math.floor(Math.random() * w), y: Math.floor(Math.pow(Math.random(), 1.5) * h * 0.55),
        rate: 0.8 + Math.random() * 2.2, phase: Math.random() * 6.3,
      }));
    };
    const draw = now => {
      const t = now / 1000;
      cx.clearRect(0, 0, w, h);
      cx.fillStyle = colour;
      for (const s of list) {
        const v = 0.5 + 0.5 * Math.sin(t * s.rate + s.phase);
        cx.globalAlpha = v > 0.88 ? 1 : v > 0.45 ? 0.75 : 0.25;
        cx.fillRect(s.x, s.y, 1, 1);
        if (v > 0.88) { cx.globalAlpha = 0.45; cx.fillRect(s.x - 1, s.y, 3, 1); cx.fillRect(s.x, s.y - 1, 1, 3); }
      }
      // now and then a star shoots across
      if (!shooting && Math.random() < 0.004) shooting = { x: Math.random() * w * 0.6, y: Math.random() * h * 0.2, t0: t };
      if (shooting) {
        const u = (t - shooting.t0) / 1.1;
        if (u > 1) shooting = null;
        else for (let j = 0; j < 8; j++) {
          const p = u * 34 - j;
          if (p < 0) break;
          cx.globalAlpha = 1 - j / 8;
          cx.fillRect(Math.floor(shooting.x + p * 2), Math.floor(shooting.y + p), 2, 1);
        }
      }
      cx.globalAlpha = 1;
      raf = requestAnimationFrame(draw);
    };
    const api = {
      recolour() { colour = getComputedStyle(root).getPropertyValue('--text').trim() || '#fff'; },
      start() { size(); api.recolour(); if (still) { draw(0); cancelAnimationFrame(raf); } else raf = requestAnimationFrame(draw); },
    };
    addEventListener('resize', () => size());
    // drawing only while the hero is on screen
    new IntersectionObserver(([e]) => {
      if (still) return;
      if (e.isIntersecting && !raf) raf = requestAnimationFrame(draw);
      else if (!e.isIntersecting) { cancelAnimationFrame(raf); raf = 0; }
    }).observe(cv);
    return api;
  })();
  stars.start();
  apply(root.dataset.theme, false);

  // a window arriving block by block, in the ordered pattern Zurbax uses
  const BAYER = [0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21];
  const pg = document.querySelector('.pix-grid');
  const cells = [];
  for (let y = 0; y < 10; y++) for (let x = 0; x < 16; x++) {
    const i = document.createElement('i');
    if (y < 2) i.className = 'top';
    if (y === 0 && x > 12) i.className = 'edge';
    const turn = (BAYER[(y & 7) * 8 + (x & 7)] + 0.5) / 64;
    i.style.setProperty('--d', `${(turn * 0.32).toFixed(3)}s`);
    cells.push(i);
  }
  pg.append(...cells);

  // rising into place, and the demo playing once it's seen
  const seen = new IntersectionObserver(entries => {
    for (const e of entries) if (e.isIntersecting) { e.target.classList.add('in', 'play'); seen.unobserve(e.target); }
  }, { rootMargin: '0px 0px -60px 0px', threshold: 0.12 });
  document.querySelectorAll('.rise').forEach(el => seen.observe(el));

  // the download: it starts, and the next steps come up
  const next = document.getElementById('next');
  document.querySelectorAll('[data-get]').forEach(a => a.addEventListener('click', () => {
    setTimeout(() => { next.hidden = false; }, 400);
  }));
  next.addEventListener('click', e => { if (e.target === next || e.target.closest('[data-close]')) next.hidden = true; });
})();
