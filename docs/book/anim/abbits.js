/*
 * The 8 bytes of the page of ab, read as the spec reads them: the header, the literals, then the 32
 * bits of B field by field, with the page filling up below. The fields and their values are those of
 * the table in chapter 4, from the real 4 KiB tables.
 */
BookAnim.define('abbits', function (ctx) {
  var BYTES = [0x02, 0x00, 0x61, 0x62, 0xdc, 0x65, 0xef, 0xdb];
  var BITS = '';
  for (var b = 4; b < 8; b++) for (var k = 7; k >= 0; k--) BITS += (BYTES[b] >> k) & 1;
  // the fields of B: [first bit, bits, kind, label under it, the label on a phone]
  var FIELDS = [
    [0, 11, 'code', 'token 1010', 'token 1010'],
    [11, 4, 'plain', 'off = 2', 'off 2'],
    [15, 6, 'code', 'ML symbol 23', 'ML 23'],
    [21, 11, 'plain', '2011', '2011']
  ];
  // per step: bytes marked, fields read, page bytes written, the sentence
  var STEPS = [
    { bytes: [], fields: 0, page: 0, say: 'The 8 bytes of a compressed page. zram stores them together with their length, 8: the format has no length of its own.' },
    { bytes: [0, 1], fields: 0, page: 0, say: 'The u16 h, little endian: 02 00 is 2. Its top bit is 0, so the literals are raw, and there are n = 2 of them.' },
    { bytes: [2, 3], fields: 0, page: 0, say: 'The 2 raw literals follow the u16: 61 62, "a" and "b". Everything after them is B, the sequences’ bitstream.' },
    { bytes: [4, 5, 6, 7], fields: 0, page: 0, say: 'B is 32 bits, read from bit 7 of each byte down to bit 0, byte after byte.' },
    { bytes: [], fields: 1, page: 0, say: 'The first token: its code 11011100011, 11 bits, is the token 1010 in the TOK table. 1010 = 2 + 16 × 31 + 512 × 1: ll = 2 literals, mlf = 31, offset class 1.' },
    { bytes: [], fields: 2, page: 0, say: 'Class 1 sends 4 plain bits, 0010: the offset is 2.' },
    { bytes: [], fields: 2, page: 2, say: 'll = 2 is below 15, so no length value for it: the 2 literals go to the page. The page has 2 bytes, "ab".' },
    { bytes: [], fields: 3, page: 2, say: 'mlf = 31 is the largest the token holds, so a length value follows: its code 111101 is symbol 23 of the ML table, which means 23 − 12 = 11 extra bits.' },
    { bytes: [], fields: 4, page: 2, say: 'The 11 extra bits 11111011011 are 2011. The value is 2\u00b9\u00b9 + 2011 = 4059, and ml = 35 + 4059 = 4094.' },
    { bytes: [], fields: 4, page: 4096, say: 'The match copies 4094 bytes from 2 back. After 2 bytes it copies what it wrote itself: "abab..." up to byte 4095. The page is full.' },
    { bytes: [], fields: 4, page: 4096, say: '0 literals are left for 0 bytes of the page: the last sequence, which has no token. 0 bits of B are left. The page is valid: 4096 bytes from 8.' }
  ];
  var X0 = 20;
  var root = ctx.svg('svg', { viewBox: '0 0 744 300', role: 'img', 'aria-label': 'The 8 bytes, the 32 bits of B read field by field, and the page' }, ctx.stage);

  /*
   * The layout: wide, the 32 bits in one row; on a phone (ctx.narrow, 360 units), bytes 0 to 2 of B
   * in one row and byte 3 in the next, a field across the break bracketed in both.
   */
  function layout() {
    var n = ctx.narrow;
    var L = {
      W: n ? 360 : 744, X0: n ? 6 : 20, CW: n ? 14 : 17, BW: n ? 34 : 52, BG: n ? 4 : 6, bfont: n ? 13 : 15,
      bitFont: n ? 12 : 14, perRow: n ? 24 : 32, rowH: n ? 70 : 0, bitsY: 108, bitH: n ? 22 : 26
    };
    L.pageY = L.bitsY + (n ? 2 * L.rowH : 0) + (n ? 0 : 88);
    L.H = L.pageY + (n ? 130 : 96);
    root.setAttribute('viewBox', '0 0 ' + L.W + ' ' + L.H);
    return L;
  }
  function bitX(L, i) { var c = i % L.perRow; return L.X0 + c * L.CW + Math.floor(c / 8) * 6; }
  function bitY(L, i) { return L.bitsY + Math.floor(i / L.perRow) * L.rowH; }

  function draw(k) {
    var st = STEPS[k], L = layout();
    while (root.firstChild) root.removeChild(root.firstChild);
    // the 8 bytes
    ctx.svg('text', { x: L.X0, y: 16, class: 'ab-head', text: 'the compressed page, 8 bytes' }, root);
    for (var j = 0; j < 8; j++) {
      var part = j < 2 ? 'ab-hdr' : j < 4 ? 'ab-lit' : 'ab-bits';
      var on = st.bytes.indexOf(j) >= 0, bx = L.X0 + j * (L.BW + L.BG);
      ctx.svg('rect', { x: bx, y: 24, width: L.BW, height: 30, rx: 3, class: part + (on ? ' ab-on' : '') }, root);
      ctx.svg('text', { x: bx + L.BW / 2, y: 44, 'text-anchor': 'middle', 'font-size': L.bfont, text: BYTES[j].toString(16).padStart(2, '0') }, root);
    }
    function under(j, t) { ctx.svg('text', { x: L.X0 + j * (L.BW + L.BG) + L.BW / 2, y: 70, 'text-anchor': 'middle', 'font-size': 12, class: 'ab-note', text: t }, root); }
    under(2, '"a"'); under(3, '"b"'); under(0.5, 'h = 2');
    // the 32 bits of B
    ctx.svg('text', { x: L.X0, y: 100, class: 'ab-head', text: ctx.narrow ? 'B, 32 bits, bit 7 first' : 'B, 32 bits, bit 7 of each byte first' }, root);
    var nf = st.fields, cur = nf > 0 ? FIELDS[nf - 1] : null;
    for (var i = 0; i < 32; i++) {
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
