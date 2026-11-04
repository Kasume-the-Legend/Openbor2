// OpenBOR Online relay for Cloudflare Workers.
// Same job and same text protocol as relay/relay.js, but over WebSocket so phones and PCs can reach it through
// Cloudflare (wss://your-worker.workers.dev/ws). It only forwards short lines between the players of a room.
// All rooms live in one Durable Object (class Hub) so room codes and the public list work across the world.

const MAX_LINE = 512, MAX_ROOMS = 2000;
const clean = (s, n) => String(s || '').replace(/[^\w .\-]/g, '').slice(0, n);

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        if (url.pathname === '/ws' || url.pathname === '/') {
            if (request.headers.get('Upgrade') !== 'websocket') {
                return new Response('OpenBOR Online relay is running. Players connect to this address with wss://.\n', { headers: { 'content-type': 'text/plain' } });
            }
            return env.HUB.get(env.HUB.idFromName('hub')).fetch(request);
        }
        return new Response('not found', { status: 404 });
    },
};

export class Hub {
    constructor() {
        this.rooms = new Map();   // code -> { code, pub, max, pakid, hostSlot, started, slots: [client|null x4] }
    }

    async fetch() {
        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        server.accept();
        const c = { ws: server, name: 'Player', room: null, ready: false, dead: false, buf: '' };
        server.addEventListener('message', (ev) => {
            if (typeof ev.data !== 'string') return;
            c.buf += ev.data;
            if (c.buf.length > MAX_LINE * 8) { server.close(1009, 'too much'); return; }
            let i;
            while ((i = c.buf.indexOf('\n')) >= 0) {
                const line = c.buf.slice(0, i).replace(/\r$/, ''); c.buf = c.buf.slice(i + 1);
                if (line.length <= MAX_LINE) { try { this.onLine(c, line); } catch (e) { /* ignore bad input */ } }
            }
            // a frame without a trailing newline is still one complete message
            if (c.buf.length) { const line = c.buf; c.buf = ''; if (line.length <= MAX_LINE) { try { this.onLine(c, line); } catch (e) { /* ignore */ } } }
        });
        const bye = () => { if (c.dead) return; c.dead = true; this.leave(c); };
        server.addEventListener('close', bye);
        server.addEventListener('error', bye);
        return new Response(null, { status: 101, webSocket: client });
    }

    send(c, line) { if (c && !c.dead) { try { c.ws.send(line + '\n'); } catch (e) { /* closing */ } } }

    code() {
        const A = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
        for (;;) {
            let s = ''; for (let i = 0; i < 5; i++) s += A[Math.floor(Math.random() * A.length)];
            if (!this.rooms.has(s)) return s;
        }
    }

    roster(r) {
        for (const c of r.slots) if (c) {
            for (let i = 0; i < 4; i++) {
                const o = r.slots[i];
                this.send(c, o ? `P ${i} ${o.ready ? 1 : 0} ${r.hostSlot === i ? 1 : 0} ${o.name}` : `P ${i} - 0 0 -`);
            }
            this.send(c, 'PEND');
        }
    }

    leave(c) {
        const r = c.room; if (!r) return; c.room = null;
        const i = r.slots.indexOf(c); if (i < 0) return;
        r.slots[i] = null;
        if (r.started) for (const o of r.slots) if (o) this.send(o, `DROP ${i}`);
        if (!r.slots.some(Boolean)) { this.rooms.delete(r.code); return; }
        if (r.hostSlot === i) r.hostSlot = r.slots.findIndex(Boolean);
        if (!r.started) this.roster(r);
    }

    // Everyone gets the same GO (seed, player mask, input delay) at the same moment, so all clients begin lockstep together.
    tryStart(r, force) {
        if (r.started) return null;
        const act = r.slots.filter(Boolean);
        if (act.length < 2) return 'need at least 2 players';
        if (!act.every((x) => x.ready)) return force ? 'not everyone is ready' : null;
        r.started = true;
        const mask = r.slots.reduce((m, x, i) => m | (x ? 1 << i : 0), 0);
        const seed = Math.floor(Math.random() * 0x7fffffff), delay = 4;
        for (const o of act) this.send(o, `GO ${seed} ${mask} ${delay}`);
        return null;
    }

    onLine(c, line) {
        const t = line.split(' '), cmd = t[0];
        if (cmd === 'HELLO') { c.name = clean(t.slice(2).join(' '), 16) || 'Player'; return this.send(c, 'WELCOME'); }
        if (cmd === 'PING') return;
        if (cmd === 'LIST') {
            for (const r of this.rooms.values()) if (r.pub && !r.started)
                this.send(c, `RL ${r.code} ${r.slots.filter(Boolean).length} ${r.max} ${r.pakid} ${r.slots[r.hostSlot] ? r.slots[r.hostSlot].name : '?'}`);
            return this.send(c, 'RLEND');
        }
        if (cmd === 'HOST' && !c.room) {
            if (this.rooms.size >= MAX_ROOMS) return this.send(c, 'ERR server full');
            const r = { code: this.code(), pub: t[1] === '1', max: Math.min(4, Math.max(2, parseInt(t[2], 10) || 4)), pakid: clean(t[3], 24), hostSlot: 0, started: false, slots: [null, null, null, null] };
            r.slots[0] = c; c.room = r; c.ready = false; this.rooms.set(r.code, r);
            console.log(`room ${r.code} opened by ${c.name}`);
            this.send(c, `ROOM ${r.code} 0`); return this.roster(r);
        }
        if (cmd === 'JOIN' && !c.room) {
            const r = this.rooms.get(clean(t[1], 8).toUpperCase());
            if (!r) return this.send(c, 'ERR no such room');
            if (r.started) return this.send(c, 'ERR game already started');
            if (r.pakid && clean(t[2], 24) !== r.pakid) return this.send(c, 'ERR different game version (pak does not match the host)');
            const i = r.slots.findIndex((x, k) => !x && k < r.max);
            if (i < 0) return this.send(c, 'ERR room is full');
            r.slots[i] = c; c.room = r; c.ready = false;
            this.send(c, `ROOM ${r.code} ${i}`); return this.roster(r);
        }
        const r = c.room; if (!r) return;
        const me = r.slots.indexOf(c);
        if (cmd === 'READY') { c.ready = t[1] === '1'; this.roster(r); this.tryStart(r, false); return; }
        if (cmd === 'START' && me === r.hostSlot && !r.started) { const err = this.tryStart(r, true); if (err) this.send(c, 'ERR ' + err); return; }
        if ((cmd === 'I' || cmd === 'C') && r.started && me >= 0) {      // I <frame> <keys-hex>   C <frame> <hash-hex>
            const f = parseInt(t[1], 10);
            if (!Number.isFinite(f) || !/^[0-9a-f]{1,16}$/.test(t[2] || '')) return;
            const msg = `${cmd} ${me} ${f} ${t[2]}`;
            for (const o of r.slots) if (o && o !== c) this.send(o, msg);
            return;
        }
        if (cmd === 'LEAVE') return this.leave(c);
    }
}
