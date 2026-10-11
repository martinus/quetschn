/*
 * Huffman's method on the six offset classes of chapter 2, with their shares of the sequences: the
 * two lightest nodes are joined until one tree is left, then the codes are read off the tree, 0 for
 * the lighter branch on the left and 1 for the right, and the average code length is computed.
 */
BookAnim.define('huffman', function (ctx) {
  var SHARE = [30.55, 13.04, 8.82, 8.12, 24.17, 15.30];
  var W, H, SL, XL, YC, DY, BW, BH, PW, PH, TY, BY, SC, mode = null;
  // the sizes of the wide layout, or of the narrow one, one unit about one pixel on a phone, with
  // leaves of two lines more: "class" and its number, and the code without the word
  function sizes(narrow) {
    if (narrow) { W = 350; H = 326; SL = 55; XL = 10; YC = 182; DY = 56; BW = 51; BH = 64; PW = 46; PH = 20; TY = 258; BY = 250; SC = 55; }
    else { W = 720; H = 362; SL = 110; XL = 30; YC = 226; DY = 62; BW = 86; BH = 50; PW = 64; PH = 20; TY = 300; BY = 282; SC = 150; }
  }

  // the merges, computed: each joins the two lightest, the lighter one on the left (bit 0)
  var nodes = {}, forest = [], merges = [], i;
  for (i = 0; i < SHARE.length; i++) { nodes['c' + i] = { id: 'c' + i, cls: i, w: SHARE[i], leaf: true }; forest.push('c' + i); }
  function byWeight(a, b) { return nodes[a].w - nodes[b].w; }
  var forests = [forest.slice().sort(byWeight)];
  for (var m = 1; forest.length > 1; m++) {
    forest.sort(byWeight);
    var a = forest.shift(), b = forest.shift(), id = 'n' + m;
    nodes[id] = { id: id, w: nodes[a].w + nodes[b].w, kids: [a, b], m: m };
    merges.push({ a: a, b: b, id: id, before: forest.concat([a, b]).sort(byWeight) });
    forest.push(id);
    forests.push(forest.slice().sort(byWeight));
  }
  var rootId = forest[0];
  // depth, code and height of every node of the finished tree
  (function walk(id, depth, code) {
    var n = nodes[id]; n.depth = depth; n.code = code;
    if (n.kids) { walk(n.kids[0], depth + 1, code + '0'); walk(n.kids[1], depth + 1, code + '1'); }
  })(rootId, 0, '');
  function height(id) { var n = nodes[id]; return n.kids ? 1 + Math.max(height(n.kids[0]), height(n.kids[1])) : 0; }
  var avg = 0, ent = 0;
  for (i = 0; i < SHARE.length; i++) {
    avg += nodes['c' + i].code.length * SHARE[i] / 100;
    ent -= SHARE[i] / 100 * Math.log(SHARE[i] / 100) / Math.LN2;
  }
  // the order the codes are read in: the most frequent first
  var readOrder = SHARE.map(function (w, c) { return c; }).sort(function (x, y) { return SHARE[y] - SHARE[x]; });
  var NM = merges.length, S_FLIP = 1 + 2 * NM, S_BITS = S_FLIP + 1, S_CODE = S_BITS + 1, S_AVG = S_CODE + readOrder.length;

  function wt(w) { var s = w.toFixed(2); return s === '100.00' ? '100' : s; }
  function name(id) {
    var n = nodes[id];
    if (n.leaf) return 'class ' + n.cls + ', ' + wt(n.w) + '%';
    return 'the node {' + leaves(id).join(', ') + '}, ' + wt(n.w);
  }
  function leaves(id) { var n = nodes[id]; return n.leaf ? [n.cls] : leaves(n.kids[0]).concat(leaves(n.kids[1])); }

  // where every node is at step k: the forest of that step, laid out left to right by weight
  function layout(k) {
    var mcount = Math.min(NM, Math.floor(k / 2)), f = forests[mcount], pos = {}, slot = 0, byDepth = k >= S_FLIP;
    function place(id) {
      var n = nodes[id], x;
      if (n.leaf) x = XL + (slot++) * SL + SL / 2;
      else x = (place(n.kids[0]) + place(n.kids[1])) / 2;
      var lvl = byDepth ? 3 - n.depth : height(id);
      pos[id] = { x: x, y: YC - lvl * DY - (n.leaf ? 10 : 0) };
      return x;
    }
    f.forEach(place);
    return { pos: pos, forest: f, mcount: mcount };
  }

  var root = ctx.svg('svg', { role: 'img', 'aria-label': 'The six offset classes joined into a Huffman tree, two lightest nodes at a time, then their codes and the average code length' }, ctx.stage);
  var gEdges = ctx.svg('g', {}, root), gNodes = ctx.svg('g', {}, root), gTop = ctx.svg('g', {}, root);

  function top(n, p) { return n.leaf ? p.y : p.y - PH / 2; }
  function bottom(n, p) { return p.y + PH / 2; }

  function draw(k) {
    if (mode !== ctx.narrow) {
      // the nodes are made for one layout: a change of layout makes them anew
      mode = ctx.narrow; sizes(mode);
      root.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
      while (gEdges.firstChild) gEdges.removeChild(gEdges.firstChild);
      while (gNodes.firstChild) gNodes.removeChild(gNodes.firstChild);
    }
    var L = layout(k), pos = L.pos, pick = null, made = null, path = {}, codeShown = {};
    if (k >= 1 && k <= 2 * NM) { var mg = merges[Math.floor((k - 1) / 2)]; if (k % 2) pick = mg; else made = mg; }
    var reading = k >= S_CODE && k < S_AVG ? 'c' + readOrder[k - S_CODE] : null;
    for (i = 0; i < readOrder.length; i++) if (k >= S_CODE + i) codeShown['c' + readOrder[i]] = true;
    if (reading) { var id = rootId; path[id] = true; nodes[reading].code.split('').forEach(function (bit) { id = nodes[id].kids[+bit]; path[id] = true; }); }

    // edges: one per child that exists in this step
    while (gTop.firstChild) gTop.removeChild(gTop.firstChild);
    Object.keys(nodes).forEach(function (id) {
      var n = nodes[id];
      (n.kids || []).forEach(function (kid, side) {
        var e = ctx.keyed(gEdges, 'e' + kid, function () { return ctx.svg('path', { class: 'hf-edge an-move' }); });
        var on = !!pos[id];
        e.style.opacity = on ? 1 : 0;
        if (!on) return;
        var p = pos[id], q = pos[kid], d = 'M' + p.x + ',' + bottom(n, p) + ' L' + q.x + ',' + top(nodes[kid], q);
        e.style.d = 'path("' + d + '")';
        e.setAttribute('d', d);
        e.setAttribute('class', 'hf-edge an-move' + (path[id] && path[kid] ? ' hf-path' : ''));
        if (k >= S_BITS) {
          var mx = (p.x + q.x) / 2 + (side ? 9 : -9), my = (bottom(n, p) + top(nodes[kid], q)) / 2 - 2;
          ctx.svg('text', { x: mx, y: my, 'text-anchor': 'middle', 'font-size': 13, class: 'hf-bit' + (path[id] && path[kid] ? ' hf-on' : ''), text: String(side) }, gTop);
        }
      });
    });
    // nodes
    Object.keys(nodes).forEach(function (id) {
      var n = nodes[id], p = pos[id];
      var g = ctx.keyed(gNodes, id, function () {
        var g2 = ctx.svg('g', { class: 'an-move' });
        if (n.leaf && mode) {
          ctx.svg('rect', { x: -BW / 2, y: 0, width: BW, height: BH, rx: 4, class: 'hf-leaf' }, g2);
          ctx.svg('text', { x: 0, y: 14, 'text-anchor': 'middle', 'font-size': 12, text: 'class' }, g2);
          ctx.svg('text', { x: 0, y: 28, 'text-anchor': 'middle', 'font-size': 12, text: String(n.cls) }, g2);
          ctx.svg('text', { x: 0, y: 43, 'text-anchor': 'middle', 'font-size': 12, text: wt(n.w) + '%' }, g2);
          ctx.svg('text', { x: 0, y: 58, 'text-anchor': 'middle', 'font-size': 12, class: 'hf-code' }, g2);
        } else if (n.leaf) {
          ctx.svg('rect', { x: -BW / 2, y: 0, width: BW, height: BH, rx: 4, class: 'hf-leaf' }, g2);
          ctx.svg('text', { x: 0, y: 14, 'text-anchor': 'middle', 'font-size': 12, text: 'class ' + n.cls }, g2);
          ctx.svg('text', { x: 0, y: 29, 'text-anchor': 'middle', 'font-size': 12, text: wt(n.w) + '%' }, g2);
          ctx.svg('text', { x: 0, y: 44, 'text-anchor': 'middle', 'font-size': 12, class: 'hf-code' }, g2);
        } else {
          ctx.svg('rect', { x: -PW / 2, y: -PH / 2, width: PW, height: PH, rx: PH / 2, class: 'hf-pill' }, g2);
          ctx.svg('text', { x: 0, y: 4, 'text-anchor': 'middle', 'font-size': 12, text: wt(n.w) }, g2);
        }
        return g2;
      });
      if (!p) { g.style.opacity = 0; return; }
      g.style.opacity = 1;
      g.style.transform = 'translate(' + p.x + 'px,' + p.y + 'px)';
      var hot = pick && (id === pick.a || id === pick.b) || made && id === made.id || reading === id;
      g.setAttribute('class', 'an-move' + (hot ? ' hf-hot' : '') + (path[id] ? ' hf-onpath' : ''));
      if (n.leaf) g.querySelector('.hf-code').textContent = codeShown[id] ? (mode ? '' : 'code ') + n.code : '';
    });

    // below the tree: the row of the step, or the result
    var ty = TY;
    if (k < S_FLIP) {
      ctx.svg('text', { x: XL, y: ty, 'font-size': 12, class: 'hf-mut', text: 'nodes left: ' + L.forest.length + ', lightest on the left' }, gTop);
    } else if (k < S_AVG && mode) {
      // the same as the wide layout's line, in lines that fit, and where a leaf shows its code
      ['depth = code length', '0 for the left branch, 1 for the right'].concat(k >= S_CODE ? ['the last line of a leaf is its code'] : []).forEach(function (line, j) {
        ctx.svg('text', { x: XL, y: ty + j * 18, 'font-size': 12, class: 'hf-mut', text: line }, gTop);
      });
    } else if (k < S_AVG) {
      ctx.svg('text', { x: XL, y: ty, 'font-size': 12, class: 'hf-mut', text: 'depth = code length; 0 for the left branch, 1 for the right' }, gTop);
    } else {
      var bars = [['fixed width', 3], ['Huffman', avg], ['entropy', ent]], sc = SC;
      bars.forEach(function (bar, j) {
        var y = BY + j * 26;
        ctx.svg('text', { x: XL + 96, y: y + 13, 'text-anchor': 'end', 'font-size': 12, text: bar[0] }, gTop);
        ctx.svg('rect', { x: XL + 106, y: y, width: bar[1] * sc, height: 18, rx: 2, class: j === 1 ? 'hf-bar-on' : 'hf-bar' }, gTop);
        ctx.svg('text', { x: XL + 114 + bar[1] * sc, y: y + 13, 'font-size': 12, class: j === 1 ? 'hf-acc' : '', text: (bar[1] === 3 ? '3' : bar[1].toFixed(2)) + ' bits' }, gTop);
      });
    }

    say(k, L);
  }

  function say(k, L) {
    if (k === 0) return ctx.say('The six offset classes as nodes, weighted by their share of the sequences and sorted by weight. A fixed width gives each of them 3 bits. Huffman\'s method starts with the two lightest.');
    if (k <= 2 * NM) {
      var mg = merges[Math.floor((k - 1) / 2)], n = nodes[mg.id], no = Math.floor((k - 1) / 2) + 1;
      if (k % 2) return ctx.say('Join ' + no + ' of ' + NM + ': of the ' + mg.before.length + ' nodes left, the two lightest are ' + name(mg.a) + ', and ' + name(mg.b) + '.');
      return ctx.say('They join under a new node of ' + wt(nodes[mg.a].w) + ' + ' + wt(nodes[mg.b].w) + ' = ' + wt(n.w) + ', which goes back into the row by its weight. ' +
        (L.forest.length > 1 ? L.forest.length + ' nodes are left.' : 'One node is left, the root: the tree is done.'));
    }
    if (k === S_FLIP) return ctx.say('The same tree with each node one level below its parent. A symbol\'s code is the path from the root to its leaf, so its code length is its depth: classes 0 and 4 are 2 levels deep, the other four 3.');
    if (k === S_BITS) return ctx.say('Each branch gets a bit: 0 for the left one, here the lighter, 1 for the right. Swapping them at a node gives other codes of the same lengths.');
    if (k < S_AVG) {
      var c = readOrder[k - S_CODE], nd = nodes['c' + c], dirs = nd.code.split('').map(function (b) { return b === '0' ? 'left' : 'right'; });
      return ctx.say('Class ' + c + ', ' + wt(nd.w) + '%: from the root ' + dirs.join(', ') + ', so its code is ' + nd.code + ', ' + nd.code.length + ' bits.');
    }
    var two = [], three = [];
    for (var c2 = 0; c2 < SHARE.length; c2++) (nodes['c' + c2].code.length === 2 ? two : three).push(wt(SHARE[c2]) + '%');
    ctx.say('On average 2 × (' + two.join(' + ') + ') + 3 × (' + three.join(' + ') + ') = ' + avg.toFixed(2) + ' bits per offset class, against 3 for the fixed width. The entropy, ' + ent.toFixed(2) + ' bits, is the least any code can reach.');
  }

  return { steps: S_AVG + 1, draw: draw, interval: 1300, narrow: true };
});
