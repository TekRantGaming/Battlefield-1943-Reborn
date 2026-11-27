// The real relay route (bf_reports.js) on this PC, for testing the game against it: an in-memory database and a
// fake GitHub that writes each issue or comment it WOULD get to <out> instead. Nothing leaves the PC.
//   node local_relay.mjs <out folder> [port 18493]
//   game: BF_REPORT_ENDPOINT=http://127.0.0.1:18493
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { report, screenshot, wholeLog } from './bf_reports.js';

const out = process.argv[2];
const port = Number(process.argv[3] || 18493);
if (!out) throw new Error('usage: node local_relay.mjs <out folder> [port]');
fs.mkdirSync(out, { recursive: true });

const sqlite = new DatabaseSync(':memory:');
const db = {
  prepare(sql) {
    const statement = sqlite.prepare(sql.replace(/\?(\d+)/g, '?'));
    let values = [];
    const order = [...sql.matchAll(/\?(\d+)?/g)].map((m, i) => (m[1] ? Number(m[1]) - 1 : i));
    const bound = {
      bind(...args) { values = order.map(i => args[i]); return bound; },
      async run() { statement.run(...values); return { success: true }; },
      async first() { return statement.get(...values) ?? null; },
      async all() { return { results: statement.all(...values) }; }
    };
    return bound;
  }
};
const env = { DB: db, BF_REPORT_TOKEN: 'local-test-token', BF_REPORT_SALT: 'local-test-salt', BF_REPORT_GITHUB_API: 'https://fake-github.local' };
const json = (value, status = 200, headers = {}) =>
  new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json', ...headers } });

let next = 3001;
globalThis.fetch = async (url, init) => {
  if (!url.startsWith('https://fake-github.local/')) throw new Error(`refused: ${url}`);
  const route = url.slice('https://fake-github.local'.length);
  const body = init.body ? JSON.parse(init.body) : null;
  const m = /\/issues(?:\/(\d+))?(\/comments)?$/.exec(route);
  if (init.method === 'POST' && m && !m[1]) {
    const n = next++;
    fs.writeFileSync(path.join(out, `issue-${n}.json`), JSON.stringify({ request: `POST ${route} (fake GitHub)`, json: body }, null, 2));
    fs.writeFileSync(path.join(out, `issue-${n}.md`), `# ${body.title}\n\nLabels: ${body.labels.join(', ')}\n\n${body.body}`);
    console.log(`relay: WOULD CREATE issue #${n}: ${JSON.stringify(body.title)}, body ${body.body.length} chars`);
    return json({ number: n, html_url: `https://github.com/TekRantGaming/Battlefield-1943-Reborn/issues/${n}` }, 201);
  }
  if (init.method === 'GET' && m && m[1]) return json({ number: Number(m[1]), state: 'open', html_url: `https://github.com/x/issues/${m[1]}` });
  if (init.method === 'POST' && m && m[2]) {
    fs.writeFileSync(path.join(out, `comment-${m[1]}-${Date.now()}.md`), body.body);
    console.log(`relay: WOULD COMMENT on issue #${m[1]}`);
    return json({ id: 1 }, 201);
  }
  return json({ message: 'Not Found' }, 404);
};

http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const headers = new Headers();
  for (const [k, v] of Object.entries(req.headers)) headers.set(k, Array.isArray(v) ? v.join(',') : v);
  headers.set('CF-Connecting-IP', req.socket.remoteAddress || '');
  const url = `http://127.0.0.1:${port}${req.url}`;
  const request = new Request(url, { method: req.method, headers, body: req.method === 'POST' ? Buffer.concat(chunks) : undefined });
  let response;
  try {
    if (req.url === '/v1/bf/reports' && req.method === 'POST') response = await report(request, env, Date.now(), json);
    else if (req.url.startsWith('/v1/bf/reports/shot/') && req.method === 'GET') response = await screenshot(request, env, json);
    else if (req.url.startsWith('/v1/bf/reports/log/') && req.method === 'GET') response = await wholeLog(request, env, json);
    else response = json({ error: 'Not found' }, 404);
  } catch (error) {
    console.error(error);
    response = json({ error: 'Server error' }, 500);
  }
  console.log(`relay: ${req.method} ${req.url} -> ${response.status}`);
  res.writeHead(response.status, Object.fromEntries(response.headers));
  res.end(Buffer.from(await response.arrayBuffer()));
}).listen(port, '127.0.0.1', () => console.log(`relay: the real route on http://127.0.0.1:${port} (fake GitHub), writing to ${out}`));
