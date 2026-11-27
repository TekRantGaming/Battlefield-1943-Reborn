// Battlefield 1943 Reborn's Report an issue relay (TekRantGaming, 16 Nov 2026: the in game form files on GitHub;
// "Bot account" for who files). A route of its own for the King Kong coordinator's Worker (server-api), kept apart
// as bf_pipelines.js is: its own tables, its own prune, nothing of King Kong's touched. The design is
// docs/REPORTS.md; tools/fake_report_relay.py makes the same checks locally.
//
// POST /v1/bf/reports         the game's report (JSON), filed on GitHub as the bot account
// GET  /v1/bf/reports/shot/<id>.jpg   a report's screenshot, linked from its issue
// GET  /v1/bf/reports/log/<id>.txt    a report's whole log (gzip, shown as text), linked from its issue
//
// Secrets (wrangler secret put, typed by TekRantGaming, never in a file): BF_REPORT_TOKEN (the bot account's
// token), BF_REPORT_SALT (any long random text: the IP hashes' salt). Optional vars: BF_REPORT_REPO (default
// TekRantGaming/Battlefield-1943-Reborn), BF_REPORT_GITHUB_API (default https://api.github.com; tests point it at a
// fake). Nothing personal is kept: the install id (a random UUID) and a salted hash of the address are only used
// for the rate limits and are pruned after 2 days; reports themselves are not stored (they are on GitHub); the
// screenshots are kept 90 days, the whole logs 30 (anonymized by the game as they were written, and scrubbed again
// before sending: bf/src/diagnostics.h, report.cpp).

export const MAX_LOG = 1400 * 1024;        // the whole log's gzip (a D1 row holds 2 MB)
export const MAX_REQUEST = 700 * 1024 + Math.ceil(MAX_LOG / 3) * 4;  // the JSON with a screenshot and a log in base64
export const MAX_BODY = 60 * 1024;         // GitHub's issue body limit is 65,536 characters
export const MIN_TITLE = 8, MAX_TITLE = 120;
export const MAX_SHOT = 400 * 1024;        // the JPEG's bytes
export const MAX_LINKS = 5;                // links in what the player typed (above "### System")
export const PER_HOUR = 3, PER_DAY = 10;   // reports per install
export const PER_IP_DAY = 20;
export const MAX_DUP_COMMENTS = 50;        // a known crash: comments on its issue, then nothing more
export const SHOT_DAYS = 90, LOG_DAYS = 30, LIMIT_DAYS = 2;
const DAY = 24 * 3600 * 1000;
const TYPES = ['crash', 'bug', 'performance', 'server', 'suggestion'];
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const VERSION = /^[0-9A-Za-z.+-]{1,32}$/;
const SIGNATURE = /^[0-9a-f]{16}$/;
const SHOT_PATH = /^\/v1\/bf\/reports\/shot\/([0-9a-f]{32})\.jpg$/;
const LOG_PATH = /^\/v1\/bf\/reports\/log\/([0-9a-f]{32})\.txt$/;
const LOG_MARK = '<!-- full log -->';

let tableReady = false;
export function forgetBfReportTables() { tableReady = false; }

async function ensureTables(db) {
  if (tableReady) return;
  await db.prepare(`CREATE TABLE IF NOT EXISTS bf_report_sent (
    install_id TEXT NOT NULL, ip_hash TEXT NOT NULL, content TEXT NOT NULL, sent INTEGER NOT NULL)`).run();
  await db.prepare('CREATE INDEX IF NOT EXISTS bf_report_sent_install ON bf_report_sent (install_id, sent)').run();
  await db.prepare('CREATE INDEX IF NOT EXISTS bf_report_sent_ip ON bf_report_sent (ip_hash, sent)').run();
  await db.prepare(`CREATE TABLE IF NOT EXISTS bf_report_signatures (
    signature TEXT PRIMARY KEY, issue INTEGER NOT NULL, comments INTEGER NOT NULL, created INTEGER NOT NULL)`).run();
  await db.prepare(`CREATE TABLE IF NOT EXISTS bf_report_shots (
    id TEXT PRIMARY KEY, created INTEGER NOT NULL, data BLOB NOT NULL)`).run();
  await db.prepare(`CREATE TABLE IF NOT EXISTS bf_report_logs (
    id TEXT PRIMARY KEY, created INTEGER NOT NULL, data BLOB NOT NULL)`).run();
  tableReady = true;
}

async function sha256(text) {
  const bytes = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text)));
  return [...bytes].map(b => b.toString(16).padStart(2, '0')).join('');
}

// A report can never ping anyone (@name gets a zero width space) and carries no HTML but <details>, <summary> and
// the game's screenshot and whole log markers. Code blocks (``` fences) are left as they are: GitHub shows them as text.
export function neutralise(text) {
  return text.replace(/@(?=[A-Za-z0-9_-])/g, '@​');
}
export function allowedHtml(body) {
  const fix = tag => /^<\/?(details|summary)>$/.test(tag) || tag === '<!-- screenshot -->' || tag === LOG_MARK
    ? tag : tag.replace(/</g, '&lt;').replace(/>/g, '&gt;');
  return body.split('```').map((part, i) => i % 2 ? part : part.replace(/<!-- screenshot -->|<!-- full log -->|<[^>\n]{1,200}>/g, fix)).join('```');
}

function base64Bytes(text) {
  try {
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; ++i) bytes[i] = raw.charCodeAt(i);
    return bytes;
  } catch {
    return null;
  }
}

async function github(env, method, path, payload) {
  const base = (env.BF_REPORT_GITHUB_API || 'https://api.github.com').replace(/\/$/, '');
  const response = await fetch(base + path, {
    method,
    headers: {
      Authorization: `Bearer ${env.BF_REPORT_TOKEN}`,
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      'User-Agent': 'battlefield-1943-reborn-report-relay',
      ...(payload ? { 'Content-Type': 'application/json' } : {})
    },
    body: payload ? JSON.stringify(payload) : undefined
  });
  let data = null;
  try { data = await response.json(); } catch { /* no body */ }
  return { status: response.status, data };
}

export async function report(request, env, now, json) {
  if (!env.BF_REPORT_TOKEN || !env.BF_REPORT_SALT) return json({ error: 'The report service is not set up yet.' }, 503);
  const installId = (request.headers.get('X-BF-Install') || '').toLowerCase();
  const version = request.headers.get('X-BF-Version') || '';
  if (!UUID.test(installId) || !VERSION.test(version)) return json({ error: "The game's headers are missing." }, 400);
  const length = Number(request.headers.get('Content-Length') || '0');
  if (length > MAX_REQUEST) return json({ error: 'The report is too large.' }, 413);
  const text = await request.text();
  if (text.length > MAX_REQUEST) return json({ error: 'The report is too large.' }, 413);
  let req;
  try { req = JSON.parse(text); } catch { return json({ error: 'Not a report.' }, 400); }
  const { type, title, body } = req || {};
  if (!TYPES.includes(type) || typeof title !== 'string' || typeof body !== 'string')
    return json({ error: 'Not a report.' }, 400);
  if (title.trim().length < MIN_TITLE || title.length > MAX_TITLE)
    return json({ error: 'The title must be 8 to 120 characters.' }, 422);
  if (body.length > MAX_BODY) return json({ error: 'The report is too large.' }, 413);
  const typed = body.split('### System')[0];
  if ((typed.match(/https?:\/\//g) || []).length > MAX_LINKS) return json({ error: 'Too many links in the report.' }, 422);
  let shot = null;
  if (req.screenshot_jpeg) {
    shot = typeof req.screenshot_jpeg === 'string' ? base64Bytes(req.screenshot_jpeg) : null;
    if (!shot || shot.length > MAX_SHOT || shot[0] !== 0xff || shot[1] !== 0xd8 || shot[2] !== 0xff)
      return json({ error: 'The screenshot is too large or not a JPEG.' }, 413);
  }
  let log = null;
  if (req.log_gz) {
    log = typeof req.log_gz === 'string' ? base64Bytes(req.log_gz) : null;
    if (!log || log.length > MAX_LOG || log[0] !== 0x1f || log[1] !== 0x8b)
      return json({ error: 'The log is too large or not gzip.' }, 413);
  }
  const signature = SIGNATURE.test(req.signature || '') ? req.signature : '';

  const db = env.DB;
  await ensureTables(db);
  // The limits: per install, per salted address hash (the salt changes daily), the same report within a day.
  const ip = request.headers.get('CF-Connecting-IP') || '';
  const ipHash = (await sha256(`${env.BF_REPORT_SALT}|${Math.floor(now / DAY)}|${ip}`)).slice(0, 32);
  const content = await sha256(`${installId}|${title}|${typed}`);
  const counts = await db.prepare(`SELECT
      (SELECT COUNT(*) FROM bf_report_sent WHERE install_id = ?1 AND sent > ?2) AS hour,
      (SELECT COUNT(*) FROM bf_report_sent WHERE install_id = ?1 AND sent > ?3) AS day,
      (SELECT COUNT(*) FROM bf_report_sent WHERE ip_hash = ?4 AND sent > ?3) AS ip,
      (SELECT COUNT(*) FROM bf_report_sent WHERE content = ?5 AND sent > ?3) AS same`)
    .bind(installId, now - 3600 * 1000, now - DAY, ipHash, content).first();
  if (counts.same > 0) return json({ error: 'This report was already sent today.' }, 409);
  if (counts.hour >= PER_HOUR || counts.day >= PER_DAY || counts.ip >= PER_IP_DAY)
    return json({ error: 'Too many reports for now; it is kept and sent later.' }, 429, { 'Retry-After': '3600' });

  const repo = env.BF_REPORT_REPO || 'TekRantGaming/Battlefield-1943-Reborn';
  const origin = new URL(request.url).origin;
  // The whole log, kept LOG_DAYS and linked where the game marked it (a duplicate crash's comment gets the link too).
  let logLink = '';
  if (log) {
    const id = crypto.randomUUID().replace(/-/g, '');
    await db.prepare('INSERT INTO bf_report_logs (id, created, data) VALUES (?, ?, ?)').bind(id, now, log).run();
    logLink = `[The whole log](${origin}/v1/bf/reports/log/${id}.txt) (anonymized, kept ${LOG_DAYS} days)`;
  }
  // A crash already filed and still open: a comment on its issue (how many players it hits) instead of a new one.
  if (signature) {
    const known = await db.prepare('SELECT issue, comments FROM bf_report_signatures WHERE signature = ?').bind(signature).first();
    if (known && known.comments < MAX_DUP_COMMENTS) {
      const issue = await github(env, 'GET', `/repos/${repo}/issues/${known.issue}`);
      if (issue.status === 200 && issue.data && issue.data.state === 'open') {
        const details = body.split('### System').slice(1).join('### System').slice(0, 4000);
        const comment = await github(env, 'POST', `/repos/${repo}/issues/${known.issue}/comments`, {
          body: allowedHtml(neutralise(`Another player hit this crash (version ${version}).\n\n### System${details}`))
            .replace(LOG_MARK, logLink) + (logLink && !details.includes(LOG_MARK) ? `\n\n${logLink}` : '')
        });
        if (comment.status !== 201) return json({ error: 'GitHub did not take the report; it is kept and sent later.' }, 502);
        await db.prepare('INSERT INTO bf_report_sent (install_id, ip_hash, content, sent) VALUES (?, ?, ?, ?)')
          .bind(installId, ipHash, content, now).run();
        await db.prepare('UPDATE bf_report_signatures SET comments = comments + 1 WHERE signature = ?').bind(signature).run();
        return json({ duplicate_of: known.issue, url: issue.data.html_url }, 200);
      }
    }
  }

  let finalBody = allowedHtml(neutralise(body)).replace(LOG_MARK, logLink);
  if (logLink && !body.includes(LOG_MARK)) finalBody += `\n\n${logLink}`;
  if (shot) {
    const id = crypto.randomUUID().replace(/-/g, '');
    await db.prepare('INSERT INTO bf_report_shots (id, created, data) VALUES (?, ?, ?)').bind(id, now, shot).run();
    finalBody = finalBody.replace('<!-- screenshot -->', `![Screenshot](${origin}/v1/bf/reports/shot/${id}.jpg)`);
  }
  // The labels: a bot account without access to the repository cannot set them (GitHub drops them silently); the
  // repository's label-reports workflow sets them from the title's [Type].
  const created = await github(env, 'POST', `/repos/${repo}/issues`, {
    title: neutralise(title), body: finalBody, labels: [type, 'from-game', 'needs-triage']
  });
  if (created.status !== 201 || !created.data) {
    console.error('bf report: GitHub answered', created.status);
    return json({ error: 'GitHub did not take the report; it is kept and sent later.' }, 502);
  }
  await db.prepare('INSERT INTO bf_report_sent (install_id, ip_hash, content, sent) VALUES (?, ?, ?, ?)')
    .bind(installId, ipHash, content, now).run();
  if (signature)
    await db.prepare(`INSERT INTO bf_report_signatures (signature, issue, comments, created) VALUES (?, ?, 0, ?)
      ON CONFLICT(signature) DO UPDATE SET issue = excluded.issue, comments = 0, created = excluded.created`)
      .bind(signature, created.data.number, now).run();
  return json({ issue: created.data.number, url: created.data.html_url }, 201);
}

export async function screenshot(request, env, json) {
  const match = SHOT_PATH.exec(new URL(request.url).pathname);
  if (!match) return json({ error: 'Not found' }, 404);
  await ensureTables(env.DB);
  const row = await env.DB.prepare('SELECT data FROM bf_report_shots WHERE id = ?').bind(match[1]).first();
  if (!row) return json({ error: 'Not found' }, 404);
  // D1 gives a BLOB back as an array of numbers (as pipelines.js and bf_pipelines.js read it), not as bytes.
  return new Response(new Uint8Array(row.data), { headers: { 'Content-Type': 'image/jpeg', 'Cache-Control': 'public, max-age=86400' } });
}

// The whole log: its gzip as it came, the browser unpacks it (Content-Encoding) and shows the text. encodeBody
// 'manual': the Worker passes the bytes through as they are.
export async function wholeLog(request, env, json) {
  const match = LOG_PATH.exec(new URL(request.url).pathname);
  if (!match) return json({ error: 'Not found' }, 404);
  await ensureTables(env.DB);
  const row = await env.DB.prepare('SELECT data FROM bf_report_logs WHERE id = ?').bind(match[1]).first();
  if (!row) return json({ error: 'Not found' }, 404);
  return new Response(new Uint8Array(row.data), {
    encodeBody: 'manual',
    headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Content-Encoding': 'gzip', 'Cache-Control': 'private, max-age=3600',
               'X-Content-Type-Options': 'nosniff' }
  });
}

export async function prune(db, now) {
  await ensureTables(db);
  await db.prepare('DELETE FROM bf_report_sent WHERE sent < ?').bind(now - LIMIT_DAYS * DAY).run();
  await db.prepare('DELETE FROM bf_report_shots WHERE created < ?').bind(now - SHOT_DAYS * DAY).run();
  await db.prepare('DELETE FROM bf_report_logs WHERE created < ?').bind(now - LOG_DAYS * DAY).run();
  await db.prepare('DELETE FROM bf_report_signatures WHERE created < ?').bind(now - SHOT_DAYS * DAY).run();
}
