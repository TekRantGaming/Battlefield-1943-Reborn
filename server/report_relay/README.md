# The Report an issue relay route (for the King Kong coordinator's Worker)

The route the game's Report an issue page sends to (bf/src/report.cpp, `bf_report_endpoint`). Written and tested
here; it goes into the Worker's source (server-api) beside bf_pipelines.js, and is deployed only by the
King Kong coordinator session after TekRantGaming's OK in that session's own chat. Design: docs/REPORTS.md.

## Files

- `bf_reports.js`: the route. Copy to server-api/src/bf_reports.js. Its tables are made by the module itself
  (bf_report_sent, bf_report_signatures, bf_report_shots, bf_report_logs), as bf_pipelines.js does; schema.sql needs
  nothing.
- `bf_reports.test.js`: its tests (node --test, the Worker tests' in-memory D1, a fake GitHub). Copy to
  server-api/test/ with the import changed to `../src/bf_reports.js`.
- `local_relay.mjs`: the route on this PC with a fake GitHub, for testing the game against the real code
  (`node local_relay.mjs <out> 18493`, game `BF_REPORT_ENDPOINT=http://127.0.0.1:18493`).

## The lines in server-api/src/usage.js

```js
import * as bfReports from './bf_reports.js';
// in fetch, beside the /v1/bf/pipelines routes:
if (pathname === '/v1/bf/reports')
  return request.method === 'POST' ? await bfReports.report(request, env, now, json) : json({ error: 'Method not allowed' }, 405);
if (pathname.startsWith('/v1/bf/reports/shot/'))
  return request.method === 'GET' ? await bfReports.screenshot(request, env, json) : json({ error: 'Method not allowed' }, 405);
if (pathname.startsWith('/v1/bf/reports/log/'))
  return request.method === 'GET' ? await bfReports.wholeLog(request, env, json) : json({ error: 'Method not allowed' }, 405);
// in scheduled, after bfPipelines.prune, on its own:
try { await bfReports.prune(env.DB, Date.now()); } catch (error) { console.error(error); }
```

## Secrets (typed by TekRantGaming with `npx wrangler secret put <name>`, never in a file or a chat)

- `BF_REPORT_TOKEN`: the bot account's token (classic, scope `public_repo` only, a year's expiry).
- `BF_REPORT_SALT`: any long random text (the salt of the address hashes used for the rate limits).

Until both are set the route answers 503 and the game keeps reports to send later, so the route can be deployed first.

## Deployed (16 Nov 2026)

By the King Kong coordinator at TekRantGaming's word: server-api commit 7ddcb15, Worker version bd256a78, both secrets
set. POST https://kk-launcher-telemetry.tekrantgaming.workers.dev/v1/bf/reports. Its one change, copied back here:
screenshot() returns new Uint8Array(row.data) (real D1 gives a BLOB back as an array of numbers).

First live report (16 Nov 2026, at his "Yes, send it"): the dev game filed issue #34 as bobandersonplexv2-ui; the
label-reports workflow labelled it bug, from-game, needs-triage in 8 s; its screenshot link served the JPEG; no user
name, PC name, path, address or code in it. Closed as not planned.

## Tested (16 Nov 2026)

- `npm test` here: 9 tests pass (refusals before GitHub, filing with mentions and HTML made harmless, the screenshot
  stored and served, the same report refused, rate limits with Retry-After, a known open crash commented, a closed
  one filed again, GitHub failing gives 502 and counts nothing, the prune).
- The dev game against local_relay.mjs (run rep_real1): the report filed as issue #3001 at the fake GitHub, 36,963
  characters, the screenshot's link served (200, image/jpeg).
