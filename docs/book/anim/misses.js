/*
 * A model of 8 table lookups that miss the cache, about 100 ns each (chapter 3), with 20 ns of work
 * after each that needs the lookup's entry. Three ways: an in-order core that stops at each miss, an
 * out-of-order core whose window holds the work of 3 lookups, and all 8 lines prefetched first. One
 * step is 20 ns.
 */
BookAnim.define('misses', function (ctx) {
  var N = 8, MISS = 100, WORK = 20, STEP = 20, AHEAD = 3, SCALE_NS = 960;
  var MODES = [['inorder', 'in order'], ['ooo', 'out of order'], ['prefetch', 'prefetched']];
  var mode = 'inorder';

  // when each lookup starts its miss (s), its line arrives (a), and its work runs (w to w + WORK)
  function plan(m) {
    var s = [], a = [], w = [], i;
    for (i = 0; i < N; i++) {
      if (m === 'inorder') s.push(i ? w[i - 1] + WORK : 0);
      else if (m === 'ooo') s.push(i < AHEAD ? 0 : w[i - AHEAD] + WORK);
      else s.push(0);
      a.push(s[i] + MISS);
      w.push(Math.max(a[i], i ? w[i - 1] + WORK : 0));
    }
    return { s: s, a: a, w: w, total: w[N - 1] + WORK };
  }
  var plans = {};
  MODES.forEach(function (m) { plans[m[0]] = plan(m[0]); });

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var btns = MODES.map(function (m) {
    var b = ctx.el('button', null, m[1], choice);
    b.type = 'button'; b.setAttribute('aria-pressed', m[0] === mode ? 'true' : 'false');
    b.addEventListener('click', function () {
      mode = m[0];
      btns.forEach(function (x) { x.setAttribute('aria-pressed', x === b ? 'true' : 'false'); });
      ctx.restart();
    });
    return b;
  });

  // the wide layout, and on a phone one about 360 units wide: legend in two rows, ticks every 200 ns,
  // each total's name above its bar
  var BAR = 16, L;
  function layout(narrow) {
    var o = narrow ? { W: 360, X0: 72, X1: 330, ROW: 24, YCORE: 82, Y0: 112, PAD: 4, TICK: 200, LAB: 8 }
      : { W: 720, X0: 100, X1: 650, ROW: 26, YCORE: 70, Y0: 112, PAD: 6, TICK: 100, LAB: 20 };
    o.YAXIS = o.Y0 + N * o.ROW + o.PAD;
    o.YTOT = o.YAXIS + (narrow ? 50 : 56);
    o.H = o.YTOT + (narrow ? 130 : 3 * 24);
    return o;
  }
  L = layout(ctx.narrow);
  function x(t) { return L.X0 + (L.X1 - L.X0) * t / SCALE_NS; }
  var root = ctx.svg('svg', { viewBox: '0 0 ' + L.W + ' ' + L.H, role: 'img', 'aria-label': 'A timeline of 8 table lookups that miss the cache, and the work after each, for an in-order core, an out-of-order core and with the lines prefetched' }, ctx.stage);
  var g = ctx.svg('g', { 'font-size': 12 }, root);

  function rect(x0, x1, y, h, cls) {
    if (x1 <= x0) return;
    ctx.svg('rect', { x: x(x0), y: y, width: x(x1) - x(x0), height: h, class: cls }, g);
  }
  function text(tx, ty, t, attrs) {
    var o = { x: tx, y: ty, text: t };
    for (var k in attrs || {}) o[k] = attrs[k];
    return ctx.svg('text', o, g);
  }

  function draw(k) {
    var p = plans[mode], T = Math.min(k * STEP, p.total), i, t, nw = ctx.narrow;
    L = layout(nw);
    var X0 = L.X0, X1 = L.X1, ROW = L.ROW, YCORE = L.YCORE, Y0 = L.Y0, YAXIS = L.YAXIS, YTOT = L.YTOT;
    root.setAttribute('viewBox', '0 0 ' + L.W + ' ' + L.H);
    while (g.firstChild) g.removeChild(g.firstChild);

    // legend and clock
    text(L.LAB, 24, 't = ' + T + ' ns', { 'font-size': 17, 'font-weight': 'bold', class: 'ms-clock-t' });
    var lx = 190;
    [['ms-miss' + (mode === 'prefetch' ? ' ms-pre' : ''), mode === 'prefetch' ? 'prefetch in flight' : 'miss in flight'], ['ms-work', 'work on the entry'], ['ms-wait', 'core waits']].forEach(function (l, j) {
      // on a phone two columns, below the clock
      var ly = nw ? 46 + 20 * (j >> 1) : 25;
      if (nw) lx = j & 1 ? 172 : L.LAB;
      ctx.svg('rect', { x: lx, y: ly - 12, width: 14, height: 14, class: l[0] }, g);
      text(lx + 20, ly, l[1]);
      lx += 40 + l[1].length * 7.4;
    });

    // the core: work, or waiting
    text(X0 - 10, YCORE + 12, 'core', { 'text-anchor': 'end', 'font-weight': 'bold' });
    var busy = 0;
    for (t = 0; t < T; t += STEP) {
      var working = p.w.some(function (w) { return w <= t && t < w + WORK; });
      if (working) busy += STEP;
      rect(t, t + STEP, YCORE, BAR, working ? 'ms-work' : 'ms-wait');
    }

    // one row per lookup
    for (i = 0; i < N; i++) {
      var y = Y0 + i * ROW, live = p.s[i] <= T && T < p.a[i];
      text(X0 - (nw ? 6 : 10), y + 12, 'lookup ' + (i + 1), { 'text-anchor': 'end', class: live ? 'ms-live' : 'ms-lab' });
      ctx.svg('line', { x1: X0, x2: X1, y1: y + BAR / 2, y2: y + BAR / 2, class: 'ms-track' }, g);
      if (T > p.s[i]) rect(p.s[i], Math.min(p.a[i], T), y, BAR, 'ms-miss' + (mode === 'prefetch' ? ' ms-pre' : ''));
      if (T > p.w[i]) rect(p.w[i], Math.min(p.w[i] + WORK, T), y, BAR, 'ms-work');
    }

    // time axis and the clock's line
    ctx.svg('line', { x1: X0, x2: X1, y1: YAXIS, y2: YAXIS, class: 'ms-axis' }, g);
    for (t = 0; t <= SCALE_NS; t += L.TICK) {
      ctx.svg('line', { x1: x(t), x2: x(t), y1: YAXIS, y2: YAXIS + 5, class: 'ms-axis' }, g);
      text(x(t), YAXIS + 19, t, { 'text-anchor': 'middle', 'font-size': nw ? 12 : 11, class: 'ms-lab' });
    }
    text(X1 + (nw ? 6 : 8), YAXIS + 19, 'ns', { 'font-size': nw ? 12 : 11, class: 'ms-lab' });
    ctx.svg('line', { x1: x(T), x2: x(T), y1: YCORE - 8, y2: YAXIS, class: 'ms-now' }, g);

    // the totals, at the end
    var end = T >= p.total;
    if (end) {
      text(L.LAB, YTOT - 10, 'total time of the 8 lookups and their work', { 'font-weight': 'bold' });
      MODES.forEach(function (m, j) {
        var y = YTOT + j * 24, q = plans[m[0]], cls = m[0] === mode ? 'ms-tot-now' : 'ms-tot';
        // on a phone the name above the bar on the left, the time above the bar's end
        if (nw) y = YTOT + 20 + j * 38;
        if (nw) text(L.LAB, y - 6, m[1], { class: m[0] === mode ? '' : 'ms-lab' });
        else text(X0 - 10, y + 12, m[1], { 'text-anchor': 'end', class: m[0] === mode ? '' : 'ms-lab' });
        rect(0, q.total, y, nw ? 14 : BAR, cls);
        if (nw) text(x(q.total), y - 6, q.total + ' ns', { 'text-anchor': 'end', class: m[0] === mode ? 'ms-tot-t' : 'ms-lab' });
        else text(x(q.total) + 8, y + 12, q.total + ' ns', { class: m[0] === mode ? 'ms-tot-t' : 'ms-lab' });
      });
    } else if (nw) {
      text(L.LAB, YTOT + 12, 'The totals of the three ways', { class: 'ms-lab' });
      text(L.LAB, YTOT + 30, 'appear at the last step.', { class: 'ms-lab' });
    } else {
      text(20, YTOT + 12, 'The totals of the three ways appear at the last step.', { class: 'ms-lab' });
    }
    ctx.say(sentence(p, k, T, busy));
  }

  function list(a) {
    var n = a.map(function (i) { return i + 1; });
    return n.length === 1 ? 'lookup ' + n[0] : 'lookups ' + n.slice(0, -1).join(', ') + ' and ' + n[n.length - 1];
  }

  function sentence(p, k, T, busy) {
    var i, P = T - STEP, parts = [];
    if (k === 0) {
      if (mode === 'inorder') return 'In order: the core starts lookup 1 and stops at the first instruction that needs its entry. Every instruction behind it stops too, even those that do not need it.';
      if (mode === 'ooo') return 'Out of order: the lookups\' addresses do not depend on each other, so the core runs past instructions that wait. Its window holds the work of ' + AHEAD + ' lookups, so lookups 1 to ' + AHEAD + ' start their misses at once, at 0 ns.';
      return 'Prefetched: before the first lookup, the code asks for all ' + N + ' lines and does not wait for them. All ' + N + ' misses start at 0 ns.';
    }
    if (T >= p.total) {
      var t = MODES.map(function (m) { return m[1] + ' ' + plans[m[0]].total + ' ns'; }).join(', ');
      return 'Done at ' + p.total + ' ns: the core worked ' + busy + ' ns and waited ' + (p.total - busy) + ' ns. The three ways: ' + t + '. The work is the same ' + N * WORK + ' ns in each; the difference is how many misses overlap.';
    }
    var started = [], arrived = [], working = -1, flying = [];
    for (i = 0; i < N; i++) {
      if (p.s[i] === P && P > 0) started.push(i);
      if (p.a[i] > P && p.a[i] <= T) arrived.push(i);
      if (p.w[i] === P) working = i;
      if (p.s[i] <= P && p.a[i] >= T) flying.push(i);
    }
    if (started.length) parts.push(list(started) + (mode === 'ooo' ? ' enters the window and starts its miss' : ' starts its miss'));
    if (working >= 0) parts.push('the work of lookup ' + (working + 1) + ' runs on the entry that has arrived');
    else if (flying.length === 1 && mode === 'inorder') parts.push('the core waits for lookup ' + (flying[0] + 1) + ', ' + (T - p.s[flying[0]]) + ' of its ' + MISS + ' ns are over');
    else if (flying.length) parts.push('the core waits, with ' + flying.length + (flying.length > 1 ? ' misses' : ' miss') + ' in flight');
    else parts.push('the core waits');
    if (arrived.length) parts.push('the line' + (arrived.length > 1 ? 's' : '') + ' of ' + list(arrived) + (arrived.length > 1 ? ' arrive' : ' arrives') + ' at ' + T + ' ns');
    var s = 'From ' + P + ' to ' + T + ' ns: ' + parts.join('; ') + '.';
    return s.charAt(0).toUpperCase() + s.slice(1);
  }

  return { steps: function () { return plans[mode].total / STEP + 1; }, draw: draw, interval: 450, narrow: true };
});
