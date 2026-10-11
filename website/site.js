// zurbax.com: the boot on the hero monitor, the tour, and the download
(() => {
  // where the full image lives; empty until it's up, and the page says so
  const FULL = '/download/2.0/zurbax.iso';

  const root = document.documentElement;
  root.classList.replace('no-js', 'js');
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;

  // the bar gets its rule once the page moves
  const top = document.querySelector('.top');
  const rule = () => top.classList.toggle('ruled', scrollY > 8);
  addEventListener('scroll', rule, { passive: true });
  rule();

  // the hero monitor starts up: logo and spinner, then the desktop
  const hero = document.querySelector('.hero-monitor');
  setTimeout(() => hero.classList.add('on'), still ? 0 : 1500);

  // rise into place
  const rise = new IntersectionObserver(entries => {
    for (const e of entries) if (e.isIntersecting) { e.target.classList.add('in'); rise.unobserve(e.target); }
  }, { rootMargin: '0px 0px -60px 0px', threshold: 0.12 });
  document.querySelectorAll('.rise').forEach(el => rise.observe(el));

  // the tour: the stop in the middle of the screen picks the screenshot
  const stops = [...document.querySelectorAll('.stop')];
  const cur = document.querySelector('.tour .shot.current');
  const nxt = document.querySelector('.tour .shot.next');
  stops.forEach(s => s.style.setProperty('--shot', `url("${s.dataset.shot}")`));
  stops.forEach(s => { const i = new Image(); i.src = s.dataset.shot; });
  let showing = cur.getAttribute('src'), busy = false, want = null;
  const swap = src => {
    if (src === showing) return;
    if (busy) { want = src; return; }
    busy = true; showing = src;
    nxt.src = src;
    const done = () => {
      cur.src = src;
      nxt.classList.remove('wipe');
      busy = false;
      if (want && want !== showing) { const w = want; want = null; swap(w); } else want = null;
    };
    if (still) return done();
    requestAnimationFrame(() => requestAnimationFrame(() => nxt.classList.add('wipe')));
    setTimeout(done, 780);
  };
  const tour = new IntersectionObserver(entries => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      stops.forEach(s => s.classList.toggle('active', s === e.target));
      swap(e.target.dataset.shot);
    }
  }, { rootMargin: '-45% 0px -45% 0px' });
  stops.forEach(s => tour.observe(s));
  stops[0].classList.add('active');

  // the download
  const full = document.querySelector('[data-full]');
  const fullNote = document.querySelector('[data-full-note]');
  if (FULL) {
    full.href = FULL;
  } else {
    full.closest('.dl-actions').classList.add('soon');
    full.removeAttribute('href');
    full.setAttribute('aria-disabled', 'true');
    fullNote.textContent = 'The full image is on its way.';
  }
  document.querySelectorAll('[data-get]').forEach(a => a.addEventListener('click', e => {
    if (!FULL) return; // the link scrolls to the download section
    e.preventDefault();
    start(FULL, 'Zurbax 2.0 (about 7 GB)');
  }));
  full.addEventListener('click', e => { e.preventDefault(); if (FULL) start(FULL, 'Zurbax 2.0 (about 7 GB)'); });

  const next = document.getElementById('next');
  let opener = null;
  function start(url, what) {
    const a = document.createElement('a');
    a.href = url; a.download = ''; a.style.display = 'none';
    document.body.append(a); a.click(); a.remove();
    next.querySelector('[data-next-kicker]').textContent = `Downloading ${what}`;
    next.querySelector('[data-next-link]').href = url;
    opener = document.activeElement;
    next.hidden = false;
    document.body.style.overflow = 'hidden';
    requestAnimationFrame(() => next.classList.add('open'));
    next.querySelector('.next-close').focus({ preventScroll: true });
  }
  function close() {
    next.classList.remove('open');
    document.body.style.overflow = '';
    setTimeout(() => { next.hidden = true; }, 300);
    if (opener) opener.focus({ preventScroll: true });
  }
  next.addEventListener('click', e => { if (e.target === next || e.target.closest('[data-close]')) close(); });
  addEventListener('keydown', e => { if (!next.hidden && e.key === 'Escape') close(); });
})();
