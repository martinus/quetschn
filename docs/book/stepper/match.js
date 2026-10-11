/*
 * The step-through of seqlz_match_page(): plays a trace that a copy of the real function recorded at
 * build time (instrument.py match, trace_match.c). Each step is the state in front of one statement:
 * the line, pos, anchor, last, h, cand, rep_hit, cand_hit, m and len. The bytes at pos and at the
 * candidates, the hash table and the sequences follow from the page and the steps before.
 */
(function () {
  var F = { line: 0, pos: 1, anchor: 2, last: 3, h: 4, cand: 5, rep: 6, chit: 7, m: 8, len: 9 };
  var MASK = (1n << 64n) - 1n;

  function hex(v, w) { var s = (v >>> 0).toString(16); while (s.length < w) s = '0' + s; return s; }
  function ch(b) { return b >= 32 && b < 127 ? String.fromCharCode(b) : b === 10 ? '↵' : '·'; }
  function el(tag, cls, text, parent) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; if (parent) parent.appendChild(e); return e; }

  function Matcher(fig) {
    var data = JSON.parse(document.getElementById(fig.id + '-data').textContent);
    var C = data.consts, SLOTS = 1 << C.HASH_BITS;
    var rows = {}, stmt = {};
    fig.querySelectorAll('.st-code .line').forEach(function (r) { rows[r.querySelector('.ln').getAttribute('data-n')] = r; });
    Object.keys(rows).forEach(function (n) { stmt[n] = rows[n].querySelector('.lc').textContent.trim(); });
    var codeBox = fig.querySelector('.st-code'), say = fig.querySelector('.st-say');
    var scrub = fig.querySelector('.st-scrub'), posEl = fig.querySelector('.st-pos');
    var state = fig.querySelector('.st-state');
    var page, pageId, steps, emits, tops, i = 0, timer = null;

    // the panes
    function pane(title) { var s = el('section', null, null, state); el('h4', null, title, s); return s; }
    var pBytes = pane('The bytes at pos and at the two candidates');
    var cmpBox = el('div', 'm-cmp', null, pBytes);
    var hashLine = el('div', 'st-regline', null, pBytes);
    var pVars = pane('The variables');
    var varsBox = el('div', 'st-vars', null, pVars);
    var pTab = pane('The hash table, ' + SLOTS + ' slots of a position');
    var tabWrap = el('div', 'st-page', null, pTab);
    var tabCanvas = el('canvas', null, null, tabWrap); tabCanvas.width = 384; tabCanvas.height = 384;
    var pageCanvas = el('canvas', null, null, tabWrap); pageCanvas.width = 384; pageCanvas.height = 384;
    el('div', 'st-legend', 'Left: the table, a slot in the match colour once a position went in, the slot of pos outlined in the literal colour. Right: the page, 64 bytes a row: emitted as a literal or as a match, the literals not yet emitted lighter, pos outlined, the candidate in the match colour.', pTab);
    var pSeq = pane('The sequences, as the encoder gets them');
    var seqBox = el('div', 'm-seqs', null, pSeq);

    function at(s) { return stmt[s[F.line]] || ''; }
    function is(s, p) { return at(s).indexOf(p) === 0; }
    function le(p, n) { var v = 0n; for (var k = n - 1; k >= 0; k--) v = (v << 8n) | BigInt(page.page[p + k] || 0); return v; }
    function hash5(p) { return Number(((le(p, 8) << 24n) * 889523592379n & MASK) >> BigInt(64 - C.HASH_BITS)); }
    function four(p) { return page.page.slice(p, p + 4); }
    function same(a, b) { return a.length === b.length && a.every(function (x, k) { return x === b[k]; }); }

    function load(id) {
      page = data.pages[id];
      pageId = id;
      steps = page.steps;
      emits = []; tops = [];
      steps.forEach(function (s, k) {
        if (is(s, 'emit(')) emits.push(k);
        if (is(s, 'u64 v_next')) tops.push(k);
      });
      scrub.max = steps.length - 1;
      fig.querySelectorAll('.st-pages button').forEach(function (b) { b.setAttribute('aria-pressed', b.getAttribute('data-page') === id ? 'true' : 'false'); });
      go(0);
    }

    // the table and the sequences after the steps before k
    function replay(k) {
      var table = new Int32Array(SLOTS).fill(-1), seqs = [];
      for (var j = 0; j < k; j++) {
        var s = steps[j];
        if (is(s, 'u64 v_next')) table[s[F.h]] = s[F.pos];
        if (is(s, 'table[seqlz_hash5')) table[hash5(s[F.pos] - 2)] = s[F.pos] - 2;
        if (is(s, 'emit(')) {
          var last = is(s, 'emit(ctx, src + anchor, SEQLZ_PAGE');
          seqs.push({ at: s[F.anchor], ll: (last ? C.PAGE : s[F.pos]) - s[F.anchor], ml: last ? 0 : s[F.len], off: last ? 0 : s[F.last] });
        }
      }
      return { table: table, seqs: seqs };
    }

    function explain(k, s, n) {
      var t = at(s), pos = s[F.pos], rp = pos - s[F.last];
      var b5 = page.page.slice(pos, pos + 5).map(function (b) { return hex(b, 2); }).join(' ');
      if (t.indexOf('memset(table') === 0) return 'The hash table, ' + SLOTS + ' slots of a 16-bit position, is cleared: every slot says 0, so every candidate starts as position 0. The search starts at position 1, with the repeat offset 1.';
      if (t.indexOf('u64 v_next') === 0) return 'Position ' + pos + '. The 5 bytes there, ' + b5 + ', hash to slot ' + s[F.h] + ', which holds ' + s[F.cand] + ': the table’s candidate. The other candidate is the repeat offset, ' + s[F.last] + ' back: position ' + rp + '. Both are read before either is tested.';
      if (t.indexOf('if (!(rep_hit | cand_hit))') === 0) {
        var r = s[F.rep], c = s[F.chit];
        if (!(r | c)) return 'The 4 bytes at ' + pos + ' equal neither those at ' + rp + ' nor those at ' + s[F.cand] + '. Position ' + pos + ' went into slot ' + s[F.h] + ' already, so a later position with the same 5 bytes finds it; the byte stays a literal and the loop goes on at ' + (pos + 1) + '.';
        return 'A hit: the 4 bytes at ' + pos + ' equal ' + (r && c ? 'both, those at ' + rp + ' and at ' + s[F.cand] : r ? 'those at ' + rp + ', the repeat offset' : 'those at ' + s[F.cand] + ', the table’s candidate') + '. Position ' + pos + ' went into slot ' + s[F.h] + ' too.';
      }
      if (t.indexOf('m = rep_hit') === 0) return (s[F.rep] ? 'The repeat offset hit' + (s[F.chit] ? ', and the table\u2019s candidate too, but the repeat offset is taken whenever it hits' : '') + ': the match starts at ' + rp + ', offset ' + s[F.last] + ', the same as the sequence before, which a token of class 0 sends in no bits at all.' : 'The match starts at the table\u2019s candidate, ' + s[F.cand] + ': offset ' + (pos - s[F.cand]) + '.');
      if (t.indexOf('while (pos > anchor') === 0) {
        var back = s[F.pos] - n[F.pos];
        return 'Backwards: while the byte before pos equals the byte before m, and pos is past the literals not emitted yet (from ' + s[F.anchor] + '), both move back. ' + (back ? back + ' byte' + (back > 1 ? 's' : '') + ' back: the match starts at ' + n[F.pos] + '.' : 'Not one: ' + (s[F.pos] === s[F.anchor] ? 'pos is at the anchor.' : 'the bytes before differ.'));
      }
      if (t.indexOf('len = 4U') === 0) return 'Forwards: the first 4 bytes match, seqlz_match_len() compares from pos + 4, 8 bytes at a time with one XOR each. The match is ' + n[F.len] + ' bytes, at offset ' + (s[F.pos] - s[F.m]) + '.';
      if (t.indexOf('emit(ctx, src + anchor, SEQLZ_PAGE') === 0) return 'pos = ' + pos + ' reached ' + C.LIMIT + ', 8 bytes before the end, where a try would read past the page: the loop stops. The rest, ' + (C.PAGE - s[F.anchor]) + ' bytes from ' + s[F.anchor] + ', is the last sequence, literals and no match.';
      if (t.indexOf('emit(') === 0) return 'A sequence: ' + (pos - s[F.anchor]) + ' literal' + (pos - s[F.anchor] === 1 ? '' : 's') + ' from ' + s[F.anchor] + ', then a match of ' + s[F.len] + ' bytes at offset ' + s[F.last] + '. The encoder writes it at once (chapter 10); the matcher keeps no list.';
      if (t.indexOf('table[seqlz_hash5') === 0) return 'The positions inside the match were skipped, none of them is in the table. One goes in: ' + (pos - 2) + ', 2 bytes before the match’s end, into slot ' + hash5(pos - 2) + '. Then the loop goes on at ' + pos + '.';
      return '';
    }

    function drawCmp(s) {
      var pos = s[F.pos], top = !is(s, 'memset'), rp = pos - s[F.last];
      cmpBox.textContent = '';
      if (!top) return;
      var cur = four(pos);
      [['at pos ' + pos, pos, null], ['at cand ' + s[F.cand], s[F.cand], cur], ['at pos − last ' + rp, rp, cur]].forEach(function (r) {
        var row = el('div', 'm-row', null, cmpBox);
        el('span', 'm-lab', r[0], row);
        var bytes = page.page.slice(r[1], r[1] + 8);
        bytes.forEach(function (b, j) {
          var c = el('span', 'm-b', hex(b, 2), row);
          if (j < 4) c.classList.add(r[2] ? (r[2][j] === b ? 'm-eq' : 'm-ne') : 'm-cur');
        });
        el('span', 'm-ch', bytes.map(ch).join(''), row);
        if (r[2]) el('span', same(four(r[1]), cur) ? 'm-yes' : 'm-no', same(four(r[1]), cur) ? 'same 4 bytes' : 'differs', row);
      });
      hashLine.textContent = 'the 5 bytes at pos hash to slot ' + s[F.h] + ' of ' + SLOTS + '; the first 4 bytes are what both candidates must match';
    }

    function drawVars(s) {
      varsBox.textContent = '';
      var hitKnown = s[F.rep] >= 0;
      [['pos', s[F.pos]], ['anchor', s[F.anchor]], ['last', s[F.last]], ['h', s[F.h]], ['cand', s[F.cand]],
       ['rep_hit', hitKnown ? s[F.rep] : '–'], ['cand_hit', hitKnown ? s[F.chit] : '–'], ['m', s[F.m]], ['len', s[F.len]]].forEach(function (c) {
        var w = el('div', 'v', null, varsBox); el('span', 'vk', c[0], w); el('span', 'vv', String(c[1]), w);
      });
    }

    function colors() {
      var cs = getComputedStyle(fig);
      function v(n) { return cs.getPropertyValue('--' + n).trim(); }
      return { lit: v('mark'), match: v('accent'), empty: v('rule'), ink: v('ink'), paper: v('paper'), pend: v('warn'), filled: v('accent'), slot: v('sheet') };
    }

    function drawTable(s, rp) {
      var col = colors(), c = tabCanvas.getContext('2d'), cell = tabCanvas.width / 64, t = rp.table;
      c.fillStyle = col.paper; c.fillRect(0, 0, tabCanvas.width, tabCanvas.height);
      // no gaps: at this size a gap would be as wide as a slot
      for (var k = 0; k < SLOTS; k++) {
        c.fillStyle = t[k] >= 0 ? col.filled : col.slot;
        c.fillRect((k % 64) * cell, Math.floor(k / 64) * cell, cell, cell);
      }
      if (!is(s, 'memset')) {
        var h = s[F.h];
        c.strokeStyle = col.lit; c.lineWidth = 4;
        c.strokeRect((h % 64) * cell - 3, Math.floor(h / 64) * cell - 3, cell + 6, cell + 6);
      }
    }

    function drawPage(s, rp) {
      var col = colors(), c = pageCanvas.getContext('2d'), cell = pageCanvas.width / 64;
      var kind = new Uint8Array(C.PAGE);
      rp.seqs.forEach(function (q) {
        for (var b = q.at; b < q.at + q.ll; b++) kind[b] = 1;
        for (b = q.at + q.ll; b < q.at + q.ll + q.ml; b++) kind[b] = 2;
      });
      var pos = s[F.pos], anchor = s[F.anchor];
      for (var b2 = anchor; b2 < pos && b2 < C.PAGE; b2++) if (!kind[b2]) kind[b2] = 3;
      c.fillStyle = col.paper; c.fillRect(0, 0, pageCanvas.width, pageCanvas.height);
      for (var k = 0; k < C.PAGE; k++) {
        c.fillStyle = [col.empty, col.lit, col.match, col.pend][kind[k]];
        c.fillRect((k % 64) * cell, Math.floor(k / 64) * cell, cell - 1, cell - 1);
      }
      function box(a, n, color) {
        c.strokeStyle = color; c.lineWidth = 2;
        for (var q = a; q < a + n && q < C.PAGE; q++) if (q >= 0) c.strokeRect((q % 64) * cell + 1, Math.floor(q / 64) * cell + 1, cell - 3, cell - 3);
      }
      if (!is(s, 'memset')) {
        var src = is(s, 'u64 v_next') || is(s, 'if (!(') ? s[F.cand] : s[F.m];
        if (!is(s, 'emit(ctx, src + anchor, SEQLZ_PAGE')) { box(src, 4, col.match); box(pos, 4, col.ink); }
      }
    }

    function drawSeqs(rp) {
      seqBox.textContent = '';
      var list = rp.seqs, from = Math.max(0, list.length - 6);
      el('div', 'm-seqh', list.length + ' sequence' + (list.length === 1 ? '' : 's') + ' so far' + (list.length > 6 ? ', the last 6:' : ''), seqBox);
      for (var q = from; q < list.length; q++) {
        var r = list[q];
        el('div', 'm-seq', '#' + (q + 1) + ' at ' + r.at + ': ' + r.ll + ' literal' + (r.ll === 1 ? '' : 's') + (r.ml ? ', match ' + r.ml + ' at offset ' + r.off : ', no match: the last'), seqBox);
      }
    }

    var calm = matchMedia('(prefers-reduced-motion: reduce)');
    function followLine(row) { codeBox.scrollTo({ top: Math.max(0, row.offsetTop - codeBox.clientHeight / 3), behavior: calm.matches ? 'auto' : 'smooth' }); }

    function go(k) {
      i = Math.max(0, Math.min(steps.length - 1, k));
      var s = steps[i], n = steps[Math.min(i + 1, steps.length - 1)];
      fig.querySelectorAll('.st-code .line.now').forEach(function (r) { r.classList.remove('now'); });
      var r = rows[s[F.line]];
      if (r) { r.classList.add('now'); followLine(r); }
      var rp = replay(i);
      say.textContent = explain(i, s, n);
      drawCmp(s); drawVars(s); drawTable(s, rp); drawPage(s, rp); drawSeqs(rp);
      scrub.value = i;
      posEl.textContent = 'step ' + (i + 1) + ' of ' + steps.length + ', position ' + s[F.pos] + ', ' + rp.seqs.length + ' of ' + emits.length + ' sequences';
      // the sequence being looked for, the number of sequences emitted before this step
      if (window.BookLinks) window.BookLinks.show(fig.id, pageId === 'slab' && rp.seqs.length < emits.length ? rp.seqs.length : null);
    }
    function seqStep(dir) {
      var list = emits, k;
      if (dir > 0) { for (k = 0; k < list.length; k++) if (list[k] > i) return go(list[k]); go(steps.length - 1); }
      else { for (k = list.length - 1; k >= 0; k--) if (list[k] < i) return go(list[k]); go(0); }
    }
    function play(on) {
      if (timer) { clearInterval(timer); timer = null; }
      fig.classList.toggle('playing', on);
      if (on) timer = setInterval(function () { if (i >= steps.length - 1) play(false); else go(i + 1); }, 420);
    }
    fig.querySelectorAll('[data-do]').forEach(function (b) {
      b.addEventListener('click', function () {
        var a = b.getAttribute('data-do');
        if (a !== 'play') play(false);
        if (a === 'prev') go(i - 1);
        if (a === 'next') go(i + 1);
        if (a === 'prevseq') seqStep(-1);
        if (a === 'nextseq') seqStep(1);
        if (a === 'first') go(0);
        if (a === 'play') play(!timer);
      });
    });
    fig.querySelectorAll('.st-pages button').forEach(function (b) { b.addEventListener('click', function () { play(false); load(b.getAttribute('data-page')); }); });
    var cmBtn = fig.querySelector('.st-cm');
    cmBtn.addEventListener('click', function () {
      var show = fig.classList.toggle('nocomments') === false;
      cmBtn.setAttribute('aria-pressed', show ? 'true' : 'false');
      go(i);
    });
    scrub.addEventListener('input', function () { play(false); go(+scrub.value); });
    fig.addEventListener('keydown', function (ev) {
      if (ev.target === scrub) return;
      if (ev.key === 'ArrowRight') { play(false); ev.shiftKey ? seqStep(1) : go(i + 1); ev.preventDefault(); }
      if (ev.key === 'ArrowLeft') { play(false); ev.shiftKey ? seqStep(-1) : go(i - 1); ev.preventDefault(); }
    });
    document.addEventListener('book-theme', function () { setTimeout(function () { go(i); }, 0); });
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () { go(i); });
    if (window.BookLinks) window.BookLinks.add(fig.id, 'found by the matcher (chapter 9)', fig, function (seq) {
      play(false);
      if (pageId !== 'slab') load('slab');
      go(emits[Math.min(seq, emits.length - 1)]);
    });
    load(fig.querySelector('.st-pages button').getAttribute('data-page'));
  }

  document.querySelectorAll('figure.stepper[data-kind="match"]').forEach(function (f) { Matcher(f); });
})();
