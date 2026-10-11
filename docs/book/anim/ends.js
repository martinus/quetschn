/*
 * The encoder's two ends in dst: the literals grow from the front after the u16, the bitstream from
 * the back down, one sequence per step; then encoder_finish() turns the bitstream around and copies it
 * to right after the literals. The data is chapter 16's page, compressed by the real encoder (ends.c).
 */
BookAnim.define('ends', function (ctx) {
  var D = ctx.data, S = D.runs[0].seq, BIG = D.runs[0].cap, HDR = D.header, ROOM = D.room;
  // the columns of a sequence's row, see ends.c
  var LL = 0, ML = 1, OFF = 2, CLS = 3, TOK = 4, TLEN = 5, ESC = 6, OB = 7, LLB = 8, MLB = 9, FL = 10,
    LIT = 11, P = 12, CNT = 13, RM = 14, WHI = 15, WLO = 16, BITS = 17;
  var run = 0;

  var choice = ctx.el('div', 'an-choice', null, ctx.bar);
  var labels = ['dst ' + D.runs[0].cap + ' (zram)', 'dst ' + D.runs[1].cap + ' (smallest)', 'dst ' + D.runs[2].cap + ' (too small)'];
  var btns = labels.map(function (l, j) {
    var b = ctx.el('button', null, l, choice);
    b.type = 'button'; b.setAttribute('aria-pressed', j === run ? 'true' : 'false');
    b.addEventListener('click', function () {
      run = j;
      btns.forEach(function (x, m) { x.setAttribute('aria-pressed', m === j ? 'true' : 'false'); });
      ctx.restart();
    });
    return b;
  });

  var W = 900, H = 280, X0 = 20, BW = 860, CW = 25, NC = 16, LX = 20, RX = 480;
  // the narrow layout, for a phone: 360 units wide, the close-ups one above the other, 8 bytes a row,
  // the room check above the bits of the sequence, the legend in rows
  var NW = 360, NH = 500, NX0 = 8, NCX = 40, NCW = 39, NRP = 52, NY1 = 110, NY2 = 232, NRC = 352, NBY = 376, NLY = 440;
  // the shortest gap in the overview bar that gets 'free: ... bytes', and that gets 'the old copy'
  var FREEMIN = 150, OLDMIN = 200;
  var root = ctx.svg('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': 'The buffer dst: literals from the front, the bitstream from the back, and two close-ups of the bytes at the two ends' }, ctx.stage);
  var g = ctx.svg('g', {}, root);

  function R() { return D.runs[run]; }
  function cap() { return R().cap; }
  // positions of the run into two pages, moved to this run's dst: p counts from the end
  function pOf(row) { return row[P] - BIG + cap(); }
  function wloOf(row) { return row[WLO] - BIG + cap(); }
  // the free room before the sequence: p moves with dst's size, lit does not
  function roomOf(row) { return row[RM] - BIG + cap(); }
  function nSeq() { return R().fail || R().n; }
  function nFinish() { return R().fail ? 1 : (R().in_place ? 5 : 4); }
  function steps() { return 1 + nSeq() + nFinish(); }

  function hex(b) { return (b < 16 ? '0' : '') + b.toString(16); }
  function byteText(b) {
    // a character for letters and signs, never for a digit, which would look like a hex byte
    if (b > 0x20 && b < 0x7f && (b < 0x30 || b > 0x39)) return String.fromCharCode(b);
    if (b === 0x20) return 'sp';
    if (b === 10) return '\\n';
    return hex(b);
  }
  function plural(n, w) { return n + ' ' + w + (n === 1 ? '' : 's'); }
  function T(x, y, s, cls, anchor, size) {
    return ctx.svg('text', { x: x, y: y, 'font-size': size || 12, 'text-anchor': anchor || 'start', class: cls || '', text: s }, g);
  }
  function rect(x, y, w, h, cls) { return ctx.svg('rect', { x: x, y: y, width: Math.max(w, 0), height: h, class: cls }, g); }

  // the state at step k: where the ends are, what this step changed
  function state(k) {
    var r = R(), s = { k: k, lit: HDR, p: cap(), cnt: 0, litPrev: HDR, pPrev: cap(), row: null, fin: -1 };
    var ns = nSeq();
    if (k >= 1 && k <= ns) {
      var row = S[k - 1], prev = k > 1 ? S[k - 2] : null;
      s.row = row; s.i = k - 1;
      s.lit = row[LIT]; s.p = pOf(row); s.cnt = row[CNT];
      s.litPrev = prev ? prev[LIT] : HDR; s.pPrev = prev ? pOf(prev) : cap();
      s.failed = r.fail === k;
      // a failed room check sets p to lit; the bits written so far stay where they are
      if (s.failed) { s.bsFrom = s.pPrev; s.p = s.lit; s.cnt = 0; }
    } else if (k > ns) {
      s.fin = k - ns - 1;
      s.lit = HDR + r.n_lit; s.p = r.p_seq; s.cnt = r.cnt_seq;
      s.litPrev = s.lit; s.pPrev = s.p;
      if (r.fail) { s.bsFrom = s.pPrev = pOf(S[r.fail - 2]); s.p = s.lit; }
    }
    return s;
  }

  function overview(s) {
    var r = R(), c = cap(), sc = BW / c;
    function bx(i) { return X0 + i * sc; }
    var Y = 30, BH = 30;
    var fin = s.fin, moved = !r.fail && fin >= (r.in_place ? 3 : 2);
    T(X0, 18, 'dst: ' + c + ' bytes' + (c === BIG ? ', the two pages zram gives' : ''), 'en-title');
    rect(X0, Y, BW, BH, 'en-free');
    // the u16: written by encoder_finish()
    var u16 = !r.fail && fin >= 1;
    rect(bx(0), Y, Math.max(bx(HDR) - bx(0), 3), BH, u16 ? (fin === 1 ? 'en-new' : 'en-u16') : 'en-hdr');
    if (!moved) {
      rect(bx(HDR), Y, bx(s.litPrev) - bx(HDR), BH, 'en-lit');
      if (s.lit > s.litPrev) rect(bx(s.litPrev), Y, Math.max(bx(s.lit) - bx(s.litPrev), 2), BH, 'en-new');
      rect(bx(s.pPrev), Y, bx(c) - bx(s.pPrev), BH, 'en-bs');
      if (s.pPrev > s.p && !s.bsFrom) rect(bx(s.p), Y, Math.max(bx(s.pPrev) - bx(s.p), 2), BH, 'en-newbs');
      if (s.cnt > 0) rect(bx(s.p - 1), Y, Math.max(sc, 2), BH, 'en-part');
      if (r.in_place && fin === 2) rect(bx(s.p), Y, bx(c) - bx(s.p), BH, 'en-newbs');
      var free = s.p - (s.cnt > 0 ? 1 : 0) - s.lit;
      var mid = (bx(s.lit) + bx(s.p)) / 2;
      if (bx(s.p) - bx(s.lit) > FREEMIN) T(mid, Y + 20, 'free: ' + free + ' bytes', 'en-muted', 'middle');
      // the two ends
      var lx = bx(s.lit), px = bx(s.p);
      ctx.svg('line', { x1: lx, y1: Y - 4, x2: lx, y2: Y + BH + 6, class: 'en-ptr' }, g);
      ctx.svg('line', { x1: px, y1: Y - 4, x2: px, y2: Y + BH + 6, class: 'en-ptr' }, g);
      var lt = (fin >= 2 && !r.fail ? 'out' : 'lit') + ' = ' + s.lit, pt = (fin >= 2 && !r.fail ? 'in' : 'p') + ' = ' + s.p;
      var lw = lt.length * 7.3, pw = pt.length * 7.3;
      if (s.p === s.lit) {
        ctx.svg('line', { x1: bx(s.bsFrom), y1: Y - 4, x2: bx(s.bsFrom), y2: Y + BH + 6, class: 'en-ptr2' }, g);
        T(Math.max(X0 + 120, lx - 3), Y + BH + 18, 'lit = p = ' + s.lit, 'en-ptrt', 'end');
      } else {
        T(Math.max(X0 + lw, lx - 3), Y + BH + 18, lt, 'en-ptrt', 'end');
        T(Math.min(X0 + BW - pw, px + 3), Y + BH + 18, pt, 'en-ptrt', 'start');
      }
    } else {
      var out = HDR + r.n_lit, end = out + r.bytes;
      rect(bx(HDR), Y, bx(out) - bx(HDR), BH, 'en-lit');
      rect(bx(r.p_seq - (r.cnt_seq > 0 ? 1 : 0)), Y, bx(c) - bx(r.p_seq - (r.cnt_seq > 0 ? 1 : 0)), BH, 'en-old');
      rect(bx(out), Y, bx(end) - bx(out), BH, fin === nFinish() - 1 ? 'en-bs' : 'en-newbs');
      ctx.svg('line', { x1: bx(end), y1: Y - 4, x2: bx(end), y2: Y + BH + 6, class: 'en-ptr' }, g);
      if (fin === nFinish() - 1) {
        // the page
        ctx.svg('path', { d: 'M' + bx(0) + ',' + (Y + BH + 6) + ' v6 H' + bx(end) + ' v-6', class: 'en-brace' }, g);
        T(Math.min(bx(end) + 4, X0 + BW - 170), Y + BH + 22, 'the page: ' + r.len + ' bytes', 'en-ptrt', 'start');
      } else {
        var t = 'out = ' + out;
        T(Math.max(X0 + t.length * 7.3, bx(out) - 3), Y + BH + 18, t, 'en-ptrt', 'end');
        // after the label of out where the two ends are close, e.g. in the narrow layout
        T(Math.min(X0 + BW - 90, Math.max(bx(end) + 3, X0 + t.length * 7.3 + 8)), Y + BH + 18, 'out + ' + r.bytes, 'en-ptrt', 'start');
        if (bx(r.p_seq) - bx(end) > OLDMIN) T(X0 + BW, Y + BH + 18, 'the old copy', 'en-muted', 'end');
      }
    }
  }

  // one strip of 16 bytes: cells[i] = { cls, text, tcls }
  function strip(x0, y0, first, title, cells, marks) {
    T(x0, y0 - 8, title, 'en-sub');
    for (var j = 0; j < NC; j++) {
      var c = cells[j], x = x0 + j * CW;
      rect(x, y0, CW - 2, 28, c.cls);
      if (c.text) T(x + (CW - 2) / 2, y0 + 18, c.text, c.tcls || '', 'middle', 11.5);
    }
    T(x0, y0 + 42, String(first), 'en-idx', 'start', 11);
    marks.forEach(function (m) {
      var x = x0 + (m.at - first) * CW - 1;
      ctx.svg('line', { x1: x, y1: y0 - 4, x2: x, y2: y0 + 32, class: 'en-ptr' }, g);
      var w = m.label.length * 7;
      var lx = Math.max(x0 + 40 + w / 2, Math.min(x0 + NC * CW - w / 2, x));
      T(lx, y0 + 42, m.label, 'en-ptrt', 'middle', 11.5);
    });
  }

  // the narrow strip: the same 16 bytes as 2 rows of 8, the first index of each row in front of it
  function stripN(y0, first, title, cells, marks) {
    T(NX0, y0 - 8, title, 'en-sub');
    for (var j = 0; j < NC; j++) {
      var c = cells[j], x = NCX + (j & 7) * NCW, y = y0 + (j >> 3) * NRP;
      rect(x, y, NCW - 2, 28, c.cls);
      if (c.text) T(x + (NCW - 2) / 2, y + 19, c.text, c.tcls || '', 'middle', 12.5);
      if (!(j & 7)) T(NCX - 5, y + 19, String(first + j), 'en-idx', 'end', 11.5);
    }
    marks.forEach(function (m) {
      // a mark between the rows is at the start of the second row
      var rel = m.at - first, row = rel >= 8 ? 1 : 0, y = y0 + row * NRP;
      var x = NCX + (rel - 8 * row) * NCW - 1;
      ctx.svg('line', { x1: x, y1: y - 4, x2: x, y2: y + 32, class: 'en-ptr' }, g);
      var w = m.label.length * 7.2;
      T(Math.max(NCX + w / 2, Math.min(NCX + 8 * NCW - w / 2, x)), y + 43, m.label, 'en-ptrt', 'middle', 12);
    });
  }

  function litCell(i, s) {
    var r = R(), c = cap();
    if (i >= c) return { cls: 'en-none' };
    if (i < HDR) {
      var u = r.n_lit;
      if (!r.fail && s.fin === 1) return { cls: 'en-new', text: hex(i === 0 ? u & 255 : u >> 8), tcls: 'en-on' };
      if (!r.fail && s.fin > 1) return { cls: 'en-u16', text: hex(i === 0 ? u & 255 : u >> 8), tcls: 'en-u16t' };
      return { cls: 'en-hdr' };
    }
    var moved = !r.fail && s.fin >= (r.in_place ? 3 : 2), out = HDR + r.n_lit;
    if (moved && i >= out && i < out + r.bytes) {
      if (s.fin === nFinish() - 1) return { cls: 'en-bs', text: hex(D.bs[i - out]), tcls: 'en-bst' };
      return { cls: 'en-newbs', text: hex(D.bs[i - out]), tcls: 'en-on' };
    }
    if (i < s.lit) {
      var b = D.lits[i - HDR];
      if (i >= s.litPrev && s.row) return { cls: 'en-new', text: byteText(b), tcls: 'en-on' };
      return { cls: 'en-lit', text: byteText(b) };
    }
    if (s.row && !s.failed && i < s.row[WHI]) return { cls: 'en-over' };
    return { cls: 'en-free' };
  }

  function bsCell(i, s) {
    var r = R(), c = cap();
    if (i >= c) return { cls: 'en-none' };
    var k = c - 1 - i;
    if (r.in_place && s.fin === 2) {
      // reversed where it is: byte 0 of the bitstream at in
      var inn = r.p_seq - (r.cnt_seq > 0 ? 1 : 0);
      if (i >= inn) return { cls: 'en-newbs', text: hex(D.bs[i - inn]), tcls: 'en-on' };
    }
    if (i >= (s.bsFrom || s.p)) {
      if (i < s.pPrev && s.row) return { cls: 'en-newbs', text: hex(D.bs[k]), tcls: 'en-on' };
      return { cls: s.fin >= (r.in_place ? 3 : 2) && !r.fail ? 'en-old' : 'en-bs', text: hex(D.bs[k]), tcls: 'en-bst' };
    }
    if (i === s.p - 1 && s.cnt > 0) return { cls: 'en-part', text: hex(D.bs[k] & (0xff00 >> s.cnt & 0xff)), tcls: 'en-bst' };
    if (s.row && !s.failed && i >= wloOf(s.row)) return { cls: 'en-over' };
    return { cls: 'en-free' };
  }

  function zoom(s) {
    var r = R(), c = cap(), Y = 112;
    var moved = !r.fail && s.fin >= (r.in_place ? 3 : 2);
    var a, b, cellsL = [], cellsR = [], j, tl, tr, mL = [], mR = [];
    if (moved) {
      var out = HDR + r.n_lit;
      a = 0; b = Math.max(0, out - 6);
      tl = 'the front: the u16, the first literals';
      tr = 'the literals end, the bitstream follows';
      for (j = 0; j < NC; j++) { cellsL.push(litCell(a + j, s)); cellsR.push(litCell(b + j, s)); }
      mL.push({ at: HDR, label: 'd + 2' });
      mR.push({ at: out, label: 'out = ' + out });
    } else {
      a = Math.max(0, Math.min(c - NC, s.lit - 11));
      b = Math.max(0, Math.min(c - NC, (s.bsFrom || s.p) - 5));
      tl = 'the front: literals, up to lit';
      tr = s.bsFrom ? 'the back: the bits written before' : 'the back: the bitstream, from p';
      if (r.in_place && s.fin === 2) tr = 'the bitstream reversed in place';
      for (j = 0; j < NC; j++) { cellsL.push(litCell(a + j, s)); cellsR.push(bsCell(b + j, s)); }
      mL.push({ at: s.lit, label: s.bsFrom ? 'lit = p' : 'lit' });
      if (s.p >= b && s.p <= b + NC) mR.push({ at: s.p, label: r.in_place && s.fin === 2 ? 'in' : 'p' });
    }
    if (ctx.narrow) {
      stripN(NY1, a, tl, cellsL, mL);
      stripN(NY2, b, tr, cellsR, mR);
      return;
    }
    strip(LX, Y, a, tl, cellsL, mL);
    strip(RX, Y, b, tr, cellsR, mR);
  }

  function bitsRow(s) {
    // narrow: the room check on a line of its own, the bits below it
    var Y = ctx.narrow ? NBY : 186, r = R(), YC = ctx.narrow ? NRC : Y + 14, BX = ctx.narrow ? X0 : RX;
    if (s.row) {
      var row = s.row, need = row[LL] + ROOM, ok = !s.failed;
      T(X0, YC, 'room check: p − lit = ' + roomOf(row) + (ok ? ' ≥ ' : ' < ') + 'll + ' + ROOM + ' = ' + need, ok ? 'en-txt' : 'en-bad');
      if (ok) {
        var parts = [['token', row[TLEN]], ['offset', row[OB]], ['ll value', row[LLB]], ['ml value', row[MLB]]];
        var x = BX, PX = 10, lx = BX;
        T(x, Y - 2, 'the bits of this sequence: ' + row[BITS], 'en-sub');
        parts.forEach(function (pt, m) {
          if (pt[1]) {
            var w = pt[1] * PX;
            rect(x, Y + 3, w - 2, 18, 'en-bit' + m);
            T(x + (w - 2) / 2, Y + 16, String(pt[1]), 'en-on', 'middle', ctx.narrow ? 11.5 : 11);
            x += w;
          }
          rect(lx, Y + 30, 12, 12, 'en-bit' + m);
          T(lx + 16, Y + 40, pt[0], 'en-muted', 'start', 11.5);
          lx += 16 + pt[0].length * 7 + 18;
        });
      }
    } else if (s.fin >= 0 && !r.fail) {
      T(X0, YC, 'literals: ' + r.n_lit + ' bytes, bitstream: ' + r.bytes + ' bytes', 'en-txt');
    } else if (s.fin >= 0) {
      T(X0, YC, 'encoder_finish(): p == lit, the page does not fit', 'en-bad', 'start', ctx.narrow ? 11.5 : 12);
    } else {
      T(X0, YC, 'acc = 0, cnt = 0', 'en-txt');
    }
  }

  function legend() {
    var Y = 256, items = [['en-lit', 'literals'], ['en-new', 'this step'], ['en-bs', 'bitstream'], ['en-newbs', 'this step'],
      ['en-part', 'bits left in acc'], ['en-over', 'written, overwritten later']];
    var x = X0;
    if (ctx.narrow) {
      // two columns: the literals and their step, the bitstream and its step, then the rest
      items.forEach(function (it, m) {
        var lx = X0 + (m & 1) * 142, ly = NLY + (m >> 1) * 20;
        rect(lx, ly, 14, 14, it[0]);
        T(lx + 19, ly + 11, it[1], 'en-muted', 'start', 11.5);
      });
      return;
    }
    items.forEach(function (it) {
      rect(x, Y, 14, 14, it[0]);
      T(x + 19, Y + 11, it[1], 'en-muted', 'start', 11.5);
      x += 19 + it[1].length * 7 + 22;
    });
  }

  function say(s) {
    var r = R(), c = cap();
    if (s.k === 0) {
      return 'encoder_init(): lit = d + 2, right after the 2 bytes kept for the u16; p = d + ' + c + ', the end of dst. The page has ' +
        r.n + ' sequences; each step writes one.';
    }
    if (s.row) {
      var row = s.row, n = r.n, i = s.i + 1;
      var head = 'Sequence ' + i + ' of ' + n + ': ' + plural(row[LL], 'literal') +
        (row[ML] ? ', then a match of ' + row[ML] + ' bytes at offset ' + row[OFF] + '. ' : ' and no match, the last one. ');
      if (s.failed) {
        return head + 'p − lit is ' + roomOf(row) + ', less than ll + ' + ROOM + ' = ' + (row[LL] + ROOM) +
          ': the page does not fit. p is set to lit, nothing is written.';
      }
      if (!row[ML]) return head + 'It has no token and no bits: the decoder knows it from the literals left. Nothing is put, p stays, ' + plural(row[CNT], 'bit') + ' stay in acc.';
      var bits = [];
      bits.push('token ' + row[TOK] + ' in ' + row[TLEN] + (row[ESC] ? ' (it has no code: the escape\'s ' + (row[TLEN] - 12) + ' and 12 bits)' : ''));
      bits.push('offset class ' + row[CLS] + ' in ' + row[OB] + (row[CLS] === 0 ? ' (the offset before)' : ''));
      if (row[LLB]) bits.push('ll value in ' + row[LLB]);
      if (row[MLB]) bits.push('ml value in ' + row[MLB]);
      var mv = s.pPrev - s.p, before = s.i > 0 ? S[s.i - 1][CNT] : 0;
      return head + 'Bits: ' + bits.join(', ') + ': ' + row[BITS] + '. ' +
        (before ? 'With the ' + plural(before, 'bit') + ' left in acc that is ' + (before + row[BITS]) + ': ' : '') +
        (row[FL] === 2 ? 'two flushes move' : 'the flush moves').replace(/^t/, before ? 't' : 'T') + ' p down ' + plural(mv, 'byte') + ', ' +
        plural(row[CNT], 'bit') + ' stay in acc.';
    }
    if (r.fail) {
      return 'encoder_finish() sees p == lit and returns 0, and so does seqlz_compress(): the page needs ' + D.len +
        ' bytes, and this dst has ' + (c - D.len) + ' more, but the room check of sequence ' + r.fail + ' wants ' + plural(ROOM - r.fail_room, 'byte') + ' more than it has.';
    }
    var f = s.fin, last = nFinish() - 1;
    if (f === 0) {
      return r.cnt_seq ? 'encoder_finish(): ' + plural(r.cnt_seq, 'bit') + ' are left in acc; they go to the byte below p, and p moves down 1. The bitstream is ' + r.bytes + ' bytes.'
        : 'encoder_finish(): cnt is 0, the last flush wrote every bit. The bitstream is the ' + r.bytes + ' bytes from p = ' + r.p_seq + ' to the end.';
    }
    if (f === 1) {
      return 'Now that the count is known, the u16 goes to d: ' + r.n_lit + ' literals, 0x' + hex(r.n_lit >> 8) + hex(r.n_lit & 255) +
        ', little endian ' + hex(r.n_lit & 255) + ' ' + hex(r.n_lit >> 8) + '. Its top bit is 0: the literals are raw.';
    }
    var out = HDR + r.n_lit, inn = r.p_seq - (r.cnt_seq ? 1 : 0);
    if (r.in_place && f === 2) {
      return 'out = ' + out + ' and in = ' + inn + ' are only ' + (inn - out) + ' bytes apart, fewer than the ' + r.bytes +
        ' bytes of the bitstream: the 8-byte copy would overwrite bytes it has not read. So the bitstream is reversed where it is: byte 0, 0x' +
        hex(D.bs[0]) + ', is now at in.';
    }
    if (f < last) {
      return (r.in_place ? 'memmove() moves the ' + r.bytes + ' bytes down to out = ' + out + ', right after the literals.'
        : 'The bitstream is copied to out = ' + out + ', right after the literals, 8 bytes at a time, each 8 turned around. Byte 0, 0x' +
          hex(D.bs[0]) + ', was the last byte of dst, at ' + (c - 1) + ', and is now at ' + out + '.');
    }
    return 'encoder_finish() returns 2 + ' + r.n_lit + ' + ' + r.bytes + ' = ' + r.len + ': the u16, the literals, the bitstream. ' +
      (c - r.len) + ' bytes of dst are not part of the page' + (c === BIG ? '.' : '. The last sequence\'s room check saw ' + roomOf(S[r.n - 1]) + ' bytes free, exactly ll + ' + ROOM + ', so 1 byte less fails.');
  }

  function draw(k) {
    while (g.firstChild) g.removeChild(g.firstChild);
    // the mode can change between two draws, e.g. a phone turned
    if (ctx.narrow) { X0 = NX0; BW = NW - 2 * NX0; FREEMIN = 130; OLDMIN = 170; root.setAttribute('viewBox', '0 0 ' + NW + ' ' + NH); }
    else { X0 = 20; BW = 860; FREEMIN = 150; OLDMIN = 200; root.setAttribute('viewBox', '0 0 ' + W + ' ' + H); }
    var s = state(k);
    overview(s);
    zoom(s);
    bitsRow(s);
    legend();
    ctx.say(say(s));
  }
  return {
    steps: steps, draw: draw, interval: 700, wide: 760,
    // step k shows dst after sequence k
    link: {
      label: 'written into dst (chapter 10)',
      seqAt: function (k) { return k >= 1 && k <= nSeq() ? k - 1 : null; },
      stepOf: function (seq) { return Math.min(seq + 1, nSeq()); }
    }
  };
});
