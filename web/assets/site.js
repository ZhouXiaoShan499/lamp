(() => {
  'use strict';

  const header = document.querySelector('[data-site-header]');
  const menu = document.querySelector('#siteMenu');
  const menuToggle = document.querySelector('[data-menu-toggle]');

  /* ------------------------------------------------------------- site year */
  document.querySelectorAll('[data-year]').forEach(node => {
    node.textContent = String(new Date().getFullYear());
  });

  /* --------------------------------------------------------------- header UI */
  if (header) {
    let queued = false;
    const syncHeader = () => {
      header.classList.toggle('is-scrolled', window.scrollY > 12);
      queued = false;
    };
    syncHeader();
    window.addEventListener('scroll', () => {
      if (queued) return;
      queued = true;
      requestAnimationFrame(syncHeader);
    }, { passive: true });
  }

  /* -------------------------------------------------------------- mobile nav */
  let lastFocused = null;
  const focusableSelector = 'a[href], button:not([disabled]), [tabindex]:not([tabindex="-1"])';

  function setMenu(open, options = {}) {
    if (!header || !menu || !menuToggle) return;
    if (open) {
      lastFocused = document.activeElement;
      menu.hidden = false;
      header.classList.add('is-open');
      menuToggle.setAttribute('aria-expanded', 'true');
      menuToggle.setAttribute('aria-label', '关闭菜单');
      document.body.classList.add('menu-open');
      if (options.focus !== false) {
        const first = menu.querySelector(focusableSelector);
        if (first) window.setTimeout(() => first.focus(), 30);
      }
    } else {
      header.classList.remove('is-open');
      menuToggle.setAttribute('aria-expanded', 'false');
      menuToggle.setAttribute('aria-label', '打开菜单');
      document.body.classList.remove('menu-open');
      window.setTimeout(() => {
        if (!header.classList.contains('is-open')) menu.hidden = true;
      }, 10);
      if (options.restoreFocus && lastFocused && typeof lastFocused.focus === 'function') {
        lastFocused.focus({ preventScroll: true });
      }
    }
  }

  if (header && menu && menuToggle) {
    menuToggle.addEventListener('click', () => {
      const open = header.classList.contains('is-open');
      setMenu(!open, { restoreFocus: open });
    });

    menu.querySelectorAll('a').forEach(link => {
      link.addEventListener('click', () => setMenu(false));
    });

    document.addEventListener('keydown', event => {
      if (!header.classList.contains('is-open')) return;
      if (event.key === 'Escape') {
        event.preventDefault();
        setMenu(false, { restoreFocus: true });
      }
      if (event.key === 'Tab') {
        // Keep keyboard navigation inside the open menu long enough to make a choice.
        const items = [menuToggle, ...menu.querySelectorAll(focusableSelector)].filter(item => item.offsetParent !== null);
        if (!items.length) return;
        const first = items[0];
        const last = items[items.length - 1];
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    });

    document.addEventListener('pointerdown', event => {
      if (!header.classList.contains('is-open')) return;
      if (!header.contains(event.target)) setMenu(false, { restoreFocus: true });
    }, { passive: true });

    window.addEventListener('resize', () => {
      if (window.innerWidth > 900 && header.classList.contains('is-open')) setMenu(false);
    }, { passive: true });
  }

  /* --------------------------------------------------------- reveal on scroll */
  const revealItems = Array.from(document.querySelectorAll('[data-reveal]'));
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  if (!revealItems.length) return;

  if (reduceMotion || !('IntersectionObserver' in window)) {
    revealItems.forEach(item => item.classList.add('is-revealed'));
    return;
  }

  const observer = new IntersectionObserver(entries => {
    entries.forEach(entry => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add('is-revealed');
      observer.unobserve(entry.target);
    });
  }, { rootMargin: '0px 0px -8% 0px', threshold: 0.03 });

  revealItems.forEach(item => observer.observe(item));
})();
