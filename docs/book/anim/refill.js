/*
 * seqlz_br_refill() on the chapter's made-up stream: load 8 bytes at p big-endian, shift them right by
 * count, OR them into the register, move p by the whole bytes that fit, count |= 56; and the drops of
 * 15 and 23 bits between the refills. Computed here with 64-bit integers, as the code does it.
 */
BookAnim.define('refill', function (ctx) {
  var STREAM = [0xdc, 0x65, 0xef, 0xdb, 0x8a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff];
  var M = (1n << 64n) - 1n;
  function be64(p) { var v = 0n; for (var k = 0; k < 8; k++) v = (v << 8n) | BigInt(STREAM[p + k]); return v; }
  function hx(v) { var s = v.toString(16); while (s.length < 16) s = '0' + s; return '0x' + s; }

  // the states, one per step: bits, count, p, the loaded v and its shifted form, what happened
  var S = [], st = { bits: 0n, count: 0, p: 0 };
  function push(extra) { S.push(Object.assign({ bits: st.bits, count: st.count, p: st.p }, extra)); }
  push({ say: 'The register starts empty: bits = 0, count = 0, and p points to the first byte of the stream.', phase: 'start' });
  function refill(n) {
    var v = be64(st.p);
    push({ v: v, say: 'Refill ' + n + ': load the 8 bytes at p = ' + st.p + ' as one big-endian number, v = ' + hx(v) + '. One load and a byte swap on x86-64 and arm64.', phase: 'load' });
    var sh = v >> BigInt(st.count);
    push({ v: v, sh: sh, say: 'Shift v right by count = ' + st.count + ': its first byte lands right below the ' + st.count + ' counted bits. ' + (st.count ? 'The lowest ' + st.count + ' bits of v fall off the bottom.' : 'Nothing falls off.'), phase: 'shift' });
    st.bits = (st.bits | sh) & M;
    push({ v: v, sh: sh, say: 'OR it in: bits = ' + hx(st.bits) + '. Below the counted bits the register held the first bits of the byte at p from the last load; the shift puts the same byte at the same place, so the OR writes the same bits again and needs no mask.', phase: 'or' });
    var mv = (63 - st.count) >> 3;
    st.p += mv;
    push({ sh: sh, say: 'p moves by the bytes that went in whole: (63 − ' + S[S.length - 1].count + ') >> 3 = ' + mv + ', to ' + st.p + '. The byte at ' + st.p + ' is in the register only in part, so the next load starts with it again.', phase: 'p' });
    var old = st.count;
    st.count = st.count | 56;
    push({ say: 'count |= 56: ' + old + ' | 56 = ' + st.count + '. That is ' + old + ' + 8 × ' + mv + ': the bits of the whole bytes that went in. The OR gives the same as the add because count\u2019s whole bytes, 8 \u00d7 (count >> 3), are at most 56 and have only bits that 56 has. After any refill count is 56 to 63.', phase: 'count' });
  }
  function drop(n) {
    st.bits = (st.bits << BigInt(n)) & M;
    st.count -= n;
    push({ dropped: n, say: 'The decoder takes ' + n + ' bits: bits <<= ' + n + ', count = ' + st.count + '. The bits taken fall off the top.', phase: 'drop' });
  }
  refill(1); drop(15); refill(2); drop(23); refill(3);

  var CW = 10.5, X0 = 76, W = X0 + 64 * CW + 8 * 2 + 84, H = 268;
  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The 64-bit register, the loaded value, and the stream with p' }, ctx.stage);
  function bx(i) { return X0 + i * CW + Math.floor(i / 8) * 2; }

  function row(y, label, v, cls, count, mark) {
    ctx.svg('text', { x: X0 - 8, y: y + 14, 'text-anchor': 'end', 'font-size': 12, class: 'rf-lab', text: label }, root);
    for (var i = 0; i < 64; i++) {
      var bit = Number((v >> BigInt(63 - i)) & 1n);
      var c = cls(i);
      ctx.svg('rect', { x: bx(i), y: y, width: CW - 1, height: 20, class: c }, root);
      ctx.svg('text', { x: bx(i) + (CW - 1) / 2, y: y + 14, 'text-anchor': 'middle', 'font-size': 10.5, class: c.indexOf('rf-count') >= 0 ? 'rf-on' : 'rf-t', text: bit }, root);
    }
    if (mark != null) ctx.svg('text', { x: bx(63) + CW + 6, y: y + 14, 'font-size': 12, class: 'rf-lab', text: mark }, root);
  }

  function draw(k) {
    var s = S[k];
    while (root.firstChild) root.removeChild(root.firstChild);
    ctx.svg('text', { x: bx(0), y: 14, 'font-size': 11, class: 'rf-lab', text: 'bit 63' }, root);
    ctx.svg('text', { x: bx(63) + CW, y: 14, 'font-size': 11, 'text-anchor': 'end', class: 'rf-lab', text: 'bit 0' }, root);
    // the register: counted bits on top, the rest
    row(22, 'bits', s.bits, function (i) { return i < s.count ? 'rf-count' : 'rf-rest'; }, s.count, 'count ' + s.count);
    if (s.v != null) row(54, 'v', s.v, function () { return 'rf-v'; });
    if (s.sh != null) row(86, 'v >> ' + (s.phase === 'p' ? S[k - 2].count : s.phase === 'or' || s.phase === 'shift' ? S[k].count : ''), s.sh, function (i) { return i < (s.phase === 'p' ? S[k - 2].count : s.count) ? 'rf-zero' : 'rf-v'; });
    // the stream, p and the 8 bytes a load reads
    var SY = 150, BW = 34;
    ctx.svg('text', { x: X0 - 8, y: SY + 18, 'text-anchor': 'end', 'font-size': 12, class: 'rf-lab', text: 'stream' }, root);
    var loadAt = s.phase === 'load' || s.phase === 'shift' || s.phase === 'or' ? s.p : -1;
    for (var j = 0; j < 16; j++) {
      var x = X0 + j * (BW + 4), used = j < s.p;
      var c = used ? 'rf-used' : 'rf-byte';
      if (loadAt >= 0 && j >= loadAt && j < loadAt + 8) c += ' rf-load';
      ctx.svg('rect', { x: x, y: SY, width: BW, height: 26, rx: 3, class: c }, root);
      ctx.svg('text', { x: x + BW / 2, y: SY + 18, 'text-anchor': 'middle', 'font-size': 13, class: used ? 'rf-ut' : '', text: STREAM[j].toString(16).padStart(2, '0') }, root);
      ctx.svg('text', { x: x + BW / 2, y: SY + 42, 'text-anchor': 'middle', 'font-size': 10, class: 'rf-lab', text: j }, root);
    }
    var px = X0 + s.p * (BW + 4) + BW / 2;
    ctx.svg('path', { d: 'M' + px + ',' + (SY + 62) + ' l-6,10 h12 z', class: 'rf-ptr an-move' }, root);
    ctx.svg('text', { x: px, y: SY + 88, 'text-anchor': 'middle', 'font-size': 12, class: 'rf-ptrt', text: 'p = ' + s.p }, root);
    // the legend
    [['rf-count', 'counted bits'], ['rf-rest', 'below count: not counted'], ['rf-v', 'the loaded value'], ['rf-used', 'bytes p has passed']].forEach(function (L, n) {
      var x = X0 + n * 160;
      ctx.svg('rect', { x: x, y: H - 16, width: 12, height: 12, class: L[0] }, root);
      ctx.svg('text', { x: x + 18, y: H - 6, 'font-size': 12, class: 'rf-lab rf-key', text: L[1] }, root);
    });
    ctx.say(s.say);
  }
  return { steps: S.length, draw: draw, interval: 2000 };
});
