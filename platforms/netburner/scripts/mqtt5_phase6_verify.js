// Phase 6 live checks: property forwarding, message expiry, DUP retransmit on resume.
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
            { protocolVersion: 5, clientId: id, clean: true, connectTimeout: 5000 }, opts));
        c.on('connect', (connack) => resolve({ c, connack }));
        c.on('error', reject);
    });
}

(async () => {
    // --- 1. Property forwarding ---
    const { c: sub } = await connect('p6-sub');
    const { c: pub } = await connect('p6-pub');
    let gotPkt = null;
    sub.on('message', (t, p, pkt) => { if (t === 'p6/props') gotPkt = pkt; });
    await new Promise((res, rej) => sub.subscribe('p6/props', { qos: 1 }, e => e ? rej(e) : res()));
    await new Promise((res, rej) => pub.publish('p6/props', 'hello', {
        qos: 1,
        properties: {
            payloadFormatIndicator: true,
            contentType: 'text/plain',
            responseTopic: 'p6/reply',
            correlationData: Buffer.from('cid-123'),
            userProperties: { origin: 'phase6' }
        }
    }, e => e ? rej(e) : res()));
    await sleep(800);
    if (!gotPkt) fail('property-forwarding', 'no message received');
    else {
        const pr = gotPkt.properties || {};
        const ok = pr.contentType === 'text/plain' && pr.responseTopic === 'p6/reply' &&
            pr.correlationData && pr.correlationData.toString() === 'cid-123' &&
            pr.userProperties && pr.userProperties.origin === 'phase6' &&
            pr.payloadFormatIndicator === true;
        if (ok) pass('property-forwarding');
        else fail('property-forwarding', JSON.stringify(pr));
    }

    // --- 2. Message expiry: retained message expires ---
    await new Promise((res, rej) => pub.publish('p6/exp', 'shortlived', {
        qos: 1, retain: true, properties: { messageExpiryInterval: 3 }
    }, e => e ? rej(e) : res()));
    await sleep(500);
    const { c: e1 } = await connect('p6-exp1');
    let exp1 = null;
    e1.on('message', (t, p, pkt) => { if (pkt.retain) exp1 = pkt; });
    await new Promise((res, rej) => e1.subscribe('p6/exp', { qos: 0 }, e => e ? rej(e) : res()));
    await sleep(700);
    e1.end(true);
    if (exp1 && exp1.properties && exp1.properties.messageExpiryInterval > 0 &&
        exp1.properties.messageExpiryInterval <= 3) {
        pass('expiry-remaining-forwarded (' + exp1.properties.messageExpiryInterval + 's left)');
    } else {
        fail('expiry-remaining-forwarded', exp1 ? JSON.stringify(exp1.properties) : 'no retained msg');
    }
    await sleep(4000);  // let it expire (3s interval)
    const { c: e2 } = await connect('p6-exp2');
    let exp2 = null;
    e2.on('message', (t, p, pkt) => { if (pkt.retain) exp2 = p.toString(); });
    await new Promise((res, rej) => e2.subscribe('p6/exp', { qos: 0 }, e => e ? rej(e) : res()));
    await sleep(700);
    e2.end(true);
    if (exp2 === null) pass('expired-retained-not-delivered');
    else fail('expired-retained-not-delivered', `got '${exp2}'`);

    // --- 3. DUP retransmit on session resume ---
    // Use a persistent session; suppress the client's PUBACK by killing the socket
    // as soon as the QoS1 publish arrives.
    const r1 = mqtt.connect(URL, {
        protocolVersion: 5, clientId: 'p6-resume', clean: false, connectTimeout: 5000,
        properties: { sessionExpiryInterval: 120 }, manualAcks: true, reconnectPeriod: 0
    });
    await new Promise((res, rej) => { r1.on('connect', res); r1.on('error', rej); });
    await new Promise((res, rej) => r1.subscribe('p6/rt', { qos: 1 }, e => e ? rej(e) : res()));
    let firstPid = null;
    r1.on('message', (t, p, pkt) => {
        if (firstPid === null) {
            firstPid = pkt.messageId;
            // kill the TCP socket without DISCONNECT and without acking
            r1.stream.destroy();
        }
    });
    await new Promise((res, rej) => pub.publish('p6/rt', 'needs-ack', { qos: 1 }, e => e ? rej(e) : res()));
    await sleep(1200);
    if (firstPid === null) { fail('dup-retransmit-on-resume', 'first delivery not received'); }
    else {
        const r2 = mqtt.connect(URL, {
            protocolVersion: 5, clientId: 'p6-resume', clean: false, connectTimeout: 5000,
            properties: { sessionExpiryInterval: 0 }, reconnectPeriod: 0
        });
        let resumed = null;
        let dupSeen = null;
        r2.on('connect', (connack) => { resumed = connack.sessionPresent; });
        r2.on('message', (t, p, pkt) => { if (t === 'p6/rt') dupSeen = pkt; });
        await new Promise((res, rej) => { r2.on('connect', res); r2.on('error', rej); });
        await sleep(1200);
        if (resumed !== true) fail('session-present-on-resume', `sessionPresent=${resumed}`);
        else pass('session-present-on-resume');
        if (dupSeen && dupSeen.dup === true && dupSeen.messageId === firstPid) {
            pass('dup-retransmit-on-resume');
        } else {
            fail('dup-retransmit-on-resume',
                 dupSeen ? `dup=${dupSeen.dup} pid=${dupSeen.messageId} (orig ${firstPid})` : 'no retransmit');
        }
        r2.end(true);
    }

    // cleanup retained
    await new Promise((res) => pub.publish('p6/exp', '', { retain: true }, () => res()));
    sub.end(true);
    pub.end(true);
    console.log(results.join('\n'));
    process.exit(results.some(r => r.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e); process.exit(2); });
