// Live MQTTS checks against nb-mqtt-broker on port 8883.
// Assumes MQTTS is enabled in broker config (admin UI or persisted settings).
const mqtt = require('mqtt');

const HOST = process.argv[2] || '172.16.82.8';
const TLS_PORT = 8883;
const results = [];
const fail = (n, m) => results.push(`FAIL ${n}: ${m}`);
const pass = (n) => results.push(`PASS ${n}`);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function fetchStatus() {
    const r = await fetch(`http://${HOST}/api/broker/status`, { cache: 'no-store' });
    if (!r.ok) throw new Error(`status HTTP ${r.status}`);
    return r.json();
}

function tlsConnect(id, timeoutMs = 45000) {
    return new Promise((resolve, reject) => {
        const c = mqtt.connect(`mqtts://${HOST}:${TLS_PORT}`, {
            protocolVersion: 5,
            clientId: id,
            clean: true,
            connectTimeout: timeoutMs,
            rejectUnauthorized: false,
            reconnectPeriod: 0,
        });
        let settled = false;
        const finish = (fn, arg) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            fn(arg);
        };
        const timer = setTimeout(() => {
            c.end(true);
            finish(reject, new Error(`connect timeout (${id})`));
        }, timeoutMs + 5000);
        c.on('connect', (connack) => finish(resolve, { c, connack }));
        c.on('error', (e) => {
            c.end(true);
            finish(reject, e);
        });
    });
}

(async () => {
    const st = await fetchStatus();
    if (st.tls_enabled && st.tls_listener_active && st.tls_port === TLS_PORT) {
        pass('tls-listener-active');
    } else {
        fail('tls-listener-active',
             `enabled=${st.tls_enabled} active=${st.tls_listener_active} port=${st.tls_port}`);
        console.log(results.join('\n'));
        process.exit(1);
    }

    const tag = Date.now().toString(36);

    const { c: probe, connack } = await tlsConnect(`tls-probe-${tag}`);
    if (connack.reasonCode === 0) pass('mqtts-connect-connack-success');
    else fail('mqtts-connect-connack-success', `reason=${connack.reasonCode}`);
    probe.end(true);
    await sleep(2000);

    const { c: sub } = await tlsConnect(`tls-sub-${tag}`);
    const { c: pub } = await tlsConnect(`tls-pub-${tag}`);
    let got = null;
    sub.on('message', (t, p) => { if (t === 'verify/tls') got = p.toString(); });
    await new Promise((res, rej) => sub.subscribe('verify/tls', { qos: 0 }, e => e ? rej(e) : res()));
    await new Promise((res, rej) => pub.publish('verify/tls', 'secure-payload', { qos: 0 }, e => e ? rej(e) : res()));
    await sleep(800);
    if (got === 'secure-payload') pass('mqtts-pubsub-route');
    else fail('mqtts-pubsub-route', `got=${got}`);
    sub.end(true);
    pub.end(true);
    await sleep(2000);

    // MaxTlsClients = 2 on SOMRT1061; third connect must not succeed.
    const { c: cap1 } = await tlsConnect(`tls-cap1-${tag}`);
    const { c: cap2 } = await tlsConnect(`tls-cap2-${tag}`);
    try {
        await tlsConnect(`tls-cap3-${tag}`, 8000);
        fail('tls-session-cap-enforced', 'third TLS client connected unexpectedly');
    } catch (e) {
        pass('tls-session-cap-enforced');
    }
    cap1.end(true);
    cap2.end(true);

    console.log(results.join('\n'));
    process.exit(results.some(x => x.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e.message || e); process.exit(2); });
