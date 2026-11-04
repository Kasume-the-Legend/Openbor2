#!/usr/bin/env node
// OpenBOR Online relay: rooms with codes, a public room list, and an ordered fan-out of lockstep inputs.
// It never looks at the game; it only forwards short text lines between the players of a room.
//   node relay.js [port]        (default 7777)
'use strict';
const net = require('net');
const PORT = parseInt(process.argv[2] || process.env.PORT || '7777', 10);
const MAX_LINE = 512, MAX_ROOMS = 2000;
const rooms = new Map();                       // code -> { code, pub, max, pakid, hostSlot, started, slots: [client|null x4] }
let nextId = 1;

const clean = (s, n) => String(s || '').replace(/[^\w .\-]/g, '').slice(0, n);
const send = (c, line) => { if (c && !c.dead) { try { c.sock.write(line + '\n'); } catch (e) { /* closing */ } } };
function code() { const A = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; for (;;) { let s = ''; for (let i = 0; i < 5; i++) s += A[Math.floor(Math.random() * A.length)]; if (!rooms.has(s)) return s; } }
function roster(r) {
    for (const c of r.slots) if (c) {
        for (let i = 0; i < 4; i++) { const o = r.slots[i]; send(c, o ? `P ${i} ${o.ready ? 1 : 0} ${r.hostSlot === i ? 1 : 0} ${o.name}` : `P ${i} - 0 0 -`); }
        send(c, 'PEND');
    }
}
function leave(c) {
    const r = c.room; if (!r) return; c.room = null;
    const i = r.slots.indexOf(c); if (i < 0) return;
    r.slots[i] = null;
    if (r.started) for (const o of r.slots) if (o) send(o, `DROP ${i}`);
    if (!r.slots.some(Boolean)) { rooms.delete(r.code); return; }
    if (r.hostSlot === i) r.hostSlot = r.slots.findIndex(Boolean);
    if (!r.started) roster(r);
}
// Everyone in the room gets the same GO (seed, player mask, input delay) at the same moment, so all clients begin lockstep together.
function tryStart(r, force) {
    if (r.started) return null;
    const act = r.slots.filter(Boolean);
    if (act.length < 2) return 'need at least 2 players';
    if (!act.every((x) => x.ready)) return force ? 'not everyone is ready' : null;   // host counts too: everyone present must ready up
    console.log(`room ${r.code} start with ${act.length}`); r.started = true;
    const mask = r.slots.reduce((m, x, i) => m | (x ? 1 << i : 0), 0), seed = (Math.random() * 0x7fffffff) | 0, delay = 4;
    for (const o of act) send(o, `GO ${seed} ${mask} ${delay}`);
    return null;
}
function onLine(c, line) {
    if (process.env.RELAY_DEBUG) console.log('<', c.name, line.slice(0, 60));
    const t = line.split(' '), cmd = t[0];
    if (cmd === 'HELLO') { c.name = clean(t.slice(2).join(' '), 16) || 'Player'; return send(c, 'WELCOME'); }
    if (cmd === 'LIST') {
        for (const r of rooms.values()) if (r.pub && !r.started) send(c, `RL ${r.code} ${r.slots.filter(Boolean).length} ${r.max} ${r.pakid} ${r.slots[r.hostSlot] ? r.slots[r.hostSlot].name : '?'}`);
        return send(c, 'RLEND');
    }
    if (cmd === 'HOST' && !c.room) {
        if (rooms.size >= MAX_ROOMS) return send(c, 'ERR server full');
        const r = { code: code(), pub: t[1] === '1', max: Math.min(4, Math.max(2, parseInt(t[2], 10) || 4)), pakid: clean(t[3], 24), hostSlot: 0, started: false, slots: [null, null, null, null] };
        r.slots[0] = c; c.room = r; c.ready = false; rooms.set(r.code, r);
        console.log(`room ${r.code} opened by ${c.name}`);
        send(c, `ROOM ${r.code} 0`); return roster(r);
    }
    if (cmd === 'JOIN' && !c.room) {
        const r = rooms.get(clean(t[1], 8).toUpperCase());
        if (!r) return send(c, 'ERR no such room');
        if (r.started) return send(c, 'ERR game already started');
        if (r.pakid && clean(t[2], 24) !== r.pakid) return send(c, 'ERR different game version (pak does not match the host)');
        const i = r.slots.findIndex((x, k) => !x && k < r.max);
        if (i < 0) return send(c, 'ERR room is full');
        r.slots[i] = c; c.room = r; c.ready = false;
        console.log(`${c.name} joined ${r.code} as slot ${i}`);
        send(c, `ROOM ${r.code} ${i}`); return roster(r);
    }
    const r = c.room; if (!r) return;
    const me = r.slots.indexOf(c);
    if (cmd === 'READY') console.log(`${c.name} ready ${t[1]}`);
    if (cmd === 'READY') { c.ready = t[1] === '1'; roster(r); tryStart(r, false); return; }
    if (cmd === 'START' && me === r.hostSlot && !r.started) { const err = tryStart(r, true); if (err) send(c, 'ERR ' + err); return; }
    if ((cmd === 'I' || cmd === 'C') && r.started && me >= 0) {          // I <frame> <keys-hex>   C <frame> <hash-hex>
        const f = parseInt(t[1], 10); if (!Number.isFinite(f) || !/^[0-9a-f]{1,16}$/.test(t[2] || '')) return;
        for (const o of r.slots) if (o && o !== c) send(o, `${cmd} ${me} ${f} ${t[2]}`);
        return;
    }
    if (cmd === 'LEAVE') return leave(c);
}
const server = net.createServer((sock) => {
    sock.setNoDelay(true); sock.setTimeout(120000);
    const c = { id: nextId++, sock, name: 'Player', room: null, ready: false, dead: false };
    let buf = '';
    sock.on('data', (d) => {
        buf += d.toString('latin1');
        if (buf.length > MAX_LINE * 8) { sock.destroy(); return; }
        let i; while ((i = buf.indexOf('\n')) >= 0) { const line = buf.slice(0, i).replace(/\r$/, ''); buf = buf.slice(i + 1); if (line.length <= MAX_LINE) { try { onLine(c, line); } catch (e) { /* ignore bad input */ } } }
    });
    const bye = () => { if (c.dead) return; c.dead = true; leave(c); };
    sock.on('close', bye); sock.on('error', bye); sock.on('timeout', () => sock.destroy());
});
server.listen(PORT, () => console.log(`OpenBOR Online relay listening on ${PORT}`));
