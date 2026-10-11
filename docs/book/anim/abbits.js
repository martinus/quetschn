/*
 * The bytes of the page of ab, read as the spec reads them: the header, the literals, then the bits
 * of B field by field, with the page filling up below. abbits.c compresses the page with the real
 * code and 4 KiB tables and gives the bytes and what each field is; the table in chapter 4 is the same.
 */
BookAnim.define('abbits', function (ctx) {
  var D = ctx.data, BYTES = D.bytes, NB = BYTES.length, B0 = 4, NBITS = 8 * (NB - B0);
  function hx(b) { return b.toString(16).padStart(2, '0'); }
  function sup(n) { return String(n).replace(/\d/g, function (c) { return '\u2070\u00b9\u00b2\u00b3\u2074\u2075\u2076\u2077\u2078\u2079'[c]; }); }
  var BITS = '';
  for (var b = B0; b < NB; b++) for (var k = 7; k >= 0; k--) BITS += (BYTES[b] >> k) & 1;
  var tl = D.tokCode.length, ml = D.mlCode.length;
  // the fields of B: [first bit, bits, kind, label under it, the label on a phone]
  var FIELDS = [
    [0, tl, 'code', 'token ' + D.tok, 'token ' + D.tok],
    [tl, D.rawBits, 'plain', 'off = ' + D.off, 'off ' + D.off],
    [tl + D.rawBits, ml, 'code', 'ML symbol ' + D.mlSym, 'ML ' + D.mlSym],
    [tl + D.rawBits + ml, D.extra, 'plain', String(D.extraVal), String(D.extraVal)]
  ];
  var used = tl + D.rawBits + ml + D.extra, left = NBITS - used;
  function bitsOf(f) { return BITS.substr(FIELDS[f][0], FIELDS[f][1]); }
  // per step: bytes marked, fields read, page bytes written, the sentence
  var STEPS = [
    { bytes: [], fields: 0, page: 0, say: 'The ' + NB + ' bytes of a compressed page. zram stores them together with their length, ' + NB + ': the format has no length of its own.' },
    { bytes: [0, 1], fields: 0, page: 0, say: 'The u16 h, little endian: ' + hx(BYTES[0]) + ' ' + hx(BYTES[1]) + ' is 2. Its top bit is 0, so the literals are raw, and there are n = 2 of them.' },
    { bytes: [2, 3], fields: 0, page: 0, say: 'The 2 raw literals follow the u16: 61 62, "a" and "b". Everything after them is B, the sequences’ bitstream.' },
    { bytes: BYTES.map(function (x, i) { return i; }).slice(B0), fields: 0, page: 0, say: 'B is ' + NBITS + ' bits, read from bit 7 of each byte down to bit 0, byte after byte.' },
    { bytes: [], fields: 1, page: 0, say: 'The first token: its code ' + bitsOf(0) + ', ' + tl + ' bits, is the token ' + D.tok + ' in the TOK table. ' + D.tok + ' = ' + D.ll + ' + 16 × ' + D.mlf + ' + 512 × ' + D.cls + ': ll = ' + D.ll + ' literals, mlf = ' + D.mlf + ', offset class ' + D.cls + '.' },
    { bytes: [], fields: 2, page: 0, say: 'Class ' + D.cls + ' sends ' + D.rawBits + ' plain bits, ' + bitsOf(1) + ': the offset is ' + D.off + '.' },
    { bytes: [], fields: 2, page: D.ll, say: 'll = ' + D.ll + ' is below 15, so no length value for it: the ' + D.ll + ' literals go to the page. The page has ' + D.ll + ' bytes, "ab".' },
    { bytes: [], fields: 3, page: D.ll, say: 'mlf = ' + D.mlf + ' is the largest the token holds, so a length value follows: its code ' + bitsOf(2) + ' is symbol ' + D.mlSym + ' of the ML table, which means ' + D.mlSym + ' − 12 = ' + D.extra + ' extra bits.' },
    { bytes: [], fields: 4, page: D.ll, say: 'The ' + D.extra + ' extra bits ' + bitsOf(3) + ' are ' + D.extraVal + '. The value is 2' + sup(D.extra) + ' + ' + D.extraVal + ' = ' + D.value + ', and ml = 35 + ' + D.value + ' = ' + D.ml + '.' },
    { bytes: [], fields: 4, page: 4096, say: 'The match copies ' + D.ml + ' bytes from ' + D.off + ' back. After ' + D.off + ' bytes it copies what it wrote itself: "abab..." up to byte 4095. The page is full.' },
    { bytes: [], fields: 4, page: 4096, say: '0 literals are left for 0 bytes of the page: the last sequence, which has no token. ' + left + ' bits of B are left' + (left ? ', all 0' : '') + '. The page is valid: 4096 bytes from ' + NB + '.' }
  ];
  var X0 = 20;
  var root = ctx.svg('svg', { viewBox: '0 0 744 300', role: 'img', 'aria-label': 'The ' + NB + ' bytes, the ' + NBITS + ' bits of B read field by field, and the page' }, ctx.stage);

  /*
   * The layout: wide, the bits of B in one row; on a phone (ctx.narrow, 360 units), 24 bits in a
   * row, a field across the break bracketed in both.
   */
  function layout() {
    var n = ctx.narrow;
    var L = {
      W: n ? 360 : 744, X0: n ? 6 : 20, CW: n ? 14 : 17, BW: n ? 34 : 52, BG: n ? 4 : 6, bfont: n ? 13 : 15,
      bitFont: n ? 12 : 14, perRow: n ? 24 : NBITS, rowH: n ? 70 : 0, bitsY: 108, bitH: n ? 22 : 26
    };
    L.pageY = L.bitsY + (n ? Math.ceil(NBITS / L.perRow) * L.rowH : 0) + (n ? 0 : 88);
    L.H = L.pageY + (n ? 130 : 96);
    root.setAttribute('viewBox', '0 0 ' + L.W + ' ' + L.H);
    return L;
  }
  function bitX(L, i) { var c = i % L.perRow; return L.X0 + c * L.CW + Math.floor(c / 8) * 6; }
  function bitY(L, i) { return L.bitsY + Math.floor(i / L.perRow) * L.rowH; }

  function draw(k) {
    var st = STEPS[k], L = layout();
    while (root.firstChild) root.removeChild(root.firstChild);
    // the bytes
    ctx.svg('text', { x: L.X0, y: 16, class: 'ab-head', text: 'the compressed page, ' + NB + ' bytes' }, root);
    for (var j = 0; j < NB; j++) {
      var part = j < 2 ? 'ab-hdr' : j < 4 ? 'ab-lit' : 'ab-bits';
      var on = st.bytes.indexOf(j) >= 0, bx = L.X0 + j * (L.BW + L.BG);
      ctx.svg('rect', { x: bx, y: 24, width: L.BW, height: 30, rx: 3, class: part + (on ? ' ab-on' : '') }, root);
      ctx.svg('text', { x: bx + L.BW / 2, y: 44, 'text-anchor': 'middle', 'font-size': L.bfont, text: BYTES[j].toString(16).padStart(2, '0') }, root);
    }
    function under(j, t) { ctx.svg('text', { x: L.X0 + j * (L.BW + L.BG) + L.BW / 2, y: 70, 'text-anchor': 'middle', 'font-size': 12, class: 'ab-note', text: t }, root); }
    under(2, '"a"'); under(3, '"b"'); under(0.5, 'h = 2');
    // the bits of B
    ctx.svg('text', { x: L.X0, y: 100, class: 'ab-head', text: ctx.narrow ? 'B, ' + NBITS + ' bits, bit 7 first' : 'B, ' + NBITS + ' bits, bit 7 of each byte first' }, root);
    var nf = st.fields, cur = nf > 0 ? FIELDS[nf - 1] : null;
    for (var i = 0; i < NBITS; i++) {
      var f = null;
      for (var q = 0; q < nf; q++) if (i >= FIELDS[q][0] && i < FIELDS[q][0] + FIELDS[q][1]) f = FIELDS[q];
      var cls = f ? (f[2] === 'code' ? 'ab-code' : 'ab-plain') : 'ab-unread';
      if (f && f !== cur) cls += ' ab-done';
      ctx.svg('rect', { x: bitX(L, i), y: bitY(L, i), width: L.CW - 2, height: L.bitH, rx: 2, class: cls }, root);
      ctx.svg('text', { x: bitX(L, i) + (L.CW - 2) / 2, y: bitY(L, i) + L.bitH - 8, 'text-anchor': 'middle', 'font-size': L.bitFont, class: f ? 'ab-tf' : '', text: BITS[i] }, root);
    }
    // a bracket under each field read, one per row it is in, its label under the longer part
    for (var r = 0; r < nf; r++) {
      var F = FIELDS[r], parts = [], p0 = F[0];
      while (p0 < F[0] + F[1]) {
        var rowEnd = (Math.floor(p0 / L.perRow) + 1) * L.perRow, p1 = Math.min(F[0] + F[1], rowEnd);
        parts.push([p0, p1 - 1]); p0 = p1;
      }
      var longest = parts.reduce(function (m, x) { return x[1] - x[0] > m[1] - m[0] ? x : m; });
      parts.forEach(function (pt) {
        var a = bitX(L, pt[0]), z = bitX(L, pt[1]) + L.CW - 2, y = bitY(L, pt[0]) + L.bitH + 8;
        ctx.svg('path', { d: 'M' + a + ',' + y + ' v6 H' + z + ' v-6', fill: 'none', class: F === cur ? 'ab-br-now' : 'ab-br' }, root);
        if (pt === longest) ctx.svg('text', { x: (a + z) / 2, y: y + 22, 'text-anchor': 'middle', 'font-size': 12, class: F === cur ? 'ab-lab-now' : 'ab-lab', text: ctx.narrow ? F[4] : F[3] }, root);
      });
    }
    // the page
    var PY = L.pageY;
    ctx.svg('text', { x: L.X0, y: PY, class: 'ab-head', text: (ctx.narrow ? 'the page: ' : 'the page, 4096 bytes: ') + st.page + ' written' }, root);
    var PW = L.W - 2 * L.X0;
    ctx.svg('rect', { x: L.X0, y: PY + 8, width: PW, height: 22, class: 'ab-empty' }, root);
    if (st.page) {
      ctx.svg('rect', { x: L.X0, y: PY + 8, width: Math.max(4, PW * Math.min(2, st.page) / 4096), height: 22, class: 'ab-litfill' }, root);
      if (st.page > 2) ctx.svg('rect', { x: L.X0 + PW * 2 / 4096 + 2, y: PY + 8, width: PW * (st.page - 2) / 4096 - 2, height: 22, class: 'ab-matchfill an-move' }, root);
    }
    var nch = ctx.narrow ? 24 : 32;
    var txt = st.page ? 'abababababababababababababababab'.slice(0, Math.min(st.page, nch)) + (st.page > nch ? ' ... ab' : '') : '';
    ctx.svg('text', { x: L.X0, y: PY + 52, 'font-size': 14, text: txt }, root);
    // the legend: a box, then its word, at fixed places, in two rows on a phone
    [['ab-litfill', 'literal'], ['ab-matchfill', 'match'], ['ab-code', 'code from a table'], ['ab-plain', 'plain bits']].forEach(function (Lg, n) {
      var x = ctx.narrow ? L.X0 + (n % 2) * 170 : L.X0 + n * 150, y = PY + 76 + (ctx.narrow ? Math.floor(n / 2) * 22 : 0);
      ctx.svg('rect', { x: x, y: y, width: 12, height: 12, class: Lg[0] }, root);
      ctx.svg('text', { x: x + 18, y: y + 10, 'font-size': 12, class: 'ab-note ab-key', text: Lg[1] }, root);
    });
    ctx.say(st.say);
  }
  return { steps: STEPS.length, draw: draw, interval: 2200, wide: 630 };
});
