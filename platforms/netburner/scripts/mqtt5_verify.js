// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Quick MQTT 5 conformance checks against the live broker:
// 1. UNSUBSCRIBE gets an UNSUBACK and stops delivery.
// 2. Zero-length retained payload deletes the retained message.
// 3. CONNACK omits Maximum QoS (implies QoS 2) and advertises Receive Maximum.
const mqtt = require('mqtt');

const HOST = process.argv[2] || '172.16.82.8';
const URL = `mqtt://${HOST}:1883`;
const results = [];
const fail = (name, msg) => { results.push(`FAIL ${name}: ${msg}`); };
const pass = (name) => { results.push(`PASS ${name}`); };

function connect(id) {
    return new Promise((resolve, reject) => {
        const c = mqtt.connect(URL, { protocolVersion: 5, clientId: id, clean: true, connectTimeout: 5000 });
        c.on('connect', (connack) => resolve({ c, connack }));
        c.on('error', reject);
    });
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
    // Test 3: CONNACK properties
    const { c: probe, connack } = await connect('v-probe');
    const props = connack.properties || {};
    if (props.maximumQoS === undefined) pass('connack-max-qos-omitted');
    else fail('connack-max-qos-omitted', `maximumQoS present = ${props.maximumQoS}`);
    if (props.receiveMaximum === 8) pass('connack-receive-maximum-8');
    else fail('connack-receive-maximum-8', `receiveMaximum = ${props.receiveMaximum}`);
    probe.end(true);

    // Test 1: unsubscribe
    const { c: sub } = await connect('v-sub');
    const { c: pub } = await connect('v-pub');
    let got = 0;
    sub.on('message', () => { got++; });
    await new Promise((res, rej) => sub.subscribe('verify/unsub', { qos: 1 }, e => e ? rej(e) : res()));
    await new Promise((res, rej) => pub.publish('verify/unsub', 'one', { qos: 1 }, e => e ? rej(e) : res()));
    await sleep(700);
    const beforeUnsub = got;
    const unsubAck = await new Promise((res, rej) => {
        const t = setTimeout(() => rej(new Error('UNSUBACK timeout')), 4000);
        sub.unsubscribe('verify/unsub', (e, packet) => { clearTimeout(t); e ? rej(e) : res(packet); });
    }).catch(err => err);
    if (unsubAck instanceof Error) fail('unsuback-received', unsubAck.message);
    else pass('unsuback-received');
    await new Promise((res, rej) => pub.publish('verify/unsub', 'two', { qos: 1 }, e => e ? rej(e) : res()));
    await sleep(700);
    if (beforeUnsub === 1 && got === 1) pass('unsubscribe-stops-delivery');
    else fail('unsubscribe-stops-delivery', `before=${beforeUnsub} after=${got}`);

    // Test 2: retained delete via empty payload
    await new Promise((res, rej) => pub.publish('verify/ret', 'keepme', { qos: 1, retain: true }, e => e ? rej(e) : res()));
    await sleep(300);
    const { c: r1 } = await connect('v-ret1');
    let retGot = null;
    r1.on('message', (t, p, pkt) => { if (pkt.retain) retGot = p.toString(); });
    await new Promise((res, rej) => r1.subscribe('verify/ret', { qos: 0 }, e => e ? rej(e) : res()));
    await sleep(700);
    if (retGot === 'keepme') pass('retained-stored');
    else fail('retained-stored', `got ${retGot}`);
    r1.end(true);
    // delete it
    await new Promise((res, rej) => pub.publish('verify/ret', '', { qos: 1, retain: true }, e => e ? rej(e) : res()));
    await sleep(300);
    const { c: r2 } = await connect('v-ret2');
    let retGot2 = null;
    r2.on('message', (t, p, pkt) => { if (pkt.retain) retGot2 = p.toString(); });
    await new Promise((res, rej) => r2.subscribe('verify/ret', { qos: 0 }, e => e ? rej(e) : res()));
    await sleep(700);
    if (retGot2 === null) pass('retained-deleted-by-empty-payload');
    else fail('retained-deleted-by-empty-payload', `still got '${retGot2}'`);
    r2.end(true);

    // Retain Handling 1: second identical subscribe should NOT resend retained
    await new Promise((res, rej) => pub.publish('verify/rh1', 'sticky', { qos: 1, retain: true }, e => e ? rej(e) : res()));
    await sleep(300);
    const { c: rh } = await connect('v-rh1');
    let rhCount = 0;
    rh.on('message', (t, p, pkt) => { if (pkt.retain) rhCount++; });
    await new Promise((res, rej) => rh.subscribe('verify/rh1', { qos: 0, rh: 1 }, e => e ? rej(e) : res()));
    await sleep(700);
    const first = rhCount;
    await new Promise((res, rej) => rh.subscribe('verify/rh1', { qos: 0, rh: 1 }, e => e ? rej(e) : res()));
    await sleep(700);
    if (first === 1 && rhCount === 1) pass('retain-handling-1');
    else fail('retain-handling-1', `first=${first} total=${rhCount}`);
    rh.end(true);
    // cleanup retained
    await new Promise((res) => pub.publish('verify/rh1', '', { qos: 0, retain: true }, () => res()));

    sub.end(true);
    pub.end(true);
    console.log(results.join('\n'));
    process.exit(results.some(r => r.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e); process.exit(2); });
