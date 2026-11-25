'use strict';
// SPDX-License-Identifier: GPL-3.0-or-later
// Converts a VLESS share URI to an Xray-core client config. Never log the URI or generated config.
const url = require('url');

function one(q, key, required) {
    const v = q[key];
    if (Array.isArray(v)) throw new Error('duplicate ' + key);
    if ((v === undefined || v === '') && required) throw new Error('missing ' + key);
    return v === undefined ? '' : String(v);
}
function parseRangeList(value, key) {
    if (!Array.isArray(value) || !value.length) throw new Error('invalid ' + key);
    return value.map(function (v) {
        v = String(v);
        if (!/^\d+(?:-\d+)?$/.test(v)) throw new Error('invalid ' + key);
        const p = v.split('-').map(Number);
        if (p.length === 2 && (p[0] < 1 || p[1] < p[0])) throw new Error('invalid ' + key);
        if (p[0] < 0 || p[p.length - 1] > 16384) throw new Error('invalid ' + key);
        return v;
    });
}
function parseFragment(raw) {
    if (!raw) return null;
    let fm;
    try { fm = JSON.parse(raw); } catch (e) { throw new Error('invalid fm JSON'); }
    if (!fm || fm.type !== 'fragment' || !fm.tcp || !Array.isArray(fm.tcp) || fm.tcp.length !== 1) {
        throw new Error('unsupported fm format');
    }
    const f = fm.tcp[0];
    if (!f || f.type !== 'fragment') throw new Error('unsupported fm fragment');
    // Share links may encode the fragment fields directly; native Xray config nests them under settings.
    const settings = f.settings || f;
    if (settings.packets !== 'tlshello') throw new Error('unsupported fm fragment');
    const lengths = parseRangeList(settings.lengths, 'fm lengths');
    const delays = parseRangeListAllowZero(settings.delays, 'fm delays');
    return { tcp: [{ type: 'fragment', settings: { packets: 'tlshello', lengths: lengths, delays: delays } }] };
}
function parseRangeListAllowZero(value, key) {
    if (!Array.isArray(value) || !value.length) throw new Error('invalid ' + key);
    return value.map(function (v) {
        v = String(v);
        if (!/^\d+(?:-\d+)?$/.test(v)) throw new Error('invalid ' + key);
        const p = v.split('-').map(Number);
        if (p[0] < 0 || p[p.length - 1] > 10000 || (p.length === 2 && p[1] < p[0])) throw new Error('invalid ' + key);
        return v;
    });
}
function parseUri(uri) {
    if (typeof uri !== 'string' || uri.length > 8192 || /[\x00-\x20\x7f]/.test(uri)) throw new Error('invalid VLESS URI');
    let u;
    try { u = url.parse(uri, true); } catch (e) { throw new Error('invalid VLESS URI'); }
    if (u.protocol !== 'vless:' || !u.hostname || !u.port || u.pathname && u.pathname !== '/') throw new Error('expected vless://UUID@host:port');
    const uuid = decodeURIComponent(u.auth || '');
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(uuid)) throw new Error('invalid VLESS UUID');
    const q = u.query || {};
    if (one(q, 'encryption', true) !== 'none') throw new Error('only encryption=none is supported');
    if (one(q, 'security', true) !== 'reality' || one(q, 'type', true) !== 'tcp') throw new Error('only REALITY over TCP is supported');
    const flow = one(q, 'flow', true);
    if (flow !== 'xtls-rprx-vision') throw new Error('unsupported VLESS flow');
    const serverName = one(q, 'sni', true);
    const publicKey = one(q, 'pbk', true);
    const shortId = one(q, 'sid', true);
    const fingerprint = one(q, 'fp', true);
    const spiderX = one(q, 'spx', false);
    if (!/^[A-Za-z0-9_-]{1,128}$/.test(publicKey) || !/^[A-Fa-f0-9]{0,32}$/.test(shortId)) throw new Error('invalid REALITY key or short ID');
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(fingerprint)) throw new Error('invalid uTLS fingerprint');
    const port = Number(u.port);
    if (!(port > 0 && port < 65536)) throw new Error('invalid server port');
    const finalmask = parseFragment(one(q, 'fm', false));
    return {
        address: u.hostname, port: port, id: uuid, flow: flow,
        serverName: serverName, publicKey: publicKey, shortId: shortId,
        fingerprint: fingerprint, spiderX: spiderX, finalmask: finalmask
    };
}
function makeConfig(profile, socksPort) {
    if (!profile || !profile.address || !profile.id) throw new Error('profile required');
    socksPort = socksPort || 10808;
    if (!(socksPort > 0 && socksPort < 65536)) throw new Error('invalid SOCKS port');
    const reality = {
        serverName: profile.serverName,
        fingerprint: profile.fingerprint,
        publicKey: profile.publicKey,
        shortId: profile.shortId
    };
    if (profile.spiderX) reality.spiderX = profile.spiderX;
    const stream = { network: 'tcp', security: 'reality', realitySettings: reality };
    if (profile.finalmask) stream.finalmask = profile.finalmask;
    return {
        log: { loglevel: 'warning' },
        inbounds: [{
            listen: '127.0.0.1', port: socksPort, protocol: 'socks',
            settings: { auth: 'noauth', udp: false }, tag: 'ridetube-socks'
        }],
        outbounds: [{
            protocol: 'vless', tag: 'ridetube-vless',
            settings: {
                vnext: [{
                    address: profile.address, port: profile.port,
                    users: [{ id: profile.id, encryption: 'none', flow: profile.flow }]
                }]
            },
            streamSettings: stream
        }]
    };
}
function fromUri(uri, socksPort) { return makeConfig(parseUri(uri), socksPort); }
module.exports = { parseUri: parseUri, makeConfig: makeConfig, fromUri: fromUri };
