/*
 * The literals of chapter 16's page in the 8 streams of chapter 13: dealt out by the encoder, literal k
 * to stream k % 8, then decoded round by round, all 8 streams one code per step. As a choice, the same
 * codes in one stream, one after the other. Data: anim/streams.c, the real codec. The count of
 * dependent steps is a model, one step per decoded literal.
 */
BookAnim.define('streams', function (ctx) {
  var D = ctx.data, LIT = D.lit, LEN = D.len, N = LIT.length, S = 8, R = D.rounds, ROUND = S * R;
  var FULL = Math.floor(N / ROUND), REST = N - FULL * ROUND, TSTEPS = Math.floor(REST / S), TR = REST % S;
  var DEP8 = FULL * R + TSTEPS + 1;
  var ROWS = Math.ceil((N + S - TR) / ROUND), CW, GG, GY, RH;
  var mode = 8;

  var tot = [], cnt = [], maxBits = 0, total = 0, i, k;
  for (i = 0; i < S; i++) { tot.push(0); cnt.push(0); }
  for (k = 0; k < N; k++) { tot[k % S] += LEN[k]; cnt[k % S]++; total += LEN[k]; }
  for (i = 0; i < S; i++) maxBits = Math.max(maxBits, tot[i]);

  function ch(b) { return b === 32 ? '␣' : b === 10 ? '↵' : b > 32 && b < 127 ? String.fromCharCode(b) : (b < 16 ? '0' : '') + b.toString(16); }
  function word(b) { return b > 32 && b < 127 ? '"' + String.fromCharCode(b) + '"' : b === 32 ? 'a space' : '0x' + (b < 16 ? '0' : '') + b.toString(16); }
  function lits(a, b) {
    var s = '', all = true;
    for (var j = a; j < b; j++) { if (LIT[j] <= 32 || LIT[j] >= 127) all = false; s += String.fromCharCode(LIT[j]); }
    if (all) return '"' + s + '"';
    return LIT.slice(a, b).map(word).join(', ');
  }
  function many(n) { return n === S ? 'all 8 streams have' : n === 1 ? 'stream 0 has' : 'streams 0 ' + (n === 2 ? 'and 1' : 'to ' + (n - 1)) + ' have'; }
  function lens(a, b) { return LEN.slice(a, b).join(', '); }
  function range(a) { return Math.min.apply(null, a) + ' to ' + Math.max.apply(null, a); }

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var btns = [[8, '8 streams'], [1, '1 stream']].map(function (m) {
    var b = ctx.el('button', null, m[1], choice);
    b.type = 'button'; b.setAttribute('aria-pressed', m[0] === mode ? 'true' : 'false');
    b.addEventListener('click', function () {
      mode = m[0];
      btns.forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); });
      layoutLanes(); plan = mode === 8 ? steps8() : steps1(); ctx.restart();
    });
    return b;
  });

  var cellB = [], cellHex = [];
  for (k = 0; k < ROWS * ROUND && k < N + S - TR; k++) {
    var b = k < N ? LIT[k] : D.spare[k - N];
    cellB.push(b); cellHex.push(!(b >= 32 && b < 127) && b !== 10);
  }

  // the drawing, wide or narrow (ctx.narrow, a phone): built again when that changes
  var built = null, root, status, stMode, stWhere, stDep, heads, cells, gridKey, laneHead, sizeHead, lanes, segs, curs, wins, junk;
  var bars, BS, bar8, bar1, GX0, zoom, W, H, X0, LW, LY, LH, LG, SC;
  function cx(c) { return GX0 + c * CW + Math.floor(c / S) * GG; }
  function build() {
    if (root) root.parentNode.removeChild(root);
    built = ctx.narrow; zoom = null; sizeHead = null;
    if (built) buildNarrow(); else buildWide();
    SC = LW / maxBits;
    layoutLanes();
  }
  function buildWide() {
    X0 = 70; CW = 17; GG = 6; LW = 40 * CW + 4 * GG; W = 852; GX0 = X0;
    GY = 64; RH = 15;
    LY = GY + ROWS * RH + 44; LH = 18; LG = 4; var BY = LY + 8 * (LH + LG) + 22;
    H = BY + 46;
    root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The literals of a page in 8 streams, dealt out and decoded round by round' }, ctx.stage);
    status = ctx.svg('g', { 'font-size': 13 }, root);
    stMode = ctx.svg('text', { x: 4, y: 18, class: 'st-head' }, status);
    stWhere = ctx.svg('text', { x: 130, y: 18 }, status);
    stDep = ctx.svg('text', { x: X0 + LW, y: 18, 'text-anchor': 'end', class: 'st-head' }, status);

    // the scratch: 40 literals per row, a row is one round
    heads = ctx.svg('g', { 'font-size': 11, class: 'st-muted' }, root);
    ctx.svg('text', { x: X0 - 6, y: GY - 22, 'text-anchor': 'end', text: 'step j' }, heads);
    ctx.svg('text', { x: X0 - 6, y: GY - 6, 'text-anchor': 'end', text: 'stream' }, heads);
    for (k = 0; k < R; k++) ctx.svg('text', { x: cx(k * S) + (S * CW - 1) / 2, y: GY - 22, 'text-anchor': 'middle', text: 'j = ' + k }, heads);
    for (k = 0; k < 40; k++) ctx.svg('text', { x: cx(k) + (CW - 1) / 2, y: GY - 6, 'text-anchor': 'middle', text: k % S }, heads);
    var rowLab = ctx.svg('g', { 'font-size': 11, class: 'st-muted' }, root);
    for (k = 0; k < ROWS; k++) ctx.svg('text', { x: X0 - 6, y: GY + k * RH + 11, 'text-anchor': 'end', text: k * ROUND }, rowLab);
    var grid = ctx.svg('g', { 'font-size': 11 }, root);
    cells = [];
    for (k = 0; k < cellB.length; k++) {
      var x = cx(k % ROUND), y = GY + Math.floor(k / ROUND) * RH;
      var r = ctx.svg('rect', { x: x, y: y, width: CW - 1, height: RH - 1, rx: 1.5 }, grid);
      var t = ctx.svg('text', { x: x + (CW - 1) / 2, y: y + 11, 'text-anchor': 'middle', text: ch(cellB[k]) }, grid);
      cells.push({ r: r, t: t, hex: cellHex[k] });
    }
    gridKey = ctx.svg('text', { x: X0, y: GY + ROWS * RH + 14, 'font-size': 11, class: 'st-muted' }, root);

    // the streams: one box per code, as wide as its code length
    laneHead = ctx.svg('text', { x: X0, y: LY - 10, 'font-size': 11, class: 'st-muted' }, root);
    lanes = ctx.svg('g', {}, root);

    // the dependent steps: two bars on one scale
    bars = ctx.svg('g', { 'font-size': 11 }, root); BS = (LW - 80) / N;
    function bar(y, label, n) {
      ctx.svg('text', { x: X0 - 6, y: y + 11, 'text-anchor': 'end', text: label }, bars);
      ctx.svg('rect', { x: X0, y: y, width: n * BS, height: 14, class: 'st-bar' }, bars);
      var f = ctx.svg('rect', { x: X0, y: y, width: 0, height: 14, class: 'st-fill' }, bars);
      var t = ctx.svg('text', { x: X0 + n * BS + 6, y: y + 11 }, bars);
      return { f: f, t: t, n: n };
    }
    ctx.svg('text', { x: X0, y: BY - 8, class: 'st-muted', text: 'dependent steps, a model: one step per decoded literal' }, bars);
    bar8 = bar(BY, '8 streams', DEP8); bar1 = bar(BY + 22, '1 stream', N);
  }
  // the narrow layout, 350 units wide: the scratch as small cells without text, one row of it zoomed in
  // as 5 rows of 8 with its literals, then the streams and the two bars one below the other
  function buildNarrow() {
    W = 350; GX0 = 28; GG = 4; CW = (W - 2 - GX0 - 4 * GG) / 40; GY = 78; RH = 7;
    var ZY = GY + ROWS * RH + 26, ZX0 = 46, ZCW = (W - 2 - ZX0) / S, ZRH = 20, ZG = ZY + 32;
    X0 = 14; LW = W - 42; LY = ZG + R * ZRH + 42; LH = 14; LG = 4;
    var BY = LY + 8 * (LH + LG) + 24;
    H = BY + 78;
    root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The literals of a page in 8 streams, dealt out and decoded round by round' }, ctx.stage);
    status = ctx.svg('g', { 'font-size': 13 }, root);
    stMode = ctx.svg('text', { x: 2, y: 16, class: 'st-head' }, status);
    stWhere = ctx.svg('text', { x: W - 2, y: 16, 'text-anchor': 'end' }, status);
    stDep = ctx.svg('text', { x: 2, y: 34, class: 'st-head' }, status);

    gridKey = ctx.svg('text', { x: 2, y: GY - 22, 'font-size': 11, class: 'st-muted' }, root);
    heads = ctx.svg('g', { 'font-size': 11, class: 'st-muted' }, root);
    for (k = 0; k < R; k++) ctx.svg('text', { x: cx(k * S) + (S * CW - 1) / 2, y: GY - 5, 'text-anchor': 'middle', text: 'j = ' + k }, heads);
    var rowLab = ctx.svg('g', { 'font-size': 11, class: 'st-muted' }, root);
    for (k = 0; k < ROWS; k += 4) ctx.svg('text', { x: GX0 - 5, y: GY + k * RH + 7, 'text-anchor': 'end', text: k * ROUND }, rowLab);
    var grid = ctx.svg('g', {}, root);
    cells = [];
    for (k = 0; k < cellB.length; k++) {
      cells.push({ r: ctx.svg('rect', { x: cx(k % ROUND), y: GY + Math.floor(k / ROUND) * RH, width: CW - 1, height: RH - 1, rx: 1 }, grid), hex: cellHex[k] });
    }
    // one row of the scratch with its literals: step j down, stream i across
    zoom = { y: ZY, box: ctx.svg('rect', { x: GX0 - 2, width: 40 * CW + 4 * GG + 3, height: RH + 1, rx: 1.5, class: 'st-zoom' }, root) };
    zoom.title = ctx.svg('text', { x: 2, y: ZY - 4, 'font-size': 11, class: 'st-muted' }, root);
    zoom.head = ctx.svg('g', { 'font-size': 11, class: 'st-muted' }, root);
    ctx.svg('text', { x: ZX0 - 4, y: ZG - 6, 'text-anchor': 'end', text: 'stream' }, zoom.head);
    for (k = 0; k < S; k++) ctx.svg('text', { x: ZX0 + k * ZCW + (ZCW - 1) / 2, y: ZG - 6, 'text-anchor': 'middle', text: k }, zoom.head);
    var zg = ctx.svg('g', { 'font-size': 12 }, root);
    zoom.rows = []; zoom.cells = [];
    for (k = 0; k < R; k++) zoom.rows.push(ctx.svg('text', { x: ZX0 - 4, y: ZG + k * ZRH + 13, 'text-anchor': 'end', 'font-size': 11, class: 'st-muted' }, zg));
    for (k = 0; k < ROUND; k++) {
      var zx = ZX0 + (k % S) * ZCW, zy = ZG + Math.floor(k / S) * ZRH;
      zoom.cells.push({ r: ctx.svg('rect', { x: zx, y: zy, width: ZCW - 2, height: ZRH - 2, rx: 2 }, zg), t: ctx.svg('text', { x: zx + (ZCW - 2) / 2, y: zy + 13, 'text-anchor': 'middle' }, zg) });
    }

    laneHead = ctx.svg('text', { x: 2, y: LY - 24, 'font-size': 11, class: 'st-muted' }, root);
    sizeHead = ctx.svg('text', { x: W - 2, y: LY - 8, 'font-size': 11, 'text-anchor': 'end', class: 'st-muted', text: 'bytes' }, root);
    lanes = ctx.svg('g', {}, root);

    bars = ctx.svg('g', { 'font-size': 11 }, root); BS = (W - 4) / N;
    function bar(y, label, n) {
      ctx.svg('text', { x: 2, y: y - 6, text: label }, bars);
      ctx.svg('rect', { x: 2, y: y, width: n * BS, height: 12, class: 'st-bar' }, bars);
      var f = ctx.svg('rect', { x: 2, y: y, width: 0, height: 12, class: 'st-fill' }, bars);
      var t = ctx.svg('text', { x: W - 2, y: y - 6, 'text-anchor': 'end' }, bars);
      return { f: f, t: t, n: n };
    }
    put(ctx.svg('text', { x: 2, y: BY - 6, class: 'st-muted' }, bars), ['dependent steps, a model:', 'one step per decoded literal']);
    bar8 = bar(BY + 30, '8 streams', DEP8); bar1 = bar(BY + 64, '1 stream', N);
  }
  // a text of one line, or of several lines for an array
  function put(e, s) {
    if (typeof s === 'string') { e.textContent = s; return; }
    while (e.firstChild) e.removeChild(e.firstChild);
    s.forEach(function (l, n) { ctx.svg('tspan', { x: e.getAttribute('x'), dy: n ? 14 : 0, text: l }, e); });
  }

  function layoutLanes() {
    while (lanes.firstChild) lanes.removeChild(lanes.firstChild);
    segs = []; curs = []; wins = []; junk = [];
    var row = 0, at = 0, scale = SC, pos = [], j;
    if (mode === 1) {
      // one stream, wrapped at a code's end; the scale shrinks until it fits the 8 rows
      for (;;) {
        pos = []; row = 0; at = 0;
        for (j = 0; j < N; j++) {
          if (at + LEN[j] * scale > LW - 1) { row++; at = 0; }
          pos.push([row, at]); at += LEN[j] * scale;
        }
        if (row < S) break;
        scale *= 0.98;
      }
    } else {
      var off = [0, 0, 0, 0, 0, 0, 0, 0];
      for (j = 0; j < N; j++) { pos.push([j % S, off[j % S] * scale]); off[j % S] += LEN[j]; }
    }
    for (j = 0; j < S; j++) {
      ctx.svg('text', { x: X0 - 6, y: LY + j * (LH + LG) + (built ? 11 : 13), 'text-anchor': 'end', 'font-size': 11, class: 'st-muted', text: built ? (mode === 8 ? j : '') : mode === 8 ? 'stream ' + j : j ? '' : 'stream' }, lanes);
      if (mode === 8) {
        wins.push(ctx.svg('rect', { y: LY + j * (LH + LG) - 2, height: LH + 4, rx: 2, class: 'st-win' }, lanes));
        junk.push(ctx.svg('rect', { x: X0 + tot[j] * scale, y: LY + j * (LH + LG), width: 10 * scale, height: LH, class: 'st-junk' }, lanes));
      }
    }
    var sg = ctx.svg('g', {}, lanes);
    for (j = 0; j < N; j++) {
      segs.push(ctx.svg('rect', { x: X0 + pos[j][1], y: LY + pos[j][0] * (LH + LG), width: LEN[j] * scale, height: LH, class: 'an-move' }, sg));
    }
    for (j = 0; j < (mode === 8 ? S : 1); j++) curs.push(ctx.svg('line', { class: 'st-cur' }, lanes));
    lanes.sizes = [];
    if (mode === 8) {
      for (j = 0; j < S; j++) {
        lanes.sizes.push(built
          ? ctx.svg('text', { x: W - 2, y: LY + j * (LH + LG) + 11, 'text-anchor': 'end', 'font-size': 11, text: D.sz[j] }, lanes)
          : ctx.svg('text', { x: X0 + LW + 18, y: LY + j * (LH + LG) + 13, 'font-size': 11, text: D.sz[j] + ' bytes' }, lanes));
      }
    }
    lanes.pos = pos; lanes.scale = scale;
  }

  // the steps of each mode: what is dealt, decoded, highlighted, and the sentence
  function steps8() {
    var L = [], j, r;
    L.push({ deal: 0, say: 'Chapter 16\'s page has ' + N + ' literals, coded with literal table ' + D.set + ' in ' + total + ' bits. Each row is ' + ROUND + ' literals, one round of the decoder. The encoder deals them out to ' + S + ' streams: literal k goes to stream k % 8.' });
    L.push({ deal: 8, now: [0, 8], say: 'Literals 0 to 7, ' + lits(0, 8) + ', go to streams 0 to 7, one each. Each becomes its code in table ' + D.set + ', of ' + lens(0, 8) + ' bits.' });
    L.push({ deal: 16, now: [8, 16], say: 'Literals 8 to 15 go to streams 0 to 7 again, behind the first codes: ' + lens(8, 16) + ' bits. A frequent byte takes less room in its stream than a rare one.' });
    L.push({ deal: ROUND, now: [16, ROUND], say: 'Literals 16 to ' + (ROUND - 1) + ' follow the same way. A row of ' + ROUND + ' is ' + R + ' literals per stream: literal k + 8 j + i of the row that starts at k is in stream i.' });
    L.push({ deal: N, sizes: 1, say: 'All ' + N + ' are dealt: ' + many(cnt.filter(function (c) { return c === cnt[0]; }).length) + ' ' + cnt[0] + ' literals' + (cnt[S - 1] !== cnt[0] ? ', the others ' + cnt[S - 1] : '') + '. Their codes take ' + range(tot) + ' bits, ' + range(D.sz) + ' bytes; the header stores the 8 sizes, so the decoder knows where each stream starts.' });
    L.push({ out: 0, fill: 0, dep: 0, say: 'The decoder starts with an empty scratch. It refills all 8 streams: each now has at least 56 bits (the outline), enough for ' + R + ' codes of at most 10 bits.' });
    for (j = 0; j < R; j++) {
      var a = S * j, moved = [];
      for (i = 0; i < S; i++) moved.push(LEN[a + i]);
      L.push({ out: a + S, now: [a, a + S], fill: 0, dep: j + 1, say: 'Step j = ' + j + ': each stream looks up the top 10 bits of its own 64, and none waits for another. Literals ' + a + ' to ' + (a + 7) + ' land in their places; the streams move on by ' + moved.join(', ') + ' bits.' + (j === R - 1 ? ' Round 1 is done: ' + ROUND + ' literals in ' + R + ' dependent steps.' : '') });
    }
    for (r = 1; r < FULL; r++) {
      L.push({ out: ROUND * (r + 1), now: [ROUND * r, ROUND * (r + 1)], fill: ROUND * r, dep: R * (r + 1), say: 'Round ' + (r + 1) + ', literals ' + ROUND * r + ' to ' + (ROUND * (r + 1) - 1) + ': a refill of all 8 streams, then ' + R + ' steps. ' + R * (r + 1) + ' dependent steps so far, for ' + ROUND * (r + 1) + ' literals.' });
    }
    var k0 = FULL * ROUND, k1 = k0 + S * TSTEPS;
    L.push({ out: k1, now: [k0, k1], fill: k0, dep: FULL * R + TSTEPS, say: REST + ' literals are left, fewer than a round. One more refill, then ' + TSTEPS + ' steps of all 8 streams: literals ' + k0 + ' to ' + (k1 - 1) + '. ' + (FULL * R + TSTEPS) + ' dependent steps.' });
    L.push({ out: N, now: [k1, N], fill: k0, dep: DEP8, spare: 1, say: 'The last step: ' + (TR === 1 ? 'stream 0 decodes literal ' + k1 : 'streams 0 ' + (TR === 2 ? 'and ' : 'to ') + (TR - 1) + ' decode literals ' + k1 + (TR === 2 ? ' and ' : ' to ') + (N - 1)) + '. Streams ' + TR + ' to 7 decode too, into bytes ' + N + ' to ' + (k1 + 7) + ' behind the literals, but with a mask of 0 they do not move on. ' + DEP8 + ' dependent steps.' });
    L.push({ out: N, dep: DEP8, spare: 1, say: N + ' literals in ' + DEP8 + ' dependent steps: ' + FULL + ' rounds of ' + R + ', then ' + TSTEPS + ' and 1. One stream would chain all ' + N + '; choose "1 stream" to see it.' });
    return L;
  }
  function steps1() {
    var L = [], j, at = 0;
    L.push({ out: 0, dep: 0, say: 'The same ' + N + ' codes in one stream, a model: the format has 8. The codes follow each other in page order, ' + total + ' bits, wrapped into rows here.' });
    for (j = 0; j < S; j++) {
      at += LEN[j];
      L.push({ out: j + 1, now: [j, j + 1], dep: j + 1, say: j === 0
        ? 'Literal 0, ' + word(LIT[0]) + ': the lookup of the top 10 bits finds a code of ' + LEN[0] + ' bits. Only then does the decoder know that literal 1\'s code starts at bit ' + at + '.'
        : 'Literal ' + j + ', ' + word(LIT[j]) + ', ' + LEN[j] + ' bits: literal ' + (j + 1) + ' starts at bit ' + at + '. ' + (j + 1) + ' steps, each waiting for the one before' + (j === S - 1 ? '; 8 streams decoded these 8 in 1 step.' : '.') });
    }
    L.push({ out: ROUND, now: [S, ROUND], dep: ROUND, say: 'Literals 8 to ' + (ROUND - 1) + ', one after the other: ' + ROUND + ' dependent steps for the ' + ROUND + ' literals that 8 streams decode in ' + R + '.' });
    for (var r = 1; r < FULL; r++) {
      L.push({ out: ROUND * (r + 1), now: [ROUND * r, ROUND * (r + 1)], dep: ROUND * (r + 1), say: 'Literals ' + ROUND * r + ' to ' + (ROUND * (r + 1) - 1) + ': ' + ROUND * (r + 1) + ' dependent steps so far; 8 streams were at ' + R * (r + 1) + ' here.' });
    }
    L.push({ out: N, now: [FULL * ROUND, N], dep: N, say: 'Literals ' + FULL * ROUND + ' to ' + (N - 1) + ': ' + N + ' dependent steps, one per literal.' });
    L.push({ out: N, dep: N, say: N + ' dependent steps against ' + DEP8 + ' with 8 streams, ' + (N / DEP8).toFixed(1) + ' times as many. The model counts one step per lookup and nothing else; on a CPU a step is a table load and two shifts, and 8 independent chains let it work on several at once.' });
    return L;
  }
  var plan = steps8();

  function set(e, cls) { e.setAttribute('class', cls); }
  function draw(s) {
    if (built !== ctx.narrow) build();
    var p = plan[s], deal = p.deal != null, now = p.now || [0, 0], j;
    var out = deal ? 0 : p.out;
    stMode.textContent = mode === 8 ? '8 streams' : '1 stream';
    stWhere.textContent = deal ? 'the encoder deals' : mode === 8
      ? (p.fill == null ? 'decoded: all' : 'round ' + Math.min(FULL + 1, Math.floor(p.fill / ROUND) + 1) + ' of ' + (FULL + 1))
      : 'literal ' + out + ' of ' + N;
    stDep.textContent = deal ? '' : p.dep + ' dependent step' + (p.dep === 1 ? '' : 's');
    heads.style.display = mode === 8 ? '' : 'none';
    // a cell of the scratch: its box and its text
    function look(k) {
      var isNow = k >= now[0] && k < now[1];
      var shown = k < N ? (deal || k < out) : !!p.spare;
      return ['an-move ' + (!shown ? 'st-empty' : k >= N ? 'st-spare' : isNow ? (deal ? 'st-new' : 'st-now') : 'st-cell'),
        !shown ? 'st-hide' : isNow ? 'st-tnow' : cellHex[k] && k < N ? 'st-hex' : ''];
    }
    for (k = 0; k < cells.length; k++) {
      var c = cells[k], lk = look(k);
      set(c.r, lk[0]);
      if (c.t) set(c.t, lk[1]);
    }
    if (zoom) {
      // the row of the step's literals; at the end the last row, with the bytes behind the literals
      var zr = now[1] > now[0] ? Math.floor(now[0] / ROUND) : p.spare || (!deal && out >= N) ? ROWS - 1 : 0, z0 = zr * ROUND;
      zoom.box.setAttribute('y', GY + zr * RH - 1);
      put(zoom.title, mode === 8 ? ['row ' + z0 + ' of the scratch, zoomed in:', 'literal ' + z0 + ' + 8 j + i under step j, stream i']
        : ['row ' + z0 + ' of the scratch, zoomed in:', 'literals ' + z0 + ' to ' + (z0 + ROUND - 1) + ', 8 per row']);
      zoom.head.style.display = mode === 8 ? '' : 'none';
      for (j = 0; j < R; j++) zoom.rows[j].textContent = mode === 8 ? 'j = ' + j : z0 + S * j;
      for (j = 0; j < ROUND; j++) {
        var zk = z0 + j, zc = zoom.cells[j], on = zk < cells.length, zl = on ? look(zk) : ['st-hide', 'st-hide'];
        set(zc.r, zl[0]); set(zc.t, zl[1]);
        zc.t.textContent = on ? ch(cellB[zk]) : '';
      }
      put(gridKey, mode === 8 ? 'the scratch: 40 literals per row, one round' : 'the scratch: the literals in page order, ' + ROUND + ' per row');
      put(laneHead, mode === 8 ? ['the 8 streams: one box per code, as wide as', 'its code length'] : ['one stream: the same codes in page order,', 'wrapped into rows']);
    } else {
      gridKey.textContent = mode === 8 ? 'the scratch: literal k + 8 j + i of row k sits under step j, stream i' : 'the scratch: the literals in page order, ' + ROUND + ' per row';
      laneHead.textContent = mode === 8 ? 'the 8 streams: one box per code, as wide as its code length' : 'one stream: the same codes in page order, wrapped into rows';
    }
    for (k = 0; k < N; k++) {
      var cl = deal ? (k >= p.deal ? 'st-hide' : k >= now[0] && k < now[1] && s < 4 ? 'st-new' : 'st-seg')
        : k < out ? (k >= now[0] && k < now[1] ? 'st-now' : 'st-done') : 'st-seg';
      set(segs[k], 'an-move ' + cl);
    }
    // the cursors: the bits each stream has used; the outline: 56 bits from the last refill
    var used = [0, 0, 0, 0, 0, 0, 0, 0], atFill = [0, 0, 0, 0, 0, 0, 0, 0];
    for (k = 0; k < out; k++) used[mode === 8 ? k % S : 0] += LEN[k];
    if (p.fill != null) for (k = 0; k < p.fill; k++) atFill[k % S] += LEN[k];
    for (j = 0; j < curs.length; j++) {
      var row, xx;
      if (mode === 8) { row = j; xx = X0 + used[j] * lanes.scale; }
      else if (out < N) { row = lanes.pos[out][0]; xx = X0 + lanes.pos[out][1]; }
      else { row = lanes.pos[N - 1][0]; xx = X0 + lanes.pos[N - 1][1] + LEN[N - 1] * lanes.scale; }
      var yy = LY + row * (LH + LG);
      curs[j].setAttribute('x1', xx); curs[j].setAttribute('x2', xx);
      curs[j].setAttribute('y1', yy - 3); curs[j].setAttribute('y2', yy + LH + 3);
      curs[j].style.display = deal ? 'none' : '';
    }
    for (j = 0; j < wins.length; j++) {
      var on = !deal && p.fill != null && s < plan.length;
      var x0 = atFill[j], x1 = Math.min(tot[j], x0 + 56);
      wins[j].setAttribute('x', X0 + x0 * lanes.scale - 1);
      wins[j].setAttribute('width', Math.max(0, (x1 - x0) * lanes.scale + 2));
      wins[j].style.display = on && x1 > x0 ? '' : 'none';
      junk[j].style.display = p.spare && j >= TR && p.fill != null ? '' : 'none';
    }
    for (j = 0; j < lanes.sizes.length; j++) lanes.sizes[j].style.display = deal && !p.sizes ? 'none' : '';
    if (sizeHead) sizeHead.style.display = mode === 8 && !(deal && !p.sizes) ? '' : 'none';
    var d8 = deal ? 0 : mode === 8 ? p.dep : DEP8, d1 = deal ? 0 : mode === 1 ? p.dep : N;
    bar8.f.setAttribute('width', d8 * BS); bar1.f.setAttribute('width', d1 * BS);
    set(bar8.f, mode === 8 ? 'st-fill' : 'st-fill st-other'); set(bar1.f, mode === 1 ? 'st-fill' : 'st-fill st-other');
    bar8.t.textContent = (mode === 8 ? d8 + ' of ' : '') + DEP8 + ' steps';
    bar1.t.textContent = (mode === 1 ? d1 + ' of ' : '') + N + ' steps';
    ctx.say(p.say);
  }
  build();
  return { steps: function () { return plan.length; }, draw: draw, interval: 1100, wide: 720 };
});
