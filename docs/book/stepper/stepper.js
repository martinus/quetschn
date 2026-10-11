/*
 * The step-through of seqlz_decode(): plays a trace that a copy of the real function recorded at build
 * time (instrument.py, trace.c). Each step is the state in front of one statement: the line, d, lit,
 * the bit reader's p, count and 64 bits, the token's entry, nl, len, off, last and skip.
 */
(function () {
  // the copy of the last sequence's literals, after the loop
  var LAST = 'copy_literals(d, d_end, lit, lit_bound, (unsigned int)';
  var F = { line: 0, d: 1, lit: 2, p: 3, count: 4, hi: 5, lo: 6, tok: 7, nl: 8, len: 9, off: 10, last: 11, skip: 12 };

  function entry(e) {
    return {
      n: e & 63, back: (e >>> 6) & 63, shift: (e >>> 12) & 3, drop: (e >>> 14) & 31,
      ll: (e >>> 19) & 15, ml: (e >>> 23) & 63, value: (e >>> 30) & 1, escape: e >>> 31
    };
  }
  var U = window.BookStepper.util, hex = U.hex, ch = U.ch, el = U.el, keepVisible = U.keepVisible;
  function bitsOf(s) { return (BigInt(s[F.hi]) << 32n) | BigInt(s[F.lo]); }
  function bin(v, n) { var s = ''; for (var i = n - 1; i >= 0; i--) s += (v >> i) & 1; return s; }

  function view(vc) {
    var fig = vc.fig, data = vc.data, C = data.consts;
    var bitsBox = fig.querySelector('.st-bits');
    var regLine = fig.querySelector('.st-regline');
    var varsBox = fig.querySelector('.st-vars');
    var inBox = fig.querySelector('.st-bytes');
    var litBox = fig.querySelector('.st-litbytes');
    var litHead = fig.querySelector('.st-lithead');
    var canvas = fig.querySelector('canvas');
    var win = fig.querySelector('.st-window');
    var costBox = fig.querySelector('.st-cost');
    var page, steps, seqStarts, seqs, when, kind;

    function at(s) { return vc.stmt(s[F.line]); }
    function is(s, prefix) { return at(s).indexOf(prefix) === 0; }

    function load(id) {
      page = data.pages[id];
      steps = page.steps;
      seqStarts = [];
      page.nLit = page.lit.length;
      // a sequence starts at its token; the last one has none, it is the copy of the literals left,
      // whose count the trace records as nl 0, outside the loop: it is set here
      steps.forEach(function (s, k) {
        if (is(s, LAST)) s[F.nl] = page.nLit - s[F.lit];
        if (is(s, 'if (tok >> TOK_ESCAPE_AT)') || is(s, LAST)) seqStarts.push(k);
      });
      // when each byte of the page is final, and whether a literal or a match wrote it
      when = new Int32Array(C.PAGE).fill(-1);
      kind = new Uint8Array(C.PAGE);
      steps.forEach(function (s, k) {
        var a = s[F.d], n = 0, t = 0;
        if (is(s, 'd += nl;') || is(s, LAST)) { n = s[F.nl]; t = 1; }
        if (is(s, 'd += len;')) { n = s[F.len]; t = 2; }
        for (var b = a; b < a + n && b < C.PAGE; b++) { when[b] = k; kind[b] = t; }
      });
      page.bitStart = -1;
      for (var k = 0; k < steps.length; k++) if (steps[k][F.p] >= 0) { page.bitStart = steps[k][F.p]; break; }
      buildBytes();
      seqs = costs();
      return { steps: steps.length, groups: seqStarts };
    }

    function buildBytes() {
      inBox.textContent = '';
      page.comp.forEach(function (b, k) {
        var c = el('span', k < C.HEADER ? 'b-hdr' : (page.bitStart >= 0 && k >= page.bitStart ? 'b-bits' : 'b-lit'), hex(b, 2));
        inBox.appendChild(c);
      });
      litBox.textContent = '';
      page.lit.forEach(function (b) { litBox.appendChild(el('span', null, ch(b))); });
      litHead.textContent = page.coded
        ? 'The literals, Huffman coded in the page, decoded into the scratch buffer first (' + page.nLit + ')'
        : 'The literals, raw, bytes 2 to ' + (C.HEADER + page.nLit - 1) + ' of the compressed page (' + page.nLit + ')';
    }

    /*
     * What each sequence costs: in seqlz the bits the bit reader moves on from its first step to the
     * next sequence's, which are its token, offset and length values (at a sequence's first step its
     * token is read but not dropped yet), and the codes of its literals; in lz4's format a token
     * byte, the literals as bytes, 2 bytes of offset and a byte more per 255 of a long length
     */
    function costs() {
      function used(s) { return s[F.p] * 8 - s[F.count]; }
      var end = steps.length - 1, list = [];
      seqStarts.forEach(function (a, q) {
        var b = q + 1 < seqStarts.length ? seqStarts[q + 1] : end, ll = 0, ml = 0, lit = 0, k;
        for (k = a; k < b; k++) {
          if (is(steps[k], 'd += nl') || is(steps[k], LAST)) { ll = steps[k][F.nl]; lit = steps[k][F.lit]; }
          if (is(steps[k], 'd += len')) ml = steps[k][F.len];
        }
        var litBits = 0;
        for (k = lit; k < lit + ll; k++) litBits += page.litlen ? page.litlen[k] : 8;
        var ext = (ll >= 15 ? 1 + Math.floor((ll - 15) / 255) : 0) + (ml && ml - 4 >= 15 ? 1 + Math.floor((ml - 19) / 255) : 0);
        list.push({ ll: ll, ml: ml, seq: used(steps[b]) - used(steps[a]), lit: litBits, lz4tok: 8 + (ml ? 16 : 0) + 8 * ext, lz4lit: 8 * ll });
      });
      return list;
    }

    function drawCost(k) {
      costBox.textContent = '';
      var q = seqOf(k) - 1, c = seqs[q];
      if (!c) { el('div', 'st-legend', 'Before the first sequence.', costBox); return; }
      var a = c.seq + c.lit, b = c.lz4tok + c.lz4lit, max = Math.max(a, b);
      function bar(label, parts, total) {
        var row = el('div', 'c-row', null, costBox);
        el('span', 'c-lab', label, row);
        var track = el('span', 'c-bar', null, row);
        parts.forEach(function (p) { if (p[0]) { var seg = el('span', p[1], null, track); seg.style.width = (100 * p[0] / max) + '%'; } });
        el('span', 'c-num', total + ' bits', row);
      }
      bar('seqlz', [[c.seq, 'c-code'], [c.lit, 'c-lit']], a);
      bar('lz4\u2019s format', [[c.lz4tok, 'c-code'], [c.lz4lit, 'c-lit']], b);
      el('div', 'st-legend', 'Sequence ' + (q + 1) + ': ' + c.ll + ' literal' + (c.ll === 1 ? '' : 's') + (c.ml ? ', a match of ' + c.ml : ', no match') +
        '. In seqlz ' + c.seq + ' bits of token, offset and lengths and ' + c.lit + ' of ' + (page.litlen ? 'coded' : 'raw') + ' literals; in lz4\u2019s format ' +
        c.lz4tok + ' and ' + c.lz4lit + '. The page\u2019s header and stream sizes are not counted.', costBox);
    }

    function seqOf(k) { var n = 0; while (n < seqStarts.length && seqStarts[n] <= k) n++; return n; }

    // what the step does, in a sentence, with the real values
    function explain(k) {
      var s = steps[k], n = steps[Math.min(k + 1, steps.length - 1)], t = at(s), e = entry(s[F.tok]);
      var d = s[F.d], nl = s[F.nl], len = s[F.len], off = s[F.off], cnt = s[F.count];
      var rawBits = e.back ? 64 - e.back : 0;
      if (t.indexOf('n_lit = get_unaligned_le16') === 0) {
        var h = page.comp[0] | page.comp[1] << 8;
        return 'The header, bytes 0 and 1 little endian: 0x' + hex(h, 4) + '. The top bit is ' + (h >> 15) + ': ' +
          (h >> 15 ? 'the literals are Huffman coded' : 'the literals are raw') + ', and there are ' + (h & 0x7fff) + ' of them.';
      }
      if (t.indexOf('q = decode_literals') === 0) return 'decode_literals() decodes the ' + page.nLit + ' coded literals into the scratch buffer, 8 streams side by side (chapter 13). The sequences’ bitstream starts right after the streams, at byte ' + page.bitStart + '.';
      if (t.indexOf('lit = scratch') === 0) return 'From now on the literals come from the scratch buffer, and the bit reader starts at byte ' + page.bitStart + '.';
      if (t.indexOf('lit = s + SEQLZ_HEADER') === 0) return 'Raw literals: they are bytes 2 to ' + (C.HEADER + page.nLit - 1) + ', and the bitstream starts after them, at byte ' + page.bitStart + '.';
      if (t.indexOf('end_gap =') === 0) return 'end_gap = d_end − lit_end, as numbers. When d − lit is end_gap, the literals left, lit_end − lit, are exactly the bytes the page still needs, d_end − d: one compare per slow sequence tells the end.';
      if (t.indexOf('if (n_lit == SEQLZ_PAGE)') === 0) return page.nLit === C.PAGE ? 'The page is all literals: no token, straight to the copy at the end.' : page.nLit + ' literals for a page of ' + C.PAGE + ' bytes: there are matches, so the loop over the sequences starts.';
      if (t.indexOf('d_fast =') === 0) return 'The fast path copies without checks, so it runs only while there is room: d at most ' + (C.PAGE - C.FAST_PAGE_ROOM) + ' (' + C.FAST_PAGE_ROOM + ' bytes before the end of the page), ' + C.FAST_LIT_ROOM + ' literal bytes left, ' + C.FAST_IN_ROOM + ' bytes of input left.';
      if (t.indexOf('tok = next_token(&br') === 0) {
        var nb = bitsOf(n), idx = Number(nb >> BigInt(64 - C.TOKEN_BITS));
        return 'next_token(): ' + cnt + ' bits in the register' + (cnt < C.TOKEN_BITS + 12 ? ', fewer than ' + (C.TOKEN_BITS + 12) + ', so it refills first; then' : ', enough, no refill;') +
          ' the top ' + C.TOKEN_BITS + ' bits, ' + bin(idx, C.TOKEN_BITS) + ' = ' + idx + ', index the token table. Its entry is 0x' + hex(n[F.tok], 8) + '.';
      }
      if (t.indexOf('if (tok >> TOK_ESCAPE_AT)') === 0) return 'Sequence ' + seqOf(k) + '. Is the entry the escape, bit 31? ' + (e.escape ? 'Yes: the token follows in 12 plain bits.' : 'No, the token has a code of its own.');
      if (t.indexOf('idx = (unsigned int)(br.bits') === 0) return 'The 12 bits after the escape’s code are the token itself.';
      if (t.indexOf('tok = token_entry(idx, 0)') === 0) return 'token_entry() builds the entry the table would have for this token, with a code of 0 bits: the escape and the 12 bits are dropped already.';
      if (t.indexOf('back = TOK_BACK(tok)') === 0) {
        return 'The entry, taken apart: a code of ' + e.n + ' bits, ll ' + e.ll + ', ml ' + e.ml + ', ' +
          (rawBits ? rawBits + ' offset bits' + (e.shift ? ', times 8' : '') : 'offset class 0, no offset bits') +
          (e.value ? ', and a length value follows' : '') + '. back = ' + e.back + '.';
      }
      if (t.indexOf('raw = ') === 0) {
        var b = bitsOf(s), raw = rawBits ? Number(((b << BigInt(e.n)) & ((1n << 64n) - 1n)) >> BigInt(e.back)) << e.shift : 0;
        return rawBits ? 'Shifts the ' + e.n + '-bit code out on top, then the ' + rawBits + ' offset bits down' + (e.shift ? ' and times 8' : '') + ': raw = ' + raw + '. No branch on the class.'
          : 'Class 0 has no offset bits: the shift by back = 0 gives 0, and nothing branches.';
      }
      if (t.indexOf('seqlz_br_drop(&br, TOK_DROP(tok))') === 0) return 'Drops the ' + e.drop + ' bits of code and offset: count ' + cnt + ' → ' + n[F.count] + '.';
      if (t.indexOf('off = back ? raw : last') === 0) return e.back ? 'The offset is raw: off = ' + n[F.off] + '.' : 'Class 0: the offset is the one before, last = ' + s[F.last] + '.';
      if (t.indexOf('nl = TOK_LL(tok)') === 0) return 'nl = ' + e.ll + ' literals' + (e.ll === C.LL_CAP ? ', the largest the token holds: a length value adds more.' : '.');
      if (t.indexOf('len = TOK_ML(tok)') === 0) return 'len = ' + e.ml + ' bytes of match' + (e.ml === C.ML_CAP + 4 ? ', the largest the token holds: a length value adds more.' : '.');
      if (t.indexOf('if (!(tok & (1U << TOK_VALUE_AT))') === 0) {
        var taken = is(n, 'tok = next_token_fast'), why = [];
        var litFast = page.coded ? C.SCRATCH - C.FAST_LIT_ROOM : page.comp.length - C.HEADER - C.FAST_LIT_ROOM;
        var inFast = page.comp.length >= C.FAST_IN_ROOM ? page.comp.length - C.FAST_IN_ROOM : 0;
        if (e.value) why.push('a length value follows');
        if (d > C.PAGE - C.FAST_PAGE_ROOM) why.push('d = ' + d + ' is within ' + C.FAST_PAGE_ROOM + ' bytes of the end');
        if (s[F.lit] > litFast) why.push('fewer than ' + C.FAST_LIT_ROOM + ' literal bytes are left');
        if (s[F.p] > inFast) why.push('fewer than ' + C.FAST_IN_ROOM + ' bytes of input are left');
        return taken ? 'The fast path: no length value, and room for its copies without checks.' : 'Not the fast path: ' + why.join(', ') + '.';
      }
      if (t.indexOf('tok = next_token_fast') === 0) return 'Reads the next token’s entry now, before the copies, so that its load runs while they do. skip = ' + s[F.skip] + ': ' + (s[F.skip] ? 'no refill this time, the last one left enough bits for two sequences.' : 'refill first, without checking the bits left.') + ' The next entry: 0x' + hex(n[F.tok], 8) + '.';
      if (t.indexOf('copy16(d, lit)') === 0) return 'Copies 16 literal bytes to the page at ' + d + ', though only nl = ' + nl + ' of them count: the match overwrites the rest. One copy instead of a loop over nl.';
      if (t.indexOf('d += nl') === 0) return 'd moves on by nl: ' + d + ' → ' + (d + nl) + '.';
      if (t.indexOf('lit += nl') === 0) return 'lit moves on by nl: literal ' + s[F.lit] + ' → ' + (s[F.lit] + nl) + '.';
      if (t.indexOf('last = off') === 0) return 'Remembers the offset, ' + off + ', for a later token of class 0.';
      if (t.indexOf('if (off - 1U >= (unsigned int)(d - (u8 *)dst)') === 0) return 'The offset must point into what is written: 1 ≤ ' + off + ' ≤ ' + d + '. ' + (off >= 1 && off <= d ? 'It does.' : 'It does not: the page is rejected.');
      if (t.indexOf('copy_match_fast') === 0) return 'Copies ' + len + ' bytes from ' + off + ' back. ' + (off >= 8 ? 'off ≥ 8: ' + (len > 32 ? 5 : len > 16 ? 4 : 2) + ' copies of 8 bytes, without a loop.' : 'off < 8: the pattern of ' + off + ' bytes is stored 8 bytes at a time.');
      if (t.indexOf('d += len') === 0) return 'd moves on by len: ' + d + ' → ' + (d + len) + '.';
      if (t.indexOf('continue') === 0) return 'Back to the top of the loop, with the next token already read.';
      if (t.indexOf('if (nl == SEQLZ_LL_CAP)') === 0) return 'The slow path. ' + (nl === C.LL_CAP ? 'nl is ' + C.LL_CAP + ': a literal length value follows.' : 'nl < ' + C.LL_CAP + ': no literal length value.');
      if (t.indexOf('nl += seqlz_br_value(&br, &t->ll)') === 0) return 'Reads the literal length value from the ll table: nl = ' + C.LL_CAP + ' + ' + (n[F.nl] - C.LL_CAP) + ' = ' + n[F.nl] + '.';
      if (t.indexOf('if (lit > lit_end') === 0) return 'The literals must be there and fit the page: ' + nl + ' ≤ ' + (page.nLit - s[F.lit]) + ' left, and ' + nl + ' ≤ ' + (C.PAGE - d) + '.';
      if (t.indexOf(LAST) === 0) return 'Sequence ' + seqOf(k) + ', the last: ' + (nl ? 'copy_literals() copies the ' + nl + ' literals left, to the end of the page.' : 'no literals are left, the last match ended at the end of the page: copy_literals() copies nothing.');
      if (t.indexOf('copy_literals(') === 0) return nl ? 'Copies ' + nl + ' literals, checked: 16 bytes at a time where there is room, the rest one by one.' : 'No literals to copy.';
      if (t.indexOf('if ((unsigned long)d - (unsigned long)lit == end_gap)') === 0) {
        var left = page.nLit - s[F.lit], room = C.PAGE - d;
        return left + ' literals left, ' + room + ' bytes of the page left. ' + (left === room ? 'The same: no match fits any more, the literals left are the last sequence, which has no token.' : 'Not the same, so more sequences with a match follow: read the next token.');
      }
      if (t.indexOf('if (len == SEQLZ_ML_CAP + 4U)') === 0) return len === C.ML_CAP + 4 ? 'len is ' + len + ': a match length value follows.' : 'len < ' + (C.ML_CAP + 4) + ': no match length value.';
      if (t.indexOf('len += seqlz_br_value(&br, &t->ml)') === 0) return 'Reads the match length value from the ml table: len = ' + (C.ML_CAP + 4) + ' + ' + (n[F.len] - C.ML_CAP - 4) + ' = ' + n[F.len] + '.';
      if (t.indexOf('if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||') === 0) return 'The offset must point into the page so far, 1 ≤ ' + off + ' ≤ ' + d + ', and the match must fit: ' + len + ' ≤ ' + (C.PAGE - d) + '.';
      if (t.indexOf('copy_match(') === 0) return 'Copies ' + len + ' bytes from ' + off + ' back, checked, ' + (off < 8 ? 'as a pattern of ' + off + ' bytes.' : '8 bytes at a time.');
      if (t.indexOf('skip = 0') === 0) return 'After a slow sequence the next fast one refills again.';
      if (t.indexOf('if (br.count < 0') === 0) return 'The page must end where its bits end: every byte read, ' + cnt + ' bits left in the last byte, and those 0.';
      if (t.indexOf('return 0') === 0) return 'A valid page: seqlz_decode() returns 0.';
      return '';
    }

    function drawBits(s) {
      var b = bitsOf(s), e = entry(s[F.tok]), cnt = Math.max(0, s[F.count]), t = at(s);
      // the token's code and offset bits are on top until the drop
      var mark = /^(if \(tok >> TOK_ESCAPE_AT\)|back = |raw = |seqlz_br_drop\(&br, TOK_DROP)/.test(t) && !e.escape;
      var nCode = mark ? e.n : 0, nOff = mark && e.back ? 64 - e.back : 0;
      bitsBox.textContent = '';
      for (var k = 0; k < 64; k++) {
        var v = Number((b >> BigInt(63 - k)) & 1n);
        var c = k < nCode ? 'r-code' : k < nCode + nOff ? 'r-off' : k < cnt ? 'r-real' : 'r-none';
        var sp = el('span', c, String(v));
        if (k === 32) bitsBox.appendChild(document.createElement('br'));
        else if (k % 8 === 0 && k) sp.classList.add('r-gap');
        bitsBox.appendChild(sp);
      }
      regLine.textContent = 'count = ' + s[F.count] + ' real bits on top, the next byte to load at ' + (s[F.p] < 0 ? '–' : s[F.p]) +
        (nCode ? '; the token’s code ' + nCode + ' bits' + (nOff ? ', the offset ' + nOff + ' bits' : '') : '');
    }

    function drawVars(s) {
      var e = entry(s[F.tok]);
      var cells = [
        ['d', s[F.d]], ['lit', s[F.lit] < 0 ? '–' : s[F.lit]], ['nl', s[F.nl]], ['len', s[F.len]],
        ['off', s[F.off]], ['last', s[F.last]], ['skip', s[F.skip]],
        ['tok', '0x' + hex(s[F.tok], 8)]
      ];
      varsBox.textContent = '';
      cells.forEach(function (c) {
        var w = el('div', 'v'); w.appendChild(el('span', 'vk', c[0])); w.appendChild(el('span', 'vv', String(c[1]))); varsBox.appendChild(w);
      });
      var f = el('div', 'v vtok');
      f.appendChild(el('span', 'vk', 'its entry'));
      f.appendChild(el('span', 'vv', e.escape ? 'escape, code ' + e.n + ' bits' :
        'code ' + e.n + ' bits, ll ' + e.ll + ', ml ' + e.ml + ', ' + (e.back ? (64 - e.back) + ' offset bits' + (e.shift ? ' × 8' : '') : 'class 0') + (e.value ? ', length value' : '')));
      varsBox.appendChild(f);
    }

    function drawInput(s) {
      var p = s[F.p], kids = inBox.children;
      for (var k = 0; k < kids.length; k++) {
        kids[k].classList.toggle('b-used', page.bitStart >= 0 && k >= page.bitStart && p >= 0 && k < p);
        kids[k].classList.toggle('b-at', k === p);
      }
      if (p >= 0 && kids[p]) keepVisible(inBox, kids[p]);
      var l = s[F.lit], lk = litBox.children, t = at(s);
      var copying = /^(copy16|copy_literals)/.test(t);
      for (var j = 0; j < lk.length; j++) {
        lk[j].classList.toggle('b-used', l >= 0 && j < l);
        lk[j].classList.toggle('b-now', copying && j >= l && j < l + s[F.nl]);
      }
      if (l >= 0 && lk[Math.min(l, lk.length - 1)]) keepVisible(litBox, lk[Math.min(l, lk.length - 1)]);
    }

    function colors() {
      var cs = getComputedStyle(fig);
      return { lit: cs.getPropertyValue('--mark').trim(), match: cs.getPropertyValue('--accent').trim(),
        empty: cs.getPropertyValue('--rule').trim(), ink: cs.getPropertyValue('--ink').trim(), paper: cs.getPropertyValue('--paper').trim() };
    }

    function drawPage(k) {
      var s = steps[k], t = at(s), col = colors(), cell = canvas.width / 64;
      // a byte final before step k in the colour of what wrote it, else empty
      var ctx = U.cells(canvas, function (b) { return when[b] >= 0 && when[b] < k ? kind[b] : 0; }, [col.empty, col.lit, col.match], col.paper);
      // the bytes the current copy writes, and where a match copies from
      var d = s[F.d], n = 0, src = -1;
      if (/^(copy16|copy_literals)/.test(t)) n = /^copy16/.test(t) ? 16 : s[F.nl];
      if (/^copy_match/.test(t)) { n = s[F.len]; src = d - s[F.off]; }
      function outline(a, len, color) {
        ctx.strokeStyle = color; ctx.lineWidth = 1.5;
        for (var q = a; q < a + len && q < C.PAGE; q++) if (q >= 0) ctx.strokeRect((q % 64) * cell + 0.75, Math.floor(q / 64) * cell + 0.75, cell - 2.5, cell - 2.5);
      }
      if (n) outline(d, n, col.ink);
      if (src >= 0) outline(src, n, col.match);
      // the bytes around d
      win.textContent = '';
      var a0 = Math.max(0, Math.min(d - 12, C.PAGE - 40));
      for (var q = a0; q < a0 + 40; q++) {
        var done2 = when[q] >= 0 && when[q] < k;
        var c = el('span', done2 ? (kind[q] === 1 ? 'w-lit' : 'w-match') : 'w-empty', done2 ? ch(page.page[q]) : ' ');
        if (q === d) c.classList.add('w-d');
        if (n && q >= d && q < d + n) c.classList.add('w-now');
        if (src >= 0 && q >= src && q < src + n) c.classList.add('w-src');
        win.appendChild(c);
      }
    }

    return {
      load: load,
      line: function (k) { return steps[k][F.line]; },
      draw: function (k) { var s = steps[k]; drawBits(s); drawVars(s); drawInput(s); drawCost(k); drawPage(k); },
      say: explain,
      pos: function (k) { return 'step ' + (k + 1) + ' of ' + steps.length + ', sequence ' + seqOf(k) + ' of ' + seqStarts.length; },
      seq: function (k) { return seqOf(k) > 0 ? seqOf(k) - 1 : null; },
      stepOfSeq: function (seq) { return seqStarts[Math.min(seq, seqStarts.length - 1)]; },
      linkLabel: 'decoded (chapter 14)'
    };
  }

  document.querySelectorAll('figure.stepper[data-kind="decode"]').forEach(function (f) { window.BookStepper.start(f, view); });
})();
