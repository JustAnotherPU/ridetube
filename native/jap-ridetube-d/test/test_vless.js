'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const vless = require('../vless.js');

const fm = encodeURIComponent(JSON.stringify({
    tcp: [{ type: 'fragment', settings: { packets: 'tlshello', lengths: ['10-20'], delays: ['0'] } }]
}));
const uri = 'vless://11111111-2222-3333-4444-555555555555@example.invalid:443?encryption=none&security=reality&type=tcp&flow=xtls-rprx-vision&sni=192.0.2.1&fp=firefox&pbk=AbCdEf0123456789_-&sid=12ab&spx=%2F&fm=' + fm;
const p = vless.parseUri(uri);
assert.strictEqual(p.address, 'example.invalid');
assert.strictEqual(p.serverName, '192.0.2.1');
assert.strictEqual(p.flow, 'xtls-rprx-vision');
assert.deepStrictEqual(p.finalmask.tcp[0].settings.lengths, ['10-20']);
assert.deepStrictEqual(p.finalmask.tcp[0].settings.delays, ['0']);
const c = vless.makeConfig(p);
assert.strictEqual(c.inbounds[0].listen, '127.0.0.1');
assert.strictEqual(c.inbounds[0].port, 10808);
assert.strictEqual(c.outbounds[0].streamSettings.realitySettings.spiderX, '/');
assert.strictEqual(c.outbounds[0].streamSettings.finalmask.tcp[0].settings.packets, 'tlshello');
assert.throws(() => vless.parseUri(uri.replace('xtls-rprx-vision', '')), /missing flow|unsupported VLESS flow/);
assert.throws(() => vless.parseUri(uri.replace('vless://', 'https://')), /expected vless/);
assert.throws(() => vless.parseUri(uri.replace('10-20', '0')), /invalid fm lengths/);
// When build_xray.sh has supplied the official binary, validate the exact generated REALITY + FinalMask config.
const xray = path.join(__dirname, '..', 'build', 'xray');
if (fs.existsSync(xray)) {
    const d = fs.mkdtempSync(path.join(os.tmpdir(), 'ridetube-xray-'));
    const cf = path.join(d, 'config.json');
    fs.writeFileSync(cf, JSON.stringify(c));
    const check = cp.spawnSync(xray, ['run', '-test', '-c', cf], { encoding: 'utf8', timeout: 15000 });
    try { fs.unlinkSync(cf); fs.rmdirSync(d); } catch (e) { /* test temp */ }
    assert.strictEqual(check.status, 0, String(check.stdout || '') + String(check.stderr || ''));
}
console.log('vless config tests: ok');
