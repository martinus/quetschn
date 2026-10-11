/*
 * A greedy matcher with a hash table of 16 slots on a short text: at each position the hash of the
 * next 5 bytes picks a slot, the slot gives a candidate, 4 bytes are compared, and the position goes
 * into the slot. A match is measured, emitted as a sequence, and the cursor jumps behind it. The hash
 * is seqlz_hash5() of src/page_lz.h cut to 4 bits; everything is computed here.
 */
BookAnim.define('hash', function (ctx) {
  var TEXTS = [
    ['a line of C', 'buf[i] = buf[i - 1] + buf[i - 2]; buf[i + 1] = buf[i];'],
    ['a line of English', 'a rose is a rose is a rose, and a nose is a nose is a nose.']
  ];
  var SLOTS = 16, HBYTES = 5, MINM = 4;
  var CELL = 17, X0 = 20, YT = 64, W = 960, H = 366;
  var which = 0, ev = [];

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var btns = TEXTS.map(function (t, i) {
    var b = ctx.el('button', null, t[0], choice);
    b.type = 'button'; b.setAttribute('aria-pressed', i === which ? 'true' : 'false');
    b.addEventListener('click', function () {
      which = i; btns.forEach(function (x, j) { x.setAttribute('aria-pressed', j === i ? 'true' : 'false'); });
      run(); ctx.restart();
    });
    return b;
  });

  // seqlz_hash5(): the low 5 bytes, shifted up by 24, times zstd's prime5bytes, the top bits
  var PRIME = BigInt('889523592379'), M64 = (BigInt(1) << BigInt(64)) - BigInt(1);
  function hash5(b, p) {
    var v = BigInt(0);
    for (var i = 0; i < HBYTES; i++) v |= BigInt(b[p + i]) << BigInt(8 * i);
    return Number((((v << BigInt(24)) * PRIME) & M64) >> BigInt(64 - 4));
  }

  // the events of the whole run; each is one step, with the table as it is drawn in that step
  function run() {
    var s = TEXTS[which][1], b = [], i;
    for (i = 0; i < s.length; i++) b.push(s.charCodeAt(i));
    // the row of bytes fills the width at most
    CELL = Math.min(17, Math.floor((W - 2 * X0) / s.length * 10) / 10);
    var n = b.length, tab = [], pos = 0, anchor = 0, seqs = [], lits = 0, tries = 0, fails = 0;
    for (i = 0; i < SLOTS; i++) tab.push(null);
    ev = [{ t: 'start', tab: tab.slice(), seqs: [], pos: 0, anchor: 0 }];
    while (pos + HBYTES <= n) {
      var h = hash5(b, pos), c = tab[h], eq = 0;
      if (c !== null) { tries++; while (eq < MINM && b[c + eq] === b[pos + eq]) eq++; }
      var e = { pos: pos, h: h, cand: c, eq: eq, tabBefore: tab.slice(), seqs: seqs.slice(), anchor: anchor };
      tab[h] = pos;
      e.tab = tab.slice();
      if (c === null || eq < MINM) {
        if (c !== null) fails++;
        e.t = 'lit'; ev.push(e); pos++; continue;
      }
      var len = MINM;
      while (pos + len < n && b[c + len] === b[pos + len]) len++;
      e.t = 'hit'; e.same5 = s.substr(c, HBYTES) === s.substr(pos, HBYTES); ev.push(e);
      ev.push({ t: 'len', pos: pos, h: h, cand: c, len: len, tab: tab.slice(), seqs: seqs.slice(), anchor: anchor });
      var q = { ll: pos - anchor, lit: s.substring(anchor, pos), ml: len, off: pos - c };
      lits += q.ll; seqs.push(q);
      ev.push({ t: 'emit', pos: pos, h: h, cand: c, len: len, tab: tab.slice(), seqs: seqs.slice(), anchor: anchor });
      pos += len; anchor = pos;
    }
    var last = { ll: n - anchor, lit: s.substring(anchor), ml: 0, off: 0 };
    lits += last.ll; seqs.push(last);
    ev.push({ t: 'end', pos: pos, tab: tab.slice(), seqs: seqs.slice(), anchor: anchor, lits: lits, tries: tries, fails: fails });
  }
  run();

  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'A text as a row of bytes, the compare of the bytes at the cursor with a candidate, a hash table of 16 slots, and the sequences emitted so far' }, ctx.stage);
  var layer = ctx.svg('g', {}, root);

  function cx(i) { return X0 + i * CELL + CELL / 2; }
  function q(str) { return '"' + str + '"'; }
  function qc(ch) { return ch === ' ' ? 'a space' : q(ch); }
  function show(ch) { return ch === ' ' ? '·' : ch; }
  function text(x, y, str, attrs) {
    var a = { x: x, y: y, text: str };
    for (var k in attrs || {}) a[k] = attrs[k];
    return ctx.svg('text', a, layer);
  }

  function draw(k) {
    var s = TEXTS[which][1], n = s.length, e = ev[k], i;
    while (layer.firstChild) layer.removeChild(layer.firstChild);
    var decided = e.t === 'start' ? 0 : e.pos;    // bytes before the cursor are decided
    var inMatch = {}, candSet = {}, done = {};
    // bytes covered by emitted matches
    var p = 0;
    e.seqs.forEach(function (sq) { p += sq.ll; for (var j = 0; j < sq.ml; j++) inMatch[p + j] = true; p += sq.ml; });
    if (e.t === 'end') decided = n;
    var cur = e.t === 'start' || e.t === 'end' ? -1 : e.pos;
    var span = e.t === 'len' || e.t === 'emit' ? e.len : (e.cand !== null && e.cand !== undefined ? MINM : 0);
    if (e.t === 'emit') for (i = 0; i < e.len; i++) inMatch[e.pos + i] = true;
    if (cur >= 0 && e.cand !== null && e.cand !== undefined) for (i = 0; i < span; i++) candSet[e.cand + i] = true;
    var cmp = cur >= 0 ? (e.t === 'lit' ? (e.cand === null ? 0 : MINM) : span) : 0;
    if (ctx.narrow) { narrow(e, s, cur, decided, inMatch, candSet); return say(e, s); }
    root.setAttribute('viewBox', '0 0 ' + W + ' ' + H);

    // the arc from the candidate to the cursor
    if (cur >= 0 && e.cand !== null) {
      var a = cx(e.cand), bx = cx(cur), top = YT - 14 - Math.min(40, (cur - e.cand) * 1.2);
      var ok = e.t !== 'lit';
      ctx.svg('path', { d: 'M' + a + ',' + (YT - 3) + ' C' + a + ',' + top + ' ' + bx + ',' + top + ' ' + bx + ',' + (YT - 3), class: ok ? 'hs-arc-ok' : 'hs-arc-bad' }, layer);
      var lx = (a + bx) / 2, ly = YT - 14 - Math.min(40, (cur - e.cand) * 1.2) * 0.75 - 6;
      text(lx, Math.max(13, ly), ok ? 'offset ' + (cur - e.cand) : 'candidate ' + e.cand, { 'text-anchor': 'middle', 'font-size': 12, class: (ok ? 'hs-acc' : 'hs-mark') + ' hs-halo' });
    }
    // the bytes
    for (i = 0; i < n; i++) {
      var x = X0 + i * CELL, cls = 'hs-todo';
      if (inMatch[i]) cls = 'hs-match';
      else if (i < decided && i !== cur) cls = 'hs-lit';
      if (e.t === 'len' && i >= cur && i < cur + e.len) cls = 'hs-match';
      ctx.svg('rect', { x: x, y: YT, width: CELL, height: 26, class: 'hs-cell ' + cls }, layer);
      text(x + CELL / 2, YT + 18, show(s[i]), { 'text-anchor': 'middle', 'font-size': 13, class: s[i] === ' ' ? 'hs-mut' : cls === 'hs-match' ? 'hs-acc' : '' });
    }
    // candidate bytes and cursor bytes, outlined
    for (i in candSet) ctx.svg('rect', { x: X0 + (+i) * CELL + 1.5, y: YT + 1.5, width: CELL - 3, height: 23, class: 'hs-cand' }, layer);
    var box = e.t === 'emit' ? e.pos + e.len : cur;
    if (box >= 0 && box < n) ctx.svg('rect', { x: X0 + box * CELL + 1.5, y: YT + 1.5, width: CELL - 3, height: 23, class: 'hs-cur' }, layer);
    if (cur >= 0) {
      if (e.t === 'lit' || e.t === 'hit') {
        // the 5 bytes the hash covers
        var hx = X0 + cur * CELL + 2, hw = HBYTES * CELL - 4;
        ctx.svg('path', { d: 'M' + hx + ',' + (YT + 30) + ' V' + (YT + 34) + ' H' + (hx + hw) + ' V' + (YT + 30), class: 'hs-brace' }, layer);
      }
    }
    // position numbers: every 5th, and the cursor and the candidate
    var marks = {};
    if (cur >= 0) marks[cur] = 'hs-acc';
    if (e.t === 'emit' && box < n) marks[box] = 'hs-acc';
    if (cur >= 0 && e.cand !== null && e.cand !== undefined) marks[e.cand] = e.t === 'lit' ? 'hs-mark' : 'hs-acc';
    for (i = 0; i < n; i++) {
      var near = false;
      for (var m in marks) if (+m !== i && Math.abs(+m - i) <= 1) near = true;
      if (marks[i]) text(cx(i), YT + 48, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: marks[i] + ' hs-bold' });
      else if (i % 5 === 0 && !near) text(cx(i), YT + 48, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: 'hs-mut' });
    }

    // the compare
    var CY = 136, CX = 130;
    if (cur >= 0 && e.cand !== null && e.t !== 'emit') {
      var shown = e.t === 'lit' ? MINM : e.t === 'hit' ? MINM : Math.min(e.len + 1, n - cur);
      text(CX - 10, CY + 4, 'here, ' + cur, { 'text-anchor': 'end', 'font-size': 12, class: 'hs-acc' });
      text(CX - 10, CY + 44, 'candidate, ' + e.cand, { 'text-anchor': 'end', 'font-size': 12, class: e.t === 'lit' ? 'hs-mark' : 'hs-acc' });
      for (i = 0; i < shown; i++) {
        var x2 = CX + i * CELL, A = s[cur + i], B = s[e.cand + i];
        var same = A === B, counted = e.t === 'lit' ? i <= e.eq : true;
        var c2 = !counted ? 'hs-todo' : same ? 'hs-match' : 'hs-bad';
        ctx.svg('rect', { x: x2, y: CY - 14, width: CELL, height: 24, class: 'hs-cell ' + c2 }, layer);
        ctx.svg('rect', { x: x2, y: CY + 26, width: CELL, height: 24, class: 'hs-cell ' + c2 }, layer);
        text(x2 + CELL / 2, CY + 3, show(A), { 'text-anchor': 'middle', 'font-size': 13, class: A === ' ' ? 'hs-mut' : '' });
        text(x2 + CELL / 2, CY + 43, show(B), { 'text-anchor': 'middle', 'font-size': 13, class: B === ' ' ? 'hs-mut' : '' });
        if (counted) text(x2 + CELL / 2, CY + 22, same ? '=' : '≠', { 'text-anchor': 'middle', 'font-size': 13, class: same ? 'hs-acc hs-bold' : 'hs-mark hs-bold' });
      }
      var note = e.t === 'lit' ? (e.eq === 0 ? 'the first byte differs' : 'byte ' + (e.eq + 1) + ' of 4 differs') :
        e.t === 'hit' ? 'the first 4 bytes are equal' : e.len + ' bytes equal';
      text(CX + shown * CELL + 12, CY + 22, note, { 'font-size': 12, class: e.t === 'lit' ? 'hs-mark' : 'hs-acc' });
    } else if (cur >= 0 && e.t !== 'emit') {
      text(CX - 10, CY + 22, 'slot ' + e.h + ' is empty: nothing to compare', { 'font-size': 12, class: 'hs-mut' });
    }

    // the hash and the table
    var TY = 254, SW = 56, SX = (W - SLOTS * SW) / 2;
    text(SX, TY - 44, 'hash table, 16 slots', { 'font-size': 12, class: 'hs-mut' });
    for (i = 0; i < SLOTS; i++) {
      var sx = SX + i * SW, hot = cur >= 0 && (e.t === 'lit' || e.t === 'hit') && i === e.h;
      text(sx + SW / 2, TY - 6, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: hot ? 'hs-acc hs-bold' : 'hs-mut' });
      ctx.svg('rect', { x: sx + 2, y: TY, width: SW - 4, height: 28, rx: 3, class: 'hs-slot' + (hot ? ' hs-slot-hot' : '') }, layer);
      var val;
      if (hot) val = (e.cand === null ? '-' : e.cand) + '→' + e.pos;
      else val = e.tab[i] === null ? '-' : String(e.tab[i]);
      text(sx + SW / 2, TY + 19, val, { 'text-anchor': 'middle', 'font-size': hot ? 12 : 13, class: hot ? 'hs-acc hs-bold' : e.tab[i] === null ? 'hs-mut' : '' });
    }
    if (cur >= 0 && (e.t === 'lit' || e.t === 'hit')) {
      var hxm = SX + e.h * SW + SW / 2, label = 'hash of ' + q(s.substr(cur, HBYTES)) + ' = ' + e.h;
      var lw = label.length * 7.2, tx = Math.max(SX + lw / 2, Math.min(SX + SLOTS * SW - lw / 2, hxm));
      text(tx, TY - 24, label, { 'text-anchor': 'middle', 'font-size': 12, class: 'hs-acc' });
      ctx.svg('path', { d: 'M' + hxm + ',' + (TY - 20) + ' V' + (TY - 17), class: 'hs-tick' }, layer);
    }

    // the sequences so far
    var QY = 316;
    text(X0, QY, 'sequences:', { 'font-size': 12, class: 'hs-mut' });
    e.seqs.forEach(function (sq, j) {
      var col = j < 3 ? 0 : 1, row = j % 3, x3 = X0 + 96 + col * 430, y3 = QY + row * 20;
      var str = (j + 1) + ': ll ' + sq.ll + (sq.ll ? ' ' + q(sq.lit) : '') + (sq.ml ? ', ml ' + sq.ml + ', offset ' + sq.off : ', no match');
      text(x3, y3, str, { 'font-size': 12, class: e.t === 'emit' && j === e.seqs.length - 1 || e.t === 'end' && j === e.seqs.length - 1 ? 'hs-acc hs-bold' : '' });
    });
    if (!e.seqs.length) text(X0 + 96, QY, 'none yet', { 'font-size': 12, class: 'hs-mut' });

    say(e, s);
  }

  /*
   * The narrow layout, about one unit per pixel on a phone: the bytes in rows of 20, the compare
   * below them, the table as 2 rows of 8 slots, one sequence per line. No arc: the offset and the
   * candidate stand at the compare.
   */
  var NW = 350, PER = 20, NC = 16.5, NX = 10, RP = 50, CC = 19;
  function narrow(e, s, cur, decided, inMatch, candSet) {
    var n = s.length, rows = Math.ceil(n / PER), i;
    function nx(j) { return NX + (j % PER) * NC; }
    function ny(j) { return 6 + Math.floor(j / PER) * RP; }
    var CY = 6 + rows * RP + 6, TY = CY + 134, QY = TY + 128, NH = QY + 20 + (ev[ev.length - 1].seqs.length - 1) * 18 + 10;
    root.setAttribute('viewBox', '0 0 ' + NW + ' ' + NH);
    // the bytes, in rows
    for (i = 0; i < n; i++) {
      var cls = 'hs-todo';
      if (inMatch[i]) cls = 'hs-match';
      else if (i < decided && i !== cur) cls = 'hs-lit';
      if (e.t === 'len' && i >= cur && i < cur + e.len) cls = 'hs-match';
      ctx.svg('rect', { x: nx(i), y: ny(i), width: NC, height: 24, class: 'hs-cell ' + cls }, layer);
      text(nx(i) + NC / 2, ny(i) + 17, show(s[i]), { 'text-anchor': 'middle', 'font-size': 13, class: s[i] === ' ' ? 'hs-mut' : cls === 'hs-match' ? 'hs-acc' : '' });
    }
    for (i in candSet) ctx.svg('rect', { x: nx(+i) + 1.5, y: ny(+i) + 1.5, width: NC - 3, height: 21, class: 'hs-cand' }, layer);
    var box = e.t === 'emit' ? e.pos + e.len : cur;
    if (box >= 0 && box < n) ctx.svg('rect', { x: nx(box) + 1.5, y: ny(box) + 1.5, width: NC - 3, height: 21, class: 'hs-cur' }, layer);
    // the 5 bytes the hash covers, one bracket per row they are in
    if (cur >= 0 && (e.t === 'lit' || e.t === 'hit')) {
      for (var b0 = cur; b0 < cur + HBYTES; ) {
        var b1 = Math.min(cur + HBYTES, (Math.floor(b0 / PER) + 1) * PER), hy = ny(b0) + 27;
        ctx.svg('path', { d: 'M' + (nx(b0) + 2) + ',' + hy + ' V' + (hy + 4) + ' H' + (nx(b1 - 1) + NC - 2) + ' V' + hy, class: 'hs-brace' }, layer);
        b0 = b1;
      }
    }
    var marks = {};
    if (cur >= 0) marks[cur] = 'hs-acc';
    if (e.t === 'emit' && box < n) marks[box] = 'hs-acc';
    if (cur >= 0 && e.cand !== null && e.cand !== undefined) marks[e.cand] = e.t === 'lit' ? 'hs-mark' : 'hs-acc';
    for (i = 0; i < n; i++) {
      var near = false;
      for (var m in marks) if (+m !== i && Math.abs(+m - i) <= 1) near = true;
      if (marks[i]) text(nx(i) + NC / 2, ny(i) + 44, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: marks[i] + ' hs-bold' });
      else if (i % 5 === 0 && !near) text(nx(i) + NC / 2, ny(i) + 44, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: 'hs-mut' });
    }

    // the compare: the bytes here above, the candidate's below
    if (cur >= 0 && e.cand !== null && e.t !== 'emit') {
      var shown = e.t === 'lit' || e.t === 'hit' ? MINM : Math.min(e.len + 1, n - cur), bad = e.t === 'lit' ? 'hs-mark' : 'hs-acc';
      text(NX, CY + 12, 'here, ' + cur, { 'font-size': 12, class: 'hs-acc' });
      if (e.t !== 'lit') text(NW - NX, CY + 12, 'offset ' + (cur - e.cand), { 'text-anchor': 'end', 'font-size': 12, class: 'hs-acc hs-bold' });
      text(NX, CY + 102, 'candidate, ' + e.cand, { 'font-size': 12, class: bad });
      for (i = 0; i < shown; i++) {
        var x2 = NX + i * CC, A = s[cur + i], B = s[e.cand + i];
        var same = A === B, counted = e.t === 'lit' ? i <= e.eq : true;
        var c2 = !counted ? 'hs-todo' : same ? 'hs-match' : 'hs-bad';
        ctx.svg('rect', { x: x2, y: CY + 18, width: CC, height: 24, class: 'hs-cell ' + c2 }, layer);
        ctx.svg('rect', { x: x2, y: CY + 62, width: CC, height: 24, class: 'hs-cell ' + c2 }, layer);
        text(x2 + CC / 2, CY + 35, show(A), { 'text-anchor': 'middle', 'font-size': 13, class: A === ' ' ? 'hs-mut' : '' });
        text(x2 + CC / 2, CY + 79, show(B), { 'text-anchor': 'middle', 'font-size': 13, class: B === ' ' ? 'hs-mut' : '' });
        if (counted) text(x2 + CC / 2, CY + 57, same ? '=' : '≠', { 'text-anchor': 'middle', 'font-size': 13, class: same ? 'hs-acc hs-bold' : 'hs-mark hs-bold' });
      }
      var note = e.t === 'lit' ? (e.eq === 0 ? 'the first byte differs' : 'byte ' + (e.eq + 1) + ' of 4 differs') :
        e.t === 'hit' ? 'the first 4 bytes are equal' : e.len + ' bytes equal';
      text(NW - NX, CY + 102, note, { 'text-anchor': 'end', 'font-size': 12, class: bad });
    } else if (cur >= 0 && e.t !== 'emit') {
      text(NX, CY + 56, 'slot ' + e.h + ' is empty: nothing to compare', { 'font-size': 12, class: 'hs-mut' });
    }

    // the table, 2 rows of 8 slots
    var SW = 41, SX = (NW - 8 * SW) / 2, hotRow = cur >= 0 && (e.t === 'lit' || e.t === 'hit');
    text(NX, TY, 'hash table, 16 slots', { 'font-size': 12, class: 'hs-mut' });
    if (hotRow) text(NW - NX, TY, 'hash of ' + q(s.substr(cur, HBYTES)) + ' = ' + e.h, { 'text-anchor': 'end', 'font-size': 12, class: 'hs-acc' });
    for (i = 0; i < SLOTS; i++) {
      var sx = SX + (i % 8) * SW, sy = TY + 25 + Math.floor(i / 8) * 50, hot = hotRow && i === e.h;
      text(sx + SW / 2, sy - 5, String(i), { 'text-anchor': 'middle', 'font-size': 11, class: hot ? 'hs-acc hs-bold' : 'hs-mut' });
      ctx.svg('rect', { x: sx + 2, y: sy, width: SW - 4, height: 28, rx: 3, class: 'hs-slot' + (hot ? ' hs-slot-hot' : '') }, layer);
      var val = hot ? (e.cand === null ? '-' : e.cand) + '→' + e.pos : e.tab[i] === null ? '-' : String(e.tab[i]);
      text(sx + SW / 2, sy + 19, val, { 'text-anchor': 'middle', 'font-size': hot ? 11 : 13, class: hot ? 'hs-acc hs-bold' : e.tab[i] === null ? 'hs-mut' : '' });
    }

    // the sequences so far, one per line
    text(NX, QY, 'sequences:', { 'font-size': 12, class: 'hs-mut' });
    if (!e.seqs.length) text(NX + 86, QY, 'none yet', { 'font-size': 12, class: 'hs-mut' });
    e.seqs.forEach(function (sq, j) {
      var str = (j + 1) + ': ll ' + sq.ll + (sq.ll ? ' ' + q(sq.lit) : '') + (sq.ml ? ', ml ' + sq.ml + ', offset ' + sq.off : ', no match');
      text(NX, QY + 20 + j * 18, str, { 'font-size': 12, class: (e.t === 'emit' || e.t === 'end') && j === e.seqs.length - 1 ? 'hs-acc hs-bold' : '' });
    });
  }

  function say(e, s) {
    var n = s.length, c = e.cand;
    if (e.t === 'start') return ctx.say('The text has ' + n + ' bytes and the table 16 empty slots. At each position the matcher hashes the next 5 bytes to a slot, takes the position stored there as a candidate, compares 4 bytes, and stores its own position in the slot.');
    var at = 'Position ' + e.pos + ': the 5 bytes ' + q(s.substr(e.pos, HBYTES)) + ' hash to slot ' + e.h;
    if (e.t === 'lit') {
      if (c === null) return ctx.say(at + ', which is empty. With no candidate, ' + qc(s[e.pos]) + ' is a literal. Slot ' + e.h + ' gets ' + e.pos + ' and the cursor moves on by 1.');
      return ctx.say(at + ', which holds ' + c + ', the start of ' + q(s.substr(c, HBYTES)) + '. Two strings in one slot: a collision. ' +
        (e.eq === 0 ? 'The first byte already differs' : 'Byte ' + (e.eq + 1) + ' differs') + ', so ' + qc(s[e.pos]) + ' is a literal, and slot ' + e.h + ' gets ' + e.pos + '.');
    }
    if (e.t === 'hit') return ctx.say(at + ', which holds ' + c + '. The 4 bytes at ' + c + ' are the same as here' + (e.same5 ? '' : ', though the 5 hashed bytes differ') + ': a match, ' + (e.pos - c) + ' bytes back. Slot ' + e.h + ' gets ' + e.pos + '.');
    if (e.t === 'len') {
      var end = e.pos + e.len >= n ? 'the text ends there' : 'byte ' + (e.pos + e.len) + ', ' + qc(s[e.pos + e.len]) + ', differs from byte ' + (c + e.len) + ', ' + qc(s[c + e.len]);
      return ctx.say('Comparing on from the 5th byte: ' + e.len + ' bytes are equal, and ' + end + '. The match is ' + e.len + ' bytes long.' +
        (c + e.len > e.pos ? ' It reads bytes of its own copy, as in the section before.' : ''));
    }
    if (e.t === 'emit') {
      var sq = e.seqs[e.seqs.length - 1];
      return ctx.say('Sequence ' + e.seqs.length + ': the ' + sq.ll + ' literal' + (sq.ll === 1 ? '' : 's') + ' since the last match' + (sq.ll ? ', ' + q(sq.lit) : '') + ', then ml ' + sq.ml + ' at offset ' + sq.off +
        '. The cursor jumps to ' + (e.pos + e.len) + '; the ' + (e.len - 1) + ' positions inside the match are not hashed.');
    }
    var lst = e.seqs[e.seqs.length - 1], left = n - e.pos;
    ctx.say((left === 1 ? '1 byte is left, fewer than the 5 the hash needs: it stays a literal. ' : left ? left + ' bytes are left, fewer than the 5 the hash needs: they stay literals. ' : '') + 'The last sequence is ll ' + lst.ll + ' and no match. ' +
      n + ' bytes became ' + e.seqs.length + ' sequences, ' + e.lits + ' literals and ' + (n - e.lits) + ' bytes in matches. ' +
      e.fails + ' of ' + e.tries + ' candidates failed the compare: collisions. Without the compare, each would have been a wrong match.');
  }

  return { steps: function () { return ev.length; }, draw: draw, interval: 1100, narrow: true };
});
