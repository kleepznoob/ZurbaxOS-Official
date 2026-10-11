// zurbax.com: reveals, counters, the tilting screenshots and the download chooser
(() => {
  const root = document.documentElement;
  root.classList.remove('no-js');
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const BASE = 'https://downloads.zurbax.com/2.0/';

  const EDITIONS = {
    'nvidia-rtx': ['Zurbax 2.0 for GeForce RTX', 'RTX 20, 30, 40 and 50 series and GTX 16 series. About 6.5 GB, for an 8 GB stick.'],
    'nvidia-gtx': ['Zurbax 2.0 for GeForce GTX', 'GTX 10 series and older. About 6.5 GB, for an 8 GB stick.'],
    'amd': ['Zurbax 2.0 for Radeon', 'RX 400 and newer, and Ryzen graphics. About 6.5 GB, for an 8 GB stick.'],
    'amd-legacy': ['Zurbax 2.0 for older Radeon', 'HD 2000 to HD 7000, R7 and R9. About 6.5 GB, for an 8 GB stick.'],
    'intel-arc': ['Zurbax 2.0 for Intel Arc', 'Arc graphics cards and Core Ultra. About 6.5 GB, for an 8 GB stick.'],
    'intel': ['Zurbax 2.0 for Intel graphics', 'HD, UHD and Iris graphics. About 6.5 GB, for an 8 GB stick.'],
    'mini': ['Zurbax 2.0 mini installer', 'Fits on a 2 GB stick. It finds your graphics card and downloads the right edition while it installs, so keep the computer online.'],
  };

  // the bar turns to glass once the page moves; the progress line fills
  const bar = document.querySelector('.bar');
  const hero = document.querySelector('.hero-shot');
  let ticking = false;
  const onScroll = () => {
    ticking = false;
    const y = scrollY;
    bar.classList.toggle('solid', y > 24);
    const max = root.scrollHeight - innerHeight;
    root.style.setProperty('--progress', max > 0 ? (y / max).toFixed(4) : 0);
    if (still) return;
    root.style.setProperty('--scroll', Math.min(y, 1200));
    if (hero) {
      const r = hero.getBoundingClientRect();
      const t = Math.min(1, Math.max(0, 1 - (r.top - innerHeight * 0.25) / (innerHeight * 0.6)));
      hero.style.setProperty('--hero-tilt', t.toFixed(3));
    }
  };
  addEventListener('scroll', () => { if (!ticking) { ticking = true; requestAnimationFrame(onScroll); } }, { passive: true });
  onScroll();

  // reveal on scroll, and count the facts up when they arrive
  const count = el => {
    const end = +el.dataset.count;
    if (still) { el.textContent = end; return; }
    const t0 = performance.now();
    const step = now => {
      const p = Math.min(1, (now - t0) / 1400);
      el.textContent = Math.round(end * (1 - Math.pow(1 - p, 3)));
      if (p < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  };
  const seen = new IntersectionObserver(entries => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      e.target.classList.add('in');
      e.target.querySelectorAll('[data-count]').forEach(count);
      seen.unobserve(e.target);
    }
  }, { threshold: 0.15, rootMargin: '0px 0px -40px 0px' });
  document.querySelectorAll('.reveal, .steps').forEach(el => seen.observe(el));

  // screenshots lean toward the pointer
  if (!still && matchMedia('(hover: hover)').matches) {
    document.querySelectorAll('.tilt').forEach(el => {
      el.addEventListener('pointermove', e => {
        const r = el.getBoundingClientRect();
        const x = (e.clientX - r.left) / r.width, y = (e.clientY - r.top) / r.height;
        el.style.setProperty('--ry', `${(x - 0.5) * 6}deg`);
        el.style.setProperty('--rx', `${(0.5 - y) * 6}deg`);
        el.style.setProperty('--mx', `${x * 100}%`);
        el.style.setProperty('--my', `${y * 100}%`);
      });
      el.addEventListener('pointerleave', () => {
        el.style.setProperty('--rx', '0deg');
        el.style.setProperty('--ry', '0deg');
      });
    });
  }

  // the download chooser: vendor, then family, then the file
  const dialog = document.getElementById('chooser');
  const panes = dialog.querySelectorAll('.pane');
  let opener = null;
  const show = step => {
    panes.forEach(p => { p.hidden = p.dataset.step !== step; });
    const first = dialog.querySelector(`.pane[data-step="${step}"] button:not(.back), .pane[data-step="${step}"] a`);
    if (first) first.focus({ preventScroll: true });
  };
  const open = () => {
    opener = document.activeElement;
    dialog.hidden = false;
    document.body.style.overflow = 'hidden';
    show('vendor');
    requestAnimationFrame(() => dialog.classList.add('open'));
  };
  const close = () => {
    dialog.classList.remove('open');
    document.body.style.overflow = '';
    setTimeout(() => { dialog.hidden = true; }, 300);
    if (opener) opener.focus({ preventScroll: true });
  };
  const pick = edition => {
    const [name, note] = EDITIONS[edition];
    dialog.querySelector('[data-name]').textContent = name;
    dialog.querySelector('[data-note]').textContent = note;
    const file = dialog.querySelector('[data-file]');
    file.href = `${BASE}zurbax-${edition}.iso`;
    file.setAttribute('download', '');
    file.textContent = edition === 'mini' ? 'Download the mini installer' : 'Download Zurbax 2.0';
    show('done');
  };

  document.querySelectorAll('[data-mini]').forEach(el => el.addEventListener('click', e => { e.preventDefault(); open(); pick('mini'); }));
  document.querySelectorAll('[data-download]').forEach(el => el.addEventListener('click', e => { e.preventDefault(); open(); }));
  dialog.addEventListener('click', e => {
    if (e.target === dialog || e.target.closest('[data-close]')) return close();
    const go = e.target.closest('[data-go]');
    if (go) return show(go.dataset.go);
    const ed = e.target.closest('[data-edition]');
    if (ed) pick(ed.dataset.edition);
  });
  addEventListener('keydown', e => {
    if (dialog.hidden) return;
    if (e.key === 'Escape') close();
    if (e.key === 'Tab') {
      const f = [...dialog.querySelectorAll('button, a[href]')].filter(x => x.offsetParent);
      if (!f.length) return;
      const i = f.indexOf(document.activeElement);
      if (e.shiftKey && i <= 0) { e.preventDefault(); f[f.length - 1].focus(); }
      else if (!e.shiftKey && i === f.length - 1) { e.preventDefault(); f[0].focus(); }
    }
  });
  if (location.hash === '#get') open();
})();
