// A playable mock of Carve DAW's playlist and piano roll for the landing page.
// The song is the app's "Neon Streets" sample (src/model/SampleSongs.cpp), the
// gestures follow the app's own help bar, and the sound is a few small Web
// Audio voices standing in for the real instruments.
(() => {
  "use strict";

  const root = document.getElementById("daw-mock");
  if (!root) return;

  const isMac = /Mac|iPhone|iPad|iPod/.test(navigator.platform || "") || /Mac OS X/.test(navigator.userAgent);
  const MOD = isMac ? "Cmd" : "Ctrl";
  const coarse = window.matchMedia && matchMedia("(pointer: coarse)").matches;
  const FONT = '-apple-system, BlinkMacSystemFont, "Helvetica Neue", "Hiragino Sans", sans-serif';
  const TEMPO = 124;
  const SPB = 60 / TEMPO;
  const GRID = 0.25; // 1/16

  const C = {
    bg: "#232327", beat: "#2a2a2f", bar: "#3a3a40", rowSep: "#2f2f36",
    label: "#2b2b30", labelSel: "#35353d", labelInk: "#b8b8c0",
    ruler: "#17171b", rulerInk: "#9a9aa4", below: "#323e44",
    playhead: "#ff5f45", accent: "#e08a3c",
    keyW: "#d8d8dc", keyB: "#2a2a2e", rowB: "#1c1c20", rowW: "#232327",
    note: "#dc9156", noteSel: "#f7d6b4", noteLine: "#1d1d22",
    vel: "#e18a41", velSel: "#f6c08e", velBg: "#1f1f24"
  };

  const DRUM = { 36: "Kick", 37: "Rim", 38: "Snare", 39: "Clap", 41: "Low Tom", 42: "Closed Hat", 46: "Open Hat", 48: "High Tom" };

  // ---------------------------------------------------------------- song

  let uid = 1;
  const nid = () => uid++;
  const mkPat = (name, len, notes) => ({ id: nid(), name, len, notes: notes.map(([s, l, p, v]) => ({ s, l, p, v })) });
  const hits = (list) => list.map(([s, p, v]) => [s, 0.25, p, v]);
  const chord = (s, l, ps, v) => ps.map((p) => [s, l, p, v]);

  function initialSong() {
    const floor = mkPat("Four Floor", 4, hits([[0, 36, 118], [1, 36, 112], [2, 36, 118], [3, 36, 112], [1, 39, 100], [3, 39, 100],
      [0.5, 42, 62], [1.5, 42, 74], [2.5, 42, 62], [3.5, 42, 74]]));
    const lift = mkPat("Four Floor Lift", 4, hits([[0, 36, 118], [1, 36, 112], [2, 36, 118], [3, 36, 112], [1, 39, 100], [3, 39, 100],
      [0.5, 42, 62], [1.5, 42, 74], [2.5, 42, 62], [3.5, 46, 86]]));
    const bassline = mkPat("Bassline", 8, [[0, 0.9, 45, 112], [1.5, 0.4, 45, 88], [2, 0.9, 45, 104], [3.5, 0.4, 52, 92],
      [4, 0.9, 41, 112], [5.5, 0.4, 41, 88], [6, 0.9, 43, 104], [7.5, 0.4, 48, 92]]);
    const pad = mkPat("Am F C G", 16, [...chord(0, 3.8, [57, 60, 64], 72), ...chord(4, 3.8, [53, 57, 60], 72),
      ...chord(8, 3.8, [52, 55, 60], 72), ...chord(12, 3.8, [50, 55, 59], 72)]);
    const hook = mkPat("Hook", 16, [[0, 0.9, 76, 100], [1, 0.45, 74, 92], [1.5, 0.45, 72, 92], [2, 1.8, 69, 104],
      [4, 0.9, 72, 100], [5, 0.45, 74, 92], [5.5, 0.45, 76, 92], [6, 1.8, 72, 104],
      [8, 0.9, 77, 100], [9, 0.45, 76, 92], [9.5, 0.45, 74, 92], [10, 1.8, 72, 104],
      [12, 0.9, 74, 100], [13, 0.45, 72, 92], [13.5, 0.45, 71, 92], [14, 1.8, 69, 104]]);

    const gens = [
      { name: "Drums", kind: "drums", color: "#c08958", mute: false, solo: false, patterns: [floor, lift], cur: floor.id },
      { name: "Bass", kind: "bass", color: "#a5bf58", mute: false, solo: false, patterns: [bassline], cur: bassline.id },
      { name: "Chords", kind: "pad", color: "#56bf5b", mute: false, solo: false, patterns: [pad], cur: pad.id },
      { name: "Lead", kind: "lead", color: "#52c0ad", mute: false, solo: false, patterns: [hook], cur: hook.id }
    ];
    const clips = [];
    const place = (g, p, starts) => starts.forEach((s) => clips.push({ id: nid(), g, p: p.id, s, l: p.len }));
    place(0, floor, [0, 4, 8, 16, 20, 24]);
    place(0, lift, [12, 28]);
    place(1, bassline, [0, 8, 16, 24]);
    place(2, pad, [0, 16]);
    place(3, hook, [16]);
    return { gens, clips };
  }

  let song = initialSong();
  const st = {
    editG: 3, selClip: song.clips[song.clips.length - 1].id, selNotes: new Set(),
    focus: "pl", playing: false, pos: 0, top: {}, lastLen: { drums: 0.25, bass: 0.5, pad: 1, lead: 0.5 },
    band: null
  };
  const gen = (i) => song.gens[i];
  const patOf = (g, id) => g.patterns.find((p) => p.id === id);
  const editPat = () => patOf(gen(st.editG), gen(st.editG).cur);
  const clipById = (id) => song.clips.find((c) => c.id === id);

  // ---------------------------------------------------------------- undo

  const undoStack = [], redoStack = [];
  let pending = null;
  const snapshot = () => JSON.stringify(song);
  const begin = () => { pending = snapshot(); };
  function commit() {
    if (pending !== null && pending !== snapshot()) {
      undoStack.push(pending);
      if (undoStack.length > 200) undoStack.shift();
      redoStack.length = 0;
    }
    pending = null;
  }
  function restore(s) {
    song = JSON.parse(s);
    st.selNotes.clear();
    if (!clipById(st.selClip)) st.selClip = null;
    renderHeader();
    draw();
  }
  function undo() { if (undoStack.length) { redoStack.push(snapshot()); restore(undoStack.pop()); } }
  function redo() { if (redoStack.length) { undoStack.push(snapshot()); restore(redoStack.pop()); } }

  // ---------------------------------------------------------------- DOM

  const ICON_PLAY = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 2.5v11l9.5-5.5z"/></svg>';
  const ICON_STOP = '<svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3.5" y="3.5" width="9" height="9" rx="1"/></svg>';
  root.innerHTML = `
    <div class="dm-title"><span class="dm-lights"><i></i><i></i><i></i></span><span>Neon Streets - Carve DAW</span></div>
    <div class="dm-bar">
      <div class="dm-group">
        <button type="button" class="dm-btn dm-play" aria-label="Play">${ICON_PLAY}</button>
        <button type="button" class="dm-btn dm-stop" aria-label="Stop">${ICON_STOP}</button>
      </div>
      <div class="dm-group"><span class="dm-k">BPM</span><span class="dm-v">${TEMPO.toFixed(1)}</span></div>
      <div class="dm-group"><span class="dm-k">BAR</span><span class="dm-v dm-pos">001.1</span></div>
      <span class="dm-crumb"></span>
    </div>
    <canvas class="dm-pl"></canvas>
    <div class="dm-prbar">
      <span class="dm-prname"><i></i><span></span></span>
      <div class="dm-pats"></div>
      <button type="button" class="dm-btn dm-new">+ New</button>
      <button type="button" class="dm-btn dm-clone">Clone</button>
      <span class="dm-len">Bars <button type="button" class="dm-btn dm-sq dm-lm" aria-label="Shorter">−</button><span class="dm-lv">4</span><button type="button" class="dm-btn dm-sq dm-lp" aria-label="Longer">+</button></span>
    </div>
    <canvas class="dm-pr"></canvas>
    <div class="dm-help"></div>
    <div class="dm-menu" hidden></div>`;

  const $ = (s) => root.querySelector(s);
  const plC = $(".dm-pl"), prC = $(".dm-pr");
  const plX = plC.getContext("2d"), prX = prC.getContext("2d");
  const playBtn = $(".dm-play"), stopBtn = $(".dm-stop"), posEl = $(".dm-pos"), crumbEl = $(".dm-crumb");
  const menu = $(".dm-menu"), helpEl = $(".dm-help");

  const PL = { lw: 112, rh: 22, row: 44, bars: 12 };
  PL.h = PL.rh + 4 * PL.row + 12;
  const PR = { kw: 54, rh: 20, gridH: 288, velH: 60 };
  PR.h = PR.rh + PR.gridH + PR.velH;
  let W = 800;

  function resize() {
    W = Math.max(600, Math.round(plC.clientWidth || root.clientWidth));
    const d = window.devicePixelRatio || 1;
    for (const [cv, h] of [[plC, PL.h], [prC, PR.h]]) {
      cv.style.height = h + "px";
      cv.width = Math.round(W * d);
      cv.height = Math.round(h * d);
      cv.getContext("2d").setTransform(d, 0, 0, d, 0, 0);
    }
    drawNow();
  }

  // ---------------------------------------------------------------- helpers

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const isBlack = (p) => [1, 3, 6, 8, 10].includes(((p % 12) + 12) % 12);
  const mtof = (p) => 440 * Math.pow(2, (p - 69) / 12);
  function rr(x, X, Y, w, h, r) {
    r = Math.min(r, w / 2, h / 2);
    x.beginPath();
    x.moveTo(X + r, Y);
    x.arcTo(X + w, Y, X + w, Y + h, r);
    x.arcTo(X + w, Y + h, X, Y + h, r);
    x.arcTo(X, Y + h, X, Y, r);
    x.arcTo(X, Y, X + w, Y, r);
    x.closePath();
  }
  function fit(x, s, w) {
    if (x.measureText(s).width <= w) return s;
    while (s.length > 1 && x.measureText(s + "…").width > w) s = s.slice(0, -1);
    return s.length > 1 ? s + "…" : "";
  }
  function mix(hex, to, t) {
    const a = parseInt(hex.slice(1), 16), b = parseInt(to.slice(1), 16);
    const ch = (n, s) => (n >> s) & 255;
    const m = (s) => Math.round(ch(a, s) + (ch(b, s) - ch(a, s)) * t);
    return `rgb(${m(16)},${m(8)},${m(0)})`;
  }
  function local(e, cv) {
    const r = cv.getBoundingClientRect();
    return { mx: (e.clientX - r.left) * (W / r.width), my: e.clientY - r.top };
  }
  function songLen() {
    let e = 0;
    for (const c of song.clips) e = Math.max(e, c.s + c.l);
    return Math.max(16, Math.ceil(e / 4) * 4);
  }

  let dirty = false;
  function draw() {
    if (dirty) return;
    dirty = true;
    requestAnimationFrame(() => { dirty = false; drawNow(); });
  }
  function drawNow() { drawPL(); drawPR(); updatePos(); }

  // ---------------------------------------------------------------- playlist

  const plPpb = () => (W - PL.lw) / (PL.bars * 4);

  function drawPL() {
    const x = plX, p = plPpb(), lw = PL.lw, rh = PL.rh;
    x.fillStyle = C.below;
    x.fillRect(0, 0, W, PL.h);

    // ruler
    x.fillStyle = C.ruler;
    x.fillRect(0, 0, W, rh);
    x.fillStyle = "rgba(224,138,60,0.35)";
    x.fillRect(lw, rh - 3, songLen() * p, 3);
    x.font = "11px " + FONT;
    x.textBaseline = "alphabetic";
    for (let b = 0; b < PL.bars; b++) {
      const X = lw + b * 4 * p;
      x.fillStyle = "#4a4a52";
      x.fillRect(Math.round(X), 4, 1, rh - 4);
      x.fillStyle = C.rulerInk;
      x.fillText(String(b + 1), X + 4, 15);
    }

    for (let r = 0; r < 4; r++) {
      const y = rh + r * PL.row, g = gen(r);
      x.fillStyle = C.bg;
      x.fillRect(lw, y, W - lw, PL.row);
      for (let b = 0; b <= PL.bars * 4; b++) {
        x.fillStyle = b % 4 ? C.beat : C.bar;
        x.fillRect(Math.round(lw + b * p), y, 1, PL.row);
      }
      x.fillStyle = C.rowSep;
      x.fillRect(0, y + PL.row - 1, W, 1);

      const sel = r === st.editG;
      x.fillStyle = sel ? C.labelSel : C.label;
      x.fillRect(0, y, lw, PL.row - 1);
      x.fillStyle = g.color;
      x.fillRect(0, y, 3, PL.row - 1);
      x.font = "600 13px " + FONT;
      x.fillStyle = sel ? "#ffffff" : C.labelInk;
      x.fillText(g.name, 10, y + 19);
      for (const [i, on, lit, t] of [[0, g.mute, "#d9a53a", "M"], [1, g.solo, "#62b866", "S"]]) {
        const X = 10 + i * 20;
        x.fillStyle = on ? lit : "#3a3a42";
        rr(x, X, y + 26, 16, 12, 2);
        x.fill();
        x.fillStyle = on ? "#18181c" : "#9a9aa4";
        x.font = "700 9px " + FONT;
        x.fillText(t, X + (t === "M" ? 4 : 5), y + 35);
      }
    }
    x.fillStyle = "#3a3a40";
    x.fillRect(lw - 1, rh, 1, 4 * PL.row);

    x.save();
    x.beginPath();
    x.rect(lw, rh, W - lw, 4 * PL.row);
    x.clip();
    for (const c of song.clips) drawClip(x, c, p);
    x.restore();

    const ph = lw + st.pos * p;
    x.fillStyle = C.playhead;
    x.fillRect(Math.round(ph), rh - 4, 1.5, 4 * PL.row + 4);
    x.beginPath();
    x.moveTo(ph - 5, rh - 8);
    x.lineTo(ph + 6, rh - 8);
    x.lineTo(ph + 0.5, rh - 1);
    x.fill();
  }

  function drawClip(x, c, p) {
    const g = gen(c.g), pt = patOf(g, c.p);
    if (!pt) return;
    const X = PL.lw + c.s * p, w = c.l * p, y = PL.rh + c.g * PL.row + 2, h = PL.row - 5;
    const sel = c.id === st.selClip;
    x.fillStyle = sel ? mix(g.color, "#ffffff", 0.3) : g.color;
    rr(x, X + 0.5, y, w - 1, h, 3);
    x.fill();
    if (sel) {
      x.strokeStyle = "#ffffff";
      x.lineWidth = 1.5;
      x.stroke();
    }

    // the pattern's notes, repeated as the clip loops it
    const notes = pt.notes.filter((n) => n.s < pt.len);
    if (notes.length) {
      let lo = 127, hi = 0;
      for (const n of notes) { lo = Math.min(lo, n.p); hi = Math.max(hi, n.p); }
      const top = y + 19, span = h - 24, range = Math.max(1, hi - lo);
      x.fillStyle = "rgba(20,20,24,0.42)";
      for (let k = 0; k * pt.len < c.l; k++) {
        if (k > 0) x.fillRect(Math.round(X + k * pt.len * p), y + 16, 1, h - 18);
        for (const n of notes) {
          const s = k * pt.len + n.s;
          if (s >= c.l) continue;
          const e = Math.min(c.l, s + n.l, (k + 1) * pt.len);
          const ny = top + (hi === lo ? span / 2 : (1 - (n.p - lo) / range) * span);
          x.fillRect(X + s * p + 1, Math.round(ny), Math.max(1.5, (e - s) * p - 1.5), 2);
        }
      }
    }

    x.font = "12px " + FONT;
    x.fillStyle = "rgba(22,22,26,0.92)";
    x.fillText(fit(x, pt.name, w - (w > 34 ? 24 : 6)), X + 5, y + 14);
    if (w > 34) {
      const bx = X + w - 16;
      x.fillStyle = "rgba(0,0,0,0.28)";
      rr(x, bx, y + 3, 13, 13, 2);
      x.fill();
      x.strokeStyle = "rgba(22,22,26,0.95)";
      x.lineWidth = 1.4;
      x.beginPath();
      x.moveTo(bx + 3.5, y + 8);
      x.lineTo(bx + 6.5, y + 11);
      x.lineTo(bx + 9.5, y + 8);
      x.stroke();
    }
    x.fillStyle = "rgba(0,0,0,0.35)";
    x.fillRect(X + w - 4, y + h - 12, 2, 9);
  }

  function plHit(mx, my) {
    if (my < PL.rh) return { zone: "ruler" };
    const r = Math.floor((my - PL.rh) / PL.row);
    if (r < 0 || r > 3) return { zone: "none" };
    const y = PL.rh + r * PL.row;
    if (mx < PL.lw) {
      if (my >= y + 24 && my <= y + 40) {
        if (mx >= 8 && mx < 28) return { zone: "mute", r };
        if (mx >= 28 && mx < 48) return { zone: "solo", r };
      }
      return { zone: "label", r };
    }
    const p = plPpb(), beat = (mx - PL.lw) / p;
    for (let i = song.clips.length - 1; i >= 0; i--) {
      const c = song.clips[i];
      if (c.g !== r) continue;
      const X0 = PL.lw + c.s * p, X1 = X0 + c.l * p;
      if (mx >= X0 && mx < X1) {
        if (X1 - X0 > 34 && mx >= X1 - 17 && my < y + 19) return { zone: "chev", r, c };
        if (mx >= X1 - 6) return { zone: "edge", r, c };
        return { zone: "clip", r, c, beat };
      }
    }
    return { zone: "empty", r, beat };
  }

  function selectGen(g, pid) {
    st.editG = g;
    if (pid) gen(g).cur = pid;
    st.selNotes.clear();
    ensureTop();
    renderHeader();
    draw();
  }
  function selectClip(c) {
    st.selClip = c.id;
    selectGen(c.g, c.p);
  }
  function removeClip(c) {
    song.clips = song.clips.filter((k) => k !== c);
    if (st.selClip === c.id) st.selClip = null;
  }
  function paintAt(r, s) {
    const g = gen(r), pt = patOf(g, g.cur);
    if (!pt || s < 0 || s + pt.len > PL.bars * 4) return;
    if (song.clips.some((c) => c.g === r && c.s < s + pt.len && s < c.s + c.l)) return;
    const c = { id: nid(), g: r, p: pt.id, s, l: pt.len };
    song.clips.push(c);
    st.selClip = c.id;
  }

  let drag = null;
  let lpTimer = null;
  const startLP = (fn) => { cancelLP(); lpTimer = setTimeout(() => { lpTimer = null; fn(); draw(); }, 480); };
  const cancelLP = () => { clearTimeout(lpTimer); lpTimer = null; };
  const noSnapKey = (e) => (isMac ? e.ctrlKey : e.altKey);
  const dupKey = (e) => (isMac ? e.metaKey || e.altKey : e.ctrlKey);

  function focusMock(area) {
    if (document.activeElement !== root) root.focus({ preventScroll: true });
    if (st.focus !== area) { st.focus = area; renderHelp(); }
  }

  plC.addEventListener("pointerdown", (e) => {
    focusMock("pl");
    closeMenu();
    const { mx, my } = local(e, plC);
    const h = plHit(mx, my);
    const right = e.button === 2;
    if (h.zone === "none") return;
    if (h.zone === "ruler") {
      seek((mx - PL.lw) / plPpb());
      drag = { type: "seek" };
      plC.setPointerCapture(e.pointerId);
      return;
    }
    if (h.zone === "mute" || h.zone === "solo") {
      const g = gen(h.r);
      g[h.zone] = !g[h.zone];
      draw();
      return;
    }
    if (h.zone === "label") { selectGen(h.r); return; }
    if (h.zone === "chev" && !right) { selectClip(h.c); openMenu(h.c, e); return; }

    begin();
    if (right) {
      if (h.c) removeClip(h.c);
      drag = { type: "erase" };
    } else if (h.c) {
      let c = h.c;
      if (dupKey(e)) {
        c = { ...c, id: nid() };
        song.clips.push(c);
      }
      selectClip(c);
      drag = { type: h.zone === "edge" ? "cresize" : "cmove", c, x0: mx, s0: c.s, l0: c.l, moved: false };
      if (e.pointerType === "touch" && h.zone !== "edge") startLP(() => { removeClip(c); drag = null; commit(); });
    } else {
      drag = { type: "paint", r: h.r };
      paintAt(h.r, Math.floor(h.beat / 4) * 4);
      const c = clipById(st.selClip);
      if (c && c.g === h.r) selectGen(h.r);
    }
    plC.setPointerCapture(e.pointerId);
    draw();
  });

  plC.addEventListener("pointermove", (e) => {
    const { mx, my } = local(e, plC);
    if (!drag) {
      const h = plHit(mx, my);
      plC.style.cursor = { edge: "ew-resize", chev: "pointer", mute: "pointer", solo: "pointer", label: "pointer",
        ruler: "pointer", clip: "grab", empty: "crosshair" }[h.zone] || "default";
      return;
    }
    const p = plPpb();
    if (drag.type === "seek") { seek((mx - PL.lw) / p); return; }
    if (drag.type === "erase") {
      const h = plHit(mx, my);
      if (h.c) { removeClip(h.c); draw(); }
      return;
    }
    if (drag.type === "paint") {
      const beat = (mx - PL.lw) / p;
      paintAt(drag.r, Math.floor(beat / 4) * 4);
      draw();
      return;
    }
    const dx = (mx - drag.x0) / p;
    if (Math.abs(mx - drag.x0) > 4) { drag.moved = true; cancelLP(); }
    const c = drag.c;
    if (drag.type === "cmove") {
      const snap = noSnapKey(e) ? 1 : 4;
      c.s = clamp(Math.round((drag.s0 + dx) / snap) * snap, 0, PL.bars * 4 - c.l);
    } else {
      c.l = clamp(Math.round(drag.s0 + drag.l0 + dx) - c.s, 1, PL.bars * 4 - c.s);
    }
    draw();
  });

  const endDrag = () => {
    cancelLP();
    if (drag && drag.type !== "seek") commit();
    drag = null;
    draw();
  };
  plC.addEventListener("pointerup", endDrag);
  plC.addEventListener("pointercancel", endDrag);
  plC.addEventListener("contextmenu", (e) => e.preventDefault());

  // the pattern picker on a clip's chevron
  function openMenu(c, e) {
    const g = gen(c.g);
    menu.innerHTML = "";
    for (const pt of g.patterns) {
      const b = document.createElement("button");
      b.type = "button";
      b.textContent = pt.name;
      if (pt.id === c.p) b.className = "cur";
      b.addEventListener("click", () => {
        begin();
        c.p = pt.id;
        commit();
        selectClip(c);
        closeMenu();
      });
      menu.appendChild(b);
    }
    const rb = root.getBoundingClientRect();
    menu.hidden = false;
    const left = clamp(e.clientX - rb.left - 8, 4, rb.width - menu.offsetWidth - 4);
    menu.style.left = left + "px";
    menu.style.top = (e.clientY - rb.top + 10) + "px";
  }
  function closeMenu() { menu.hidden = true; }
  document.addEventListener("pointerdown", (e) => { if (!menu.hidden && !menu.contains(e.target)) closeMenu(); }, true);

  // ---------------------------------------------------------------- piano roll

  function ensureTop() {
    const g = gen(st.editG), pt = editPat();
    if (g.kind === "drums" || !pt) return;
    const rows = Math.floor(PR.gridH / 12);
    const top = st.top[st.editG];
    const visible = (t) => pt.notes.some((n) => n.p <= t && n.p > t - rows);
    if (top !== undefined && (visible(top) || !pt.notes.length)) return;
    if (!pt.notes.length) { st.top[st.editG] = top !== undefined ? top : 84; return; }
    let lo = 127, hi = 0;
    for (const n of pt.notes) { lo = Math.min(lo, n.p); hi = Math.max(hi, n.p); }
    st.top[st.editG] = clamp(Math.round((lo + hi) / 2 + rows / 2), rows - 1, 127);
  }

  function prGeom() {
    const g = gen(st.editG), pt = editPat(), drums = g.kind === "drums";
    const rowH = drums ? 18 : 12;
    const rows = Math.floor(PR.gridH / rowH);
    const top = drums ? 50 : st.top[st.editG];
    const view = pt.len * 1.0625;
    return { g, pt, drums, rowH, rows, top, p: (W - PR.kw) / view };
  }

  function drawPR() {
    const x = prX, G = prGeom(), { pt, rowH, rows, top, p, drums } = G;
    const kw = PR.kw, rh = PR.rh, gy = rh, gh = PR.gridH;
    x.fillStyle = C.bg;
    x.fillRect(0, 0, W, PR.h);

    // ruler
    x.fillStyle = "#2a2a2f";
    x.fillRect(0, 0, W, rh);
    x.font = "11px " + FONT;
    for (let b = 0; b * 4 <= pt.len * 1.07; b++) {
      const X = kw + b * 4 * p;
      x.fillStyle = "#5c5c66";
      x.fillRect(Math.round(X), 3, 1, rh - 3);
      x.fillStyle = "#a0a0aa";
      x.fillText(String(b + 1), X + 4, 14);
    }

    // rows and keys
    x.font = "10px " + FONT;
    for (let i = 0; i < rows; i++) {
      const pitch = top - i, y = gy + i * rowH;
      const named = DRUM[pitch];
      x.fillStyle = drums ? (named ? C.rowW : C.rowB) : (isBlack(pitch) ? C.rowB : C.rowW);
      x.fillRect(kw, y, W - kw, rowH);
      x.fillStyle = "rgba(0,0,0,0.22)";
      x.fillRect(kw, y + rowH - 1, W - kw, 1);
      if (drums) {
        x.fillStyle = named ? C.keyW : "#34343a";
        x.fillRect(0, y, kw - 1, rowH - 1);
        if (named) {
          x.fillStyle = "#2a2a2e";
          x.fillText(fit(x, named, kw - 8), 5, y + rowH - 5);
        }
      } else {
        x.fillStyle = isBlack(pitch) ? C.keyB : C.keyW;
        x.fillRect(0, y, kw - 1, rowH - 1);
        if (pitch % 12 === 0) {
          x.fillStyle = "#707078";
          x.fillText("C" + (pitch / 12 - 1), kw - 22, y + rowH - 2);
          x.fillStyle = "#3a3a40";
          x.fillRect(kw, y + rowH - 1, W - kw, 1);
        }
      }
    }

    // vertical grid
    for (let s = 0; s * GRID <= pt.len * 1.07; s++) {
      const b = s * GRID;
      x.fillStyle = s % 16 === 0 ? "#3d3d44" : s % 4 === 0 ? "#303036" : "#28282d";
      x.fillRect(Math.round(kw + b * p), gy, 1, gh);
    }
    x.fillStyle = "rgba(0,0,0,0.32)";
    x.fillRect(kw + pt.len * p, gy, W, gh + PR.velH);
    x.fillStyle = "#55555e";
    x.fillRect(Math.round(kw + pt.len * p), gy, 1, gh);

    // notes
    x.save();
    x.beginPath();
    x.rect(kw, gy, W - kw, gh);
    x.clip();
    for (const n of pt.notes) {
      const i = top - n.p;
      if (i < 0 || i >= rows) continue;
      const X = kw + n.s * p, w = Math.max(4, n.l * p - 1), y = gy + i * rowH + 1;
      const sel = st.selNotes.has(n);
      x.fillStyle = sel ? C.noteSel : C.note;
      rr(x, X + 0.5, y, w, rowH - 2, 2);
      x.fill();
      x.strokeStyle = sel ? "#ffffff" : C.noteLine;
      x.lineWidth = 1;
      x.stroke();
    }
    if (st.band) {
      const b = st.band;
      x.fillStyle = "rgba(224,138,60,0.12)";
      x.strokeStyle = "rgba(224,138,60,0.8)";
      x.fillRect(Math.min(b.x0, b.x1), Math.min(b.y0, b.y1), Math.abs(b.x1 - b.x0), Math.abs(b.y1 - b.y0));
      x.strokeRect(Math.min(b.x0, b.x1) + 0.5, Math.min(b.y0, b.y1) + 0.5, Math.abs(b.x1 - b.x0), Math.abs(b.y1 - b.y0));
    }
    x.restore();

    // velocity lane
    const vy = gy + gh;
    x.fillStyle = C.velBg;
    x.fillRect(0, vy, W, PR.velH);
    x.fillStyle = "rgba(0,0,0,0.32)";
    x.fillRect(kw + pt.len * p, vy, W, PR.velH);
    x.fillStyle = "#3a3a40";
    x.fillRect(0, vy, W, 1);
    x.fillStyle = "#8a8a94";
    x.font = "11px " + FONT;
    x.fillText("Vel", 14, vy + 34);
    const vh = PR.velH - 12;
    for (const n of pt.notes) {
      const X = kw + n.s * p, w = Math.max(3, Math.min(n.l * p - 2, drums ? 10 : 1e9));
      const bh = (n.v / 127) * vh;
      const sel = st.selNotes.has(n);
      x.fillStyle = sel ? C.velSel : C.vel;
      x.fillRect(X + 1, vy + PR.velH - 4 - bh, w, bh);
      x.fillStyle = "rgba(255,255,255,0.35)";
      x.fillRect(X + 1, vy + PR.velH - 4 - bh, w, 1.5);
    }

    // playhead, where a placement of this pattern is playing
    const lp = localPos(pt);
    if (lp !== null) {
      x.fillStyle = C.playhead;
      x.fillRect(Math.round(kw + lp * p), 0, 1.5, PR.h);
    }
  }

  function localPos(pt) {
    if (!st.playing && st.pos === 0) return null;
    for (const c of song.clips) {
      if (c.g === st.editG && c.p === pt.id && st.pos >= c.s && st.pos < c.s + c.l) return (st.pos - c.s) % pt.len;
    }
    return null;
  }

  function prAt(mx, my) {
    const G = prGeom();
    const beat = (mx - PR.kw) / G.p;
    const row = Math.floor((my - PR.rh) / G.rowH);
    return { G, beat, pitch: G.top - row };
  }
  function noteAt(mx, my) {
    const { G, pitch } = prAt(mx, my);
    const ns = G.pt.notes;
    for (let i = ns.length - 1; i >= 0; i--) {
      const n = ns[i];
      if (n.p !== pitch) continue;
      const X0 = PR.kw + n.s * G.p, X1 = X0 + Math.max(4, n.l * G.p - 1);
      if (mx >= X0 && mx <= X1 + 1) return { n, edge: X1 - X0 >= 12 ? mx >= X1 - 6 : mx >= X1 - 2 };
    }
    return null;
  }
  function removeNotes(list) {
    const pt = editPat(), kill = new Set(list);
    pt.notes = pt.notes.filter((n) => !kill.has(n));
    for (const n of list) st.selNotes.delete(n);
  }
  function setVel(mx, my) {
    const G = prGeom(), beat = (mx - PR.kw) / G.p;
    const vy = PR.rh + PR.gridH, v = clamp(Math.round(((vy + PR.velH - 4 - my) / (PR.velH - 12)) * 127), 1, 127);
    const pool = st.selNotes.size ? [...st.selNotes] : G.pt.notes;
    for (const n of pool) {
      const end = n.s + Math.max(n.l, 6 / G.p);
      if (beat >= n.s && beat < end) n.v = v;
    }
  }

  prC.addEventListener("pointerdown", (e) => {
    focusMock("pr");
    closeMenu();
    const { mx, my } = local(e, prC);
    const G = prGeom();
    const right = e.button === 2;
    if (my < PR.rh) return;
    if (my >= PR.rh + PR.gridH) {
      begin();
      drag = { type: "vel" };
      setVel(mx, my);
      prC.setPointerCapture(e.pointerId);
      draw();
      return;
    }
    const { beat, pitch } = prAt(mx, my);
    if (mx < PR.kw) {
      if (!G.drums || DRUM[pitch]) audition(pitch, 100);
      drag = { type: "keys", last: pitch };
      prC.setPointerCapture(e.pointerId);
      return;
    }
    begin();
    const hit = noteAt(mx, my);
    if (right) {
      if (hit) removeNotes([hit.n]);
      drag = { type: "erase" };
    } else if (e.shiftKey) {
      st.band = { x0: mx, y0: my, x1: mx, y1: my, base: new Set(st.selNotes) };
      drag = { type: "band" };
    } else if (hit) {
      let lead = hit.n;
      if (!st.selNotes.has(lead)) st.selNotes = new Set([lead]);
      let items = [...st.selNotes];
      if (hit.edge) {
        drag = { type: "resize", x0: mx, lead, items: items.map((n) => ({ n, l: n.l })) };
      } else {
        if (dupKey(e)) {
          const copies = items.map((n) => ({ ...n }));
          lead = copies[items.indexOf(lead)];
          G.pt.notes.push(...copies);
          st.selNotes = new Set(copies);
          items = copies;
        }
        drag = { type: "move", x0: mx, y0: my, lead, lastP: lead.p, items: items.map((n) => ({ n, s: n.s, p: n.p })), moved: false };
        audition(lead.p, lead.v);
        if (e.pointerType === "touch") {
          const victim = lead;
          startLP(() => { removeNotes([...st.selNotes].includes(victim) ? [...st.selNotes] : [victim]); drag = null; commit(); });
        }
      }
    } else {
      if (beat < 0 || beat >= G.pt.len || (G.drums && !DRUM[pitch])) { pending = null; return; }
      const s = noSnapKey(e) ? Math.max(0, beat) : Math.floor(beat / GRID) * GRID;
      const l = Math.min(st.lastLen[G.g.kind], G.pt.len - s);
      const n = { s, l: Math.max(GRID / 4, l), p: pitch, v: 100 };
      G.pt.notes.push(n);
      st.selNotes = new Set([n]);
      drag = { type: "move", x0: mx, y0: my, lead: n, lastP: n.p, items: [{ n, s: n.s, p: n.p }], moved: false };
      audition(n.p, n.v);
    }
    prC.setPointerCapture(e.pointerId);
    draw();
  });

  prC.addEventListener("pointermove", (e) => {
    const { mx, my } = local(e, prC);
    if (!drag) {
      let cur = "default";
      if (my >= PR.rh + PR.gridH) cur = "ns-resize";
      else if (mx < PR.kw) cur = "pointer";
      else if (my >= PR.rh) {
        const hit = noteAt(mx, my);
        cur = hit ? (hit.edge ? "ew-resize" : "grab") : "crosshair";
      }
      prC.style.cursor = cur;
      return;
    }
    const G = prGeom();
    if (drag.type === "vel") { setVel(mx, my); draw(); return; }
    if (drag.type === "keys") {
      const { pitch } = prAt(mx, my);
      if (pitch !== drag.last && (!G.drums || DRUM[pitch])) { drag.last = pitch; audition(pitch, 100); }
      return;
    }
    if (drag.type === "erase") {
      const hit = noteAt(mx, my);
      if (hit) { removeNotes([hit.n]); draw(); }
      return;
    }
    if (drag.type === "band") {
      const b = st.band;
      b.x1 = mx; b.y1 = my;
      const b0 = (Math.min(b.x0, mx) - PR.kw) / G.p, b1 = (Math.max(b.x0, mx) - PR.kw) / G.p;
      const pHi = G.top - Math.floor((Math.min(b.y0, my) - PR.rh) / G.rowH);
      const pLo = G.top - Math.floor((Math.max(b.y0, my) - PR.rh) / G.rowH);
      st.selNotes = new Set(b.base);
      for (const n of G.pt.notes) if (n.p >= pLo && n.p <= pHi && n.s < b1 && n.s + n.l > b0) st.selNotes.add(n);
      draw();
      return;
    }
    const free = noSnapKey(e);
    const dxb = (mx - drag.x0) / G.p;
    if (Math.abs(mx - drag.x0) > 4 || Math.abs(my - (drag.y0 || my)) > 4) cancelLP();
    if (drag.type === "move") {
      let db = free ? dxb : Math.round(dxb / GRID) * GRID;
      let dp = Math.round(-(my - drag.y0) / G.rowH);
      let minS = Infinity, maxE = -Infinity, minP = 127, maxP = 0;
      for (const it of drag.items) {
        minS = Math.min(minS, it.s); maxE = Math.max(maxE, it.s + it.n.l);
        minP = Math.min(minP, it.p); maxP = Math.max(maxP, it.p);
      }
      db = clamp(db, -minS, G.pt.len - maxE);
      dp = clamp(dp, (G.drums ? 35 : 0) - minP, (G.drums ? 50 : 127) - maxP);
      for (const it of drag.items) { it.n.s = it.s + db; it.n.p = it.p + dp; }
      if (drag.lead.p !== drag.lastP) {
        drag.lastP = drag.lead.p;
        if (!G.drums || DRUM[drag.lead.p]) audition(drag.lead.p, drag.lead.v);
      }
    } else if (drag.type === "resize") {
      const dl = free ? dxb : Math.round(dxb / GRID) * GRID;
      for (const it of drag.items) {
        it.n.l = clamp(it.l + dl, free ? 0.05 : GRID, G.pt.len - it.n.s);
      }
      st.lastLen[G.g.kind] = drag.lead.l;
    }
    draw();
  });

  const endPR = () => {
    cancelLP();
    if (drag && drag.type === "resize") st.lastLen[gen(st.editG).kind] = drag.lead.l;
    if (drag && drag.type !== "keys") commit();
    drag = null;
    st.band = null;
    draw();
  };
  prC.addEventListener("pointerup", endPR);
  prC.addEventListener("pointercancel", endPR);
  prC.addEventListener("contextmenu", (e) => e.preventDefault());
  prC.addEventListener("wheel", (e) => {
    const G = prGeom();
    if (G.drums || document.activeElement !== root) return;
    e.preventDefault();
    const step = Math.sign(e.deltaY) * Math.max(1, Math.round(Math.abs(e.deltaY) / 30));
    st.top[st.editG] = clamp(st.top[st.editG] - step, G.rows - 1, 127);
    draw();
  }, { passive: false });

  // ---------------------------------------------------------------- header

  const patsEl = $(".dm-pats"), lvEl = $(".dm-lv");
  function renderHeader() {
    const g = gen(st.editG), pt = editPat();
    $(".dm-prname i").style.background = g.color;
    $(".dm-prname span").textContent = g.name;
    crumbEl.textContent = `${g.name} / ${pt.name}`;
    patsEl.innerHTML = "";
    for (const p of g.patterns) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "dm-btn dm-tab" + (p.id === g.cur ? " on" : "");
      b.textContent = p.name;
      b.addEventListener("click", () => { selectGen(st.editG, p.id); focusMock("pr"); });
      patsEl.appendChild(b);
    }
    lvEl.textContent = String(pt.len / 4);
  }
  $(".dm-new").addEventListener("click", () => {
    begin();
    const g = gen(st.editG);
    const p = mkPat(`${g.name} ${g.patterns.length + 1}`, 4, []);
    g.patterns.push(p);
    commit();
    selectGen(st.editG, p.id);
    focusMock("pr");
  });
  $(".dm-clone").addEventListener("click", () => {
    begin();
    const g = gen(st.editG), src = editPat();
    const p = { id: nid(), name: src.name + " 2", len: src.len, notes: src.notes.map((n) => ({ ...n })) };
    g.patterns.push(p);
    commit();
    selectGen(st.editG, p.id);
    focusMock("pr");
  });
  const setLen = (d) => {
    begin();
    const pt = editPat();
    pt.len = clamp(pt.len + d * 4, 4, 32);
    commit();
    renderHeader();
    draw();
  };
  $(".dm-lm").addEventListener("click", () => setLen(-1));
  $(".dm-lp").addEventListener("click", () => setLen(1));

  // ---------------------------------------------------------------- help bar

  const TEXT = {
    en: {
      paint: "paint pattern", move: "move", dup: "duplicate", clipLen: "clip length", remove: "remove",
      editPat: "edit pattern", pickPat: "switch pattern", playStop: "play/stop", undo: "undo", addNote: "add note",
      resize: "resize", select: "select", velocity: "velocity", noSnap: "ignore snap", scroll: "scroll", transpose: "transpose"
    },
    ja: {
      paint: "パターンを描く", move: "移動", dup: "複製", clipLen: "クリップの長さ", remove: "削除",
      editPat: "パターンを編集", pickPat: "パターン切り替え", playStop: "再生 / 停止", undo: "元に戻す", addNote: "ノートを追加",
      resize: "長さ", select: "範囲選択", velocity: "ベロシティ", noSnap: "スナップ無視", scroll: "スクロール", transpose: "移調"
    }
  };
  function renderHelp() {
    const lang = (document.documentElement.lang || "en").startsWith("ja") ? "ja" : "en";
    const t = TEXT[lang];
    const del = coarse ? "long press" : "right-click";
    const items = st.focus === "pr"
      ? [[coarse ? "tap" : "click", "addNote"], ["drag note", "move"], [MOD + "+drag note", "dup"], ["drag edge", "resize"],
        ["Shift+drag", "select"], [del, "remove"], ["drag lane", "velocity"], [isMac ? "Ctrl" : "Alt", "noSnap"],
        ["↑ ↓", "transpose"], ["wheel", "scroll"], ["Space", "playStop"], [MOD + "+Z", "undo"]]
      : [["drag", "paint"], ["drag clip", "move"], [MOD + "+drag clip", "dup"], ["drag clip edge", "clipLen"],
        [del, "remove"], [coarse ? "tap clip" : "click clip", "editPat"], ["▾", "pickPat"], ["Space", "playStop"], [MOD + "+Z", "undo"]];
    const shown = coarse ? items.filter(([k]) => !/Space|Shift|Cmd|Ctrl|Alt|wheel|↑/.test(k)) : items;
    helpEl.innerHTML = shown.map(([k, v]) => `<span><b>${k}</b>${t[v]}</span>`).join("");
  }
  new MutationObserver(renderHelp).observe(document.documentElement, { attributes: true, attributeFilter: ["lang"] });

  // ---------------------------------------------------------------- keyboard

  document.addEventListener("keydown", (e) => {
    if (!root.contains(document.activeElement)) return;
    const mod = e.metaKey || e.ctrlKey;
    if (e.code === "Space") {
      e.preventDefault();
      togglePlay();
    } else if (e.key === "Delete" || e.key === "Backspace") {
      e.preventDefault();
      begin();
      if (st.focus === "pr" && st.selNotes.size) removeNotes([...st.selNotes]);
      else if (st.focus === "pl" && st.selClip) removeClip(clipById(st.selClip));
      commit();
      draw();
    } else if (mod && e.key.toLowerCase() === "z") {
      e.preventDefault();
      if (e.shiftKey) redo(); else undo();
    } else if (mod && e.key.toLowerCase() === "a" && st.focus === "pr") {
      e.preventDefault();
      st.selNotes = new Set(editPat().notes);
      draw();
    } else if ((e.key === "ArrowUp" || e.key === "ArrowDown") && st.focus === "pr" && st.selNotes.size) {
      e.preventDefault();
      const d = (e.key === "ArrowUp" ? 1 : -1) * (e.shiftKey ? 12 : 1);
      const ns = [...st.selNotes];
      if (ns.every((n) => n.p + d >= 0 && n.p + d <= 127)) {
        begin();
        for (const n of ns) n.p += d;
        commit();
        audition(ns[0].p, ns[0].v);
        draw();
      }
    } else if (e.key === "Escape") {
      closeMenu();
    }
  });

  // ---------------------------------------------------------------- audio

  let ac = null, master = null, noise = null, buses = [];
  function ensureAudio() {
    if (ac) {
      if (ac.state === "suspended") ac.resume();
      return;
    }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    ac = new AC();
    const comp = ac.createDynamicsCompressor();
    comp.threshold.value = -16;
    comp.ratio.value = 4;
    master = ac.createGain();
    master.gain.value = 0.75;
    master.connect(comp).connect(ac.destination);
    noise = ac.createBuffer(1, ac.sampleRate, ac.sampleRate);
    const d = noise.getChannelData(0);
    for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
    const vols = [0.9, 0.85, 0.8, 0.6], pans = [0, 0, -0.2, 0.2];
    buses = vols.map((v, i) => {
      const g = ac.createGain();
      g.gain.value = v;
      if (ac.createStereoPanner) {
        const pn = ac.createStereoPanner();
        pn.pan.value = pans[i];
        g.connect(pn).connect(master);
      } else {
        g.connect(master);
      }
      return g;
    });
    // the Lead's delay, as on the mixer screenshot
    const dl = ac.createDelay(1), fb = ac.createGain(), wet = ac.createGain();
    dl.delayTime.value = 0.75 * SPB;
    fb.gain.value = 0.3;
    wet.gain.value = 0.22;
    buses[3].connect(dl);
    dl.connect(fb).connect(dl);
    dl.connect(wet).connect(master);
  }

  const osc = (type, f, t, end, detune = 0) => {
    const o = ac.createOscillator();
    o.type = type;
    o.frequency.setValueAtTime(f, t);
    o.detune.value = detune;
    o.start(t);
    o.stop(end);
    return o;
  };
  const noiseSrc = (t, end) => {
    const s = ac.createBufferSource();
    s.buffer = noise;
    s.loop = true;
    s.start(t, Math.random() * 0.5);
    s.stop(end);
    return s;
  };
  const perc = (t, peak, decay) => {
    const g = ac.createGain();
    g.gain.setValueAtTime(Math.max(0.0005, peak), t);
    g.gain.exponentialRampToValueAtTime(0.0005, t + decay);
    return g;
  };
  const vca = (t, peak, atk, hold, rel) => {
    const g = ac.createGain();
    const h = Math.max(atk, hold);
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(peak, t + atk);
    g.gain.setValueAtTime(peak, t + h);
    g.gain.setTargetAtTime(0, t + h, rel);
    return g;
  };
  const filter = (type, f, q) => {
    const b = ac.createBiquadFilter();
    b.type = type;
    b.frequency.value = f;
    b.Q.value = q;
    return b;
  };

  function drum(p, t, a, out) {
    if (p === 36) {
      const o = osc("sine", 160, t, t + 0.5);
      o.frequency.exponentialRampToValueAtTime(46, t + 0.11);
      o.connect(perc(t, a * 1.05, 0.42)).connect(out);
      noiseSrc(t, t + 0.02).connect(filter("highpass", 1500, 0.7)).connect(perc(t, a * 0.12, 0.012)).connect(out);
    } else if (p === 39) {
      const bp = filter("bandpass", 1150, 0.9);
      bp.connect(out);
      for (const off of [0, 0.011, 0.022]) noiseSrc(t + off, t + off + 0.03).connect(perc(t + off, a * 0.55, 0.012)).connect(bp);
      noiseSrc(t + 0.03, t + 0.3).connect(perc(t + 0.03, a * 0.45, 0.2)).connect(bp);
    } else if (p === 38) {
      noiseSrc(t, t + 0.25).connect(filter("bandpass", 1800, 0.6)).connect(perc(t, a * 0.5, 0.18)).connect(out);
      osc("triangle", 185, t, t + 0.15).connect(perc(t, a * 0.35, 0.1)).connect(out);
    } else if (p === 46) {
      noiseSrc(t, t + 0.45).connect(filter("highpass", 7000, 0.7)).connect(perc(t, a * 0.32, 0.34)).connect(out);
    } else if (p === 37) {
      osc("square", 1650, t, t + 0.05).connect(filter("bandpass", 1650, 2)).connect(perc(t, a * 0.3, 0.03)).connect(out);
    } else if (p === 41 || p === 48) {
      const f = p === 41 ? 110 : 175;
      const o = osc("sine", f, t, t + 0.4);
      o.frequency.exponentialRampToValueAtTime(f * 0.62, t + 0.3);
      o.connect(perc(t, a * 0.6, 0.32)).connect(out);
    } else {
      noiseSrc(t, t + 0.08).connect(filter("highpass", 7500, 0.7)).connect(perc(t, a * 0.34, 0.045)).connect(out);
    }
  }

  function voice(gi, p, t, dur, vel) {
    if (!ac) return;
    const g = song.gens[gi], out = buses[gi], a = vel / 127, f = mtof(p), end = t + dur;
    if (g.kind === "drums") {
      drum(p, t, a, out);
    } else if (g.kind === "bass") {
      const lp = filter("lowpass", 220, 5);
      lp.frequency.setValueAtTime(220, t);
      lp.frequency.exponentialRampToValueAtTime(1500, t + 0.008);
      lp.frequency.exponentialRampToValueAtTime(320, t + 0.22);
      const amp = vca(t, 0.32 * a, 0.004, dur, 0.035);
      osc("sawtooth", f, t, end + 0.3).connect(lp);
      const sub = ac.createGain();
      sub.gain.value = 0.55;
      osc("sine", f, t, end + 0.3).connect(sub).connect(amp);
      lp.connect(amp).connect(out);
    } else if (g.kind === "pad") {
      const lp = filter("lowpass", 1700, 0.5);
      const amp = vca(t, 0.055 * a, 0.06, dur, 0.22);
      for (const d of [-8, 8]) osc("sawtooth", f, t, end + 1.4, d).connect(lp);
      lp.connect(amp).connect(out);
    } else {
      const lp = filter("lowpass", 4200, 2);
      lp.frequency.setValueAtTime(4200, t);
      lp.frequency.exponentialRampToValueAtTime(2100, t + 0.3);
      const amp = vca(t, 0.12 * a, 0.006, dur, 0.07);
      osc("sawtooth", f, t, end + 0.6).connect(lp);
      osc("square", f, t, end + 0.6, 6).connect(lp);
      lp.connect(amp).connect(out);
    }
  }

  function audition(p, v) {
    ensureAudio();
    if (!ac) return;
    const g = gen(st.editG);
    voice(st.editG, p, ac.currentTime + 0.005, g.kind === "drums" ? 0.2 : 0.28, v);
  }

  // ---------------------------------------------------------------- transport

  let playT0 = 0, playB0 = 0, nextB = 0, timer = null, raf = 0;
  const beatAt = (time) => playB0 + (time - playT0) / SPB;

  function scheduleRange(x0, x1, off) {
    const solo = song.gens.some((g) => g.solo);
    for (const c of song.clips) {
      const g = song.gens[c.g];
      if (g.mute || (solo && !g.solo)) continue;
      const pt = patOf(g, c.p);
      if (!pt || !pt.len || c.s >= x1 || c.s + c.l <= x0) continue;
      for (let k = 0; k * pt.len < c.l; k++) {
        for (const n of pt.notes) {
          if (n.s >= pt.len) continue;
          const rel = k * pt.len + n.s;
          if (rel >= c.l) continue;
          const b = c.s + rel;
          if (b < x0 || b >= x1) continue;
          const t = playT0 + (b + off - playB0) * SPB;
          const dur = Math.min(n.l, c.l - rel, pt.len - n.s) * SPB;
          voice(c.g, n.p, Math.max(t, ac.currentTime), dur, n.v);
        }
      }
    }
  }
  function tick() {
    const end = beatAt(ac.currentTime + 0.12);
    const L = songLen();
    let a = nextB;
    while (a < end) {
      const loop = Math.floor(a / L);
      const segEnd = Math.min(end, (loop + 1) * L);
      scheduleRange(a - loop * L, segEnd - loop * L, loop * L);
      a = segEnd;
    }
    nextB = end;
  }
  function frame() {
    if (!st.playing) return;
    st.pos = ((beatAt(ac.currentTime) % songLen()) + songLen()) % songLen();
    drawNow();
    raf = requestAnimationFrame(frame);
  }
  function play() {
    ensureAudio();
    if (!ac) return;
    st.playing = true;
    playB0 = st.pos;
    playT0 = ac.currentTime + 0.06;
    nextB = playB0;
    tick();
    timer = setInterval(tick, 25);
    playBtn.classList.add("on");
    raf = requestAnimationFrame(frame);
  }
  function stop() {
    if (st.playing) {
      st.playing = false;
      clearInterval(timer);
      cancelAnimationFrame(raf);
      st.pos = playB0;
      if (master) {
        // cut what was already scheduled ahead
        master.gain.setTargetAtTime(0, ac.currentTime, 0.02);
        master = rewire(master);
      }
    } else {
      st.pos = 0;
    }
    playBtn.classList.remove("on");
    drawNow();
  }
  // Swap the master gain for a new one so voices scheduled ahead stay silent.
  function rewire(old) {
    const fresh = ac.createGain();
    fresh.gain.value = 0.75;
    const comp = ac.createDynamicsCompressor();
    comp.threshold.value = -16;
    comp.ratio.value = 4;
    fresh.connect(comp).connect(ac.destination);
    for (const b of buses) {
      try { b.disconnect(); } catch (_) {}
    }
    const pans = [0, 0, -0.2, 0.2];
    buses.forEach((b, i) => {
      if (ac.createStereoPanner) {
        const pn = ac.createStereoPanner();
        pn.pan.value = pans[i];
        b.connect(pn).connect(fresh);
      } else {
        b.connect(fresh);
      }
    });
    const dl = ac.createDelay(1), fb = ac.createGain(), wet = ac.createGain();
    dl.delayTime.value = 0.75 * SPB;
    fb.gain.value = 0.3;
    wet.gain.value = 0.22;
    buses[3].connect(dl);
    dl.connect(fb).connect(dl);
    dl.connect(wet).connect(fresh);
    setTimeout(() => { try { old.disconnect(); } catch (_) {} }, 300);
    return fresh;
  }
  function togglePlay() { if (st.playing) stop(); else play(); }
  function seek(b) {
    const L = songLen();
    const pos = clamp(Math.round(b / GRID) * GRID, 0, L - GRID);
    if (st.playing) {
      stop();
      st.pos = pos;
      play();
    } else {
      st.pos = pos;
      drawNow();
    }
  }
  function updatePos() {
    const bar = Math.floor(st.pos / 4) + 1, beat = Math.floor(st.pos % 4) + 1;
    posEl.textContent = String(bar).padStart(3, "0") + "." + beat;
  }

  playBtn.addEventListener("click", () => { focusMock(st.focus); if (!st.playing) play(); });
  stopBtn.addEventListener("click", () => { focusMock(st.focus); stop(); });
  document.addEventListener("visibilitychange", () => { if (document.hidden && st.playing) stop(); });

  // ---------------------------------------------------------------- go

  ensureTop();
  renderHeader();
  renderHelp();
  new ResizeObserver(resize).observe(root);
  resize();
})();
