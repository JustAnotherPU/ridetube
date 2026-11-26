'use strict';
/**
 * frameLoop: ctx.loop (mgu.js), also run by kit/tools/hu-stub.js for the emulator. fn runs while screen
 * `stateName` is visible: once per screen refresh (up to 60 fps) when the GPU renderer shows it, else about
 * `fps` times a second.
 *
 * A page that does not change still costs a whole draw in JS each time (display.js _send then drops the
 * frame as the same as the last one). After IDLE_AFTER such frames in a row the loop looks every IDLE_MS
 * instead of every 16 ms: a change of data shows up to IDLE_MS late unless the app kicks. It is back at
 * 16 ms at once on a frame that changed, on kick() (also from inside fn: an animation that kicks each frame
 * stays fast), on the screen shown again, and on input the kit gets (display.wake: touch, controller, bar,
 * sheets, scrolling, layout).
 */
const IDLE_AFTER = 30;   // ~0.5 s of unchanged frames at 60 fps
const IDLE_MS = 100;

function frameLoop(ui, display, log, stateName, fps, fn) {
    let timer = null, slow = false, on = false, running = false, frames = 0, spent = 0, idle = 0, seen = -1, statT = Date.now();
    const gpuOn = () => display.active(stateName);
    const tick = async () => {
        timer = null; slow = false;
        if (!on || running) return;
        running = true;
        const t0 = Date.now();
        try { await fn(); } catch (e) { log.error('loop ' + stateName, e); }
        running = false;
        const dt = Date.now() - t0;
        frames++; spent += dt;
        // display.changes counts the frames sent that differ from the one before (a tick that sends none: unchanged)
        if (display.changes !== seen) { seen = display.changes; idle = 0; } else idle++;
        // software frames allocate image and PNG buffers; Node 8 frees them lazily, which lets
        // memory float up to the platform's limit (the app is then killed). Collect regularly.
        if (!gpuOn() && frames % 3 === 0 && typeof global.gc === 'function') { try { global.gc(); } catch (e) { /* ignore */ } }
        if (frames % (gpuOn() ? 600 : 100) === 0) {
            const n = gpuOn() ? 600 : 100, tm = ui.timing || { png: 0, push: 0 }, png = Math.round(tm.png / n), push = Math.round(tm.push / n), all = (spent / n).toFixed(1);
            const rate = (n * 1000 / Math.max(1, Date.now() - statT)).toFixed(1);
            log.info('loop ' + stateName + ': ' + all + ' ms per frame in JS' + (gpuOn() ? ' (GPU renderer)' : ' (draw ' + Math.max(0, all - png - push) + ', png ' + png + ', hmi ' + push + '; asked for ' + fps + ' fps)') + ', ' + rate + ' frames/s');
            spent = 0; tm.png = 0; tm.push = 0; statT = Date.now();
        }
        schedule(dt);
    };
    const schedule = (dt) => {
        if (!on || timer !== null || running) return;
        if (gpuOn()) {
            // the renderer sends a tick when it can take the next frame; if this frame was
            // not sent (unchanged), look again shortly (every IDLE_MS once the page has been still a while)
            // (a tick normally follows a sent frame; 250 ms is the safety net if none comes)
            slow = !display.gpu.waiting && idle >= IDLE_AFTER;
            timer = setTimeout(tick, display.gpu.waiting ? 250 : slow ? IDLE_MS : 16);
        } else timer = setTimeout(tick, Math.max(15, 1000 / fps - (dt || 0)));
    };
    display.onTick(() => {
        // a tick from the renderer, or the renderer went away (then back to the timer)
        if (!on || running) return;
        if (timer !== null) { clearTimeout(timer); timer = null; }
        if (gpuOn()) tick(); else schedule(0);
    });
    // input the kit got: fast again, and the next frame now if the loop was waiting IDLE_MS
    if (display.onWake) display.onWake(() => {
        idle = 0;
        if (slow && timer !== null) { clearTimeout(timer); timer = setTimeout(tick, 0); }
    });
    const set = (v) => {
        on = v;
        if (on) idle = 0;
        if (on && timer === null && !running) timer = setTimeout(tick, 0);
        if (!on && timer !== null) { clearTimeout(timer); timer = null; }
    };
    ui.onVisible((name, v) => { if (name === stateName) set(v); });
    if (ui.isVisible(stateName)) set(true);
    return {
        stop: () => set(false),
        kick: () => { idle = 0; if (on && !running) { if (timer !== null) clearTimeout(timer); timer = setTimeout(tick, 0); } },
    };
}

module.exports = { frameLoop, IDLE_AFTER, IDLE_MS };
