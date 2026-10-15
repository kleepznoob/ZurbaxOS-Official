// zurbax.com: the page in Hive's themes, its moving pixels, and the download
(() => {
  const root = document.documentElement;
  root.classList.replace('no-js', 'js');
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  // the ordered pattern things build up in, here as in Zurbax (an 8x8 Bayer matrix)
  const BAYER = [0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21];

  // Hive's themes, as the system has them: id, name, background, text, the two accents
  const THEMES = [
    ['carbon', 'Carbon', '#121212', '#e6e6e6', '#e68e0d', '#d35f5f'],
    ['midnight', 'Midnight', '#1a1b26', '#c0caf5', '#7aa2f7', '#bb9af7'],
  ];
  const OLD = { 'matte-black': 'carbon', 'tokyo-night': 'midnight' }; /* what they were called */
  const byId = id => THEMES.find(t => t[0] === (OLD[id] || id)) || THEMES[0];
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

  // the name in big pixels: letters of 11 by 13, bands of colour from top to
  // bottom with their edges dithered, a light along each top edge and a
  // shadow a pixel down and right; it builds up block by block, in the
  // ordered pattern Zurbax's windows use
  const LETTERS = {
    Z: ['.XXXXXXXXX.', 'XXXXXXXXXXX', 'XXXXXXXXXXX', '......XXXX.', '.....XXXX..', '....XXXX...', '...XXXX....', '..XXXX.....', '.XXXX......', 'XXXX.......', 'XXXXXXXXXXX', 'XXXXXXXXXXX', '.XXXXXXXXX.'],
    U: ['XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXXX...XXXX', '.XXXXXXXXX.', '..XXXXXXX..'],
    R: ['XXXXXXXXX..', 'XXXXXXXXXX.', 'XXX....XXXX', 'XXX.....XXX', 'XXX....XXXX', 'XXXXXXXXXX.', 'XXXXXXXXX..', 'XXX..XXXX..', 'XXX...XXXX.', 'XXX....XXX.', 'XXX....XXXX', 'XXX.....XXX', 'XXX.....XXX'],
    B: ['XXXXXXXXX..', 'XXXXXXXXXX.', 'XXX....XXXX', 'XXX.....XXX', 'XXX....XXXX', 'XXXXXXXXXX.', 'XXXXXXXXXX.', 'XXX....XXXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX....XXXX', 'XXXXXXXXXX.', 'XXXXXXXXX..'],
    A: ['...XXXXX...', '..XXXXXXX..', '.XXXX.XXXX.', 'XXXX...XXXX', 'XXX.....XXX', 'XXX.....XXX', 'XXXXXXXXXXX', 'XXXXXXXXXXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX', 'XXX.....XXX'],
    X: ['XXX.....XXX', 'XXX.....XXX', 'XXXX...XXXX', '.XXXX.XXXX.', '..XXXXXXX..', '...XXXXX...', '....XXX....', '...XXXXX...', '..XXXXXXX..', '.XXXX.XXXX.', 'XXXX...XXXX', 'XXX.....XXX', 'XXX.....XXX'],
  };
  const BAND = [1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5]; /* the band of each row */
  (() => {
    const svg = document.querySelector('.pixel-mark');
    if (!svg) return;
    const word = 'ZURBAX', W = 11, H = 13, GAP = 2;
    const cells = [];
    [...word].forEach((ch, li) => {
      const g = LETTERS[ch];
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++)
        if (g[y][x] === 'X') cells.push({
          x: li * (W + GAP) + x, y,
          top: y === 0 || g[y - 1][x] !== 'X',
          /* a darker edge where the stroke ends on the right or below: the letters stand up */
          low: x === W - 1 || g[y][x + 1] !== 'X' || y === H - 1 || g[y + 1][x] !== 'X',
        });
    });
    const turn = (x, y) => (BAYER[(y & 7) * 8 + (x & 7)] + 0.5) / 64;
    const r = (x, y, cls, d) => `<rect x="${x}" y="${y}" width="1.04" height="1.04" class="${cls}" style="--d:${d}s"/>`;
    let shade = '', body = '', light = '', dark = '';
    for (const c of cells) {
      const d = (turn(c.x, c.y) * 0.6).toFixed(3);
      let b = BAND[c.y];
      /* where one band meets the next, the two in a checker */
      if (c.y > 0 && BAND[c.y - 1] !== b && (c.x + c.y) % 2) b = BAND[c.y - 1];
      shade += r(c.x + 1, c.y + 1, 'sh', d);
      body += r(c.x, c.y, `px b${b}`, d);
      if (c.top) light += r(c.x, c.y, 'px hl', d);
      else if (c.low) dark += r(c.x, c.y, 'px dk', d);
    }
    const width = word.length * (W + GAP) - GAP + 1;
    svg.setAttribute('viewBox', `0 0 ${width} ${H + 1}`);
    svg.innerHTML = shade + body + dark + light;
    if (!still) svg.classList.add('build');
  })();

  apply(root.dataset.theme, false);

  // a window arriving block by block, in the ordered pattern Zurbax uses
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
