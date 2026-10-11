/*
 * The book's animations: a figure with controls (start, back, play, step, a slider), a sentence for
 * the current step, and a stage the animation draws into. An animation is a file anim/<name>.js that
 * calls BookAnim.define(name, setup); setup(ctx) returns { steps, draw(k), interval }; steps is a number,
 * or a function when the reader's choices change it. draw(k) draws
 * step k from nothing or over what is there; it must give the same picture for k whatever came before.
 */
(function () {
  if (window.BookAnim) return;

  /*
   * The same sequence of chapter 16's page in every figure that shows it: a figure adds itself with
   * BookLinks.add(id, label, fig, go), where go(seq) jumps it to sequence seq (0-based), and calls
   * BookLinks.show(id, seq) whenever its step shows a sequence of that page, or null. Under its
   * sentence it then gets links to the same sequence in the other figures.
   */
  var links = [], shown = {};
  window.BookLinks = {
    add: function (id, label, fig, go) {
      links.push({ id: id, label: label, fig: fig, go: go });
      links.sort(function (a, b) { return a.fig.compareDocumentPosition(b.fig) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1; });
      Object.keys(shown).forEach(function (k) { window.BookLinks.show(k, shown[k]); });
    },
    show: function (id, seq) {
      shown[id] = seq;
      var me = links.filter(function (l) { return l.id === id; })[0];
      if (!me) return;
      var box = me.fig.querySelector('.xlinks');
      if (!box) {
        box = el('p', 'xlinks');
        var say = me.fig.querySelector('.an-say, .st-say');
        say.parentNode.insertBefore(box, say.nextSibling);
      }
      box.textContent = '';
      var others = links.filter(function (l) { return l.id !== id; });
      if (seq == null || !others.length) { box.hidden = true; return; }
      box.hidden = false;
      el('span', 'xl-head', 'Sequence ' + (seq + 1) + ' of chapter 16\u2019s page:', box);
      others.forEach(function (l) {
        var a = el('a', null, l.label, box);
        a.href = '#' + l.fig.id;
        a.addEventListener('click', function (ev) {
          ev.preventDefault();
          l.go(seq);
          l.fig.scrollIntoView({ block: 'start', behavior: calm.matches ? 'auto' : 'smooth' });
          l.fig.focus({ preventScroll: true });
        });
      });
    }
  };
  var defs = {};
  var calm = matchMedia('(prefers-reduced-motion: reduce)');
  var NS = 'http://www.w3.org/2000/svg';

  function el(tag, cls, text, parent) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    if (parent) parent.appendChild(e);
    return e;
  }
  function svg(tag, attrs, parent) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs || {}) {
      if (k === 'text') e.textContent = attrs[k]; else e.setAttribute(k, attrs[k]);
    }
    if (parent) parent.appendChild(e);
    return e;
  }

  var ICONS = {
    first: '<path d="M6 5v14M18 6l-8 6 8 6z"/>',
    prev: '<path d="M15 6l-7 6 7 6"/>',
    play: '<path class="i-play" d="M8 5l11 7-11 7z"/><path class="i-pause" d="M8 5v14M16 5v14"/>',
    next: '<path d="M9 6l7 6-7 6"/>'
  };
  var TITLES = { first: 'To the start', prev: 'Step back (left)', play: 'Play or pause', next: 'Step (right)' };

  function start(fig) {
    var name = fig.getAttribute('data-anim');
    var dataEl = document.getElementById(fig.id + '-data');
    var bar = fig.querySelector('.an-bar'), sayEl = fig.querySelector('.an-say'), stage = fig.querySelector('.an-stage');
    var ctrl = el('div', 'an-ctl', null, bar);
    Object.keys(ICONS).forEach(function (k) {
      var b = el('button', 'icon', null, ctrl);
      b.type = 'button'; b.setAttribute('data-do', k); b.title = TITLES[k]; b.setAttribute('aria-label', TITLES[k]);
      b.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true">' + ICONS[k] + '</svg>';
    });
    var scrub = el('input', 'an-scrub', null, bar);
    scrub.type = 'range'; scrub.min = 0; scrub.value = 0; scrub.setAttribute('aria-label', 'Step');
    var pos = el('span', 'an-pos', null, bar);
    var cs = getComputedStyle(fig);
    var ctx = {
      fig: fig, stage: stage, el: el, svg: svg, calm: calm.matches,
      data: dataEl ? JSON.parse(dataEl.textContent) : null,
      say: function (t) { sayEl.textContent = t; },
      sayHTML: function (h) { sayEl.innerHTML = h; },
      // a colour token of the page, e.g. color('accent'), for a canvas
      color: function (t) { return getComputedStyle(fig).getPropertyValue('--' + t).trim(); },
      // an element that stays from step to step, so that CSS transitions move it
      keyed: function (parent, key, make) {
        var e = parent.querySelector('[data-key="' + key + '"]');
        if (!e) { e = make(); e.setAttribute('data-key', key); parent.appendChild(e); }
        return e;
      },
      // extra controls of an animation go into the bar, e.g. a choice of input
      bar: bar
    };
    var a = defs[name](ctx), i = 0, timer = null;
    function n() { return typeof a.steps === 'function' ? a.steps() : a.steps; }
    function go(k) {
      i = Math.max(0, Math.min(n() - 1, k));
      scrub.max = n() - 1; scrub.value = i;
      pos.textContent = 'step ' + (i + 1) + ' of ' + n();
      a.draw(i);
      if (a.link) window.BookLinks.show(fig.id, a.link.seqAt(i));
    }
    // an animation of chapter 16's page sequence by sequence: { label, seqAt(k), stepOf(seq) }
    if (a.link) window.BookLinks.add(fig.id, a.link.label, fig, function (seq) { play(false); go(a.link.stepOf(seq)); });
    function play(on) {
      if (timer) { clearInterval(timer); timer = null; }
      fig.classList.toggle('playing', on);
      if (on) {
        if (i >= n() - 1) go(0);
        timer = setInterval(function () { if (i >= n() - 1) play(false); else go(i + 1); }, a.interval || 900);
      }
    }
    ctrl.addEventListener('click', function (ev) {
      var b = ev.target.closest('button'); if (!b) return;
      var d = b.getAttribute('data-do');
      if (d !== 'play') play(false);
      if (d === 'first') go(0);
      if (d === 'prev') go(i - 1);
      if (d === 'next') go(i + 1);
      if (d === 'play') play(!timer);
    });
    scrub.addEventListener('input', function () { play(false); go(+scrub.value); });
    fig.addEventListener('keydown', function (ev) {
      if (ev.target === scrub || ev.target.tagName === 'SELECT') return;
      if (ev.key === 'ArrowRight') { play(false); go(i + 1); ev.preventDefault(); }
      if (ev.key === 'ArrowLeft') { play(false); go(i - 1); ev.preventDefault(); }
    });
    // a change of input from the animation's own controls: steps may change
    ctx.restart = function () { play(false); go(0); };
    ctx.go = function (k) { play(false); go(k); };
    ctx.redraw = function () { go(i); };
    document.addEventListener('book-theme', function () { setTimeout(function () { go(i); }, 0); });
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () { go(i); });
    calm.addEventListener('change', function () { ctx.calm = calm.matches; });
    go(0);
  }

  window.BookAnim = {
    define: function (name, setup) {
      defs[name] = setup;
      var fig = document.querySelector('figure.anim[data-anim="' + name + '"]');
      if (fig) start(fig);
    }
  };
})();
