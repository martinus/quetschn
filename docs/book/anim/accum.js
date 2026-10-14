/*
 * The encoder's 64-bit accumulator for the first sequences of chapter 16's page: each enc_put()
 * shifts acc left and ORs a code in at the bottom; each enc_flush() shifts the bits that count to the
 * top, stores the word little endian in the 8 bytes below p, and moves p down by the whole bytes. The
 * values come from the real encoder's helpers, through accum.c.
 */
BookAnim.define('accum', function (ctx) {
  var OPS = ctx.data.ops, X0 = 34, BW = 13, CW = 52, W = 900, H = 274;
  // the steps: the start, every put, every flush twice (the store, then p and cnt)
  var steps = [{ kind: 'start' }], seqOf = {}, valOf = null;
  OPS.forEach(function (o) {
    if (o.op === 'seq') { seqOf[o.seq] = o; return; }
    if (o.op === 'value') { valOf = o; return; }
    if (o.op === 'put') { steps.push({ kind: 'put', o: o, val: o.what !== 'token' ? valOf : null }); return; }
    steps.push({ kind: 'store', o: o });
    steps.push({ kind: 'move', o: o });
  });
  var nSeq = Object.keys(seqOf).length;

  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The 64 bits of acc, the value put or the word stored, and the last bytes of dst from the end backwards' }, ctx.stage);
  var g = ctx.svg('g', {}, root);

  function bitsOf(hex) {
    var s = '';
    for (var i = 0; i < hex.length; i++) s += ('000' + parseInt(hex[i], 16).toString(2)).slice(-4);
    return s;
  }
  function bin(v, n) { var s = v.toString(2); while (s.length < n) s = '0' + s; return s; }
  function hex2(b) { return (b < 16 ? '0' : '') + b.toString(16); }
  function T(x, y, s, cls, anchor, size) {
    return ctx.svg('text', { x: x, y: y, 'font-size': size || 12, 'text-anchor': anchor || 'start', class: cls || '', text: s }, g);
  }
  function rect(x, y, w, h, cls) { return ctx.svg('rect', { x: x, y: y, width: w, height: h, class: cls }, g); }
  function end(k) { return k === 0 ? 'end' : 'end − ' + k; }
  function endShort(k) { return 'end−' + k; }
  function plural(n, w) { return n + ' ' + w + (n === 1 ? '' : 's'); }

  // the state after step k: acc, cnt, p (bytes from the end), the bytes of dst, how far dst is written
  function state(k) {
    // fullTo: the bytes before it (counted from the end) hold whole bytes of the bitstream; partAt: a part of one
    var st = { acc: '0000000000000000', cnt: 0, p: 0, tail: null, wrote: 0, fullTo: 0, partAt: -1 };
    for (var j = 1; j <= k; j++) {
      var s = steps[j], o = s.o;
      if (s.kind === 'put') { st.acc = o.acc1; st.cnt = o.cnt1; }
      if (s.kind === 'store') {
        st.tail = o.tail; st.wrote = Math.max(st.wrote, o.p0 + 8);
        st.fullTo = o.p0 + (o.cnt0 >> 3); st.partAt = o.cnt0 & 7 ? st.fullTo : -1;
      }
      if (s.kind === 'move') { st.p = o.p1; st.cnt = o.cnt1; st.fullTo = o.p1; st.partAt = o.cnt1 ? o.p1 : -1; }
    }
    return st;
  }

  function accRow(st, s) {
    var Y = 26, bits = bitsOf(st.acc), n = s.kind === 'put' ? s.o.n : 0;
    T(X0, Y - 9, 'acc, bit 63 to bit 0', 'ac-sub');
    T(X0 + 64 * BW, Y - 9, 'cnt = ' + st.cnt, 'ac-sub', 'end');
    for (var i = 0; i < 64; i++) {
      var b = 63 - i, x = X0 + i * BW, counts = b < st.cnt, fresh = b < n;
      var cls = fresh ? 'ac-new' : counts ? (s.kind === 'store' ? 'ac-out' : 'ac-on') : 'ac-off';
      rect(x, Y, BW - 1, 22, cls);
      T(x + (BW - 1) / 2, Y + 15, bits[i], fresh || (counts && s.kind === 'store') ? 'ac-tin' : counts ? '' : 'ac-faint', 'middle', 11);
    }
    for (var q = 1; q < 8; q++) ctx.svg('line', { x1: X0 + q * 8 * BW - 0.5, y1: Y - 3, x2: X0 + q * 8 * BW - 0.5, y2: Y + 25, class: 'ac-sep' }, g);
  }

  function putRow(s) {
    var o = s.o, Y = 92, x = X0 + (64 - o.n) * BW;
    T(X0, Y - 9, 'the value enc_put() appends: ' + plural(o.n, 'bit') + ', 0x' + o.v, 'ac-sub');
    o.parts.forEach(function (pt, m) {
      var bits = bin(pt[2], pt[1]), w = pt[1] * BW;
      for (var i = 0; i < pt[1]; i++) {
        rect(x + i * BW, Y, BW - 1, 22, 'ac-p' + (pt[0] === 'offset' || pt[0] === 'extra' ? 1 : pt[0] === 'escape' ? 2 : 0));
        T(x + i * BW + (BW - 1) / 2, Y + 15, bits[i], 'ac-tin', 'middle', 11);
      }
      ctx.svg('path', { d: 'M' + (x + 1) + ',' + (Y + 27) + ' v4 H' + (x + w - 2) + ' v-4', class: 'ac-brace' }, g);
      var label = pt[0] + (pt[0] === 'token' && s.o.parts.length > 1 && s.o.parts[0][0] === 'escape' ? ' ' + pt[2] : '');
      T(x + w / 2 - 1, Y + 45, label, 'ac-muted', w < label.length * 7 && m === 0 && x < X0 + 60 ? 'start' : 'middle', 11.5);
      x += w;
    });
  }

  function storeRow(s, st) {
    var o = s.o, Y = 92, bits = bitsOf(o.word);
    T(X0, Y - 9, 'acc << (64 − ' + o.cnt0 + '): the word stored', 'ac-sub');
    for (var i = 0; i < 64; i++) {
      var x = X0 + i * BW, on = i < o.cnt0;
      rect(x, Y, BW - 1, 22, on ? 'ac-out' : 'ac-off');
      T(x + (BW - 1) / 2, Y + 15, bits[i], on ? 'ac-tin' : 'ac-faint', 'middle', 11);
    }
    for (var q = 0; q < 8; q++) {
      var cx = X0 + q * 8 * BW + 4 * BW, byte = parseInt(o.word.substr(q * 2, 2), 16);
      if (q) ctx.svg('line', { x1: X0 + q * 8 * BW - 0.5, y1: Y - 3, x2: X0 + q * 8 * BW - 0.5, y2: Y + 25, class: 'ac-sep' }, g);
      T(cx, Y + 40, hex2(byte), 'ac-hex', 'middle', 12);
      // byte 7 of the word, the top, goes to p − 1, the column of p; the others below it
      var dx = X0 + (o.p0 + q) * CW + CW / 2;
      ctx.svg('line', { x1: cx, y1: Y + 46, x2: dx, y2: 176, class: 'ac-fan' }, g);
    }
  }

  function dstRow(st, s) {
    var Y = 180, N = 16;
    for (var j = 0; j < N; j++) {
      var x = X0 + j * CW, written = st.tail && j < st.wrote, v = written ? st.tail[23 - j] : null;
      var cls = !written ? 'ac-empty' : j < st.fullTo ? 'ac-fin' : j === st.partAt ? 'ac-part' : 'ac-below';
      if (s.kind === 'store' && j >= s.o.p0 && j < s.o.p0 + 8) cls += ' ac-hit';
      rect(x, Y, CW - 4, 30, cls);
      if (written) T(x + (CW - 4) / 2, Y + 20, hex2(v), j < st.fullTo || j === st.partAt ? 'ac-fint' : 'ac-faint', 'middle', 13);
      T(x + (CW - 4) / 2, Y + 44, endShort(j + 1), 'ac-muted', 'middle', 11);
    }
    var px = X0 + st.p * CW - 2;
    ctx.svg('line', { x1: px, y1: Y - 5, x2: px, y2: Y + 35, class: 'ac-ptr' }, g);
    T(px, Y + 64, 'p = ' + end(st.p), 'ac-ptrt', st.p === 0 ? 'start' : 'middle');
    T(X0 + N * CW - 4, Y + 84, 'dst from its end backwards: bitstream byte 0 first', 'ac-muted', 'end', 11.5);
  }

  // the narrow layout, for a phone, 360 units wide: the 64 bits as 2 rows of 32 with a gap between the
  // bytes, dst as 2 rows of 8 with 'end' in front of each row's labels, and in place of the lines from
  // the word's bytes to dst a label under each byte that names its byte of dst
  var NW = 360, NH = 362, NB = 10, NG = 6, NX0 = 11, NDX = 34, NCW = 40, NDY = 200, NDP = 70;
  // bit 63 − i of a row of 64: its x and its row
  function nbx(i) { var c = i & 31; return NX0 + c * NB + (c >> 3) * NG; }
  function nby(i, y) { return y + (i >> 5) * 24; }
  // byte q of 8, as the bits 8q to 8q + 7 from the left: its middle
  function nbyte(q) { return nbx(q * 8) + (8 * NB - 1) / 2; }

  function accRowN(st, s) {
    var Y = 24, bits = bitsOf(st.acc), n = s.kind === 'put' ? s.o.n : 0;
    T(NX0, Y - 8, 'acc, bit 63 to bit 0', 'ac-sub');
    T(NX0 + 32 * NB + 3 * NG - 1, Y - 8, 'cnt = ' + st.cnt, 'ac-sub', 'end');
    for (var i = 0; i < 64; i++) {
      var b = 63 - i, x = nbx(i), y = nby(i, Y), counts = b < st.cnt, fresh = b < n;
      var cls = fresh ? 'ac-new' : counts ? (s.kind === 'store' ? 'ac-out' : 'ac-on') : 'ac-off';
      rect(x, y, NB - 1, 20, cls);
      T(x + (NB - 1) / 2, y + 14.5, bits[i], fresh || (counts && s.kind === 'store') ? 'ac-tin' : counts ? '' : 'ac-faint', 'middle', 11.5);
    }
  }

  function putRowN(s) {
    // the value at the columns of acc's bits it goes to, all in the lower row: at most 32 bits
    var o = s.o, Y = 96, i = 64 - o.n;
    T(NX0, Y - 8, 'the value enc_put() appends: ' + plural(o.n, 'bit') + ', 0x' + o.v, 'ac-sub');
    o.parts.forEach(function (pt, m) {
      var bits = bin(pt[2], pt[1]), x0 = nbx(i);
      for (var j = 0; j < pt[1]; j++, i++) {
        rect(nbx(i), Y, NB - 1, 20, 'ac-p' + (pt[0] === 'offset' || pt[0] === 'extra' ? 1 : pt[0] === 'escape' ? 2 : 0));
        T(nbx(i) + (NB - 1) / 2, Y + 14.5, bits[j], 'ac-tin', 'middle', 11.5);
      }
      var x1 = nbx(i - 1) + NB - 1;
      ctx.svg('path', { d: 'M' + (x0 + 1) + ',' + (Y + 25) + ' v4 H' + (x1 - 1) + ' v-4', class: 'ac-brace' }, g);
      var label = pt[0] + (pt[0] === 'token' && s.o.parts.length > 1 && s.o.parts[0][0] === 'escape' ? ' ' + pt[2] : '');
      T((x0 + x1) / 2, Y + 43, label, 'ac-muted', 'middle', 11.5);
    });
  }

  function storeRowN(s) {
    var o = s.o, Y = 96, bits = bitsOf(o.word);
    T(NX0, Y - 8, 'acc << (64 − ' + o.cnt0 + '): the word stored', 'ac-sub');
    for (var i = 0; i < 64; i++) {
      var x = nbx(i), y = nby(i, Y) + (i >> 5) * 18, on = i < o.cnt0;
      rect(x, y, NB - 1, 20, on ? 'ac-out' : 'ac-off');
      T(x + (NB - 1) / 2, y + 14.5, bits[i], on ? 'ac-tin' : 'ac-faint', 'middle', 11.5);
    }
    for (var q = 0; q < 8; q++) {
      // byte 7 of the word, the top, goes to p − 1; under each byte the byte of dst it goes to
      var t = T(nbyte(q), Y + 33 + (q >> 2) * 42, '', 'ac-hex', 'middle', 11.5);
      ctx.svg('tspan', { text: hex2(parseInt(o.word.substr(q * 2, 2), 16)) }, t);
      ctx.svg('tspan', { text: ' → ' + endShort(o.p0 + q + 1), class: 'ac-muted' }, t);
    }
  }

  function dstRowN(st, s) {
    var N = 16;
    for (var j = 0; j < N; j++) {
      var x = NDX + (j & 7) * NCW, y = NDY + (j >> 3) * NDP, written = st.tail && j < st.wrote, v = written ? st.tail[23 - j] : null;
      var cls = !written ? 'ac-empty' : j < st.fullTo ? 'ac-fin' : j === st.partAt ? 'ac-part' : 'ac-below';
      if (s.kind === 'store' && j >= s.o.p0 && j < s.o.p0 + 8) cls += ' ac-hit';
      rect(x, y, NCW - 4, 30, cls);
      if (written) T(x + (NCW - 4) / 2, y + 20, hex2(v), j < st.fullTo || j === st.partAt ? 'ac-fint' : 'ac-faint', 'middle', 13);
      T(x + (NCW - 4) / 2, y + 44, '−' + (j + 1), 'ac-muted', 'middle', 11.5);
      if (!(j & 7)) T(NDX - 6, y + 44, 'end', 'ac-muted', 'end', 11.5);
    }
    // p between two rows is at the end of the first: the line stays next to the bytes written
    var row = st.p === 0 ? 0 : (st.p - 1) >> 3, px = NDX + (st.p - 8 * row) * NCW - 2, py = NDY + row * NDP;
    ctx.svg('line', { x1: px, y1: py - 5, x2: px, y2: py + 35, class: 'ac-ptr' }, g);
    var pt = 'p = ' + end(st.p), w = pt.length * 7.2;
    T(Math.max(NDX + w / 2, Math.min(NDX + 8 * NCW - 4 - w / 2, px)), py - 9, pt, 'ac-ptrt', 'middle');
    T(NX0, NDY + 2 * NDP + 2, 'dst from its end backwards:', 'ac-muted', 'start', 11.5);
    T(NX0, NDY + 2 * NDP + 18, 'bitstream byte 0 first', 'ac-muted', 'start', 11.5);
  }

  function drawN(s, st) {
    root.setAttribute('viewBox', '0 0 ' + NW + ' ' + NH);
    accRowN(st, s);
    if (s.kind === 'put') putRowN(s);
    if (s.kind === 'store') storeRowN(s);
    if (s.kind === 'move') {
      T(NX0, 88, 'enc_flush(), after the store', 'ac-sub');
      T(NX0, 116, 'p −= ' + s.o.cnt0 + ' >> 3, ' + plural(s.o.p1 - s.o.p0, 'byte') + ';', 'ac-big', 'start', 15);
      T(NX0, 140, 'cnt &= 7, ' + s.o.cnt0 + ' → ' + s.o.cnt1, 'ac-big', 'start', 15);
    }
    dstRowN(st, s);
  }

  function say(k, st) {
    var s = steps[k];
    if (s.kind === 'start') {
      return 'encoder_init(): acc = 0, cnt = 0, and p at the end of dst. Next: the bits of the first ' + nSeq +
        ' sequences of chapter 16\'s page, put by put and flush by flush.';
    }
    var o = s.o, q = seqOf[o.seq];
    if (s.kind === 'put' && o.what === 'token') {
      var head = 'Sequence ' + (o.seq + 1) + ': ' + plural(q.ll, 'literal') + ', a match of ' + q.ml + ' bytes at offset ' + q.off + '. ';
      var tok = q.esc ? 'Token ' + q.tok + ' has no code of its own: the escape\'s code ' + bin(o.parts[0][2], o.parts[0][1]) +
        ', then ' + q.tok + ' in 12 bits. ' : 'Token ' + q.tok + ' has the ' + q.tlen + '-bit code ' + bin(o.parts[0][2], o.parts[0][1]) + '. ';
      var off = q.cls === 0 ? 'The offset is the one before, class 0, no bits. '
        : 'The offset' + (q.cls >= 4 || q.cls === 1 ? ', divided by ' + (q.cls === 1 ? 2 : 8) + ' in class ' + q.cls + ', is ' + o.parts[o.parts.length - 1][2] : ' in class ' + q.cls + ' is ' + q.off) +
          ' in ' + plural(q.raw, 'bit') + '. ';
      return head + tok + off + 'One enc_put(): acc shifts left by ' + o.n + ', the bits go in at the bottom, cnt ' + o.cnt0 + ' → ' + o.cnt1 + '.';
    }
    if (s.kind === 'put') {
      var v = s.val, cap = o.what === 'll' ? 15 : 31, len = o.what === 'll' ? 'll = ' + q.ll : 'ml − 4 = ' + (q.ml - 4);
      return len + ' is ' + cap + ' or more, so the token says ' + cap + ' and a length value of ' + (v.value + cap) + ' − ' + cap + ' = ' + v.value + ' follows: symbol ' + v.sym +
        ', code ' + bin(o.parts[0][2], o.parts[0][1]) + (o.parts.length > 1 ? ' and ' + o.parts[1][1] + ' extra bits' : '') +
        '. cnt ' + o.cnt0 + ' → ' + o.cnt1 + '.' + (o.what === 'll' ? ' After an ll value the encoder flushes at once: an ml value on top could overflow the 64 bits.' : '');
    }
    var top = o.word.substr(0, 2);
    if (s.kind === 'store') {
      if (o.cnt0 < 8) {
        return 'enc_flush() with only ' + plural(o.cnt0, 'bit') + ': the shift by ' + (64 - o.cnt0) + ' puts them on top, and the store writes the same byte, ' +
          top + ', to p − 1 = ' + end(o.p0 + 1) + ' again, with zeros below.';
      }
      return 'enc_flush(): acc << ' + (64 - o.cnt0) + ' puts the ' + o.cnt0 + ' bits that count on top, the oldest at bit 63. A little-endian store of the word writes its top byte, ' +
        top + ', to p − 1 = ' + end(o.p0 + 1) + ', and the other 7 below it.';
    }
    return 'p moves down by ' + o.cnt0 + ' >> 3 = ' + plural(o.p1 - o.p0, 'byte') + ', and cnt = ' + o.cnt0 + ' & 7 = ' + o.cnt1 + '.' +
      (o.cnt1 ? ' Those ' + o.cnt1 + ' bits are the top of the byte at p − 1 and are written again by the next flush.' : '') +
      ' The bits of acc above cnt stay, but no longer count.';
  }

  function draw(k) {
    while (g.firstChild) g.removeChild(g.firstChild);
    var s = steps[k], st = state(k);
    // the mode can change between two draws, e.g. a phone turned
    if (ctx.narrow) { drawN(s, st); ctx.say(say(k, st)); return; }
    root.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
    accRow(st, s);
    if (s.kind === 'put') putRow(s);
    if (s.kind === 'store') storeRow(s, st);
    if (s.kind === 'move') {
      T(X0, 83, 'enc_flush(), after the store', 'ac-sub');
      T(X0, 112, 'p −= ' + s.o.cnt0 + ' >> 3, ' + plural(s.o.p1 - s.o.p0, 'byte') + ';  cnt &= 7, ' + s.o.cnt0 + ' → ' + s.o.cnt1, 'ac-big', 'start', 15);
    }
    dstRow(st, s);
    ctx.say(say(k, st));
  }
  return { steps: steps.length, draw: draw, interval: 1400, wide: 760 };
});
