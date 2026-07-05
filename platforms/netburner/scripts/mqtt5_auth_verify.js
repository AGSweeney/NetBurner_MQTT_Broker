// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Live checks for MQTT CONNECT authentication (Phase 7).
const mqtt = require('mqtt');
const HOST = process.argv[2] || '172.16.82.8';
const URL = `mqtt://${HOST}:1883`;
const API = `http://${HOST}/api/mqtt/users`;
const results = [];
const fail = (n, m) => results.push(`FAIL ${n}: ${m}`);
const pass = (n) => results.push(`PASS ${n}`);

function tryConnect(opts) {
    return new Promise((resolve) => {
        const c = mqtt.connect(URL, Object.assign(
            { protocolVersion: 5, clean: true, connectTimeout: 5000, reconnectPeriod: 0 }, opts));
        const timer = setTimeout(() => { c.end(true); resolve({ ok: false, code: 'timeout' }); }, 6000);
        c.on('connect', () => { clearTimeout(timer); c.end(true); resolve({ ok: true }); });
        c.on('error', (e) => { clearTimeout(timer); c.end(true); resolve({ ok: false, code: e.code }); });
    });
}

async function api(payload) {
    const r = await fetch(API, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    });
    const data = await r.json();
    if (!r.ok || !data.success) throw new Error(`API ${JSON.stringify(payload)} -> ${JSON.stringify(data)}`);
}

(async () => {
    // Baseline: auth disabled, anonymous connect works.
    let r = await tryConnect({ clientId: 'auth-open' });
    if (r.ok) pass('open-broker-anonymous-ok'); else fail('open-broker-anonymous-ok', r.code);

    // Provision a user and enable auth.
    await api({ action: 'add', username: 'alice', password: 'secret123' });
    await api({ action: 'settings', authRequired: 'true', allowAnonymous: 'false' });
    const list = await (await fetch(API)).json();
    if (list.auth_required === true && list.users.includes('alice')) pass('user-provisioned-auth-enabled');
    else fail('user-provisioned-auth-enabled', JSON.stringify(list));

    // Valid credentials accepted.
    r = await tryConnect({ clientId: 'auth-good', username: 'alice', password: 'secret123' });
    if (r.ok) pass('valid-credentials-accepted'); else fail('valid-credentials-accepted', r.code);

    // Wrong password: CONNACK 134 (0x86 Bad User Name or Password).
    r = await tryConnect({ clientId: 'auth-bad', username: 'alice', password: 'nope' });
    if (!r.ok && r.code === 134) pass('wrong-password-rejected-0x86');
    else fail('wrong-password-rejected-0x86', JSON.stringify(r));

    // No credentials: CONNACK 135 (0x87 Not Authorized).
    r = await tryConnect({ clientId: 'auth-anon' });
    if (!r.ok && r.code === 135) pass('anonymous-rejected-0x87');
    else fail('anonymous-rejected-0x87', JSON.stringify(r));

    // Allow anonymous: anonymous admitted, wrong password still rejected.
    await api({ action: 'settings', authRequired: 'true', allowAnonymous: 'true' });
    r = await tryConnect({ clientId: 'auth-anon2' });
    if (r.ok) pass('allow-anonymous-honored'); else fail('allow-anonymous-honored', JSON.stringify(r));
    r = await tryConnect({ clientId: 'auth-bad2', username: 'alice', password: 'nope' });
    if (!r.ok && r.code === 134) pass('anon-mode-still-checks-passwords');
    else fail('anon-mode-still-checks-passwords', JSON.stringify(r));

    // Delete user: valid credentials now rejected.
    await api({ action: 'settings', authRequired: 'true', allowAnonymous: 'false' });
    await api({ action: 'delete', username: 'alice' });
    r = await tryConnect({ clientId: 'auth-del', username: 'alice', password: 'secret123' });
    if (!r.ok && r.code === 134) pass('deleted-user-rejected');
    else fail('deleted-user-rejected', JSON.stringify(r));

    // Restore default: auth off, open broker.
    await api({ action: 'settings', authRequired: 'false', allowAnonymous: 'false' });
    r = await tryConnect({ clientId: 'auth-restore' });
    if (r.ok) pass('auth-disabled-restored'); else fail('auth-disabled-restored', JSON.stringify(r));

    console.log(results.join('\n'));
    process.exit(results.some(x => x.startsWith('FAIL')) ? 1 : 0);
})().catch(e => { console.error('ERROR', e); process.exit(2); });
