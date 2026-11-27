# Report an issue: the in game bug report form that files on GitHub

TekRantGaming, 16 Nov 2026: "it needs to have git hub integration for feedback with template to automatically submit
data give user text boxes n stuff you can expand upon this to better make this than i suggested".

Status: built in dev only. Nothing is on GitHub and nothing is deployed ("keep everything local for now, I will let
you know when we will put anything on git"). The game's default is the browser route with no relay address set; the
relay is tested against a local fake (tools/fake_report_relay.py) and a dry run.

Mockups: runs/ui-designs/build_report.py (out/r1_form.png, r2_review.png, r3_outcome.png).

## The pages

REPORT AN ISSUE on Home (and on the main menu once the game runs) opens three steps on one paper sheet:

1. **Your report.** Type of issue (Crash, Bug, Performance, Server or connection, Suggestion), Title (required, up to
   100), What happened (required, up to 2000), Steps to reproduce, What you expected, What happened instead (each
   optional, up to 1000), How often (Every time, Often, Sometimes, Once), and a Screenshot (optional). Suggestion asks
   "Your idea" and "Why it helps" instead of the steps and expected/actual.
   - Controller: up and down pick a field; left and right pick a type or how often; A opens the on screen keyboard
     at the right for a text field (QWERTY, numbers, punctuation, Shift, Space, New line, Delete, Done; LT shift, RT
     space, X delete, B done typing). RB goes on to step 2.
   - Keyboard: type straight in; Tab next field; Enter a new line in the long fields; Ctrl+Enter next step; Esc back.
   - Mouse: click a field, a chip, a key.
   - The draft is kept (user\reports\draft.json) if you leave the page or the game, until it is sent or cleared.
2. **What gets sent.** Every attached part with its own tick and its size, and the issue exactly as GitHub will show
   it (the same Markdown the relay or the browser gets). A ticks or unticks; RT and LT scroll the preview.
3. **Send.** Send now (the relay: no GitHub account), Open on GitHub with my account (the browser route), Copy the
   report (clipboard, for Discord). The result: sent (the issue's number, A opens it), already known (added to an
   open issue), or saved for later (kept, sent by itself at the next start; copy or GitHub instead).

## What is attached (each part can be unticked)

| Part | What | Ticked by default |
|---|---|---|
| Game | version, release or dev build, OS family, the game's tick rate, which Diagnostics logging was on | always |
| PC | Windows version and build, CPU name, cores and threads, RAM, GPU name and driver version | always |
| Display and graphics | resolution, window mode, refresh, renderer, the graphics settings (a fixed list of keys) | always |
| Last match | server name, official or community, map, players, when (never its address) | if one was played this run or the last |
| Crash report | the newest crash-*.txt or hang-*.txt from the last 7 days (text only; the minidump stays on the PC) | Crash type |
| Log | the last 400 lines (up to 40 KB) of bf1943.log, or of bf1943.previous.log when the last run crashed | not for Suggestion |
| Whole log | the same log whole (with this run's rotated parts), gzip, at most 1.4 MB compressed (else its start and newest part); kept 30 days by the relay and linked from the issue | not for Suggestion |
| Screenshot | the newest picture the game saved with F12 (user\screenshots), downscaled to 1280 wide JPEG | when there is one from the last day |
| Contact | the soldier name or a name the player types, if they want a reply | off (anonymous) |

The log of a run that crashed is kept: at each start bf1943.log is moved to bf1943.previous.log before the new one
opens (before this, the next start wrote over it).

### Scrubbed before anything is shown or sent (report.cpp Scrub; its cases in ScrubSelfTest)

Each one is replaced by a word in brackets, `[user]`, `[name]`, `[ip]`, `[recovery code]`, `[install]`, `[key]`,
`[id]`, `[email]` (brackets, not `<...>`, which GitHub would take for HTML). A line a rule cannot read is left out.

- The Windows user name (GetUserName and the user folder's name) and the PC name, anywhere in the text.
- Paths under the user folder: `C:\Users\<name>\...` becomes `C:\Users\[user]\...`; `/home/<name>/` the same.
- IPv4 and IPv6 addresses (127.0.0.1 kept, it tells us about local tests), and host:port pairs of them.
- The account recovery code (the settings' account_code, in any spacing or case) and its formatted form.
- The install id (install-id.txt), any 32+ character hex or base64 run (keys, tokens), Authorization and token=
  values, e-mail addresses.
- What the player types is scrubbed the same way (a pasted path or address).

### The log itself is anonymized as it is written (Settings > Diagnostics, 26 Nov 2026; bf/src/diagnostics.h)

A player's bf1943.log goes through the same rules on its way to the disk (the SDK's SetLogFileFilter, on a thread of
its own: the game's threads never wait for the disk or the rules), with more: the soldier name becomes `[you]`, other
players' names `[player N]` (from the host's player list and chat lines), addresses `[ip N]` (numbered per run, so
lines about one server still match up), and chat text is left out of the log. A report scrubs it again before
sending. A dedicated server's log is left as it is.

Settings > Diagnostics (all on by default, each can be turned off; at the next start):

| Area | What the log gets |
|---|---|
| Audio | every 5 s when sound went wrong: output underruns, XMA voices starved of input or unreadable, the game's mixer late; an all-clear line every minute; the device and its buffer at the start |
| Rendering | frame statistics every 5 s, the renderer's frames over 50 ms with what they waited for, the game's own frames over 100 ms, the graphics settings not at their defaults |
| Network | in a match, every 10 s: packets and bytes from each server, the longest gap, gaps over 150 ms, the ping; each gap over 400 ms |
| Input | the controls' settings and bindings not at their defaults (controllers' connections are always logged) |
| System | the system at the start (OS, CPU, RAM, GPU and driver, Wine/Proton); every minute the process's memory and peak, the PC's free memory, the game's own physical and virtual memory, the CPU, the game's file reads (count, MB, the slowest, over 100 ms), power and the CPU's clock; as they happen: a change to battery, the CPU held under 60% of its top clock while playing, memory over 95%, the window's focus and minimising |

Every area's lines carry the scene (starting, menus, loading, deploy screen, playing, paused, end of round, tutorial),
and each scene's end is logged with what happened in it: how long, frames over 50 ms and the longest, audio underruns,
network gaps. Network logging also times every online request (the backend's and HTTP): failed or slower than 2 s
each, and a count a minute. Voice chat logs its relay going silent (12 s; it sends every 5 s) and coming back.

Added 27 Nov 2026 (later; the engine designer's wish list):

- **Black box** (any area on; bf/src/diagnostics_blackbox.cpp): the last 30 s of every game frame (its time, the main
  thread's waits, file reads, packets in and out and the longest gap, audio underruns, sounds EA's mixer stopped) and
  every renderer frame (time between presents, GPU wait, the renderer's own work, pipelines made, draws skipped while
  compiling, texture loads with their size and time and the largest, HD copies) kept in memory, written to the log only
  on a trigger: a game frame over 100 ms while playing (at most one dump every 2 minutes, 10 a run), the player's mark
  key, the hang watchdog, a crash. A dump is a line a second, the worst 40 slow frames one by one, and the last 5 s of
  frame times (about 80 lines).
- **Mark key**: `keybind_mark_problem`, F10 by default (Settings > Controls > Bindings, "Mark a problem for the log"):
  "BF mark: the player marked a problem", a black box dump, a notice on screen for 3 s; at most one in 3 s and 30 a
  run. A report's table gets a Marks row (how many, the last one's time).
- **Scenes**: each scene's line also has the 1% and 0.1% lows, how many frames were unevenly paced, pipelines made (ready
  at once, compiled, failed), shader failures, texture loads, HD copies made and remade, swap chain resizes and
  rebuilds, how often EA's mixer looked overloaded; a long scene gets a "still" line every 10 minutes, and the last scene
  its line when the window closes.
- **System**: the minute line also has the video memory against the driver's budget (VK_EXT_memory_budget), the PC's
  CPUs (the game's cores and other programs'), page faults, the game's memory peak; a second line has the busiest
  threads by CPU, how long the game's threads waited for a CPU (Linux), and the main thread's longest waits by guest
  caller. As they happen: slow frames while other programs kept many cores busy, video memory near its budget.
- **Network**: our own sends to each server (their rate and gaps; "we sent nothing to ... for N ms"), and the ping
  sampled every second (its average, range and how much it moved).
- **Rendering** (the renderer's native_x_renderer_diagnostics): each pipeline that was ready over 50 ms after it was
  asked for, with its shaders (30 a minute); one line each time the game's pictures change and its patched textures
  are made again (why, how many, how long); swap chain resizes.
- **Memory**: each failed guest allocation with its size and how much of the heap was free (the first 20, then at each
  power of two).
- **Crashes on Linux**: crashes/crash-<time>.txt (the signal, the address, the thread, the host stack as module +
  offset) and hang-<time>.txt (every thread's stack), as Windows has its .txt and .dmp; both platforms write the black
  box into the log first.

Added 27 Nov 2026 (the second round, after the first match with the black box; bf/src/diagnostics_extra.cpp):

- **Build line** first in the log: the version servers are told, the commit and whether it had local changes (as of
  the build's CMake configure), dev or release, the ReXGlue SDK's version.
- **Run summary** when the window closes ("BF summary:"): the time in each kind of scene, the black box's triggers by
  cause and the worst five, joins and the deploy screen to alive, the totals below, warnings by kind.
- **Black box**: each slow frame says what it most likely was (HD copies, texture loads, pipelines, new shaders, the
  GPU and its busiest passes, the main thread waiting, the game waiting for the renderer, a slow file read, or the
  game's own work); the renderer's passes are timed on the GPU (native_x_renderer_gpu_sections, on with Rendering).
- **Ghosts** (the game's network objects): an object id the server named that this game does not have, or has without
  its object, with the id and the guest caller (30 a minute); the most objects known against the game's 8192. The dev
  build's "an object id not in the table" line is gone (it mixed these with "no object" references).
- **Rubber banding**: the view jumping over 2 m (and 8 times the usual move) in one frame on foot.
- **Input**: a press to the game's next input read and to the next frame (median, 95%, longest), presses too short
  for the game to see; controllers connected or disconnected in a match.
- **Display**: the refresh, the cap, V-Sync, the video driver and desktop; presents over 1.5 frames late. Variable
  refresh is not reported to the game.
- **Renderer queue**: the game's waits for room in the renderer's queue, the frames the renderer is behind.
- **Network**: each server's packet spacing (median, 95%) and the largest packet, packets over 1400 bytes. Packet loss
  and order are not knowable without the game's protocol.
- **Audio**: output devices added and removed; reads of the sound and music files over 50 ms; EA's "overloaded" checks
  that fell within 0.2 s of a slow frame.
- **Memory**: the largest free block of the game's memory (each minute), the memory at each round's end against the
  first round's.
- **Threads**: a warning when the game's threads wait over 15% of the time for a CPU while the PC is over 85% busy.
- **Renderer totals**: HD copies made again with the same picture as before (what a cache would save), the pipeline
  storage's additions this run and a failed write.
- **Warnings and errors** counted by kind per scene and for the run (our fix-ups firing show here).
- **Settings** changed while the game runs.
- tools/bf_log_summary.py: a log in one screen (the build, the summary, the scenes, marks and dumps with their causes,
  warnings by kind).

Added 27 Nov 2026 (the third round: "name everything directly", "make the verbosity modular"):

- **Levels**: each area's row (and a new Black box row) is Off or On, and **Logging verbosity** sets how much the
  areas that are on write: Normal (the default and the lowest a player can choose), Detailed or Verbose (settings file
  diag_verbosity = normal / detailed / verbose). Every Diagnostics row but the log's size applies at once, in the game (diagnostics.h
  ApplyNow: the levels read again, the renderer's switches set, the parts an area turns on started). Quiet remains for dev builds and settings files (an area's own value
  "quiet", "detailed" or "verbose" overrides the verbosity; diagnostics.h Level). Features ask for the level they need
  (Want) and scale their per-minute line limits with it (Scaled: none at Quiet, 4x Detailed, 20x Verbose). Quiet keeps
  the summaries (scene lines, the minute's health, the run's summary, the black box's worst 10 frames with their
  causes); Detailed adds every slow frame's whole time account; Verbose the 2 s around a trigger frame by frame. The
  renderer's lines follow the Rendering level (native_x_renderer_diagnostics_level; its slow frames from 100, 50, 33
  or 20 ms). BF_DEV_DIAG_LEVEL=<level> sets every area in dev builds.
- **Every millisecond named**: the renderer's thread is timed by command kind at its one dispatch (draws, clears,
  resolves, presents, frame end and the rest; native_x_renderer.h FrameSlice) and the frame end by step; the rest of
  its time is waiting for the game's commands. The game's frame thread is timed by our own hooks (OursTimer: our frame
  limit's wait, the SDK's VdSwap, look, tick blend, the pad hook, ...) and its kernel waits; the rest is the game's own
  code. A slow frame's cause is its largest named part, and Detailed prints the whole account.

Added 27 Nov 2026 (the gaps closed):

- **Audio threads**: EA's control step, the game's render callback (the SDK's audio worker) and the XMA decoder's work
  timed (rex/audio/diagnostics.h Timed): per black box frame, per second (the longest), per scene.
- **Every thread**: the process's threads sampled 4 times a second off the game's threads (CPU and waiting for a CPU),
  30 s kept: a dump names the busiest threads around its trigger.
- **The game's code by function** (Black box at Detailed and up): the game frame thread sampled about 1000 times a
  second (Linux: a real-time signal; Windows: a brief suspend), each sample named exactly from the binary's own symbol
  table (Linux dev builds; else the game's function table, "nearest"), a sample outside the game's code by its module
  and the game's function that called it ("libc.so.6 __sched_yield from the game's sub_821659B0"). The network's
  handling on that thread shows here as the game's functions.
- **GPU**: the game's own 3D view and the whole frame timed on the GPU beside the renderer's passes.

Log file size: Automatic (25 MB while any area is on, else 5 MB), Small (5 MB) or Large (200 MB); one older part
kept beside it (bf1943.1.log) when it is larger than 5 MB.

## How it reaches GitHub

A GitHub token never ships in the game. Two routes were compared:

| | A. Browser, prefilled issue form | B. Relay on our side |
|---|---|---|
| Player needs | a GitHub account, and to sign in on the PC | nothing |
| Steam Deck, Big Picture, controller only | awkward: a browser opens over the game | one press in the game |
| Size | a URL: kept under 8000 bytes, so the form text and the system table fit, the log does not (copied to the clipboard to paste) | up to 60 KB body (GitHub's limit is 65,536 characters) |
| Screenshot | the folder opens with the file selected, the player drags it in | stored by the relay, linked in the issue |
| Labels | from the issue form template (applied for anyone) | set by the relay |
| Spam control | GitHub's own (an account per issue) | ours: per install rate limits, size and content checks, duplicates |
| Replies | GitHub e-mails the player | only through the contact they chose to give |
| Needs his word | the templates pushed to the public repo | the templates, a bot account or app token, and the Worker deploy |

**Recommendation: B, with A always offered beside it and used whenever B is off or fails.** Most players have no
GitHub account and many play with a controller; one press with no sign in is what gets reports filed, and the relay
is where rate limits and duplicate merging can live. A stays for players who want to follow their issue by e-mail.

### The relay (built and tested locally only; deploy on his word)

`POST <bf_report_endpoint>/v1/bf/reports` (the plan: a route on Kong's Worker beside /v1/bf/pipelines; the backend in
server/ would do too). Headers X-BF-Install and X-BF-Version; JSON body `{type, title, body, labels, signature,
version, screenshot_jpeg?}` (the body is the Markdown the review page shows; `<!-- screenshot -->` in it is where the
relay puts the screenshot's link; screenshot_jpeg is base64).

- The token lives only in the Worker's secrets: a bot account's token (his pick; see the rollout plan, step 2), the
  bot having no access to the repository itself.
- Rate limits per install id (X-BF-Install, the random UUID the pipelines use): 3 an hour, 10 a day; per IP 20 a day,
  counted in memory with a daily salt, never stored.
- Checks: JSON shape, title 8 to 100 characters, body up to 60 KB, screenshot up to 400 KB and a real JPEG, at most 5
  links, `@` mentions broken (`@\u200b`) so a report cannot ping anyone, no HTML but `<details>`/`<summary>`.
- Duplicates: the same install and the same title and text within a day is refused (409). Crashes carry a signature
  (exception, faulting module and offset, the first game function): the relay keeps signature to issue number, and
  a crash already filed and open gets a short comment (version, GPU, OS) instead of a new issue, capped at 50.
- Labels: from-game, needs-triage, and the type's (crash, bug, performance, server, suggestion).
- Answers: 201 `{issue, url}`, 200 `{duplicate_of, url}`, 409, 413, 429 (with Retry-After), 5xx. The game keeps the
  report on anything but 200 and 201.

### The game's side

- `bf_report_endpoint`: the live relay, https://kk-launcher-telemetry.tekrantgaming.workers.dev, by default since 16
  Nov 2026 (v0.4.7 testing). Empty means Send now is hidden and the browser route is the main button. Test runs
  (tools/run_game.ps1) pass a closed local port, so no test files a real issue; `BF_REPORT_ENDPOINT` overrides both.
- `BF_REPORT_DRY_RUN=1` (dev and player builds): nothing leaves the PC; the request (headers and JSON) and the
  browser URL are written to user\reports\dry-run-<time>.json and .url, and the page says DRY RUN.
- Offline or failed: user\reports\pending-<time>.json, tried again at the next start and every 10 minutes on the menu,
  at most 5 times; Home's row shows "1 WAITING". Copy is always offered.

## The repo side (tools/github_issue_forms, not pushed)

Kept outside any `.github` folder until he says, then copied into the release repo's `.github/ISSUE_TEMPLATE/`:

- `ISSUE_TEMPLATE/crash.yml`, `bug.yml`, `performance.yml`, `server.yml`, `suggestion.yml`: issue forms whose field ids
  match the game's browser route (`what`, `steps`, `expected`, `actual`, `often`, `system`, `log`, `contact`), each
  with its labels; `config.yml` (blank issues off, Discord as a contact link).
- `labels.yml`: the labels and colours (crash, bug, performance, server, suggestion, from-game, needs-triage).

## Testing (all local)

- Dry run: `BF_REPORT_DRY_RUN=1`. Send now writes user\reports\dry-run-<time>.md (the issue as GitHub would show it),
  .json (the request body) and .http (with its headers); GitHub instead writes dry-run-<time>.url (the link and what
  was copied for the Log box). Nothing is opened or sent.
- The fake relay: `python -I tools/fake_report_relay.py --out runs/<name>/relay` and the game with
  `BF_REPORT_ENDPOINT=http://127.0.0.1:18490`. It answers as the real one will and writes the issue it WOULD create
  (issue-<n>.md, the API request in issue-<n>.json). `--fail` tests the kept report.
- Dev switches: `BF_DEV_REPORT_FILL=1` types a made up crash report; `BF_DEV_REPORT_SELFTEST=1` logs the scrubbing's
  cases (report.cpp ScrubSelfTest).

## Rollout plan (16 Nov 2026, "Yes, plan it"; each step waits for his word where marked)

Order: the issue forms first (the browser route needs them and works with no server), then the relay, then a game
release that points at it. Nothing here is done yet.

### 1. Issue forms and labels on the release repo (DONE 16 Nov 2026 at his "can you do it all")

Pushed to the default branch in-dev (a9b622c, 40bc47c: the forms' names quoted, the Bug form's colon had hidden
it), not main; the seven labels made (the existing bug label's description changed to ours); the repository variable
REPORT_BOT_LOGIN = bobandersonplexv2-ui. Checked: the New issue page lists the five forms and Discord, and a game
link fills every field (looked at, not submitted).

- Copy tools/github_issue_forms/ISSUE_TEMPLATE/*.yml into the release repo's `.github/ISSUE_TEMPLATE/` (on its
  default branch, in-dev, which GitHub reads them from) and push.
- Make the seven labels (labels.yml) with `gh label create`.
- Check: GitHub's "New issue" page shows the five forms and Discord; a dev build's "Open on GitHub with my account"
  (not a dry run) opens the Crash form filled in. Looked at only, never submitted.
- Note: config.yml turns blank issues off for players; he can still open one with the URL's template left out.

### 2. Who files the issues: a bot account (his pick, 16 Nov 2026: "Bot account")

What he does, with his own logins (nothing of it goes through a chat or a file):
- Make a second GitHub account for the reports (GitHub allows one free machine account a person), its own e-mail
  address and two factor sign in. His: bobandersonplexv2-ui (16 Nov 2026), REPORT_BOT_LOGIN on the release repo.
- Give it no access to the repository: anyone can open issues on a public repository, and without access a leaked
  token can never push code. GitHub then drops the labels the relay asks for, so the release repo's
  label-reports workflow (tools/github_issue_forms/workflows) sets them from the title, for the bot's issues only
  (its login in the repository variable REPORT_BOT_LOGIN).
- Make a classic token on the bot account, scope `public_repo` only, a year's expiry (a fine grained token cannot
  act on a repository another account owns, as far as I know: to be checked when it is made). A reminder to renew
  it before it runs out; the game keeps reports while the relay answers 503 or 502, so a lapse loses nothing.
- Type it into the Worker: `npx wrangler secret put BF_REPORT_TOKEN`, and a random `BF_REPORT_SALT` the same way.

### 3. The relay route on the Worker (BUILT and tested locally, 16 Nov 2026; deployed by the King Kong coordinator after his OK in its chat)

- server/report_relay/bf_reports.js, to go in as server-api/src/bf_reports.js beside bf_pipelines.js (its own route
  and tables, nothing of King Kong's touched); the usage.js lines, the secrets and the tests are in
  server/report_relay/README.md. POST /v1/bf/reports with the checks (shape, sizes, links, @ mentions, HTML, rate
  limits per install and per salted address hash, the same report in a day, a known open crash commented instead of
  filed again), then the GitHub API call; GET /v1/bf/reports/shot/<id>.jpg serves a report's screenshot.
- 26 Nov 2026 (TekRantGaming's OK for larger reports): `log_gz`, the whole log (gzip, at most 1.4 MB), kept in
  bf_report_logs for 30 days and linked where the game marked the issue (`<!-- full log -->`); a duplicate crash's
  comment links it too. GET /v1/bf/reports/log/<id>.txt serves it as text (Content-Encoding gzip). The request may now
  be 700 KB + the log's base64. Until the route is deployed again, the live relay answers 413 to a report with its
  whole log and the game sends it again without (report.cpp PostOnce); the usage.js line for the log route is in
  server/report_relay/README.md.
- D1 tables made by the module: bf_report_sent (install, salted address hash, content hash, time: pruned after 2
  days), bf_report_signatures (crash signature to issue, 90 days), bf_report_shots (the JPEG, 90 days),
  bf_report_logs (the whole log, 30 days).
- Tested: 9 tests with a fake GitHub (`npm test` in server/report_relay), and the dev game against the real code run
  locally (local_relay.mjs): filed as #3001 at the fake GitHub, its screenshot link served. No real issue made.
- First live check after the deploy: one report filed, looked at, then closed and deleted (his OK for that one issue).

### 4. The game (the next release)

- bf_report_endpoint's default set to the Worker's address in the release build; until the relay is live, releases
  ship with it empty (GitHub in the browser only).
- Before release: the Linux build (report.cpp's Linux paths are not built yet: screenshots are Windows only there,
  sending goes through curl), and the usual release tests with the form walked through.
- The privacy page (website) gets a line: what a report sends, that it is shown first, kept 90 days on our side.
- Release notes: "Report an issue now files bug reports on GitHub from the game".
