'use strict';

// ctx.loop's pacing (kit/runtime/loop.js) on a fake clock: a still page slows down to IDLE_MS after IDLE_AFTER
// unchanged frames; a changing page, kick() (also from inside the draw: an animation), input (display.wake), the
// screen shown again and a renderer tick keep or bring it back to 16 ms at once. No renderer needed:
//   node kit/tools/test_loop.js

const assert = require('assert');

// the fake clock: setTimeout / clearTimeout / Date.now are this test's while it runs
let now = 0, seq = 0, timers = [];
const real = { setTimeout: global.setTimeout, clearTimeout: global.clearTimeout, now: Date.now };
global.setTimeout = (fn, ms) => { const t = { fn, at: now + Math.max(0, ms | 0), id: ++seq }; timers.push(t); return t; };
global.clearTimeout = (t) => { timers = timers.filter((x) => x !== t); };
Date.now = () => now;
const { frameLoop, IDLE_AFTER, IDLE_MS } = require('../runtime/loop');
const flush = () => new Promise((r) => setImmediate(r));          // (tick is async: its awaits)
async function advance(ms) {
    const end = now + ms;
    for (;;) {
        await flush();
        timers.sort((a, b) => a.at - b.at || a.id - b.id);
        const t = timers[0];
        if (!t || t.at > end) break;
        timers.shift();
        now = t.at;
        t.fn();
    }
    now = end;
    await flush();
}

function rig(draw) {
    const display = {
        changes: 0, gpu: { waiting: false }, ticks: [], wakes: [],
        active: () => true, onTick(fn) { this.ticks.push(fn); }, onWake(fn) { this.wakes.push(fn); },
        wake() { this.wakes.forEach((fn) => fn()); }, tick() { this.ticks.forEach((fn) => fn()); },
    };
    const vis = [];
    let shown = true;
    const ui = { timing: { png: 0, push: 0 }, onVisible: (fn) => vis.push(fn), isVisible: () => shown };
    const at = [];
    const log = { info() {}, error: (w, e) => { throw e; } };
    const r = { display, at, show(v) { shown = v; vis.forEach((fn) => fn('VIEW', v)); } };
    r.h = frameLoop(ui, display, log, 'VIEW', 10, () => { at.push(now); if (draw) return draw(r); });
    return r;
}
const gaps = (at) => at.slice(1).map((t, i) => t - at[i]);

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('a still page: 16 ms for IDLE_AFTER frames, then every IDLE_MS', async () => {
    const r = rig();
    await advance(2000);
    const g = gaps(r.at);
    assert.deepStrictEqual(g.slice(0, IDLE_AFTER), new Array(IDLE_AFTER).fill(16), 'fast at first');
    assert(g.slice(IDLE_AFTER).length > 10 && g.slice(IDLE_AFTER).every((x) => x === IDLE_MS), 'then slow: ' + g.slice(IDLE_AFTER));
    assert(r.at.length <= IDLE_AFTER + 1 + 16, r.at.length + ' frames in 2 s (125 before)');
});

test('a page that changes every frame stays at 16 ms', async () => {
    const r = rig((x) => { x.display.changes++; });
    await advance(2000);
    assert(gaps(r.at).every((x) => x === 16));
});

test('one change on a slow page: fast again from that frame on', async () => {
    const r = rig();
    await advance(1500);
    r.display.changes++;                                   // (data came: the next frame differs)
    const n = r.at.length;
    await advance(1000);
    const g = gaps(r.at.slice(n - 1));
    assert.strictEqual(g[0], IDLE_MS, 'the change is seen at the next slow look');
    assert(g.slice(1, 1 + IDLE_AFTER).every((x) => x === 16), 'then 16 ms: ' + g.slice(0, 35));
});

test('input (display.wake) on a slow page: the next frame at once, then 16 ms', async () => {
    const r = rig();
    await advance(1500);
    now += 30;                                            // between two slow looks
    const t = now;
    r.display.wake();
    await advance(0);
    assert.strictEqual(r.at[r.at.length - 1], t, 'drawn at once');
    const n = r.at.length;
    await advance(IDLE_AFTER * 16);
    assert(gaps(r.at.slice(n - 1)).every((x) => x === 16), 'fast for IDLE_AFTER frames');
    await advance(1000);
    assert.strictEqual(gaps(r.at).pop(), IDLE_MS, 'and slow again with nothing changing');
});

test('input while the page is fast: the 16 ms cadence goes on, no extra frames', async () => {
    const r = rig();
    await advance(100);
    for (let i = 0; i < 20; i++) { r.display.wake(); await advance(5); }
    assert(gaps(r.at).every((x) => x === 16), gaps(r.at).join(','));
});

test('kick() on a slow page: at once; kick() from inside the draw (an animation) keeps it at 16 ms', async () => {
    const r = rig();
    await advance(1500);
    const t = now;
    r.h.kick();
    await advance(0);
    assert.strictEqual(r.at[r.at.length - 1], t);
    let anim = true;
    const a = rig((x) => { if (anim) x.h.kick(); });
    await advance(2000);
    assert(gaps(a.at).every((x) => x === 16), 'an animation that kicks stays fast');
    anim = false;
    await advance(2000);
    assert.strictEqual(gaps(a.at).pop(), IDLE_MS, 'and slows down once it stops');
});

test('a kick while the draw is running is not lost', async () => {
    let release = null;
    const r = rig((x) => (x.at.length === 40 ? new Promise((res) => { release = res; }) : undefined));
    await advance(1500);                                  // frame 40 (slow by then) waits
    assert(release, 'the draw is running');
    r.h.kick();
    release();
    const n = r.at.length, t = now;
    await advance(200);
    assert.strictEqual(r.at[n] - t, 16, 'the frame after it comes 16 ms later, not IDLE_MS');
});

test('renderer ticks (the 1 s keepalive) do not end the slow mode', async () => {
    const r = rig();
    await advance(1500);
    r.display.tick();                                     // a tick after the keepalive resend
    await advance(0);
    const n = r.at.length;
    await advance(500);
    assert(gaps(r.at.slice(n - 1)).every((x) => x === IDLE_MS), gaps(r.at.slice(n - 1)).join(','));
});

test('shown again: 16 ms at once; hidden: no frames', async () => {
    const r = rig();
    await advance(1500);
    r.show(false);
    const n = r.at.length;
    await advance(1000);
    assert.strictEqual(r.at.length, n, 'nothing drawn while hidden');
    r.show(true);
    await advance(IDLE_AFTER * 16);
    assert(gaps(r.at.slice(n)).every((x) => x === 16), gaps(r.at.slice(n)).join(','));
});

test('a frame waiting for the renderer keeps the 250 ms safety net', async () => {
    const r = rig();
    await advance(1500);
    r.display.gpu.waiting = true;
    const n = r.at.length;
    await advance(1000);
    assert.strictEqual(gaps(r.at.slice(n - 1)).pop(), 250);
});

(async () => {
    let failed = 0;
    for (const t of tests) {
        timers = []; now = 0;
        try { await t.fn(); console.log('ok   ' + t.name); } catch (e) { failed++; console.log('FAIL ' + t.name + '\n     ' + (e && e.message)); }
    }
    global.setTimeout = real.setTimeout; global.clearTimeout = real.clearTimeout; Date.now = real.now;
    if (failed) { console.log('loop: ' + failed + ' failed'); process.exit(1); }
    console.log('loop: all ok');
})();
