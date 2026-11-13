/* Memory Sentinel viewer (UC-4.2, 4.3). Reads the merged report (report.py Report.data) and draws:
 *   a header with totals, a filter per problem, one card per detector run (what it checked, what it cannot see),
 *   every finding as a tile, and the selected finding's evidence as a diagram:
 *     level-survivor   the reference chain from a root to the survivor
 *     shared-cycle     the ring of types, the side to weaken dashed
 *     heap stomps      the bytes around the block's end (or start, after a free), changed bytes in red
 *     source findings  who holds the pointer, and why the collector cannot see it
 * No dependencies; all text goes in through textContent. MemorySentinelViewer.mount(root, data) draws a report,
 * MemorySentinelViewer.diagram(finding) returns one diagram (the manual uses it).
 */
(function () {
  "use strict";

  var PROBLEMS = ["dangling", "cycle", "leak", "stomp", "crash"];
  var NAMES = { dangling: "Dangling pointers", cycle: "Reference cycles", leak: "Outlived their level", stomp: "Memory stomps", crash: "Crashes" };
  var KINDS = { scan: "Static pass", leaktest: "Level leak test", heap: "Heap sampler", run: "Headless run" };
  var C = {
    dangling: "#ff8a5c", cycle: "#b58cff", leak: "#2fd1c0", stomp: "#ff5c9a", crash: "#ff5c5c",
    error: "#ff5c5c", warning: "#ffb547", info: "#7a8bff", ink: "#e7eaee", muted: "#94a0af", faint: "#5d6877",
    line: "#2a3340", card: "#1b222b", well: "#141920", page: "#0e1116", accent: "#5cc8ff", good: "#3dd68c"
  };
  var SVGNS = "http://www.w3.org/2000/svg";

  function el(tag, attrs) {
    var n = document.createElement(tag);
    for (var k in (attrs || {})) {
      if (k === "text") n.textContent = attrs[k];
      else if (k === "class") n.className = attrs[k];
      else if (k === "style") n.setAttribute("style", attrs[k]);
      else if (k.slice(0, 2) === "on") n.addEventListener(k.slice(2), attrs[k]);
      else n.setAttribute(k, attrs[k]);
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c == null || c === false) continue;
      n.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
    }
    return n;
  }

  function s(tag, attrs, text) {
    var n = document.createElementNS(SVGNS, tag);
    for (var k in (attrs || {})) n.setAttribute(k, attrs[k]);
    if (text != null) n.textContent = text;
    return n;
  }

  function num(v) { return typeof v === "number" ? v.toLocaleString("en-US", { maximumFractionDigits: 1 }) : String(v); }
  function textWidth(t, size, mono) { return String(t).length * size * (mono ? 0.62 : 0.56); }
  function fit(t, max) { t = String(t || ""); return t.length <= max ? t : "…" + t.slice(t.length - max + 1); }
  function shortObj(p) { p = String(p || ""); var i = Math.max(p.lastIndexOf("."), p.lastIndexOf(":")); return i >= 0 ? p.slice(i + 1) : p; }
  function whereOf(f) {
    var w = f.where || {};
    if (w.file) return w.file.replace(/\\/g, "/").split("/").slice(-3).join("/") + ":" + w.line;
    return w.object || "";
  }

  function svgRoot(w, h, label) {
    var r = s("svg", { viewBox: "0 0 " + w + " " + h, width: w, height: h, role: "img", "aria-label": label });
    var defs = s("defs");
    ["ink", "muted", "error", "warning", "accent", "cycle", "leak", "stomp", "dangling", "good"].forEach(function (k) {
      var m = s("marker", { id: "ms-arrow-" + k, viewBox: "0 0 10 10", refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: "auto-start-reverse" });
      m.appendChild(s("path", { d: "M0,0 L10,5 L0,10 z", fill: C[k] }));
      defs.appendChild(m);
    });
    r.appendChild(defs);
    return r;
  }

  function node(g, x, y, top, bottom, edge, minW) {
    var w = Math.max(minW || 120, textWidth(top, 13) + 28, textWidth(bottom || "", 11.5, true) + 28);
    var h = bottom ? 46 : 32;
    g.appendChild(s("rect", { x: x, y: y, width: w, height: h, rx: 7, fill: C.card, stroke: edge, "stroke-opacity": 0.7 }));
    g.appendChild(s("rect", { x: x, y: y, width: 4, height: h, rx: 2, fill: edge }));
    g.appendChild(s("text", { x: x + 14, y: y + 20, fill: C.ink, "font-size": 13, "font-weight": 700 }, top));
    if (bottom) g.appendChild(s("text", { x: x + 14, y: y + 37, fill: C.muted, "font-size": 11.5, class: "mono" }, bottom));
    return { w: w, h: h };
  }

  function arrow(g, x1, y1, x2, y2, color, dashed, width) {
    g.appendChild(s("line", { x1: x1, y1: y1, x2: x2, y2: y2, stroke: C[color] || color, "stroke-width": width || 2,
      "stroke-dasharray": dashed ? "6 5" : "none", "marker-end": "url(#ms-arrow-" + color + ")" }));
  }

  // ------------------------------------------------------------------ diagrams

  function viaLabel(v) {
    v = String(v || "");
    if (v.indexOf("AddReferencedObjects") === 0) return "AddReferencedObjects";
    return v.replace(/\s*\(0x[0-9a-fA-F]+\)/, "");
  }

  function chainFig(f) {
    var e = f.evidence || {};
    var chain = e.chain || [];
    if (!chain.length) {
      var r0 = svgRoot(520, 80, "survivor with no chain");
      node(r0, 16, 16, e["class"] || "survivor", e.rooted ? "in the root set: no chain leads to it" : "no chain found", C.leak, 360);
      return { svg: r0, caption: "The reference search found no chain of references to this object." };
    }
    var rows = chain.length, h = rows * 74 + 10, w = 600;
    var r = svgRoot(w, h, "reference chain from a root to the survivor");
    chain.forEach(function (l, i) {
      var y = 8 + i * 74, last = i === rows - 1;
      var edge = last ? C.error : (i === 0 ? C.accent : C.muted);
      var tag = i === 0 ? "   root" : (last ? "   survivor" : "");
      node(r, 24, y, (l["class"] || "") + tag, fit(shortObj(l.object), 56), edge, 300);
      if (!last) {
        arrow(r, 40, y + 48, 40, y + 72, "muted");
        r.appendChild(s("text", { x: 54, y: y + 64, fill: C.accent, "font-size": 12, class: "mono" }, fit(viaLabel(chain[i + 1].via), 64)));
      }
    });
    return { svg: r, caption: "Root first. Each arrow is labelled with what holds the next object; cutting any arrow frees the survivor." };
  }

  function cycleFig(f) {
    var e = f.evidence || {};
    var cyc = (e.cycle || []).slice(0, -1), edges = e.edges || [];
    var n = Math.max(1, cyc.length), w = 560, h = n === 1 ? 190 : 330;
    var r = svgRoot(w, h, "shared pointer cycle");
    var cx = w / 2, cy = n === 1 ? 120 : h / 2, R = n === 1 ? 0 : Math.min(120, 60 + 26 * n);
    var start = n === 2 ? Math.PI : -Math.PI / 2;
    var pts = cyc.map(function (_, i) { var a = start + 2 * Math.PI * i / n; return [cx + Math.cos(a) * R, cy + Math.sin(a) * R]; });
    edges.slice(0, n).forEach(function (ed, i) {
      var weak = e.weaken && ed.member === e.weaken;
      var col = weak ? "warning" : "cycle";
      var a = pts[i], b = pts[(i + 1) % n];
      if (n === 1) {
        r.appendChild(s("path", { d: "M" + (a[0] - 34) + "," + (a[1] - 18) + " C" + (a[0] - 60) + "," + (a[1] - 96) + " " + (a[0] + 60) + "," + (a[1] - 96) + " " + (a[0] + 34) + "," + (a[1] - 18),
          fill: "none", stroke: C[col], "stroke-width": 2, "marker-end": "url(#ms-arrow-" + col + ")" }));
        r.appendChild(s("text", { x: a[0], y: a[1] - 84, fill: C[col], "font-size": 12, "text-anchor": "middle", class: "mono" }, ed.member));
        return;
      }
      var dx = b[0] - a[0], dy = b[1] - a[1], len = Math.hypot(dx, dy), ux = dx / len, uy = dy / len;
      var bend = n === 2 ? 46 : 26;
      var mx = (a[0] + b[0]) / 2 - uy * bend, my = (a[1] + b[1]) / 2 + ux * bend;
      r.appendChild(s("path", { d: "M" + (a[0] + ux * 46) + "," + (a[1] + uy * 22) + " Q" + mx + "," + my + " " + (b[0] - ux * 50) + "," + (b[1] - uy * 24),
        fill: "none", stroke: C[col], "stroke-width": weak ? 3 : 2, "stroke-dasharray": weak ? "7 5" : "none", "marker-end": "url(#ms-arrow-" + col + ")" }));
      var lx = (a[0] + b[0]) / 2 - uy * (bend + 14), ly = (a[1] + b[1]) / 2 + ux * (bend + 14) + 4;
      r.appendChild(s("text", { x: lx, y: ly, fill: C[col], "font-size": 12, "text-anchor": Math.abs(uy) > 0.7 ? (-uy > 0 ? "start" : "end") : "middle", class: "mono" }, fit(ed.member, 30)));
    });
    cyc.forEach(function (name, i) {
      var tw = Math.max(96, textWidth(name, 13) + 30);
      node(r, pts[i][0] - tw / 2, pts[i][1] - 16, name, null, C.cycle, tw);
    });
    return { svg: r, caption: e.weaken ? "Each arrow is a strong shared pointer. The dashed one points back: make " + e.weaken + " a TWeakPtr." :
      "A type that holds a shared pointer to its own type: fine as a chain or tree, a leak the day one links back." };
  }

  function bytesFig(f) {
    var e = f.evidence || {};
    var size = e.size || 0, off = e.offset || 0, past = off >= size;
    var found = (e.found || "").split(" ").filter(Boolean), exp = (e.expected || "").split(" ").filter(Boolean);
    var before = 6, total = before + 16, cell = 28, gap = 3, x0 = 16, y0 = 58;
    var w = x0 * 2 + total * (cell + gap), h = 172;
    var r = svgRoot(w, h, "bytes around the block");
    var guard = f.rule === "guard-fault";
    var head = guard ? (past ? "touched " + (off - size) + " bytes past the end of a " + num(size) + "-byte block" : "touched a freed " + num(size) + "-byte block")
      : (f.rule === "double-free" ? "a " + num(size) + "-byte block, freed twice" :
        (past ? (off === size ? "the first byte past the end of a " + num(size) + "-byte block changed" : "the first changed byte is " + (off - size) + " past the end of a " + num(size) + "-byte block")
          : "the first changed byte is at offset " + off + " of a freed " + num(size) + "-byte block"));
    r.appendChild(s("text", { x: x0, y: 22, fill: C.ink, "font-size": 13, "font-weight": 700 }, head));
    for (var i = 0; i < total; i++) {
      var at = off - before + i, k = i - before, x = x0 + i * (cell + gap);
      if (at < 0) continue;
      if (at === size && past) {
        r.appendChild(s("line", { x1: x - 2, y1: y0 - 14, x2: x - 2, y2: y0 + cell + 12, stroke: C.ink, "stroke-width": 2 }));
        r.appendChild(s("text", { x: x - 6, y: y0 - 18, fill: C.muted, "font-size": 11, "text-anchor": "end" }, "end of block"));
        r.appendChild(s("text", { x: x + 4, y: y0 - 18, fill: guard ? C.error : C.good, "font-size": 11 }, guard ? "no-access page" : "canary"));
      }
      var fill = C.card, label = "..", stroke = C.line;
      if (guard && past && at >= size) { fill = "url(#ms-hatch)"; label = ""; stroke = C.error; }
      else if (k >= 0 && k < found.length) {
        var bad = found[k] !== exp[k];
        fill = bad ? C.error : C.good; label = found[k];
        stroke = bad ? C.error : C.good;
        r.appendChild(s("rect", { x: x, y: y0, width: cell, height: cell, rx: 4, fill: fill, "fill-opacity": bad ? 0.85 : 0.35, stroke: stroke }));
        r.appendChild(s("text", { x: x + cell / 2, y: y0 + 18, fill: C.ink, "font-size": 11, "text-anchor": "middle", class: "mono" }, label));
        continue;
      } else if (past && at >= size) { fill = C.good; stroke = C.good; }
      r.appendChild(s("rect", { x: x, y: y0, width: cell, height: cell, rx: 4, fill: fill, "fill-opacity": fill === C.good ? 0.14 : 1, stroke: stroke }));
      if (label) r.appendChild(s("text", { x: x + cell / 2, y: y0 + 18, fill: C.faint, "font-size": 11, "text-anchor": "middle", class: "mono" }, label));
    }
    var hatch = s("pattern", { id: "ms-hatch", width: 6, height: 6, patternUnits: "userSpaceOnUse", patternTransform: "rotate(45)" });
    hatch.appendChild(s("rect", { width: 6, height: 6, fill: "#2a1418" }));
    hatch.appendChild(s("line", { x1: 0, y1: 0, x2: 0, y2: 6, stroke: C.error, "stroke-width": 2 }));
    r.querySelector("defs").appendChild(hatch);
    var writer = firstYou(e.write_stack || e.second_free_stack || []);
    var y = y0 + cell + 34;
    r.appendChild(s("text", { x: x0, y: y, fill: C.accent, "font-size": 12, "font-weight": 700 }, "caught by: " + (e.caught_by || "")));
    if (writer) r.appendChild(s("text", { x: x0, y: y + 22, fill: C.error, "font-size": 12, class: "mono" },
      (f.rule === "double-free" ? "freed again in: " : "stopped in: ") + fit(writer, 76)));
    if (e.register) r.appendChild(s("text", { x: x0, y: y + 44, fill: C.muted, "font-size": 11.5, class: "mono" },
      e.register + " watched " + e.watched_word));
    return { svg: r, caption: guard ? "The block sits right against a no-access page; the access faulted on it, at the exact instruction." :
      (f.rule === "double-free" ? "The block was still in quarantine when it was freed again." :
        (past ? "Green: canary bytes that held. Red: canary bytes that changed. The canary starts at the end of the block." :
          "The freed block was filled with a known pattern. Red: bytes written after the free.")) };
  }

  function pointerFig(f) {
    var e = f.evidence || {};
    var w = 620, h = 170, r = svgRoot(w, h, "who holds the pointer");
    var rule = f.rule;
    var owner = e.owner || "owner";
    var title = { "lambda-captures-uobject": [e.call || "delegate", "keeps the lambda", "captures " + (e.capture || "this")],
                  "void-hides-uobject": [e.value || "object", "the object", e.via || "void*"],
                  "shared-self-capture": [owner, "holds " + (e.member || "a delegate"), "lambda holds " + owner] }[rule];
    if (title) {
      var a = node(r, 16, 34, title[0], title[1], C.accent, 150);
      var bx = Math.max(330, 16 + a.w + 150);
      node(r, bx, 34, rule === "shared-self-capture" ? owner : "the object", rule === "shared-self-capture" ? "never freed" : "can be collected first", C.error, 150);
      arrow(r, 16 + a.w + 8, 57, bx - 8, 57, "error", rule !== "shared-self-capture");
      r.appendChild(s("text", { x: 16 + a.w + 16, y: 47, fill: C.error, "font-size": 12, class: "mono" }, fit(title[2], 30)));
    } else {
      var gc = node(r, 16, 16, "garbage collector", "walks UPROPERTY members", C.good, 190);
      var ow = node(r, 16, 100, owner, e.owner_kind || "", C.accent, 190);
      var ox = Math.max(360, 16 + Math.max(gc.w, ow.w) + 130);
      node(r, ox, 100, "the object", "can be collected", C.error, 160);
      arrow(r, 40, 16 + gc.h + 2, 40, 98, "good");
      arrow(r, 16 + ow.w + 8, 123, ox - 8, 123, "error", true);
      r.appendChild(s("text", { x: 16 + ow.w + 16, y: 114, fill: C.error, "font-size": 12, class: "mono" }, fit(e.member || "", 28)));
      r.appendChild(s("text", { x: 16 + ow.w + 16, y: 146, fill: C.error, "font-size": 11.5 }, "not in the collector's list"));
      var gx = 16 + gc.w + 10;
      r.appendChild(s("path", { d: "M" + gx + ",38 C" + (ox - 60) + ",38 " + (ox + 40) + ",50 " + (ox + 50) + ",96", fill: "none", stroke: C.faint, "stroke-width": 1.5, "stroke-dasharray": "3 4" }));
      r.appendChild(s("text", { x: (gx + ox) / 2 + 30, y: 32, fill: C.faint, "font-size": 11.5 }, "no path: it is not seen"));
    }
    return { svg: r, caption: rule === "lambda-captures-uobject" ? "The lambda runs later, on whatever the raw pointer points at by then." :
      rule === "void-hides-uobject" ? "A void* tells the collector nothing about what it holds." :
      rule === "shared-self-capture" ? "The object holds the delegate, the delegate holds the object." :
      (f.severity === "info" ? "A back pointer: safe while the struct stays inside its owner." : "The collector frees what it cannot see held. The member keeps the address.") };
  }

  function doubleFreeFig(f) {
    var e = f.evidence || {};
    var first = firstYou(e.free_stack || []), second = firstYou(e.second_free_stack || []);
    var r = svgRoot(600, 190, "a block freed twice");
    var b = node(r, 200, 70, num(e.size) + "-byte block", "held in quarantine", C.stomp, 200);
    node(r, 16, 12, "first free", fit(first || "unknown", 40), C.muted, 250);
    node(r, 16, 128, "freed again", fit(second || "unknown", 40), C.error, 250);
    arrow(r, 150, 60, 230, 70, "muted");
    arrow(r, 150, 128, 230, 118, "error");
    r.appendChild(s("text", { x: 200 + b.w + 16, y: 98, fill: C.good, "font-size": 12 }, "not freed again:"));
    r.appendChild(s("text", { x: 200 + b.w + 16, y: 116, fill: C.good, "font-size": 12 }, "the quarantine kept it"));
    return { svg: r, caption: "The block was still in quarantine when it was freed again, so the second free was refused and reported." };
  }

  function crashFig(f) {
    var frames = ((f.evidence || {}).frames || []).slice(0, 8);
    var r = svgRoot(640, 30 + frames.length * 22, "crash stack");
    r.appendChild(s("text", { x: 12, y: 20, fill: C.ink, "font-size": 13, "font-weight": 700 }, "exit code " + ((f.evidence || {}).exit_code)));
    frames.forEach(function (fr, i) {
      r.appendChild(s("text", { x: 12, y: 44 + i * 22, fill: /Core|kernel|ntdll/i.test(fr.module) ? C.faint : C.ink, "font-size": 12, class: "mono" },
        fit(fr.function + (fr.at ? "  " + fr.at.split(/[\\/]/).pop() : ""), 92)));
    });
    return { svg: r, caption: "The first frames of the crash, as the log printed them." };
  }

  function diagram(f) {
    if (f.rule === "level-survivor") return chainFig(f);
    if (f.rule === "shared-cycle" || f.rule === "shared-self-edge") return cycleFig(f);
    if (f.rule === "double-free") return doubleFreeFig(f);
    if (f.problem === "stomp") return bytesFig(f);
    if (f.rule === "run-crash") return crashFig(f);
    return pointerFig(f);
  }

  function firstYou(stack) {
    for (var i = 0; i < stack.length; i++) if (isYou(stack[i])) return stack[i].replace(/\s*\[.*\]\s*$/, "");
    return "";
  }
  function isYou(frame) {
    return /\.(cpp|h|inl):\d+\)/.test(frame) && !/SentinelHeap\.cpp|UnrealEditor-Core|\[.*-Core\.dll\]/.test(frame);
  }

  // ------------------------------------------------------------------ the page

  function detectorCard(file) {
    var ch = file.checked || {}, rows = [];
    var pick = {
      scan: [["files", "files read"], ["types", "types"], ["members", "members"], ["lines", "lines"]],
      leaktest: [["objects_before", "objects before"], ["level_objects", "in the level"], ["objects_after", "after unload"], ["survivors", "survived"], ["chains_found", "chains found"]],
      heap: [["allocations", "allocations"], ["sampled", "canary samples"], ["guard_sampled", "guard pages"], ["canary_checks", "checks"], ["rotations", "DR rotations"], ["watched_writes", "watched writes"]],
      run: [["exit_code", "exit code"], ["seconds", "seconds"]]
    }[file.kind] || [];
    pick.forEach(function (p) { if (ch[p[0]] != null) rows.push(el("dt", { text: p[1] }), el("dd", { text: num(ch[p[0]]) })); });
    if (file.kind === "heap") {
      if ((ch.suspect_sizes || []).length) rows.push(el("dt", { text: "suspect sizes" }), el("dd", { text: ch.suspect_sizes.join(", ") }));
      rows.push(el("dt", { text: "sampled" }), el("dd", { text: "1 in " + ch.sample_every + (ch.guard_every ? ", guard 1 in " + ch.guard_every : "") }));
    }
    var n = (file.counts || {}).error || 0, wn = (file.counts || {}).warning || 0;
    var state = !file.ok ? el("span", { class: "ms-state bad", text: "did not run" })
      : (n + wn ? el("span", { class: "ms-state found", text: (n + wn) + " found" }) : el("span", { class: "ms-state ok", text: "clean" }));
    var blind = (file.blind_spots || []).length ? el("details", null, el("summary", { text: "What it cannot see (" + file.blind_spots.length + ")" }),
      el.apply(null, ["ul", null].concat(file.blind_spots.map(function (b) { return el("li", { text: b }); })))) : null;
    return el("article", { class: "ms-det" },
      el("header", null, el("h3", { text: KINDS[file.kind] || file.kind }), state),
      file.ok ? el.apply(null, ["dl", null].concat(rows)) : el("p", { text: file.failure, style: "margin:0;color:" + C.error }),
      blind, el("div", { class: "ms-file", text: file.name + (file.target && file.target.map ? "  |  " + file.target.map : "") }));
  }

  function stackBlock(title, stack) {
    if (!stack || !stack.length) return null;
    return el("details", null, el("summary", { text: title + " (" + stack.length + " frames)" }),
      el.apply(null, ["ol", { class: "ms-stack" }].concat(stack.map(function (fr) { return el("li", { class: isYou(fr) ? "you" : "", text: fr }); }))));
  }

  function detailView(f, file) {
    if (!f) return el("div", { class: "ms-empty", text: "Select a finding to see its evidence." });
    var d = diagram(f);
    var fig = el("figure", { class: "ms-figure" }, d.svg, el("figcaption", { text: d.caption }));
    var e = f.evidence || {};
    var code = f.where && f.where.code && f.where.file ? el("pre", { class: "ms-code", text: f.where.line + " | " + f.where.code }) : null;
    return el("div", null,
      el("h2", { text: f.title }),
      el("div", { class: "ms-meta", text: [f.severity, f.rule, f.id, whereOf(f), file ? file.name : ""].filter(Boolean).join("  |  ") }),
      fig, code,
      el("section", { class: "ms-sec" }, el("h5", { text: "Why it matters" }), el("p", { text: f.why })),
      el("section", { class: "ms-sec fix" }, el("h5", { text: "Fix" }), el("p", { text: f.fix })),
      el("section", { class: "ms-sec known" }, el("h5", { text: "How it is known" }), el("p", { text: f.how_known + (f.suppressed ? "  Suppressed: " + f.suppressed : "") })),
      stackBlock("Write stack", e.write_stack), stackBlock("Second free", e.second_free_stack),
      stackBlock("Allocated at", e.alloc_stack), stackBlock("Freed at", e.free_stack),
      el("details", null, el("summary", { text: "The finding as data" }), el("pre", { class: "ms-code", text: JSON.stringify(f, null, 1) })));
  }

  function mount(root, data, opts) {
    opts = opts || {};
    var state = { problem: "", query: "", selected: null };
    var findings = (data.findings || []).map(function (f, i) { f._i = i; f.problem = f.problem || (data.rules[f.rule] || {}).problem || "crash"; return f; });
    var rank = { error: 0, warning: 1, info: 2 };
    findings.sort(function (a, b) { return (a.suppressed ? 3 : 0) + rank[a.severity] - ((b.suppressed ? 3 : 0) + rank[b.severity]); });
    var live0 = findings.filter(function (f) { return !f.suppressed; });
    state.selected = (opts.select && live0.filter(function (f) { return f.rule === opts.select; })[0])
      || live0.slice().sort(function (a, b) { return PROBLEMS.indexOf(a.problem) - PROBLEMS.indexOf(b.problem); })[0] || null;

    function visible() {
      var q = state.query.toLowerCase();
      return findings.filter(function (f) {
        if (state.problem && f.problem !== state.problem) return false;
        if (!q) return true;
        return (f.title + " " + whereOf(f) + " " + f.rule).toLowerCase().indexOf(q) >= 0;
      });
    }

    function render() {
      root.textContent = "";
      var live = findings.filter(function (f) { return !f.suppressed; });
      var count = function (sev) { return live.filter(function (f) { return f.severity === sev; }).length; };
      var proj = (data.files || []).map(function (f) { return (f.target || {}).project || (f.target || {}).root || ""; }).filter(Boolean)[0] || "";
      root.appendChild(el("header", { class: "ms-head" },
        el("div", null, el("h1", { class: "ms-title" }, el("b", { text: "Memory Sentinel" }), " report"),
          el("p", { class: "ms-sub", text: (proj ? String(proj).split(/[\\/]/).pop() + "  |  " : "") + (data.files || []).length + " detector runs  |  made " + String(data.made || "").replace("T", " ").replace("+00:00", " UTC") })),
        el("div", { class: "ms-totals" },
          el("div", { class: "ms-total" }, el("b", { class: "sev-error", text: String(count("error")) }), el("span", { text: "errors" })),
          el("div", { class: "ms-total" }, el("b", { class: "sev-warning", text: String(count("warning")) }), el("span", { text: "warnings" })),
          el("div", { class: "ms-total" }, el("b", { class: "sev-info", text: String(count("info")) }), el("span", { text: "info" })))));

      if ((data.unreadable || []).length) {
        root.appendChild(el("div", { class: "ms-unread", text: "Could not read: " + data.unreadable.join("; ") }));
      }

      var chips = el("div", { class: "ms-problems", role: "toolbar", "aria-label": "Filter by problem" });
      [""].concat(PROBLEMS).forEach(function (p) {
        var n = live.filter(function (f) { return !p || f.problem === p; }).length;
        if (p === "crash" && !n) return;
        chips.appendChild(el("button", { class: "ms-chip", type: "button", "aria-pressed": String(state.problem === p), style: p ? "--c:" + C[p] : "",
          onclick: function () { state.problem = p; render(); } }, el("i"), p ? NAMES[p] : "All", el("em", { text: String(n) })));
      });
      var search = el("input", { class: "ms-search", type: "search", placeholder: "Filter findings", "aria-label": "Filter findings", value: state.query });
      search.addEventListener("input", function () { state.query = search.value; renderList(); });
      chips.appendChild(search);
      root.appendChild(chips);

      var main = el("div", { class: "ms-main" });
      var list = el("div", { class: "ms-list" });
      var detail = el("aside", { class: "ms-detail", "aria-live": "polite" });
      main.appendChild(list);
      main.appendChild(detail);
      root.appendChild(main);

      var order = { scan: 0, leaktest: 1, heap: 2, run: 3 };
      var runs = (data.files || []).slice().sort(function (a, b) { return (order[a.kind] || 9) - (order[b.kind] || 9) || String(a.made).localeCompare(String(b.made)); });
      root.appendChild(el("h3", { class: "ms-runs-title", text: "Detector runs  " + runs.length }));
      root.appendChild(el("p", { class: "ms-runs-sub", text: "What each run checked, and what it cannot see. A clean run shows its counts, so it can be told apart from one that did not run." }));
      var dets = el("div", { class: "ms-detectors" });
      runs.forEach(function (f) { dets.appendChild(detectorCard(f)); });
      root.appendChild(dets);

      function renderDetail() {
        detail.textContent = "";
        detail.appendChild(detailView(state.selected, state.selected ? data.files[state.selected.file] : null));
      }
      function renderList() {
        list.textContent = "";
        var vis = visible();
        if (!vis.length) list.appendChild(el("div", { class: "ms-empty", text: findings.length ? "Nothing matches this filter." : "No findings: every detector that ran came back clean." }));
        PROBLEMS.forEach(function (p) {
          var group = vis.filter(function (f) { return f.problem === p; });
          if (!group.length) return;
          var g = el("section", { class: "ms-group", style: "--c:" + C[p] }, el("h4", { text: NAMES[p] + "  " + group.length }));
          group.forEach(function (f) {
            g.appendChild(el("button", { class: "ms-tile" + (f.suppressed ? " ms-supp" : ""), type: "button", style: "--c:" + C[p],
              "aria-current": String(state.selected === f),
              onclick: function () {
                state.selected = f;
                renderList(); renderDetail();
                if (window.matchMedia && window.matchMedia("(max-width: 860px)").matches) detail.scrollIntoView({ behavior: "smooth", block: "start" });
              } },
              el("span", { class: "ms-sev sev-" + f.severity, text: f.severity.toUpperCase() }),
              el("span", { class: "ms-t", text: f.title }),
              el("span", { class: "ms-w", text: whereOf(f) + (f.suppressed ? "  (suppressed)" : "") })));
          });
          list.appendChild(g);
        });
      }
      renderList();
      renderDetail();
    }
    render();
  }

  window.MemorySentinelViewer = { mount: mount, diagram: diagram };
})();
