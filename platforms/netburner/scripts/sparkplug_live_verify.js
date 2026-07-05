// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Live Sparkplug B lifecycle against the device broker. Payloads are binary
// Buffers with embedded zero bytes; the broker stays payload-agnostic.
const mqtt = require('mqtt');
const HOST = process.argv[2] || '172.16.82.8';
const URL = `mqtt://${HOST}:1883`;
const results = [];
const fail = (n, m) => results.push(`FAIL ${n}: ${m}`);
const pass = (n) => results.push(`PASS ${n}`);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

const G = 'TestGroup', N = 'TestNode', D = 'TestDevice', H = 'TestHost';
const T = {
    nbirth: `spBv1.0/${G}/NBIRTH/${N}`,
    dbirth: `spBv1.0/${G}/DBIRTH/${N}/${D}`,
    ndata: `spBv1.0/${G}/NDATA/${N}`,
    ddata: `spBv1.0/${G}/DDATA/${N}/${D}`,
    ndeath: `spBv1.0/${G}/NDEATH/${N}`,
    state: `spBv1.0/STATE/${H}`,
};

const offline = Buffer.from([0x53, 0x50, 0x42, 0x00, 0x4F, 0x46, 0x46, 0x4C, 0x49, 0x4E, 0x45, 0x00, 0xFF, 0x10]);
const online = Buffer.from([0x53, 0x50, 0x42, 0x00, 0x4F, 0x4E, 0x4C, 0x49, 0x4E, 0x45, 0x00, 0xFF, 0x11]);
function spPayload(len, seed) {
    const b = Buffer.alloc(len);
    for (let i = 0; i < len; i++) b[i] = (i % 7 === 0) ? 0 : (seed + i * 31) & 0xFF;
    if (len > 2) { b[0] = 0x08; b[1] = 0x00; }
    return b;
}
const eq = (a, b) => Buffer.isBuffer(a) && Buffer.isBuffer(b) && a.equals(b);

function connect(id, opts = {}) {
    return new Promise((resolve, reject) => {
        const c = mqtt.connect(URL, Object.assign(
            { protocolVersion: 5, clientId: id, clean: true, connectTimeout: 5000,
              reconnectPeriod: 0, properties: { sessionExpiryInterval: 0 } }, opts));
        c.on('connect', (ack) => resolve({ c, ack }));
        c.on('error', reject);
    });
}
const sub = (c, f) => new Promise((res, rej) => c.subscribe(f, { qos: 1 }, e => e ? rej(e) : res()));
const pub = (c, t, p, o) => new Promise((res, rej) => c.publish(t, p, o, e => e ? rej(e) : res()));

(async () => {
    // 1-3. Host: clean start, expiry 0, retained QoS1 offline STATE will; online STATE.
    const { c: host, ack } = await connect('sp-live-host', {
        will: { topic: T.state, payload: offline, qos: 1, retain: true }
    });
    if (ack.sessionPresent === false) pass('host-clean-start-session-present-0');
    else fail('host-clean-start-session-present-0', `sp=${ack.sessionPresent}`);

    const rx = [];
    host.on('message', (t, p, pk) => rx.push({ t, p, qos: pk.qos, retain: pk.retain }));
    await sub(host, 'spBv1.0/#');
    await pub(host, T.state, online, { qos: 1, retain: true });
    await sleep(400);

    // 4-5. Edge node: NDEATH will, births + data.
    const { c: node } = await connect('sp-live-node', {
        will: { topic: T.ndeath, payload: spPayload(40, 0x77), qos: 1, retain: false }
    });
    const nbirth = spPayload(600, 0x01), dbirth = spPayload(400, 0x02);
    const ndata = spPayload(64, 0x03), ddata = spPayload(64, 0x04);
    await pub(node, T.nbirth, nbirth, { qos: 1 });
    await pub(node, T.dbirth, dbirth, { qos: 1 });
    await pub(node, T.ndata, ndata, { qos: 0 });
    await pub(node, T.ddata, ddata, { qos: 0 });
    await sleep(600);

    // 6. Host received all, binary-identical (proves large NBIRTH > 512B intact too).
    const find = (t) => rx.find(m => m.t === t);
    if (eq(find(T.nbirth) && find(T.nbirth).p, nbirth)) pass('nbirth-payload-intact-600B');
    else fail('nbirth-payload-intact-600B', 'mismatch/missing');
    if (eq(find(T.dbirth) && find(T.dbirth).p, dbirth)) pass('dbirth-payload-intact');
    else fail('dbirth-payload-intact', 'mismatch/missing');
    if (eq(find(T.ndata) && find(T.ndata).p, ndata) && eq(find(T.ddata) && find(T.ddata).p, ddata))
        pass('node-and-device-data-routed');
    else fail('node-and-device-data-routed', 'mismatch/missing');

    // 7-8. Abrupt node death -> NDEATH published at QoS 1.
    rx.length = 0;
    node.stream.destroy();
    await sleep(1500);
    const death = find(T.ndeath);
    if (death && death.qos === 1 && eq(death.p, spPayload(40, 0x77))) pass('ndeath-on-abrupt-disconnect');
    else fail('ndeath-on-abrupt-disconnect', death ? `qos=${death.qos}` : 'missing');

    // 9-10. Node reconnects fresh, re-births.
    rx.length = 0;
    const { c: node2, ack: ack2 } = await connect('sp-live-node', {
        will: { topic: T.ndeath, payload: spPayload(40, 0x77), qos: 1, retain: false }
    });
    if (ack2.sessionPresent === false) pass('node-reconnect-fresh-session');
    else fail('node-reconnect-fresh-session', `sp=${ack2.sessionPresent}`);
    await pub(node2, T.nbirth, nbirth, { qos: 1 });
    await pub(node2, T.dbirth, dbirth, { qos: 1 });
    await sleep(500);
    if (find(T.nbirth) && find(T.dbirth)) pass('rebirth-delivered');
    else fail('rebirth-delivered', 'missing');
    node2.end(true);

    // 11-12. Host disconnects with reason 0x04 -> offline STATE will replaces online.
    await new Promise(res => host.end(false, { reasonCode: 4 }, res));
    await sleep(1200);

    // 13-14. Late subscriber sees retained offline STATE with RETAIN=1.
    const { c: late } = await connect('sp-live-late');
    let lateState = null;
    late.on('message', (t, p, pk) => { if (t === T.state && pk.retain) lateState = { p, qos: pk.qos }; });
    await sub(late, T.state);
    await sleep(600);
    if (lateState && lateState.retain !== false && eq(lateState.p, offline) && lateState.qos === 1)
        pass('late-subscriber-retained-offline-state');
    else fail('late-subscriber-retained-offline-state', lateState ? lateState.p.toString('hex') : 'missing');

    // cleanup retained STATE
    await pub(late, T.state, Buffer.alloc(0), { qos: 0, retain: true });
    late.end(true);

    console.log(results.join('\n'));
    process.exit(results.some(x => x.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e); process.exit(2); });
