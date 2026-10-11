/*
 * The 9 bytes of the page of ab, read as the spec reads them: the header, the literals, then the 40
 * bits of B field by field, with the page filling up below. The fields and their values are those of
 * the table in chapter 4, from the real 4 KiB tables.
 */
BookAnim.define('abbits', function (ctx) {
  var BYTES = [0x02, 0x00, 0x61, 0x62, 0xdc, 0x65, 0xef, 0xdb, 0x8a];
  var BITS = '';
  for (var b = 4; b < 9; b++) for (var k = 7; k >= 0; k--) BITS += (BYTES[b] >> k) & 1;
  // the fields of B: [first bit, bits, kind, label under it]
  var FIELDS = [
    [0, 11, 'code', 'token 1010'],
    [11, 4, 'plain', 'off = 2'],
    [15, 6, 'code', 'ML symbol 23'],
    [21, 11, 'plain', '2011'],
    [32, 8, 'code', 'token 0']
  ];
  // per step: bytes marked, fields read, page bytes written, the sentence
  var STEPS = [
    { bytes: [], fields: 0, page: 0, say: 'The 9 bytes of a compressed page. zram stores them together with their length, 9: the format has no length of its own.' },
    { bytes: [0, 1], fields: 0, page: 0, say: 'The u16 h, little endian: 02 00 is 2. Its top bit is 0, so the literals are raw, and there are n = 2 of them.' },
    { bytes: [2, 3], fields: 0, page: 0, say: 'The 2 raw literals follow the u16: 61 62, "a" and "b". Everything after them is B, the sequences’ bitstream.' },
    { bytes: [4, 5, 6, 7, 8], fields: 0, page: 0, say: 'B is 40 bits, read from bit 7 of each byte down to bit 0, byte after byte.' },
    { bytes: [], fields: 1, page: 0, say: 'The first token: its code 11011100011, 11 bits, is the token 1010 in the TOK table. 1010 = 2 + 16 × 31 + 512 × 1: ll = 2 literals, mlf = 31, offset class 1.' },
    { bytes: [], fields: 2, page: 0, say: 'Class 1 sends 4 plain bits, 0010: the offset is 2.' },
    { bytes: [], fields: 2, page: 2, say: 'll = 2 is below 15, so no length value for it: the 2 literals go to the page. The page has 2 bytes, "ab".' },
    { bytes: [], fields: 3, page: 2, say: 'mlf = 31 is the largest the token holds, so a length value follows: its code 111101 is symbol 23 of the ML table, which means 23 − 12 = 11 extra bits.' },
    { bytes: [], fields: 4, page: 2, say: 'The 11 extra bits 11111011011 are 2011. The value is 2\u00b9\u00b9 + 2011 = 4059, and ml = 35 + 4059 = 4094.' },
    { bytes: [], fields: 4, page: 4096, say: 'The match copies 4094 bytes from 2 back. After 2 bytes it copies what it wrote itself: "abab..." up to byte 4095. The page is full.' },
    { bytes: [], fields: 5, page: 4096, say: 'The next token: 10001010 is the token 0 in the TOK table: ll = 0, mlf = 0, class 0, so no offset bits.' },
    { bytes: [], fields: 5, page: 4096, say: 'No literals to copy, and the page is full: this is the last sequence. 0 bits of B are left, every literal is used. The page is valid: 4096 bytes from 9.' }
  ];
  var CW = 17, X0 = 20, W = X0 * 2 + 40 * CW + 4 * 6, H = 300;
  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The 9 bytes, the 40 bits of B read field by field, and the page' }, ctx.stage);

  function bitX(i) { return X0 + i * CW + Math.floor(i / 8) * 6; }

  function draw(k) {
    var st = STEPS[k];
    while (root.firstChild) root.removeChild(root.firstChild);
    // the 9 bytes
    ctx.svg('text', { x: X0, y: 16, class: 'ab-head', text: 'the compressed page, 9 bytes' }, root);
    var BW = 52;
    for (var j = 0; j < 9; j++) {
      var part = j < 2 ? 'ab-hdr' : j < 4 ? 'ab-lit' : 'ab-bits';
      var on = st.bytes.indexOf(j) >= 0;
      ctx.svg('rect', { x: X0 + j * (BW + 6), y: 24, width: BW, height: 30, rx: 3, class: part + (on ? ' ab-on' : '') }, root);
      ctx.svg('text', { x: X0 + j * (BW + 6) + BW / 2, y: 44, 'text-anchor': 'middle', 'font-size': 15, text: BYTES[j].toString(16).padStart(2, '0') }, root);
    }
    ctx.svg('text', { x: X0 + 2 * (BW + 6) + BW / 2, y: 70, 'text-anchor': 'middle', 'font-size': 12, class: 'ab-note', text: '"a"' }, root);
    ctx.svg('text', { x: X0 + 3 * (BW + 6) + BW / 2, y: 70, 'text-anchor': 'middle', 'font-size': 12, class: 'ab-note', text: '"b"' }, root);
    ctx.svg('text', { x: X0 + 0.5 * (BW + 6) + BW / 2, y: 70, 'text-anchor': 'middle', 'font-size': 12, class: 'ab-note', text: 'h = 2' }, root);
    // the 40 bits of B
    ctx.svg('text', { x: X0, y: 100, class: 'ab-head', text: 'B, 40 bits, bit 7 of each byte first' }, root);
    var nf = st.fields, cur = nf > 0 ? FIELDS[nf - 1] : null;
    for (var i = 0; i < 40; i++) {
      var f = null;
      for (var q = 0; q < nf; q++) if (i >= FIELDS[q][0] && i < FIELDS[q][0] + FIELDS[q][1]) f = FIELDS[q];
      var cls = f ? (f[2] === 'code' ? 'ab-code' : 'ab-plain') : 'ab-unread';
      if (f && f !== cur) cls += ' ab-done';
      ctx.svg('rect', { x: bitX(i), y: 108, width: CW - 2, height: 26, rx: 2, class: cls }, root);
      ctx.svg('text', { x: bitX(i) + (CW - 2) / 2, y: 126, 'text-anchor': 'middle', 'font-size': 14, class: f ? 'ab-tf' : '', text: BITS[i] }, root);
    }
    // a bracket and a label under each field read
    for (var r = 0; r < nf; r++) {
      var F = FIELDS[r], a = bitX(F[0]), z = bitX(F[0] + F[1] - 1) + CW - 2, y = 142;
      ctx.svg('path', { d: 'M' + a + ',' + y + ' v6 H' + z + ' v-6', fill: 'none', class: F === cur ? 'ab-br-now' : 'ab-br' }, root);
      ctx.svg('text', { x: (a + z) / 2, y: y + 22, 'text-anchor': 'middle', 'font-size': 12, class: F === cur ? 'ab-lab-now' : 'ab-lab', text: F[3] }, root);
    }
    // the page
    ctx.svg('text', { x: X0, y: 196, class: 'ab-head', text: 'the page, 4096 bytes: ' + st.page + ' written' }, root);
    var PW = W - 2 * X0;
    ctx.svg('rect', { x: X0, y: 204, width: PW, height: 22, class: 'ab-empty' }, root);
    if (st.page) {
      ctx.svg('rect', { x: X0, y: 204, width: Math.max(4, PW * Math.min(2, st.page) / 4096), height: 22, class: 'ab-litfill' }, root);
      if (st.page > 2) ctx.svg('rect', { x: X0 + PW * 2 / 4096 + 2, y: 204, width: PW * (st.page - 2) / 4096 - 2, height: 22, class: 'ab-matchfill an-move' }, root);
    }
    var txt = st.page ? 'abababababababababababababababab'.slice(0, Math.min(st.page, 32)) + (st.page > 32 ? ' ... ab' : '') : '';
    ctx.svg('text', { x: X0, y: 248, 'font-size': 14, text: txt }, root);
    // the legend: a box, then its word, at fixed places
    [['ab-litfill', 'literal'], ['ab-matchfill', 'match'], ['ab-code', 'code from a table'], ['ab-plain', 'plain bits']].forEach(function (L, n) {
      var x = X0 + n * 150;
      ctx.svg('rect', { x: x, y: 272, width: 12, height: 12, class: L[0] }, root);
      ctx.svg('text', { x: x + 18, y: 282, 'font-size': 12, class: 'ab-note ab-key', text: L[1] }, root);
    });
    ctx.say(st.say);
  }
  return { steps: STEPS.length, draw: draw, interval: 2200 };
});
