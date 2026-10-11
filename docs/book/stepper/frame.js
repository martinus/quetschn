/*
 * The frame both step-throughs share, stepper.js for seqlz_decode() and match.js for
 * seqlz_match_page(): the controls and the arrow keys, the page buttons, the comments button, the code
 * pane that keeps the current line a third down, the redraw in the other theme, and the links to the
 * same sequence of chapter 16's page in the other figures. A view, made by makeView(ctx), does the rest:
 *
 *   load(id)       -> { steps, groups }: the steps of page id, and the first step of each group (a
 *                     sequence), for the sequence buttons and shift + arrow
 *   line(k)        -> the line of src/ that step k stands in front of
 *   draw(k)        draws its panes for step k, from nothing or over what is there
 *   say(k), pos(k) -> the sentence of step k, and the text after the slider
 *   seq(k)         -> the sequence of chapter 16's page that step k shows, or null
 *   stepOfSeq(seq) -> the step to show for that sequence
 *   linkLabel      -> how the other figures name this one
 */
(function () {
  if (window.BookStepper) return;
  var calm = matchMedia('(prefers-reduced-motion: reduce)');

  var U = {
    el: function (tag, cls, text, parent) {
      var e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text != null) e.textContent = text;
      if (parent) parent.appendChild(e);
      return e;
    },
    hex: function (v, w) { var s = (v >>> 0).toString(16); while (s.length < w) s = '0' + s; return s; },
    ch: function (b) { return b >= 32 && b < 127 ? String.fromCharCode(b) : b === 10 ? '↵' : '·'; },
    scrollBox: function (box, top) { box.scrollTo({ top: Math.max(0, top), behavior: calm.matches ? 'auto' : 'smooth' }); },
    // a byte box scrolls only when the byte leaves it; the boxes are position: relative, so offsetTop
    // counts from their top
    keepVisible: function (box, child) {
      var top = child.offsetTop, h = box.clientHeight;
      if (top < box.scrollTop || top > box.scrollTop + h - child.offsetHeight) U.scrollBox(box, top - h / 3);
    },
    // a colour token of the page as [r, g, b], through a 1 x 1 canvas, which reads any CSS colour
    rgb: (function () {
      var c = document.createElement('canvas').getContext('2d', { willReadFrequently: true });
      c.canvas.width = c.canvas.height = 1;
      return function (css) {
        c.clearRect(0, 0, 1, 1); c.fillStyle = css; c.fillRect(0, 0, 1, 1);
        var d = c.getImageData(0, 0, 1, 1).data;
        return [d[0], d[1], d[2]];
      };
    })(),
    /*
     * 4096 cells, 64 a row, cell k in palette[colour(k)], each a square with a gap of one pixel in
     * the colour gap, or without gaps if gap is null, as one image: 4096 fillRect() calls took 1 ms
     * and more per step
     */
    cells: function (canvas, colour, palette, gap) {
      var c = canvas.getContext('2d'), w = canvas.width, cell = w / 64, img = c.createImageData(w, canvas.height);
      var px = new Uint32Array(img.data.buffer), inner = gap ? cell - 1 : cell;
      function rgba(css) { var v = U.rgb(css); return (255 << 24 | v[2] << 16 | v[1] << 8 | v[0]) >>> 0; }
      var pal = palette.map(rgba);
      if (gap) px.fill(rgba(gap));
      for (var k = 0; k < 4096; k++) {
        var x0 = (k % 64) * cell, y0 = Math.floor(k / 64) * cell, v = pal[colour(k)];
        for (var y = 0; y < inner; y++) px.fill(v, (y0 + y) * w + x0, (y0 + y) * w + x0 + inner);
      }
      c.putImageData(img, 0, 0);
      return c;
    }
  };

  function start(fig, makeView) {
    var data = JSON.parse(document.getElementById(fig.id + '-data').textContent);
    var rows = {}, stmt = {}, tops = {};
    fig.querySelectorAll('.st-code .line').forEach(function (r) {
      var n = r.querySelector('.ln').getAttribute('data-n');
      rows[n] = r; stmt[n] = r.querySelector('.lc').textContent.trim();
    });
    var codeBox = fig.querySelector('.st-code'), sayEl = fig.querySelector('.st-say');
    var scrub = fig.querySelector('.st-scrub'), posEl = fig.querySelector('.st-pos');
    var view = makeView({ fig: fig, data: data, state: fig.querySelector('.st-state'), stmt: function (line) { return stmt[line] || ''; } });
    var cur = null, i = 0, timer = null;

    // the current line a third down the pane; where the rows are is read once, not on every step
    function followLine(n) {
      if (!(n in tops)) tops[n] = rows[n].offsetTop;
      U.scrollBox(codeBox, tops[n] - codeBox.clientHeight / 3);
    }
    window.addEventListener('resize', function () { tops = {}; });

    function load(id) {
      var r = view.load(id);
      cur = { id: id, n: r.steps, groups: r.groups };
      scrub.max = cur.n - 1;
      fig.querySelectorAll('.st-pages button').forEach(function (b) { b.setAttribute('aria-pressed', b.getAttribute('data-page') === id ? 'true' : 'false'); });
      go(0);
    }
    function go(k) {
      i = Math.max(0, Math.min(cur.n - 1, k));
      fig.querySelectorAll('.st-code .line.now').forEach(function (r) { r.classList.remove('now'); });
      var n = view.line(i);
      if (rows[n]) { rows[n].classList.add('now'); followLine(n); }
      sayEl.textContent = view.say(i);
      view.draw(i);
      scrub.value = i;
      posEl.textContent = view.pos(i);
      if (window.BookLinks) window.BookLinks.show(fig.id, cur.id === 'slab' ? view.seq(i) : null);
    }
    // to the start of the next group, or of this one or the one before
    function groupStep(dir) {
      var g = cur.groups, k;
      if (dir > 0) { for (k = 0; k < g.length; k++) if (g[k] > i) return go(g[k]); return go(cur.n - 1); }
      for (k = g.length - 1; k >= 0; k--) if (g[k] < i) return go(g[k]);
      go(0);
    }
    function play(on) {
      if (timer) { clearInterval(timer); timer = null; }
      fig.classList.toggle('playing', on);
      if (on) {
        if (i >= cur.n - 1) go(0);
        timer = setInterval(function () { if (i >= cur.n - 1) play(false); else go(i + 1); }, 420);
      }
    }

    fig.querySelectorAll('[data-do]').forEach(function (b) {
      b.addEventListener('click', function () {
        var a = b.getAttribute('data-do');
        if (a !== 'play') play(false);
        if (a === 'first') go(0);
        if (a === 'prev') go(i - 1);
        if (a === 'next') go(i + 1);
        if (a === 'prevseq') groupStep(-1);
        if (a === 'nextseq') groupStep(1);
        if (a === 'play') play(!timer);
      });
    });
    fig.querySelectorAll('.st-pages button').forEach(function (b) {
      b.addEventListener('click', function () { play(false); load(b.getAttribute('data-page')); });
    });
    var cmBtn = fig.querySelector('.st-cm');
    cmBtn.addEventListener('click', function () {
      var show = fig.classList.toggle('nocomments') === false;
      cmBtn.setAttribute('aria-pressed', show ? 'true' : 'false');
      tops = {};
      go(i);
    });
    scrub.addEventListener('input', function () { play(false); go(+scrub.value); });
    fig.addEventListener('keydown', function (ev) {
      if (ev.target === scrub) return;
      if (ev.key === 'ArrowRight') { play(false); ev.shiftKey ? groupStep(1) : go(i + 1); ev.preventDefault(); }
      if (ev.key === 'ArrowLeft') { play(false); ev.shiftKey ? groupStep(-1) : go(i - 1); ev.preventDefault(); }
    });
    // colours follow the theme
    document.addEventListener('book-theme', function () { setTimeout(function () { go(i); }, 0); });
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () { go(i); });
    if (window.BookLinks) window.BookLinks.add(fig.id, view.linkLabel, fig, function (seq) {
      play(false);
      if (cur.id !== 'slab') load('slab');
      go(view.stepOfSeq(seq));
    });
    load(fig.querySelector('.st-pages button').getAttribute('data-page'));
  }

  window.BookStepper = { util: U, start: start };
})();
