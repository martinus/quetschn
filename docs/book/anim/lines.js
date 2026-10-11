/*
 * The 180 token lookups of chapter 16's page (anim/lines.c finds their 11-bit indices with the real
 * codec), and the cache lines of the token table they touch: most significant bit first, as seqlz
 * reads its bitstream, against the same bits read the other way, the index bit-reversed.
 */
BookAnim.define('lines', function (ctx) {
  var D = ctx.data, IDX = D.idx, LEN = D.len, NL = IDX.length, BITS = 11, ONE = 12, GROUP = 4;
  var COLS = 16, CELL = 24, CH = 20, GX = [44, 500], GY = 64, W = 900, H = GY + 8 * CH + 66;

  function rev(v) { var r = 0; for (var i = 0; i < BITS; i++) r = r << 1 | (v >> i & 1); return r; }
  function bin(v) { var s = v.toString(2); while (s.length < BITS) s = '0' + s; return s; }
  // the entry a lookup reads, most significant bit first (0) or bits reversed (1)
  function entry(side, i) { return side ? rev(IDX[i]) : IDX[i]; }
  // the lines that hold an entry of the code of lookup i, whatever bits follow it
  function codeLines(side, i) {
    var l = LEN[i], free = BITS - l, pre = IDX[i] >> free, set = {}, j;
    for (j = 0; j < 1 << free; j++) {
      var e = pre << free | j;
      set[(side ? rev(e) : e) >> 4] = 1;
    }
    return set;
  }
  // how many lookups are done after step k
  function upto(k) { return k <= ONE ? k : Math.min(NL, ONE + GROUP * (k - ONE)); }
  var STEPS = ONE + Math.ceil((NL - ONE) / GROUP) + 1;

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var endBtn = ctx.el('button', null, 'to the end', choice);
  endBtn.type = 'button';
  endBtn.addEventListener('click', function () {
    var scrub = ctx.fig.querySelector('.an-scrub');
    scrub.value = scrub.max;
    scrub.dispatchEvent(new Event('input'));
  });

  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The 128 cache lines of the token table, twice: the lines that the token lookups of one page touch with the bitstream read most significant bit first, and with the index bit-reversed' }, ctx.stage);
  var cells = [ctx.svg('g', {}, root), ctx.svg('g', {}, root)];
  var g = ctx.svg('g', { 'font-size': 12 }, root);
  var cellEls = [[], []];
  [0, 1].forEach(function (side) {
    for (var c = 0; c < 128; c++) {
      cellEls[side].push(ctx.svg('rect', { x: GX[side] + (c % COLS) * CELL, y: GY + Math.floor(c / COLS) * CH, width: CELL, height: CH }, cells[side]));
    }
  });

  function text(x, y, t, attrs) {
    var o = { x: x, y: y, text: t };
    for (var k in attrs || {}) o[k] = attrs[k];
    return ctx.svg('text', o, g);
  }
  // the bits of a lookup, the code's bits marked
  function bits(x, y, v, l, codeFirst) {
    var s = bin(v), t = ctx.svg('text', { x: x, y: y }, g);
    for (var i = 0; i < BITS; i++) {
      var inCode = codeFirst ? i < l : i >= BITS - l;
      ctx.svg('tspan', { class: inCode ? 'ln-code-t' : 'ln-lab', text: s[i] }, t);
    }
    return t;
  }

  function draw(k) {
    var n = upto(k), first = k <= ONE ? n - 1 : upto(k - 1), last = n - 1, side, i;
    while (g.firstChild) g.removeChild(g.firstChild);
    var seen = [{}, {}], count = [0, 0], newIn = [0, 0], hits = [[], []];
    for (i = 0; i < n; i++) {
      for (side = 0; side < 2; side++) {
        var line = entry(side, i) >> 4;
        hits[side][line] = (hits[side][line] || 0) + 1;
        if (!seen[side][line]) {
          seen[side][line] = 1; count[side]++;
          if (i >= first) newIn[side]++;
        }
      }
    }
    var cur = [{}, {}], code = [{}, {}];
    for (side = 0; side < 2; side++) {
      for (i = first; i < n && i >= 0 && k < STEPS - 1; i++) cur[side][entry(side, i) >> 4] = 1;
      if (n > 0 && k < STEPS - 1) code[side] = codeLines(side, last);
      for (var c = 0; c < 128; c++) {
        var cls = cur[side][c] ? 'ln-cur' : seen[side][c] ? 'ln-seen' : 'ln-free';
        cellEls[side][c].setAttribute('class', 'an-move ' + cls);
        if (code[side][c]) ctx.svg('rect', { x: GX[side] + (c % COLS) * CELL + 1, y: GY + Math.floor(c / COLS) * CH + 1, width: CELL - 2, height: CH - 2, class: 'ln-code' }, g);
        // how many of the lookups so far read this line
        if (hits[side][c]) text(GX[side] + (c % COLS) * CELL + CELL / 2, GY + Math.floor(c / COLS) * CH + 14, hits[side][c], { 'text-anchor': 'middle', 'font-size': 11, class: cur[side][c] ? 'ln-on-cur' : '' });
      }
    }

    var titles = ['most significant bit first (seqlz)', 'least significant bit first (bits reversed)'];
    for (side = 0; side < 2; side++) {
      var gx = GX[side];
      text(gx, GY - 34, titles[side], { 'font-weight': 'bold' });
      if (n > 0 && k < STEPS - 1) {
        var e = entry(side, last);
        text(gx, GY - 14, 'index ' + e + ' = ');
        bits(gx + 7.2 * ('index ' + e + ' = ').length, GY - 14, e, LEN[last], side === 0);
        text(gx + 7.2 * ('index ' + e + ' = ').length + 7.2 * 12, GY - 14, 'line ' + (e >> 4), { class: 'ln-cur-t' });
      }
      for (var r = 0; r < 8; r++) text(gx - 6, GY + r * CH + 14, r * COLS, { 'text-anchor': 'end', 'font-size': 11, class: 'ln-lab' });
      text(gx, GY + 8 * CH + 24, 'lines touched: ' + count[side] + ' of 128', { 'font-size': 15, 'font-weight': 'bold' });
    }
    // the legend
    var ly = H - 16, lx = GX[0];
    [['ln-cur', 'the line of this lookup'], ['ln-seen', 'touched before, and how often'], ['ln-code', 'lines this code can reach']].forEach(function (l) {
      ctx.svg('rect', { x: lx, y: ly - 11, width: 14, height: 14, class: l[0] }, g);
      text(lx + 20, ly, l[1]);
      lx += 46 + l[1].length * 7.2;
    });
    text(W - 20, H - 16, n + ' of ' + NL + ' lookups', { 'text-anchor': 'end', class: 'ln-lab' });
    ctx.say(sentence(k, n, first, last, count, newIn));
  }

  function sentence(k, n, first, last, count, newIn) {
    if (k === 0) return 'Chapter 16\'s page has ' + NL + ' sequences, and each starts with a lookup in the token table: 2048 entries of 4 bytes, 16 to a cache line, 128 lines. Each lookup reads the entry the next 11 bits of the bitstream point to.';
    if (k === STEPS - 1) return 'All ' + NL + ' token lookups of this page: most significant bit first they touch ' + count[0] + ' of the 128 lines, bits reversed ' + count[1] + '. These are this page\'s numbers; the chapter\'s 42.5 against 55.3 lines are the mean of 20 000 pages.';
    if (k <= ONE) {
      var l = LEN[last], s = bin(IDX[last]), a = IDX[last], b = rev(a);
      var na = Object.keys(codeLines(0, last)).length, nb = Object.keys(codeLines(1, last)).length;
      return 'Lookup ' + n + ': the next 11 bits are ' + s + ', and the code is the first ' + l + ', ' + s.slice(0, l) +
        '. Most significant bit first that is entry ' + a + ', line ' + (a >> 4) + (newIn[0] ? ', a new line' : ', touched before') +
        '. Bits reversed it is entry ' + b + ', line ' + (b >> 4) + (newIn[1] ? ', a new line' : ', touched before') +
        '. Whatever bits follow, this code lands in ' + na + (na > 1 ? ' lines' : ' line') + ' in the first layout and in ' + nb + ' in the second.';
    }
    var lens = LEN.slice(first, n);
    return 'Lookups ' + (first + 1) + ' to ' + n + ', codes of ' + lens.slice(0, -1).join(', ') + (lens.length > 1 ? ' and ' : '') + lens[lens.length - 1] +
      ' bits: ' + newIn[0] + ' new ' + (newIn[0] === 1 ? 'line' : 'lines') + ' most significant bit first, ' + newIn[1] + ' bits reversed. So far ' + count[0] + ' against ' + count[1] + ' lines.';
  }

  return { steps: STEPS, draw: draw, interval: 600 };
});
