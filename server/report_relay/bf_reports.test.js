// node --test server/report_relay (Node 22.12 or later: node:sqlite). GitHub is a fake: no request leaves the PC.
import { test, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { gzipSync } from 'node:zlib';
import { report, screenshot, wholeLog, prune, forgetBfReportTables, neutralise, allowedHtml, PER_HOUR, MAX_LOG } from './bf_reports.js';

// The Worker tests' D1 stand in (server-api test/bf_pipelines.test.js), schema made by the module itself.
function d1() {
  const sqlite = new DatabaseSync(':memory:');
  return {
    sqlite,
    prepare(sql) {
      const statement = sqlite.prepare(sql.replace(/\?(\d+)/g, '?'));
      let values = [];
      const order = [...sql.matchAll(/\?(\d+)?/g)].map((match, index) => match[1] ? Number(match[1]) - 1 : index);
      const bound = {
        bind(...args) { values = order.map(index => args[index]); return bound; },
        async run() { statement.run(...values); return { success: true }; },
        async first() { return statement.get(...values) ?? null; },
        async all() { return { results: statement.all(...values) }; }
      };
      return bound;
    }
  };
}

function json(value, status = 200, headers = {}) {
  return new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json', ...headers } });
}

const INSTALL = '0f8e2a11-4b2c-4d3e-8f00-123456789abc', OTHER = '1a2b3c4d-1111-4222-8333-444455556666';
const NOW = Date.UTC(2026, 10, 16, 6, 0, 0);
const JPEG = Buffer.from([0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 4, 0xff, 0xd9]);
const LOG = gzipSync(Buffer.from('[2026-11-26 10:00:00.000] [info] [core] [t1] BF diagnostics: on: audio\n'));
let env, calls, ghState, ghFail, realFetch;

beforeEach(() => {
  forgetBfReportTables();
  env = { DB: d1(), BF_REPORT_TOKEN: 'test-token', BF_REPORT_SALT: 'test-salt', BF_REPORT_GITHUB_API: 'https://fake-github.test' };
  calls = [], ghState = 'open', ghFail = false;
  realFetch = globalThis.fetch;
  let next = 2001;
  globalThis.fetch = async (url, init) => {
    const call = { url, method: init.method, headers: init.headers, body: init.body ? JSON.parse(init.body) : null };
    calls.push(call);
    assert.ok(url.startsWith('https://fake-github.test/'), `only the fake GitHub: ${url}`);
    if (ghFail) return json({ message: 'Server Error' }, 500);
    const m = /\/repos\/TekRantGaming\/Battlefield-1943-Reborn\/issues(?:\/(\d+))?(\/comments)?$/.exec(url.replace('https://fake-github.test', ''));
    if (!m) return json({ message: 'Not Found' }, 404);
    if (init.method === 'POST' && !m[1]) { const n = next++; return json({ number: n, html_url: `https://github.com/x/issues/${n}` }, 201); }
    if (init.method === 'POST' && m[2]) return json({ id: 1 }, 201);
    if (init.method === 'GET') return json({ number: Number(m[1]), state: ghState, html_url: `https://github.com/x/issues/${m[1]}` });
    return json({ message: 'no' }, 405);
  };
});
afterEach(() => { globalThis.fetch = realFetch; });

function request(payload, { install = INSTALL, ip = '203.0.113.7', raw } = {}) {
  const body = raw ?? JSON.stringify(payload);
  return new Request('https://relay.test/v1/bf/reports', {
    method: 'POST', body,
    headers: { 'Content-Type': 'application/json', 'X-BF-Install': install, 'X-BF-Version': '0.4.6.8', 'CF-Connecting-IP': ip,
               'Content-Length': String(Buffer.byteLength(body)) }
  });
}
const crash = (extra = {}) => ({
  type: 'crash', title: '[Crash] Game closes when a Corsair lands', signature: '5e99acb4be0577a6',
  body: '### What happened\n\nIt closed. Ping @someone\n\n### System\n\n| | |\n|---|---|\n| GPU | X |\n\n<details><summary>Log</summary>\n\n```\n<b>kept</b>\n```\n\n</details>\n\n<!-- screenshot -->\n',
  ...extra
});
const post = async (payload, opts) => {
  const r = await report(request(payload, opts), env, opts?.now ?? NOW, json);
  return { status: r.status, data: await r.json() };
};

test('not set up: 503, and nothing reaches GitHub', async () => {
  delete env.BF_REPORT_TOKEN;
  assert.equal((await post(crash())).status, 503);
  assert.equal(calls.length, 0);
});

test('missing headers or a bad report: refused before GitHub', async () => {
  assert.equal((await post(crash(), { install: 'nope' })).status, 400);
  assert.equal((await post(null, { raw: '{not json' })).status, 400);
  assert.equal((await post(crash({ type: 'spam' }))).status, 400);
  assert.equal((await post(crash({ title: 'short' }))).status, 422);
  assert.equal((await post(crash({ body: 'x'.repeat(61 * 1024) }))).status, 413);
  const links = Array.from({ length: 6 }, (_, i) => `https://example.com/${i}`).join(' ');
  assert.equal((await post(crash({ body: `### What happened\n\n${links}\n\n### System\n` }))).status, 422);
  assert.equal((await post(crash({ screenshot_jpeg: Buffer.from('not a jpeg').toString('base64') }))).status, 413);
  assert.equal((await post(crash({ log_gz: Buffer.from('not gzip').toString('base64') }))).status, 413);
  const big = Buffer.alloc(MAX_LOG + 1); big[0] = 0x1f, big[1] = 0x8b;
  assert.equal((await post(crash({ log_gz: big.toString('base64') }))).status, 413);
  assert.equal(calls.length, 0);
});

test('a report: filed once with the bot token, mentions and HTML made harmless, the screenshot stored and linked', async () => {
  const r = await post(crash({ screenshot_jpeg: JPEG.toString('base64') }));
  assert.equal(r.status, 201);
  assert.deepEqual(r.data, { issue: 2001, url: 'https://github.com/x/issues/2001' });
  assert.equal(calls.length, 1);
  const c = calls[0];
  assert.equal(c.method, 'POST');
  assert.equal(c.headers.Authorization, 'Bearer test-token');
  assert.deepEqual(c.body.labels, ['crash', 'from-game', 'needs-triage']);
  assert.match(c.body.body, /Ping @​someone/);
  assert.match(c.body.body, /```\n<b>kept<\/b>\n```/, 'code blocks untouched');
  assert.match(c.body.body, /<details><summary>Log<\/summary>/);
  const link = /!\[Screenshot\]\((https:\/\/relay\.test\/v1\/bf\/reports\/shot\/([0-9a-f]{32})\.jpg)\)/.exec(c.body.body);
  assert.ok(link, 'the screenshot linked');
  const shot = await screenshot(new Request(link[1]), env, json);
  assert.equal(shot.status, 200);
  assert.equal(shot.headers.get('Content-Type'), 'image/jpeg');
  assert.deepEqual(Buffer.from(await shot.arrayBuffer()), JPEG);
  assert.equal((await screenshot(new Request('https://relay.test/v1/bf/reports/shot/' + 'a'.repeat(32) + '.jpg'), env, json)).status, 404);
});

test('the whole log: stored, linked where the game marked it, served as gzip text', async () => {
  const body = crash().body.replace('<!-- screenshot -->', '<!-- full log -->\n\n<!-- screenshot -->');
  const r = await post(crash({ body, log_gz: LOG.toString('base64') }));
  assert.equal(r.status, 201);
  const issue = calls[0].body.body;
  assert.ok(!issue.includes('<!-- full log -->'), 'the mark replaced');
  const link = /\[The whole log\]\((https:\/\/relay\.test\/v1\/bf\/reports\/log\/([0-9a-f]{32})\.txt)\) \(anonymized, kept 30 days\)/.exec(issue);
  assert.ok(link, 'the log linked');
  const got = await wholeLog(new Request(link[1]), env, json);
  assert.equal(got.status, 200);
  assert.equal(got.headers.get('Content-Encoding'), 'gzip');
  assert.match(got.headers.get('Content-Type'), /^text\/plain/);
  assert.equal((await wholeLog(new Request('https://relay.test/v1/bf/reports/log/' + 'b'.repeat(32) + '.txt'), env, json)).status, 404);
  // A report without the mark (an older game) gets no link; a log without the mark is linked at the end.
  const plain = await post(crash({ signature: '', title: '[Bug] No log with this one' }), { install: OTHER, ip: '198.51.100.9' });
  assert.equal(plain.status, 201);
  assert.ok(!calls[1].body.body.includes('The whole log'));
});

test('a known crash with a whole log: the comment links it', async () => {
  assert.equal((await post(crash())).status, 201);
  const body = crash().body.replace('<!-- screenshot -->', '<!-- full log -->\n\n<!-- screenshot -->');
  const again = await post(crash({ body, title: '[Crash] Same crash, its log', log_gz: LOG.toString('base64') }),
                           { install: OTHER, ip: '198.51.100.9' });
  assert.equal(again.status, 200);
  assert.match(calls[2].body.body, /\[The whole log\]\(https:\/\/relay\.test\/v1\/bf\/reports\/log\/[0-9a-f]{32}\.txt\)/);
  assert.ok(!calls[2].body.body.includes('<!-- full log -->'));
});

test('the same report again the same day: 409, GitHub not asked', async () => {
  assert.equal((await post(crash({ signature: '' }))).status, 201);
  assert.equal((await post(crash({ signature: '' }), { now: NOW + 3600 * 1000 })).status, 409);
  assert.equal(calls.length, 1);
});

test('rate limits per install, with Retry-After', async () => {
  for (let i = 0; i < PER_HOUR; ++i) assert.equal((await post(crash({ signature: '', title: `[Bug] Report number ${i}` }))).status, 201);
  const r = await report(request(crash({ signature: '', title: '[Bug] One report too many' })), env, NOW, json);
  assert.equal(r.status, 429);
  assert.equal(r.headers.get('Retry-After'), '3600');
  // Another install from another address is not held back.
  assert.equal((await post(crash({ signature: '', title: '[Bug] Someone else' }), { install: OTHER, ip: '198.51.100.9' })).status, 201);
});

test('a known open crash: a comment on its issue, not a new issue; a closed one gets a new issue', async () => {
  assert.equal((await post(crash())).status, 201);
  const again = await post(crash({ title: '[Crash] Same crash, other words' }), { install: OTHER, ip: '198.51.100.9' });
  assert.equal(again.status, 200);
  assert.equal(again.data.duplicate_of, 2001);
  assert.deepEqual(calls.map(c => c.method + ' ' + c.url.replace('https://fake-github.test', '')), [
    'POST /repos/TekRantGaming/Battlefield-1943-Reborn/issues',
    'GET /repos/TekRantGaming/Battlefield-1943-Reborn/issues/2001',
    'POST /repos/TekRantGaming/Battlefield-1943-Reborn/issues/2001/comments'
  ]);
  assert.match(calls[2].body.body, /Another player hit this crash \(version 0\.4\.6\.8\)/);
  ghState = 'closed';
  const third = await post(crash({ title: '[Crash] It is back' }), { install: '22222222-1111-4222-8333-444455556666', ip: '192.0.2.1' });
  assert.equal(third.status, 201);
  assert.equal(third.data.issue, 2002);
});

test('GitHub fails: 502 (the game keeps the report) and nothing is counted', async () => {
  ghFail = true;
  assert.equal((await post(crash())).status, 502);
  ghFail = false;
  assert.equal((await post(crash())).status, 201, 'the retry is not taken for a repeat');
});

test('prune: limits after 2 days, logs after 30, screenshots after 90', async () => {
  assert.equal((await post(crash({ screenshot_jpeg: JPEG.toString('base64'), log_gz: LOG.toString('base64') }))).status, 201);
  const count = table => env.DB.sqlite.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n;
  await prune(env.DB, NOW + 3 * 24 * 3600 * 1000);
  assert.equal(count('bf_report_sent'), 0);
  assert.equal(count('bf_report_shots'), 1);
  assert.equal(count('bf_report_logs'), 1);
  await prune(env.DB, NOW + 31 * 24 * 3600 * 1000);
  assert.equal(count('bf_report_logs'), 0);
  assert.equal(count('bf_report_shots'), 1);
  await prune(env.DB, NOW + 91 * 24 * 3600 * 1000);
  assert.equal(count('bf_report_shots'), 0);
  assert.equal(count('bf_report_signatures'), 0);
});

test('helpers', () => {
  assert.equal(neutralise('hi @TekRantGaming and a@b'), 'hi @​TekRantGaming and a@​b');
  assert.equal(allowedHtml('<script>x</script> ```<i>y</i>``` <details>'), '&lt;script&gt;x&lt;/script&gt; ```<i>y</i>``` <details>');
  assert.equal(allowedHtml('<!-- full log --> <!-- other -->'), '<!-- full log --> &lt;!-- other --&gt;');
});
