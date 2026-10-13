// zurbax.com/guide: the bar's rule once the page moves, and the contents
// following the section being read
(() => {
  document.documentElement.classList.add('js');
  // the Halloween look, on or off (remembered)
  const spooky = document.querySelector('.spooky');
  if (spooky) {
    const sync = () => spooky.setAttribute('aria-pressed', String(document.documentElement.classList.contains('halloween')));
    sync();
    spooky.addEventListener('click', () => {
      const on = document.documentElement.classList.toggle('halloween');
      try { localStorage.setItem('zurbax-halloween', on ? 'on' : 'off'); } catch (e) {}
      document.querySelector('meta[name=theme-color]').content = on ? '#4a1d7a' : '#ff5a1f';
      sync();
    });
  }
  const top = document.querySelector('.top');
  const rule = () => top.classList.toggle('ruled', scrollY > 8);
  addEventListener('scroll', rule, { passive: true });
  rule();
  const links = new Map([...document.querySelectorAll('.g-toc a')].map(a => [a.getAttribute('href').slice(1), a]));
  const seen = new IntersectionObserver(entries => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      links.forEach(a => a.classList.remove('on'));
      const a = links.get(e.target.id);
      if (a) a.classList.add('on');
    }
  }, { rootMargin: '-30% 0px -60% 0px' });
  document.querySelectorAll('.g-sec').forEach(s => seen.observe(s));
})();
