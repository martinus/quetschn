/*
 * The step-through of seqlz_match_page(): plays a trace that a copy of the real function recorded at
 * build time (instrument.py match, trace_match.c). Each step is the state in front of one statement:
 * the line, pos, anchor, last, h, cand, rep_hit, cand_hit, m and len. The bytes at pos and at the
 * candidates, the hash table and the sequences follow from the page and the steps before.
 */
(function () {
  var F = { line: 0, pos: 1, anchor: 2, last: 3, h: 4, cand: 5, rep: 6, chit: 7, m: 8, len: 9, h2: 10 };
  var U = window.BookStepper.util, hex = U.hex, ch = U.ch, el = U.el;
  // the kinds of step the drawing needs, found once per page from the statement's text
  var OP = { top: 1, insert: 2, emit: 3, last: 4 };


  function view(vc) {
    var fig = vc.fig, data = vc.data, state = vc.state;
    var C = data.consts, SLOTS = 1 << C.HASH_BITS;
    var page, steps, ops, emits, cache;

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

    function at(s) { return vc.stmt(s[F.line]); }
    function is(s, p) { return at(s).indexOf(p) === 0; }
    function four(p) { return page.page.slice(p, p + 4); }
    function same(a, b) { return a.length === b.length && a.every(function (x, k) { return x === b[k]; }); }

    function load(id) {
      page = data.pages[id];
      steps = page.steps;
      ops = new Uint8Array(steps.length);
      emits = [];
      steps.forEach(function (s, k) {
        ops[k] = is(s, 'u64 v_next') ? OP.top : is(s, 'table[seqlz_hash5') ? OP.insert
          : is(s, 'emit(ctx, src + anchor, SEQLZ_PAGE') ? OP.last : is(s, 'emit(') ? OP.emit : 0;
        if (ops[k] === OP.emit || ops[k] === OP.last) emits.push(k);
      });
      cache = null;
      return { steps: steps.length, groups: emits };
    }

    // the table and the sequences after the steps before k, on from the last step drawn if that was
    // before k: from step 0 it was up to 4667 steps on every draw
    function replay(k) {
      if (!cache || cache.k > k) cache = { k: 0, table: new Int32Array(SLOTS).fill(-1), seqs: [] };
      for (var j = cache.k; j < k; j++) {
        var s = steps[j], op = ops[j];
        if (op === OP.top) cache.table[s[F.h]] = s[F.pos];
        if (op === OP.insert) cache.table[s[F.h2]] = s[F.pos] - 2;
        if (op === OP.emit || op === OP.last) {
          var last = op === OP.last;
          cache.seqs.push({ at: s[F.anchor], ll: (last ? C.PAGE : s[F.pos]) - s[F.anchor], ml: last ? 0 : s[F.len], off: last ? 0 : s[F.last] });
        }
      }
      cache.k = k;
      return cache;
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
      if (t.indexOf('table[seqlz_hash5') === 0) return 'The positions inside the match were skipped, none of them is in the table. One goes in: ' + (pos - 2) + ', 2 bytes before the match’s end, into slot ' + s[F.h2] + '. Then the loop goes on at ' + pos + '.';
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

    function drawTable(s, rp, col) {
      var cell = tabCanvas.width / 64, t = rp.table;
      // no gaps: at this size a gap would be as wide as a slot
      var c = U.cells(tabCanvas, function (k) { return t[k] >= 0 ? 1 : 0; }, [col.slot, col.filled], null);
      if (!is(s, 'memset')) {
        var h = s[F.h];
        c.strokeStyle = col.lit; c.lineWidth = 4;
        c.strokeRect((h % 64) * cell - 3, Math.floor(h / 64) * cell - 3, cell + 6, cell + 6);
      }
    }

    function drawPage(s, rp, col) {
      var cell = pageCanvas.width / 64;
      var kind = new Uint8Array(C.PAGE);
      rp.seqs.forEach(function (q) {
        for (var b = q.at; b < q.at + q.ll; b++) kind[b] = 1;
        for (b = q.at + q.ll; b < q.at + q.ll + q.ml; b++) kind[b] = 2;
      });
      var pos = s[F.pos], anchor = s[F.anchor];
      for (var b2 = anchor; b2 < pos && b2 < C.PAGE; b2++) if (!kind[b2]) kind[b2] = 3;
      var c = U.cells(pageCanvas, function (k) { return kind[k]; }, [col.empty, col.lit, col.match, col.pend], col.paper);
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

    return {
      load: load,
      line: function (k) { return steps[k][F.line]; },
      draw: function (k) {
        var s = steps[k], rp = replay(k), col = colors();
        drawCmp(s); drawVars(s); drawTable(s, rp, col); drawPage(s, rp, col); drawSeqs(rp);
      },
      say: function (k) { return explain(k, steps[k], steps[Math.min(k + 1, steps.length - 1)]); },
      pos: function (k) { return 'step ' + (k + 1) + ' of ' + steps.length + ', position ' + steps[k][F.pos] + ', ' + replay(k).seqs.length + ' of ' + emits.length + ' sequences'; },
      // the sequence being looked for: the number of sequences emitted before this step
      seq: function (k) { var n = replay(k).seqs.length; return n < emits.length ? n : null; },
      stepOfSeq: function (seq) { return emits[Math.min(seq, emits.length - 1)]; },
      linkLabel: 'found by the matcher (chapter 9)'
    };
  }

  document.querySelectorAll('figure.stepper[data-kind="match"]').forEach(function (f) { window.BookStepper.start(f, view); });
})();
