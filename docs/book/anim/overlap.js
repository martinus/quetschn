/*
 * A match that copies what it is writing: the literals, then a match of 10 bytes at offset 1, 2 or 3,
 * one byte per step, as a byte loop copies it, or as memmove() would, from the bytes as they were
 * before the copy.
 */
BookAnim.define('overlap', function (ctx) {
  var LITS = { 1: 'a', 2: 'ab', 3: 'abc' }, LEN = 10, CELL, X0, Y0 = 70, W, H;
  // the layout: 46 units a cell, or 27 on a phone, where the drawing is 360 units wide
  function layout() {
    CELL = ctx.narrow ? 27 : 46; X0 = ctx.narrow ? 4 : 20;
    W = ctx.narrow ? 360 : X0 * 2 + CELL * 13; H = ctx.narrow ? 182 : 165;
    root.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
  }
  var off = 2, mode = 'loop';
  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  function button(label, set, pressed) {
    var b = ctx.el('button', null, label, choice);
    b.type = 'button'; b.setAttribute('aria-pressed', pressed ? 'true' : 'false');
    b.addEventListener('click', function () { set(b); ctx.restart(); });
    return b;
  }
  var offBtns = [1, 2, 3].map(function (o) {
    return button('offset ' + o, function (b) { off = o; offBtns.forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); }); }, o === off);
  });
  var modeBtns = [['loop', 'byte loop'], ['memmove', 'memmove()']].map(function (m) {
    return button(m[1], function (b) { mode = m[0]; modeBtns.forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); }); }, m[0] === mode);
  });

  var root = ctx.svg('svg', { viewBox: '0 0 638 165', role: 'img', 'aria-label': 'The output bytes and the copy of a match, one byte per step' }, ctx.stage);
  var arcs = ctx.svg('g', { fill: 'none', 'stroke-width': 1.6 }, root);
  var cells = ctx.svg('g', {}, root);
  var labels = ctx.svg('g', { 'font-size': 12 }, root);

  // the bytes after step k: what the page holds, and which of them are not written yet
  function state(k) {
    var lit = LITS[off], n = lit.length, out = [], i;
    for (i = 0; i < n + LEN; i++) out.push(null);
    var lits = Math.min(k, n);
    for (i = 0; i < lits; i++) out[i] = lit[i];
    var copied = Math.max(0, k - n);
    // memmove() reads the source as it was before the copy started: the literals and nothing after
    var before = out.slice(0, n).concat(new Array(LEN).fill(null));
    for (i = 0; i < copied; i++) {
      var d = n + i, s = d - off;
      out[d] = mode === 'loop' ? out[s] : before[s];
    }
    return { out: out, n: n, copied: copied };
  }

  function draw(k) {
    layout();
    var st = state(k), n = st.n, last = k - 1, cur = -1, src = -1;
    if (k > n) { cur = n + st.copied - 1; src = cur - off; }
    while (labels.firstChild) labels.removeChild(labels.firstChild);
    while (arcs.firstChild) arcs.removeChild(arcs.firstChild);
    for (var i = 0; i < n + LEN; i++) {
      var x = X0 + i * CELL, v = st.out[i], isLit = i < n;
      var g = ctx.keyed(cells, 'c' + i, function () { return ctx.svg('g', {}); });
      g.innerHTML = '';
      var written = isLit ? i < k : i <= cur;
      var cls = !written ? 'ov-empty' : isLit ? 'ov-lit' : v == null ? 'ov-bad' : 'ov-match';
      ctx.svg('rect', { x: x, y: Y0, width: CELL - 4, height: 40, rx: 3, class: 'an-move ' + cls + (i === cur ? ' ov-now' : '') + (i === src ? ' ov-src' : '') }, g);
      ctx.svg('text', { x: x + (CELL - 4) / 2, y: Y0 + 26, 'text-anchor': 'middle', 'font-size': ctx.narrow ? 15 : 17, class: written && !isLit && v != null ? 'ov-tm' : written && isLit ? 'ov-tl' : '', text: written ? (v == null ? '?' : v) : '' }, g);
      ctx.svg('text', { x: x + (CELL - 4) / 2, y: Y0 + 58, 'text-anchor': 'middle', 'font-size': ctx.narrow ? 11.5 : 11, class: 'ov-idx', text: i }, labels);
    }
    // arcs from every source to its byte, the current one strong
    for (var j = n; j <= cur; j++) {
      var a = X0 + (j - off) * CELL + (CELL - 4) / 2, b = X0 + j * CELL + (CELL - 4) / 2, top = Y0 - 12 - off * 9;
      ctx.svg('path', { d: 'M' + a + ',' + (Y0 - 3) + ' C' + a + ',' + top + ' ' + b + ',' + top + ' ' + b + ',' + (Y0 - 3), class: j === cur ? 'ov-arc-now' : 'ov-arc' }, arcs);
    }
    if (ctx.narrow) {
      ctx.svg('text', { x: X0, y: H - 26, 'font-size': 12, class: 'ov-key', text: 'literals 0 to ' + (n - 1) + ',' }, labels);
      ctx.svg('text', { x: X0, y: H - 8, 'font-size': 12, class: 'ov-key', text: 'then a match of ' + LEN + ' at offset ' + off }, labels);
    } else ctx.svg('text', { x: X0, y: H - 10, class: 'ov-key', text: 'literals 0 to ' + (n - 1) + ', then a match of ' + LEN + ' bytes at offset ' + off }, labels);

    if (k === 0) ctx.say('The page starts empty. The compressed page says: ' + n + ' literal' + (n > 1 ? 's' : '') + ', "' + LITS[off] + '", then a match of ' + LEN + ' bytes at offset ' + off + '. The offset is smaller than the length, so the match will read bytes it writes itself.');
    else if (k <= n) ctx.say('Literal ' + (k - 1) + ': "' + LITS[off][k - 1] + '" is copied from the compressed page to byte ' + (k - 1) + '.');
    else {
      var v2 = st.out[cur];
      if (mode === 'loop') ctx.say('Byte ' + cur + ' is a copy of byte ' + cur + ' − ' + off + ' = ' + src + ', "' + v2 + '"' + (src >= n ? ', which the match wrote ' + off + (off > 1 ? ' steps' : ' step') + ' ago.' : ', a literal.') + (cur === n + LEN - 1 ? ' The match has made "' + LITS[off] + '" repeat: ' + st.out.join('') + '.' : ''));
      else ctx.say('memmove() copies the source as it was before the copy: byte ' + src + (src < n ? ' was the literal "' + v2 + '".' : ' was not written yet, so byte ' + cur + ' gets whatever was in memory there, here "?".') + (cur === n + LEN - 1 ? ' Only the first ' + off + ' bytes of the match are right: a block copy is wrong for an overlapping match.' : ''));
    }
  }
  return { steps: function () { return LITS[off].length + LEN + 1; }, draw: draw, interval: 750 };
});
