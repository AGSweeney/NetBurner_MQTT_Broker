// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Live interop: retained QoS 1 wills, DISCONNECT 0x04/0x00, birth/death/state lifecycle.
const mqtt = require('mqtt');
const HOST = process.argv[2] || '172.16.82.8';
const URL = `mqtt://${HOST}:1883`;
const results = [];
const fail = (n, m) => results.push(`FAIL ${n}: ${m}`);
const pass = (n) => results.push(`PASS ${n}`);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function connect(id, opts = {}) {
    return new Promise((resolve, reject) => {
        const c = mqtt.connect(URL, Object.assign(
            { protocolVersion: 5, clientId: id, clean: true, connectTimeout: 5000, reconnectPeriod: 0 }, opts));
        c.on('connect', () => resolve(c));
        c.on('error', reject);
    });
}

function subscribeQos1(c, topic) {
    return new Promise((res, rej) => c.subscribe(topic, { qos: 1 }, (e, granted) => {
        if (e) return rej(e);
        res(granted);
    }));
}

function publish(c, topic, payload, opts) {
    return new Promise((res, rej) => c.publish(topic, payload, opts, e => e ? rej(e) : res()));
}

function clearRetained(c, topic) {
    return publish(c, topic, '', { qos: 0, retain: true });
}

(async () => {
    const STATE = 'bd/dev1/state';
    const monitor = await connect('bd-monitor');
    const events = [];
    monitor.on('message', (t, p, pkt) => {
        if (t === STATE) events.push({ payload: p.toString(), qos: pkt.qos, retain: pkt.retain });
    });
    await subscribeQos1(monitor, STATE);

    // ---- Birth: device connects with retained QoS 1 will, publishes retained birth ----
    const device = await connect('bd-device', {
        will: { topic: STATE, payload: 'offline', qos: 1, retain: true }
    });
    await publish(device, STATE, 'online', { qos: 1, retain: true });
    await sleep(700);
    const birth = events.find(e => e.payload === 'online');
    if (birth && birth.qos === 1) pass('birth-online-qos1-received');
    else fail('birth-online-qos1-received', JSON.stringify(events));

    // ---- Death: abrupt socket loss fires the retained QoS 1 will ----
    events.length = 0;
    device.stream.destroy();
    await sleep(1500);
    const death = events.find(e => e.payload === 'offline');
    if (death && death.qos === 1) pass('death-will-qos1-delivered');
    else fail('death-will-qos1-delivered', JSON.stringify(events));

    // ---- Late subscriber proves the will was RETAINED at QoS 1 ----
    const late = await connect('bd-late');
    let lateMsg = null;
    late.on('message', (t, p, pkt) => {
        if (t === STATE && pkt.retain) lateMsg = { payload: p.toString(), qos: pkt.qos, retain: pkt.retain };
    });
    await subscribeQos1(late, STATE);
    await sleep(700);
    if (lateMsg && lateMsg.payload === 'offline' && lateMsg.qos === 1 && lateMsg.retain === true) {
        pass('retained-qos1-will-for-late-subscriber');
    } else {
        fail('retained-qos1-will-for-late-subscriber', JSON.stringify(lateMsg));
    }
    late.end(true);

    // ---- Rebirth: device reconnects, birth overwrites retained death ----
    events.length = 0;
    const device2 = await connect('bd-device', {
        will: { topic: STATE, payload: 'offline', qos: 1, retain: true }
    });
    await publish(device2, STATE, 'online', { qos: 1, retain: true });
    await sleep(700);
    if (events.some(e => e.payload === 'online')) pass('rebirth-online-received');
    else fail('rebirth-online-received', JSON.stringify(events));

    // ---- Graceful shutdown (DISCONNECT 0x00): will suppressed ----
    events.length = 0;
    await new Promise(res => device2.end(false, {}, res));
    await sleep(1500);
    if (!events.some(e => e.payload === 'offline')) pass('graceful-0x00-suppresses-will');
    else fail('graceful-0x00-suppresses-will', JSON.stringify(events));

    // ---- DISCONNECT 0x04: will published despite clean protocol shutdown ----
    events.length = 0;
    const device3 = await connect('bd-device', {
        will: { topic: STATE, payload: 'offline', qos: 1, retain: true }
    });
    await sleep(300);
    await new Promise(res => device3.end(false, { reasonCode: 4 }, res));
    await sleep(1500);
    const will04 = events.find(e => e.payload === 'offline');
    if (will04 && will04.qos === 1) pass('disconnect-0x04-publishes-will');
    else fail('disconnect-0x04-publishes-will', JSON.stringify(events));

    // Cleanup retained state.
    await clearRetained(monitor, STATE);
    monitor.end(true);

    console.log(results.join('\n'));
    process.exit(results.some(x => x.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e); process.exit(2); });
