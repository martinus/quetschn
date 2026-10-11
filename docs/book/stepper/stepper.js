/*
 * The step-through of seqlz_decode(): plays a trace that a copy of the real function recorded at build
 * time (instrument.py, trace.c). Each step is the state in front of one statement: the line, d, lit,
 * the bit reader's p, count and 64 bits, the token's entry, nl, len, off, last and skip.
 */
(function () {
  var F = { line: 0, d: 1, lit: 2, p: 3, count: 4, hi: 5, lo: 6, tok: 7, nl: 8, len: 9, off: 10, last: 11, skip: 12 };

  function entry(e) {
    return {
      n: e & 63, back: (e >>> 6) & 63, shift: (e >>> 12) & 3, drop: (e >>> 14) & 31,
      ll: (e >>> 19) & 15, ml: (e >>> 23) & 63, value: (e >>> 30) & 1, escape: e >>> 31
    };
  }
  function hex(v, w) { var s = (v >>> 0).toString(16); while (s.length < w) s = '0' + s; return s; }
  function bitsOf(s) { return (BigInt(s[F.hi]) << 32n) | BigInt(s[F.lo]); }
  function bin(v, n) { var s = ''; for (var i = n - 1; i >= 0; i--) s += (v >> i) & 1; return s; }
  function ch(b) { return b >= 32 && b < 127 ? String.fromCharCode(b) : b === 10 ? '↵' : '·'; }
  function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }

  function Stepper(fig) {
    var data = JSON.parse(document.getElementById(fig.id + '-data').textContent);
    var C = data.consts;
    var rows = {};
    fig.querySelectorAll('.st-code .line').forEach(function (r) { rows[r.querySelector('.ln').getAttribute('data-n')] = r; });
    var codeBox = fig.querySelector('.st-code');
    var say = fig.querySelector('.st-say');
    var scrub = fig.querySelector('.st-scrub');
    var pos = fig.querySelector('.st-pos');
    var bitsBox = fig.querySelector('.st-bits');
    var regLine = fig.querySelector('.st-regline');
    var varsBox = fig.querySelector('.st-vars');
    var inBox = fig.querySelector('.st-bytes');
    var litBox = fig.querySelector('.st-litbytes');
    var litHead = fig.querySelector('.st-lithead');
    var canvas = fig.querySelector('canvas');
    var win = fig.querySelector('.st-window');
    var page, steps, seqStarts, when, kind, i = 0, timer = null, stmt = {};

    // the statement text of each line, to find what a step does
    Object.keys(rows).forEach(function (n) { stmt[n] = rows[n].querySelector('.lc').textContent.trim(); });
    function at(s) { return stmt[s[F.line]] || ''; }
    function is(s, prefix) { return at(s).indexOf(prefix) === 0; }

    function load(id) {
      page = data.pages[id];
      steps = page.steps;
      seqStarts = [];
      steps.forEach(function (s, k) { if (is(s, 'if (tok >> TOK_ESCAPE_AT)')) seqStarts.push(k); });
      // when each byte of the page is final, and whether a literal or a match wrote it
      when = new Int32Array(C.PAGE).fill(-1);
      kind = new Uint8Array(C.PAGE);
      steps.forEach(function (s, k) {
        var a = s[F.d], n = 0, t = 0;
        if (is(s, 'd += nl;')) { n = s[F.nl]; t = 1; }
        if (is(s, 'd += len;')) { n = s[F.len]; t = 2; }
        for (var b = a; b < a + n && b < C.PAGE; b++) { when[b] = k; kind[b] = t; }
      });
      page.bitStart = -1;
      for (var k = 0; k < steps.length; k++) if (steps[k][F.p] >= 0) { page.bitStart = steps[k][F.p]; break; }
      page.nLit = page.lit.length;
      buildBytes();
      scrub.max = steps.length - 1;
      fig.querySelectorAll('.st-pages button').forEach(function (b) { b.setAttribute('aria-pressed', b.getAttribute('data-page') === id ? 'true' : 'false'); });
      go(0);
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
      if (t.indexOf('copy_literals(') === 0) return nl ? 'Copies ' + nl + ' literals, checked: 16 bytes at a time where there is room, the rest one by one.' : 'No literals to copy.';
      if (t.indexOf('if (d == d_end)') === 0) return d === C.PAGE ? 'The page is full: this must be the last sequence.' : 'The page is not full yet.';
      if (t.indexOf('if (len != 4U') === 0) return 'The last sequence must have ml − 4 = 0 and offset class 0: len = ' + len + ', back = ' + e.back + '. ' + (len === 4 && e.back === 0 ? 'It has.' : 'It has not: rejected.');
      if (t.indexOf('break') === 0) return 'The last sequence: out of the loop.';
      if (t.indexOf('if (len == SEQLZ_ML_CAP + 4U)') === 0) return len === C.ML_CAP + 4 ? 'len is ' + len + ': a match length value follows.' : 'len < ' + (C.ML_CAP + 4) + ': no match length value.';
      if (t.indexOf('len += seqlz_br_value(&br, &t->ml)') === 0) return 'Reads the match length value from the ml table: len = ' + (C.ML_CAP + 4) + ' + ' + (n[F.len] - C.ML_CAP - 4) + ' = ' + n[F.len] + '.';
      if (t.indexOf('if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||') === 0) return 'The offset must point into the page so far, 1 ≤ ' + off + ' ≤ ' + d + ', and the match must fit: ' + len + ' ≤ ' + (C.PAGE - d) + '.';
      if (t.indexOf('copy_match(') === 0) return 'Copies ' + len + ' bytes from ' + off + ' back, checked, ' + (off < 8 ? 'as a pattern of ' + off + ' bytes.' : '8 bytes at a time.');
      if (t.indexOf('skip = 0') === 0) return 'After a slow sequence the next fast one refills again.';
      if (t.indexOf('if (d != d_end') === 0) return 'The page must end where its bits end: d = ' + d + ', all ' + page.nLit + ' literals used, every byte read, ' + cnt + ' bits left in the last byte, and those 0.';
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

    function keepVisible(box, child) {
      // the boxes are position: relative, so offsetTop counts from their top
      var top = child.offsetTop, h = box.clientHeight;
      if (top < box.scrollTop || top > box.scrollTop + h - child.offsetHeight) box.scrollTop = Math.max(0, top - h / 2);
    }

    function colors() {
      var cs = getComputedStyle(fig);
      return { lit: cs.getPropertyValue('--mark').trim(), match: cs.getPropertyValue('--accent').trim(),
        empty: cs.getPropertyValue('--rule').trim(), ink: cs.getPropertyValue('--ink').trim(), paper: cs.getPropertyValue('--paper').trim() };
    }

    function drawPage(k) {
      var s = steps[k], t = at(s), col = colors(), ctx = canvas.getContext('2d');
      var cell = canvas.width / 64;
      ctx.fillStyle = col.paper; ctx.fillRect(0, 0, canvas.width, canvas.height);
      for (var b = 0; b < C.PAGE; b++) {
        var done = when[b] >= 0 && when[b] < k;
        ctx.fillStyle = done ? (kind[b] === 1 ? col.lit : col.match) : col.empty;
        ctx.fillRect((b % 64) * cell, Math.floor(b / 64) * cell, cell - 1, cell - 1);
      }
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

    function go(k) {
      i = Math.max(0, Math.min(steps.length - 1, k));
      var s = steps[i];
      fig.querySelectorAll('.st-code .line.now').forEach(function (r) { r.classList.remove('now'); });
      var r = rows[s[F.line]];
      if (r) { r.classList.add('now'); keepVisible(codeBox, r); }
      say.textContent = explain(i);
      drawBits(s); drawVars(s); drawInput(s); drawPage(i);
      scrub.value = i;
      pos.textContent = 'step ' + (i + 1) + ' of ' + steps.length + ', sequence ' + seqOf(i) + ' of ' + seqStarts.length;
    }

    function seqStep(dir) {
      var n = seqOf(i);
      if (dir > 0) go(n < seqStarts.length ? seqStarts[n] : steps.length - 1);
      else go(n >= 2 && seqStarts[n - 1] === i ? seqStarts[n - 2] : n >= 1 && seqStarts[n - 1] < i ? seqStarts[n - 1] : 0);
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
    fig.querySelectorAll('.st-pages button').forEach(function (b) {
      b.addEventListener('click', function () { play(false); load(b.getAttribute('data-page')); });
    });
    scrub.addEventListener('input', function () { play(false); go(+scrub.value); });
    fig.addEventListener('keydown', function (ev) {
      if (ev.target === scrub) return;
      if (ev.key === 'ArrowRight') { play(false); ev.shiftKey ? seqStep(1) : go(i + 1); ev.preventDefault(); }
      if (ev.key === 'ArrowLeft') { play(false); ev.shiftKey ? seqStep(-1) : go(i - 1); ev.preventDefault(); }
    });
    // colours follow the theme
    document.addEventListener('book-theme', function () { setTimeout(function () { go(i); }, 0); });
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () { go(i); });
    load(fig.querySelector('.st-pages button').getAttribute('data-page'));
  }

  document.querySelectorAll('figure.stepper').forEach(function (f) { Stepper(f); });
})();
