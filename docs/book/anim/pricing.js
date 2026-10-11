/*
 * lit_cost() of chapter 11 on stream 0 of chapter 16's page: one 64-bit add per literal sums its code
 * lengths in all 8 tables, in 8 lanes of 8 bits; after 25 literals the lanes move into 16-bit lanes
 * before they can overflow. As a choice, the same adds without that move, until a lane overflows.
 * Data: anim/pricing.c, the real tables and lit_cost().
 */
BookAnim.define('pricing', function (ctx) {
  var D = ctx.data, LIT = D.lit, N = LIT.length, RUN = D.run, S = 8, T = 8;
  var S0 = [], k;
  for (k = 0; k < N; k += S) S0.push(LIT[k]);
  var M = S0.length, CHUNKS = Math.ceil(M / RUN);
  function cost(b) { return D.cost[b]; }
  var mode = 'widen';

  function ch(b) { return b === 32 ? '␣' : b === 10 ? '↵' : b > 32 && b < 127 ? String.fromCharCode(b) : (b < 16 ? '0' : '') + b.toString(16); }
  function word(b) { return b > 32 && b < 127 ? '"' + String.fromCharCode(b) + '"' : b === 32 ? 'a space' : '0x' + (b < 16 ? '0' : '') + b.toString(16); }
  function hx(v) { return (v < 16 ? '0' : '') + v.toString(16); }
  function u64(l) { var s = '0x'; for (var t = T - 1; t >= 0; t--) s += hx(l[t]); return s; }
  function list(a) { return a.slice(0, -1).join(', ') + ' and ' + a[a.length - 1]; }

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var btns = [['widen', 'move to 16 bits'], ['narrow', '8-bit lanes only']].map(function (m) {
    var b = ctx.el('button', null, m[1], choice);
    b.type = 'button'; b.setAttribute('aria-pressed', m[0] === mode ? 'true' : 'false');
    b.addEventListener('click', function () {
      mode = m[0];
      btns.forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); });
      plan = build(); ctx.restart();
    });
    return b;
  });

  // the plan: one snapshot per step
  function build() {
    var L = [], x = [0, 0, 0, 0, 0, 0, 0, 0], ev = [0, 0, 0, 0], od = [0, 0, 0, 0], done = 0, overV = 0, i, t;
    function snap(o) {
      o.x = x.slice(); o.ev = ev.slice(); o.od = od.slice(); o.done = done; L.push(o);
    }
    function add(n) { // the next n literals of stream 0, one 64-bit add each, carries included
      var over = -1;
      for (var a = 0; a < n; a++, done++) {
        var c = cost(S0[done]), carry = 0;
        for (var q = 0; q < T; q++) {
          var v = x[q] + c[q] + carry;
          if (v > 255 && over < 0) { over = q; overV = v; }
          x[q] = v & 255; carry = v >> 8;
        }
      }
      return over;
    }
    function widen() {
      for (var q = 0; q < 4; q++) { ev[q] += x[2 * q]; od[q] += x[2 * q + 1]; }
    }
    snap({ say: 'lit_cost[b] is one u64: the code length of byte b in table 0 in its lowest byte, table 7 in its top byte. Stream 0 of chapter 16\'s page gets every 8th literal, ' + M + ' of them; we follow its sum x[0], 8 lanes of 8 bits, one per table.' });
    for (i = 0; i < 4; i++) {
      var b = S0[done];
      add(1);
      snap({ cur: [done - 1, done], cost: b, say: i === 0
        ? 'Literal 0 of the page, ' + word(b) + ': lit_cost[0x' + hx(b) + '] = ' + u64(cost(b)) + ', from ' + cost(b)[0] + ' bits in table 0 to ' + cost(b)[7] + ' in table 7. One add puts all 8 lengths into x[0].'
        : 'Literal ' + 8 * (done - 1) + ', ' + word(b) + ': ' + cost(b).join(', ') + ' bits in tables 0 to 7. One add, and all 8 lanes grow at once: ' + done + ' adds for ' + done + ' literals in 8 tables.' });
    }
    if (mode === 'widen') {
      for (var ck = 0; ck < CHUNKS; ck++) {
        var from = done, to = Math.min(M, (ck + 1) * RUN);
        add(to - from);
        var mx = Math.max.apply(null, x);
        snap({ cur: [from, to], say: ck === 0
          ? 'Literals ' + 8 * from + ' to ' + 8 * (to - 1) + ' of the page, every 8th: ' + RUN + ' adds in all. The fullest lane holds ' + mx + ', table ' + x.indexOf(mx) + '. ' + RUN + ' codes of at most 10 bits are at most 250; a 26th could make 260, which a byte cannot hold. So the chunk ends here.'
          : 'Chunk ' + (ck + 1) + ': ' + (to - from) + ' literals' + (to - from < RUN ? ', all that stream 0 has left' : '') + ', ' + (to - from) + ' adds into a cleared x[0]. The fullest lane holds ' + mx + '.' });
        widen();
        var keep = x.slice();
        snap({ widen: 1, kept: keep, say: ck === 0
          ? 'x & lanes keeps tables 0, 2, 4 and 6, each the low byte of a 16-bit lane of even[0]; x >> 8 & lanes moves tables 1, 3, 5 and 7 into odd[0]. Both are added, and x[0] starts again from 0 for the next chunk.'
          : 'The move to 16 bits again: even[0] and odd[0] now hold the sums of ' + to + ' literals, e.g. table 0 ' + ev[0] + ' bits, far above what a byte holds.' + (to === M ? ' Stream 0 is priced: ' + to + ' adds and ' + (ck + 1) + ' moves.' : '') });
        x = [0, 0, 0, 0, 0, 0, 0, 0];
      }
      var tot = [];
      for (t = 0; t < T; t++) { var s = 0; for (i = 0; i < S; i++) s += D.bits[i][t]; tot.push(s); }
      var best = tot.indexOf(Math.min.apply(null, tot));
      snap({ totals: tot, say: 'Streams 1 to 7 are priced the same way, each in its own x[j], in the same loop. Over all 8 streams the tables take ' + list(tot) + ' bits, tables 0 to 7.' });
      var bb = D.bits.map(function (r) { return r[best]; });
      snap({ totals: tot, best: best, say: 'Table ' + best + ' has the fewest bits, ' + tot[best] + ', and codes the page' + (best === D.set ? ', as byte 2 of the compressed page says' : '') + '. Its lanes also give each stream\'s size: ' + bb[0] + ' bits for stream 0 are ' + Math.ceil(bb[0] / 8) + ' bytes.' });
    } else {
      var f0 = done;
      add(RUN - done);
      snap({ cur: [f0, RUN], say: 'The first ' + RUN + ' literals of stream 0, ' + RUN + ' adds. The fullest lane holds ' + Math.max.apply(null, x) + '. Now the adds go on in the same 8-bit lanes.' });
      var over = -1;
      while (over < 0 && done < M) {
        var b2 = S0[done], before = x.slice();
        over = add(1);
        var sum = overV, q2 = Math.max(0, over);
        snap({ cur: [done - 1, done], cost: b2, over: over, say: over < 0
          ? 'Literal ' + 8 * (done - 1) + ', ' + word(b2) + ': the fullest lane holds ' + Math.max.apply(null, x) + ', still a byte.'
          : 'Literal ' + 8 * (done - 1) + ', ' + word(b2) + ': table ' + q2 + '\'s lane would be ' + before[q2] + ' + ' + cost(b2)[q2] + (sum > before[q2] + cost(b2)[q2] ? ' + 1' : '') + ' = ' + sum + '. A byte keeps ' + (sum & 255) + ', and the carry goes into the lane of table ' + (q2 + 1) + ', which is now off by 1.' });
      }
      var real = D.bits[0];
      function wraps(r) { var w = r.map(function (v) { return Math.floor(v / 256); }), lo = Math.min.apply(null, w), hi = Math.max.apply(null, w); return lo === hi ? lo : lo + (hi === lo + 1 ? ' or ' : ' to ') + hi; }
      add(M - done);
      var w2 = x.slice();
      snap({ cur: [0, M], over: -2, say: 'After all ' + M + ' literals the lanes read ' + list(w2) + ', tables 0 to 7. The true sums of stream 0 are ' + list(real) + ': every lane wrapped around ' + wraps(real) + ' times, and the carries between lanes changed them further.' });
    }
    return L;
  }
  var plan = build();

  // the drawing, wide or narrow (ctx.narrow, a phone): built again when that changes
  var built = null, root, stL, stR, cells, costBox, costTxt, xBox, xFill, xTxt, xHex, xBot, FH, totBox, totTxt;
  var costHex, xU64, evLab, odLab, totLab, evBox, evTxt, odBox, odTxt;
  function buildDrawing() {
    if (root) root.parentNode.removeChild(root);
    built = ctx.narrow;
    costBox = []; costTxt = []; xBox = []; xFill = []; xTxt = []; xHex = []; xBot = []; totBox = []; totTxt = [];
    evBox = []; evTxt = []; odBox = []; odTxt = []; cells = [];
    if (built) buildNarrow(); else buildWide();
  }
  function buildWide() {
    var X0 = 110, CWD = 80, W = X0 + T * CWD + 20, SW = 25, SY = 30, SH = 20;
    var HY = SY + CHUNKS * SH + 28, CY = HY + 8, XY = CY + 56, EY = XY + 86, OY = EY + 40, TY = OY + 54;
    var H = TY + 44;
    root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The code lengths of stream 0\'s literals summed in 8 lanes of one 64-bit number' }, ctx.stage);
    function colX(t) { return X0 + (T - 1 - t) * CWD; }
    stL = ctx.svg('text', { x: 4, y: 16, 'font-size': 13, class: 'pr-head' }, root);
    stR = ctx.svg('text', { x: X0 + T * CWD, y: 16, 'font-size': 13, 'text-anchor': 'end', class: 'pr-head' }, root);
    // stream 0's literals, a row per chunk of 25
    var strip = ctx.svg('g', { 'font-size': 11 }, root);
    for (k = 0; k < CHUNKS; k++) ctx.svg('text', { x: X0 - 8, y: SY + k * SH + 13, 'text-anchor': 'end', class: 'pr-muted', text: 'chunk ' + (k + 1) }, strip);
    for (k = 0; k < M; k++) {
      var cx = X0 + (k % RUN) * SW, cy = SY + Math.floor(k / RUN) * SH;
      cells.push({ r: ctx.svg('rect', { x: cx, y: cy, width: SW - 2, height: SH - 3, rx: 2 }, strip), t: ctx.svg('text', { x: cx + (SW - 2) / 2, y: cy + 12, 'text-anchor': 'middle', text: ch(S0[k]) }, strip) });
    }
    var heads = ctx.svg('g', { 'font-size': 11, class: 'pr-muted' }, root);
    for (k = 0; k < T; k++) ctx.svg('text', { x: colX(k) + CWD / 2, y: HY, 'text-anchor': 'middle', text: 'table ' + k }, heads);

    var lab = ctx.svg('g', { 'font-size': 12 }, root);
    ctx.svg('text', { x: X0 - 8, y: CY + 19, 'text-anchor': 'end', text: 'lit_cost[b]' }, lab);
    ctx.svg('text', { x: X0 - 8, y: XY + 34, 'text-anchor': 'end', text: 'x[0]' }, lab);
    evLab = ctx.svg('text', { x: X0 - 8, y: EY + 19, 'text-anchor': 'end', text: 'even[0]' }, lab);
    odLab = ctx.svg('text', { x: X0 - 8, y: OY + 19, 'text-anchor': 'end', text: 'odd[0]' }, lab);
    totLab = ctx.svg('text', { x: X0 - 8, y: TY + 19, 'text-anchor': 'end', text: 'all 8 streams' }, lab);

    var costG = ctx.svg('g', {}, root);
    var xG = ctx.svg('g', {}, root);
    FH = 52;
    for (k = 0; k < T; k++) {
      var x0 = colX(k) + 2;
      costBox.push(ctx.svg('rect', { x: x0, y: CY, width: CWD - 4, height: 28, rx: 2 }, costG));
      costTxt.push(ctx.svg('text', { x: x0 + (CWD - 4) / 2, y: CY + 19, 'text-anchor': 'middle', 'font-size': 14 }, costG));
      xBox.push(ctx.svg('rect', { x: x0, y: XY, width: CWD - 4, height: 56, rx: 2 }, xG));
      xFill.push(ctx.svg('rect', { x: x0, width: CWD - 4, class: 'pr-fill an-move' }, xG)); xBot.push(XY + 54);
      xTxt.push(ctx.svg('text', { x: x0 + (CWD - 4) / 2, y: XY + 28, 'text-anchor': 'middle', 'font-size': 16, class: 'pr-num' }, xG));
      xHex.push(ctx.svg('text', { x: x0 + (CWD - 4) / 2, y: XY + 46, 'text-anchor': 'middle', 'font-size': 11, class: 'pr-muted' }, xG));
      totBox.push(ctx.svg('rect', { x: x0, y: TY, width: CWD - 4, height: 28, rx: 2 }, root));
      totTxt.push(ctx.svg('text', { x: x0 + (CWD - 4) / 2, y: TY + 19, 'text-anchor': 'middle', 'font-size': 14 }, root));
    }
    costHex = ctx.svg('text', { x: X0, y: CY + 44, 'font-size': 11, class: 'pr-muted' }, root);
    xU64 = ctx.svg('text', { x: X0, y: XY + 72, 'font-size': 11, class: 'pr-muted' }, root);
    for (k = 0; k < 4; k++) {
      var lx = colX(2 * k + 1) + 2;
      evBox.push(ctx.svg('rect', { x: lx, y: EY, width: 2 * CWD - 4, height: 28, rx: 2 }, root));
      evTxt.push(ctx.svg('text', { x: lx + CWD - 2, y: EY + 19, 'text-anchor': 'middle', 'font-size': 13 }, root));
      odBox.push(ctx.svg('rect', { x: lx, y: OY, width: 2 * CWD - 4, height: 28, rx: 2 }, root));
      odTxt.push(ctx.svg('text', { x: lx + CWD - 2, y: OY + 19, 'text-anchor': 'middle', 'font-size': 13 }, root));
    }
  }
  // the narrow layout, 350 units wide: a chunk of 25 literals in 2 rows, the 8 tables as 2 rows of 4
  // cards (tables 7 to 4, then 3 to 0, the byte order of the u64), even[0] and odd[0] in 2 rows each
  function buildNarrow() {
    var W = 350, PR = 13, SX0 = 52, SW = (W - 2 - SX0) / PR, SY = 30, SH = 18;
    var CX0 = 80, CWD = (W - 2 - CX0) / 4, GY = SY + (CHUNKS - 1) * (2 * SH + 6) + Math.ceil((M - (CHUNKS - 1) * RUN) / PR) * SH + 16;
    var CRH = 136, HY = GY + 2 * CRH - 4, EY = HY + 54, OY = EY + 68, H = OY + 58;
    root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The code lengths of stream 0\'s literals summed in 8 lanes of one 64-bit number' }, ctx.stage);
    // without even[0] and odd[0] the drawing ends below x[0]
    root.boxes = ['0 0 ' + W + ' ' + H, '0 0 ' + W + ' ' + (HY + 48)];
    // table t: its column and its row of cards
    function colX(t) { return CX0 + (3 - t % 4) * CWD; }
    function rowY(t) { return GY + (t >= 4 ? 0 : CRH); }
    stL = ctx.svg('text', { x: 2, y: 16, 'font-size': 13, class: 'pr-head' }, root);
    stR = ctx.svg('text', { x: W - 2, y: 16, 'font-size': 13, 'text-anchor': 'end', class: 'pr-head' }, root);
    // stream 0's literals, a chunk of 25 in 2 rows
    var strip = ctx.svg('g', { 'font-size': 11 }, root);
    for (k = 0; k < CHUNKS; k++) ctx.svg('text', { x: SX0 - 6, y: SY + k * (2 * SH + 6) + 12, 'text-anchor': 'end', class: 'pr-muted', text: 'chunk ' + (k + 1) }, strip);
    for (k = 0; k < M; k++) {
      var ck = Math.floor(k / RUN), kk = k % RUN;
      var cx = SX0 + (kk % PR) * SW, cy = SY + ck * (2 * SH + 6) + Math.floor(kk / PR) * SH;
      cells.push({ r: ctx.svg('rect', { x: cx, y: cy, width: SW - 2, height: SH - 3, rx: 2 }, strip), t: ctx.svg('text', { x: cx + (SW - 2) / 2, y: cy + 11.5, 'text-anchor': 'middle', text: ch(S0[k]) }, strip) });
    }
    var heads = ctx.svg('g', { 'font-size': 11, class: 'pr-muted' }, root);
    var lab = ctx.svg('g', { 'font-size': 11 }, root);
    totLab = ctx.svg('g', {}, lab);
    for (var r = 0; r < 2; r++) {
      var ry = GY + r * CRH;
      ctx.svg('text', { x: CX0 - 4, y: ry + 32, 'text-anchor': 'end', text: 'lit_cost[b]' }, lab);
      ctx.svg('text', { x: CX0 - 4, y: ry + 72, 'text-anchor': 'end', text: 'x[0]' }, lab);
      ctx.svg('text', { x: CX0 - 4, y: ry + 111, 'text-anchor': 'end', text: 'all 8' }, totLab);
      ctx.svg('text', { x: CX0 - 4, y: ry + 123, 'text-anchor': 'end', text: 'streams' }, totLab);
    }
    evLab = ctx.svg('text', { x: CX0 - 4, y: EY + 33, 'text-anchor': 'end', 'font-size': 12, text: 'even[0]' }, root);
    odLab = ctx.svg('text', { x: CX0 - 4, y: OY + 33, 'text-anchor': 'end', 'font-size': 12, text: 'odd[0]' }, root);
    var costG = ctx.svg('g', {}, root);
    var xG = ctx.svg('g', {}, root);
    FH = 44;
    for (k = 0; k < T; k++) {
      var x0 = colX(k) + 2, y0 = rowY(k), bw = CWD - 4;
      ctx.svg('text', { x: x0 + bw / 2, y: y0 + 11, 'text-anchor': 'middle', text: 'table ' + k }, heads);
      costBox.push(ctx.svg('rect', { x: x0, y: y0 + 16, width: bw, height: 24, rx: 2 }, costG));
      costTxt.push(ctx.svg('text', { x: x0 + bw / 2, y: y0 + 33, 'text-anchor': 'middle', 'font-size': 14 }, costG));
      xBox.push(ctx.svg('rect', { x: x0, y: y0 + 46, width: bw, height: 48, rx: 2 }, xG));
      xFill.push(ctx.svg('rect', { x: x0, width: bw, class: 'pr-fill an-move' }, xG)); xBot.push(y0 + 92);
      xTxt.push(ctx.svg('text', { x: x0 + bw / 2, y: y0 + 69, 'text-anchor': 'middle', 'font-size': 16, class: 'pr-num' }, xG));
      xHex.push(ctx.svg('text', { x: x0 + bw / 2, y: y0 + 86, 'text-anchor': 'middle', 'font-size': 11, class: 'pr-muted' }, xG));
      totBox.push(ctx.svg('rect', { x: x0, y: y0 + 100, width: bw, height: 24, rx: 2 }, root));
      totTxt.push(ctx.svg('text', { x: x0 + bw / 2, y: y0 + 117, 'text-anchor': 'middle', 'font-size': 14 }, root));
    }
    costHex = ctx.svg('text', { x: 2, y: HY + 10, 'font-size': 11, class: 'pr-muted' }, root);
    xU64 = ctx.svg('text', { x: 2, y: HY + 26, 'font-size': 11, class: 'pr-muted' }, root);
    for (k = 0; k < 4; k++) {
      var lx = colX(2 * k + 1) + 2, ly = 2 * k + 1 >= 4 ? 0 : 32;
      evBox.push(ctx.svg('rect', { x: lx, y: EY + ly, width: 2 * CWD - 4, height: 26, rx: 2 }, root));
      evTxt.push(ctx.svg('text', { x: lx + CWD - 2, y: EY + ly + 18, 'text-anchor': 'middle', 'font-size': 13 }, root));
      odBox.push(ctx.svg('rect', { x: lx, y: OY + ly, width: 2 * CWD - 4, height: 26, rx: 2 }, root));
      odTxt.push(ctx.svg('text', { x: lx + CWD - 2, y: OY + ly + 18, 'text-anchor': 'middle', 'font-size': 13 }, root));
    }
  }
  // a text of one line, or of several lines for an array
  function put(e, s) {
    if (typeof s === 'string') { e.textContent = s; return; }
    while (e.firstChild) e.removeChild(e.firstChild);
    s.forEach(function (l, n) { ctx.svg('tspan', { x: e.getAttribute('x'), dy: n ? 14 : 0, text: l }, e); });
  }

  function set(e, cls) { e.setAttribute('class', cls); }
  function show(e, on) { e.style.display = on ? '' : 'none'; }
  function draw(s) {
    if (built !== ctx.narrow) buildDrawing();
    var p = plan[s], cur = p.cur || [0, 0], i;
    stL.textContent = 'stream 0: ' + p.done + ' of ' + M + ' literals';
    stR.textContent = p.done + ' add' + (p.done === 1 ? '' : 's');
    for (i = 0; i < M; i++) {
      var now = i >= cur[0] && i < cur[1] && !p.widen && p.over !== -2;
      set(cells[i].r, 'an-move ' + (now ? 'pr-now' : i < p.done ? 'pr-done' : 'pr-todo'));
      set(cells[i].t, now ? 'pr-tnow' : i < p.done ? '' : 'pr-muted');
    }
    var c = p.cost != null ? cost(p.cost) : null;
    for (i = 0; i < T; i++) {
      set(costBox[i], c ? 'pr-cost' : 'pr-empty');
      costTxt[i].textContent = c ? c[i] : '';
      var v = p.x[i], h = Math.round(FH * v / 255);
      var bad = p.over != null && p.over !== -1 && (p.over === -2 || i >= p.over && i <= p.over + 1);
      set(xBox[i], 'pr-lane' + (p.widen ? i % 2 ? ' pr-odd' : ' pr-even' : '') + (bad ? ' pr-bad' : ''));
      xFill[i].setAttribute('y', xBot[i] - h); xFill[i].setAttribute('height', h);
      xTxt[i].textContent = v; xHex[i].textContent = '0x' + hx(v);
      show(totBox[i], !!p.totals); show(totTxt[i], !!p.totals);
      if (p.totals) {
        set(totBox[i], i === p.best ? 'pr-best' : 'pr-cost');
        set(totTxt[i], i === p.best ? 'pr-tnow' : '');
        totTxt[i].textContent = p.totals[i];
      }
    }
    costHex.textContent = c ? 'lit_cost[0x' + hx(p.cost) + '] = ' + u64(c) + ', byte ' + word(p.cost) : '';
    var xs = 'x[0] = ' + u64(p.x), moved = 'moved into even[0] and odd[0], then cleared';
    put(xU64, !p.widen ? xs : built ? [xs + ',', moved] : xs + ', ' + moved);
    var wide = mode === 'widen';
    if (built) root.setAttribute('viewBox', root.boxes[wide ? 0 : 1]);
    show(evLab, wide); show(odLab, wide); show(totLab, !!p.totals);
    for (i = 0; i < 4; i++) {
      show(evBox[i], wide); show(evTxt[i], wide); show(odBox[i], wide); show(odTxt[i], wide);
      set(evBox[i], 'pr-wide pr-even' + (p.widen ? ' pr-hot' : ''));
      set(odBox[i], 'pr-wide pr-odd' + (p.widen ? ' pr-hot' : ''));
      evTxt[i].textContent = 'table ' + 2 * i + ': ' + p.ev[i];
      odTxt[i].textContent = 'table ' + (2 * i + 1) + ': ' + p.od[i];
    }
    ctx.say(p.say);
  }
  buildDrawing();
  return { steps: function () { return plan.length; }, draw: draw, interval: 1300, wide: 650 };
});
