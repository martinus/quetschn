/*
 * From code lengths to a decode table: the canonical codes of the six offset classes by the rule of
 * deflate, the 8 entries of a 3-bit table filled code by code, then four lookups. All computed here
 * from the lengths 2, 3, 3, 3, 2, 3 and the classes 4, 1, 0, 5 of chapter 2.
 */
BookAnim.define('table', function (ctx) {
  var LEN = [2, 3, 3, 3, 2, 3], MSG = [4, 1, 0, 5], L = 3, i, l;
  var W = 720, H = 330;

  // the canonical rule: count, first, then consecutive numbers per length, in symbol order
  var count = [0, 0, 0, 0], first = [0, 0, 0, 0], code = [], order = [];
  LEN.forEach(function (n) { count[n]++; });
  for (l = 2; l <= L; l++) first[l] = (first[l - 1] + count[l - 1]) * 2;
  var next = first.slice();
  for (l = 1; l <= L; l++) for (i = 0; i < LEN.length; i++) if (LEN[i] === l) { code[i] = next[l]++; order.push(i); }
  function bin(v, n) { var s = v.toString(2); while (s.length < n) s = '0' + s; return s; }
  // the table: for each code, the indices whose top l bits are the code
  var table = [], fills = [];
  order.forEach(function (c) {
    var lo = code[c] << (L - LEN[c]), hi = (code[c] + 1) << (L - LEN[c]);
    fills.push({ c: c, lo: lo, hi: hi });
    for (var j = lo; j < hi; j++) table[j] = c;
  });
  var bits = MSG.map(function (c) { return bin(code[c], LEN[c]); }).join('');
  var looks = [], at = 0;
  MSG.forEach(function () { var idx = parseInt((bits + '000').substr(at, L), 2); looks.push({ at: at, idx: idx, c: table[idx] }); at += LEN[table[idx]]; });

  var N = order.length, S_COUNT = 1, S_FIRST = 2, S_CODE = 3, S_FILL = S_CODE + N, S_LOOK = S_FILL + N, S_END = S_LOOK + looks.length;

  var root = ctx.svg('svg', { role: 'img', 'aria-label': 'The canonical codes of the six offset classes, the 8 entries of a 3-bit decode table, and a bitstream decoded with it' }, ctx.stage);
  var g = ctx.svg('g', {}, root);
  function t(x, y, s, cls, a) {
    var at2 = { x: x, y: y, 'font-size': 13, text: s };
    if (cls) at2['class'] = cls;
    for (var k in a || {}) at2[k] = a[k];
    return ctx.svg('text', at2, g);
  }

  function draw(k) {
    while (g.firstChild) g.removeChild(g.firstChild);
    // narrow, about one unit per pixel on a phone: count and first beside the codes, the decode table
    // below them, the bits below that
    var nw = ctx.narrow;
    root.setAttribute('viewBox', nw ? '0 0 350 482' : '0 0 ' + W + ' ' + H);
    var codesKnown = k < S_CODE ? 0 : Math.min(N, k - S_CODE + 1), filled = k < S_FILL ? 0 : Math.min(N, k - S_FILL + 1);
    var nowCode = k >= S_CODE && k < S_FILL ? order[k - S_CODE] : -1;
    var nowFill = k >= S_FILL && k < S_LOOK ? fills[k - S_FILL] : null;
    var look = k >= S_LOOK && k < S_END ? looks[k - S_LOOK] : null;
    var known = {};
    for (i = 0; i < codesKnown; i++) known[order[i]] = true;

    // left: the symbols, their lengths and codes
    var X1 = 24, Y1 = 34, RH = 24;
    t(X1, Y1, 'class', 'tb-mut', { 'font-size': 12 }); t(X1 + 70, Y1, 'length', 'tb-mut', { 'font-size': 12 }); t(X1 + 140, Y1, 'code', 'tb-mut', { 'font-size': 12 });
    for (i = 0; i < LEN.length; i++) {
      var y = Y1 + 10 + i * RH, hot = i === nowCode || nowFill && nowFill.c === i || look && look.c === i;
      ctx.svg('rect', { x: X1 - 6, y: y, width: 206, height: RH - 3, rx: 3, class: hot ? 'tb-row-hot' : 'tb-row' }, g);
      t(X1 + 16, y + 16, String(i), null, { 'text-anchor': 'middle' });
      t(X1 + 92, y + 16, String(LEN[i]), k >= S_COUNT && LEN[i] === 2 ? 'tb-mark' : null, { 'text-anchor': 'middle' });
      if (known[i]) t(X1 + 140, y + 16, bin(code[i], LEN[i]), 'tb-acc tb-bold');
    }
    // middle: count and first
    var X2 = nw ? 232 : 268;
    if (k >= S_COUNT) {
      t(X2, Y1 + 26, 'count[2] = ' + count[2], 'tb-mark', { 'font-size': 12 });
      t(X2, Y1 + 46, 'count[3] = ' + count[3], null, { 'font-size': 12 });
    }
    if (k >= S_FIRST) {
      t(X2, Y1 + 80, 'first[2] = ' + first[2], null, { 'font-size': 12 });
      t(X2, Y1 + 100, 'first[3] = ' + first[3], null, { 'font-size': 12 });
    }
    // right: the decode table
    var X3 = nw ? 63 : 456, RH3 = 20, Y3 = nw ? 214 : Y1;
    t(X3, Y3, 'index', 'tb-mut', { 'font-size': 12 }); t(X3 + 70, Y3, 'entry', 'tb-mut', { 'font-size': 12 });
    for (i = 0; i < (1 << L); i++) {
      var y3 = Y3 + 10 + i * RH3, isFilled = false, isNow = nowFill && i >= nowFill.lo && i < nowFill.hi, isLook = look && look.idx === i;
      for (var f = 0; f < filled; f++) if (i >= fills[f].lo && i < fills[f].hi) isFilled = true;
      ctx.svg('rect', { x: X3 - 6, y: y3, width: 236, height: RH3 - 3, rx: 3, class: isNow || isLook ? 'tb-row-hot' : 'tb-row' }, g);
      var ib = bin(i, L);
      if (nowFill && isNow) {
        // the top l bits are the code, the rest can be anything
        var lc = LEN[nowFill.c];
        t(X3 + 6, y3 + 13, ib.substr(0, lc), 'tb-acc tb-bold');
        t(X3 + 6 + lc * 7.8, y3 + 13, ib.substr(lc), 'tb-mut');
      } else t(X3 + 6, y3 + 13, ib, isLook ? 'tb-acc tb-bold' : null);
      if (isFilled) t(X3 + 70, y3 + 13, 'class ' + table[i] + ', ' + LEN[table[i]] + ' bits', isNow || isLook ? 'tb-acc tb-bold' : null, { 'font-size': 12 });
    }
    // bottom: the bits and the window of 3
    var Y4 = nw ? 418 : 262, BX = nw ? 50 : 140, BWd = 26;
    if (k >= S_LOOK) {
      t(BX - 14, Y4 + 18, 'bits', 'tb-mut', { 'text-anchor': 'end', 'font-size': 12 });
      var used = look ? look.at : k >= S_END ? bits.length : 0;
      for (i = 0; i < bits.length; i++) {
        var x4 = BX + i * BWd;
        ctx.svg('rect', { x: x4, y: Y4, width: BWd, height: 26, class: i < used ? 'tb-used' : 'tb-bit' }, g);
        t(x4 + BWd / 2, Y4 + 18, bits[i], i < used ? 'tb-mut' : null, { 'text-anchor': 'middle' });
      }
      var off = 0;
      var shownCodes = k >= S_END ? MSG.length : k - S_LOOK + 1;
      MSG.forEach(function (c, j) { var w = LEN[c]; if (j >= shownCodes) return; ctx.svg('path', { d: 'M' + (BX + off * BWd + 3) + ',' + (Y4 + 32) + ' v4 h' + (w * BWd - 6) + ' v-4', class: 'tb-brace' }, g); t(BX + (off + w / 2) * BWd, Y4 + 52, 'class ' + c, 'tb-mut', { 'text-anchor': 'middle', 'font-size': 11 }); off += w; });
      if (look) {
        ctx.svg('rect', { x: BX + look.at * BWd - 2, y: Y4 - 3, width: L * BWd + 4, height: 32, rx: 3, class: 'tb-window' }, g);
        t(BX + (look.at + L / 2) * BWd, Y4 - 10, 'peek 3', 'tb-acc', { 'text-anchor': 'middle', 'font-size': 12 });
      }
    }
    say(k);
  }

  function say(k) {
    if (k === 0) return ctx.say('The code lengths of the six offset classes, from the Huffman tree: 2, 3, 3, 3, 2, 3. They are all a table stores, one byte per symbol. The codes follow from them by a fixed rule.');
    if (k === S_COUNT) return ctx.say('Step 1 of the rule counts the codes of each length: count[2] = ' + count[2] + ', classes 0 and 4, and count[3] = ' + count[3] + ', classes 1, 2, 3 and 5.');
    if (k === S_FIRST) return ctx.say('Step 2: first[2] = (0 + count[1]) * 2 = ' + first[2] + ', and first[3] = (first[2] + count[2]) * 2 = (' + first[2] + ' + ' + count[2] + ') * 2 = ' + first[3] + '. The 3-bit codes go on where the 2-bit codes end, shifted one bit to the left.');
    if (k < S_FILL) {
      var c = order[k - S_CODE], l2 = LEN[c], nth = code[c] - first[l2];
      return ctx.say('Class ' + c + ' is symbol ' + (nth + 1) + ' of ' + count[l2] + ' with ' + l2 + ' bits, so it gets first[' + l2 + '] + ' + nth + ' = ' + code[c] + ', in ' + l2 + ' binary digits ' + bin(code[c], l2) + '.');
    }
    if (k < S_LOOK) {
      var fl = fills[k - S_FILL], l3 = LEN[fl.c], idx = [];
      for (i = fl.lo; i < fl.hi; i++) idx.push(bin(i, L));
      var tail = k === S_LOOK - 1 ? ' All 8 entries are full: the code is complete, every 3 bits start a code.' : '';
      return ctx.say('Class ' + fl.c + ', code ' + bin(code[fl.c], l3) + ', fills 2^(3 - ' + l3 + ') = ' + (fl.hi - fl.lo) + (fl.hi - fl.lo === 1 ? ' entry, ' : ' entries, ') + idx.join(' and ') +
        (l3 < L ? ': every index that starts with ' + bin(code[fl.c], l3) + '.' : '.') + tail);
    }
    if (k < S_END) {
      var lk = looks[k - S_LOOK], left = bits.length - lk.at;
      return ctx.say('Peek at 3 bits without taking them: ' + bin(lk.idx, L) + '. Entry ' + bin(lk.idx, L) + ' says class ' + lk.c + ', ' + LEN[lk.c] + ' bits, so the decoder drops ' + LEN[lk.c] + ' of the ' + left + ' bits left' +
        (LEN[lk.c] < L ? ', and the third bit stays for the next code.' : '.'));
    }
    ctx.say(looks.length + ' lookups for ' + looks.length + ' codes, ' + bits.length + ' bits: the classes ' + MSG.join(', ') + '. One lookup per code, whatever its length.');
  }

  return { steps: S_END + 1, draw: draw, interval: 1300, narrow: true };
});
