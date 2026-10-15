/*
 * The description of a page's own literal table, read symbol by symbol with the code OWN: chapter 4's
 * text page at level 3. At the top the description's bits, below them the 256 byte values, which get
 * a length or none, and the code space the lengths fill, 1024 units of a 10-bit code. owndesc.c
 * compresses the page with the real code and reads the description as the format says.
 */
BookAnim.define('owndesc', function (ctx) {
  var D = ctx.data, SYMS = D.syms, NS = SYMS.length, USED = D.used, NB = D.desc.length, NBITS = 8 * NB;
  var BITS = '';
  for (var b = 0; b < NB; b++) for (var k = 7; k >= 0; k--) BITS += (D.desc[b] >> k) & 1;
  function hx(v) { return '0x' + v.toString(16).padStart(2, '0'); }
  function name(v) { return v === 32 ? 'the space' : v > 32 && v < 127 ? '"' + String.fromCharCode(v) + '"' : hx(v); }
  function sup(n) { return String(n).replace(/\d/g, function (c) { return '⁰¹²³⁴⁵⁶⁷⁸⁹'[c]; }); }
  function glyph(v) { return v === 32 ? '␣' : v > 32 && v < 127 ? String.fromCharCode(v) : ''; }
  // the lengths the page has, shortest first; each gets a colour
  var LENS = [];
  SYMS.forEach(function (s) { if (s.len && LENS.indexOf(s.len) < 0) LENS.push(s.len); });
  LENS.sort(function (a, b) { return a - b; });
  function lenClass(l) { return 'od-c' + Math.min(LENS.indexOf(l), 2); }
  // the Elias gamma code of n >= 1: as many zeros as n has bits after its top bit, then n
  function gamma(n) { var s = n.toString(2); return '0'.repeat(s.length - 1) + s; }

  function sayOf(i) {
    var s = SYMS[i], bits = s.code + (s.extra ? ' ' + s.extra : '');
    if (s.len) {
      return 'Symbol ' + s.s + ', ' + s.code + ': a length. ' + name(s.b) + ' (' + hx(s.b) + ') gets a code of ' + s.len +
        ' bits, which takes 1024 / 2' + sup(s.len) + ' = ' + (1024 >> s.len) + ' units of the code space: ' + s.space + ' of 1024 now.';
    }
    var k = s.s - 9;
    var head = 'Symbol ' + s.s + ', ' + s.code + ': a run ' + k + ' bit' + (k > 1 ? 's' : '') + ' wide';
    var cover = s.run === 1 ? name(s.b) + ' has no code.' : 'the ' + s.run + ' byte values ' + hx(s.b) + ' to ' + hx(s.b + s.run - 1) + ' have no code.';
    if (k === 1) return head + ', so it is 1 and no bits follow: ' + cover;
    var g = gamma(s.run);
    return head + ', then its ' + (k - 1) + ' bits below the top 1, ' + s.extra + ': 1' + s.extra + ' = ' + s.run + ', ' + cover +
      ' Elias gamma writes the run as ' + g + ', ' + g.length + ' bits; here ' + bits.replace(' ', ' + ') + ', ' + (s.code.length + s.extra.length) + ' bits, and the code of symbol ' + s.s + ' also says that a run comes.';
  }
  var last = SYMS[NS - 1], after = last.b + 1;
  var STEPS = [{ n: 0, say: 'The ' + NB + ' bytes of the text page’s own table, from byte ' + D.at + ' of the page on, read with the code OWN: symbol l − 1 for a length l, symbol 9 + k for a run k bits wide. No byte value has a code yet.' }];
  for (var i = 0; i < NS; i++) STEPS.push({ n: i + 1, cur: i, say: sayOf(i) });
  STEPS.push({ n: NS, end: true, say: 'The code space is full, 1024 of 1024: the code is complete, so the description ends after ' + USED + ' bits. ' + (after < 256 ? 'The byte values from ' + hx(after) + ' on have no code. ' : '') + 'The ' + (NBITS - USED) + ' bits left in byte ' + NB + ' are 0.' });
  STEPS.push({ n: NS, end: true, codes: true, say: 'The codes follow from the lengths alone, canonical: by length, then by byte value. ' + D.codes.length + ' byte values have a code, the shortest ones first.' });

  var root = ctx.svg('svg', { role: 'img', 'aria-label': 'The bits of the description, the 256 byte values and their lengths, and the code space' }, ctx.stage);
  function layout() {
    var n = ctx.narrow;
    var L = { W: n ? 360 : 744, X0: n ? 4 : 20, CW: n ? 10 : 10, perRow: n ? 32 : 64, bitH: 18, rowH: 26, bitsY: 26, cols: n ? 16 : 32, cell: n ? 20 : 20 };
    L.rows = Math.ceil(NBITS / L.perRow);
    L.gridHead = L.bitsY + L.rows * L.rowH + 22;
    L.gridY = L.gridHead + 10;
    L.gx = L.X0 + 22;
    L.gridH = (256 / L.cols) * L.cell;
    L.barHead = L.gridY + L.gridH + 26;
    L.barY = L.barHead + 10;
    L.barW = L.W - 2 * L.X0;
    L.codesY = L.barY + 52;
    L.codesPer = n ? 3 : 7;
    L.H = L.codesY + Math.ceil(D.codes.length / L.codesPer) * 20 + 8;
    root.setAttribute('viewBox', '0 0 ' + L.W + ' ' + L.H);
    return L;
  }
  function bitX(L, i) { var c = i % L.perRow; return L.X0 + c * L.CW + Math.floor(c / 8) * 4; }
  function bitY(L, i) { return L.bitsY + Math.floor(i / L.perRow) * L.rowH; }

  function draw(k) {
    var st = STEPS[k], L = layout();
    while (root.firstChild) root.removeChild(root.firstChild);
    var cur = st.cur != null ? SYMS[st.cur] : null;
    // the bits: those of symbols read, the current one's code and extra bits, the rest unread
    ctx.svg('text', { x: L.X0, y: 14, class: 'od-head', text: 'the description, ' + NB + ' bytes, bit 7 first' }, root);
    for (var i = 0; i < NBITS; i++) {
      var cls = 'od-unread', tf = '';
      for (var j = 0; j < st.n; j++) {
        var s = SYMS[j], a = s.at, m = a + s.code.length, z = m + s.extra.length;
        if (i >= a && i < z) { cls = (i < m ? 'od-code' : 'od-extra') + (s === cur ? '' : ' od-done'); tf = 'od-tf'; }
      }
      if (st.end && i >= USED) { cls = 'od-pad'; tf = ''; }
      ctx.svg('rect', { x: bitX(L, i), y: bitY(L, i), width: L.CW - 1.5, height: L.bitH, rx: 2, class: cls }, root);
      ctx.svg('text', { x: bitX(L, i) + (L.CW - 1.5) / 2, y: bitY(L, i) + L.bitH - 5, 'text-anchor': 'middle', 'font-size': 11, class: tf, text: BITS[i] }, root);
    }
    // the byte values: a length, none, or not read yet
    var len = {}, none = {}, now = {};
    for (var q = 0; q < st.n; q++) {
      var S = SYMS[q];
      if (S.len) len[S.b] = S.len; else for (var r = 0; r < S.run; r++) none[S.b + r] = true;
      if (S === cur) { if (S.len) now[S.b] = true; else for (r = 0; r < S.run; r++) now[S.b + r] = true; }
    }
    if (st.end) for (var v = after; v < 256; v++) none[v] = true;
    ctx.svg('text', { x: L.X0, y: L.gridHead, class: 'od-head', text: ctx.narrow ? 'the byte values, 16 per row' : 'the 256 byte values, 32 per row' }, root);
    for (v = 0; v < 256; v++) {
      var cx = L.gx + (v % L.cols) * L.cell, cy = L.gridY + Math.floor(v / L.cols) * L.cell;
      if (v % L.cols === 0) ctx.svg('text', { x: L.X0, y: cy + 14, 'font-size': 10, class: 'od-note', text: v.toString(16).padStart(2, '0') }, root);
      var c = len[v] ? lenClass(len[v]) : none[v] ? 'od-none' : 'od-unread';
      ctx.svg('rect', { x: cx, y: cy, width: L.cell - 2, height: L.cell - 2, rx: 2, class: c + (now[v] ? ' od-now' : '') }, root);
      var gl = glyph(v);
      if (gl) ctx.svg('text', { x: cx + (L.cell - 2) / 2, y: cy + 13, 'text-anchor': 'middle', 'font-size': 11, class: len[v] ? 'od-tf' : 'od-note', text: gl }, root);
    }
    // the code space: each length takes 1024 >> l of 1024 units, in the order read
    var filled = cur ? cur.space : st.end ? 1024 : 0;
    ctx.svg('text', { x: L.X0, y: L.barHead, class: 'od-head', text: 'the code space: ' + filled + ' of 1024' }, root);
    ctx.svg('rect', { x: L.X0, y: L.barY, width: L.barW, height: 22, class: 'od-unread' }, root);
    var u = 0;
    for (q = 0; q < st.n; q++) {
      S = SYMS[q];
      if (!S.len) continue;
      var bw = L.barW * (1024 >> S.len) / 1024, bx = L.X0 + L.barW * u / 1024;
      ctx.svg('rect', { x: bx, y: L.barY, width: bw - 1, height: 22, class: lenClass(S.len) + (S === cur ? ' od-now' : '') }, root);
      if (bw > 14) ctx.svg('text', { x: bx + bw / 2, y: L.barY + 15, 'text-anchor': 'middle', 'font-size': 11, class: 'od-tf', text: glyph(S.b) }, root);
      u += 1024 >> S.len;
    }
    // the legend, then the codes once they are known
    var leg = LENS.map(function (l) { return [lenClass(l), l + ' bits']; }).concat([['od-none', 'no code']]);
    leg.forEach(function (Lg, n) {
      var x = L.X0 + n * (ctx.narrow ? 86 : 110), y = L.barY + 32;
      ctx.svg('rect', { x: x, y: y, width: 12, height: 12, class: Lg[0] }, root);
      ctx.svg('text', { x: x + 17, y: y + 10, 'font-size': 11, class: 'od-note od-key', text: Lg[1] }, root);
    });
    if (st.codes) {
      D.codes.forEach(function (c, n) {
        var x = L.X0 + (n % L.codesPer) * (L.barW / L.codesPer), y = L.codesY + 14 + Math.floor(n / L.codesPer) * 20;
        ctx.svg('text', { x: x, y: y, 'font-size': 12, class: 'od-key', text: (glyph(c[0]) || hx(c[0])) + ' ' + c[2] }, root);
      });
    }
    ctx.say(st.say);
  }
  return { steps: STEPS.length, draw: draw, interval: 2600, wide: 630 };
});
