'use strict';
// The server owner's dashboard (server/dashboard.py, docs/DASHBOARD.md), drawn into the manager's Dashboard tab and
// the public stats page (public.html) alike. Everything a server or a player named goes in as text, never markup;
// the charts are plain SVG made here, so the page needs nothing from the internet.
(function () {
  const NS = 'http://www.w3.org/2000/svg';
  // The dashboard's look, shared by both pages (they define the colours: --paper, --ink, --card...).
  const style = document.createElement('style');
  style.textContent = `
    .dcards { display: grid; grid-template-columns: repeat(auto-fill, minmax(170px, 1fr)); gap: 12px; margin-bottom: 18px; }
    .dcard { background: var(--paper); color: var(--ink); padding: 12px 14px 13px; border-left: 5px solid #b9ad7c; border-radius: 2px; min-width: 0; }
    .dcard.green { border-left-color: #6e9a3c; } .dcard.amber { border-left-color: #d1a03a; } .dcard.red { border-left-color: #ce3e30; } .dcard.blue { border-left-color: #3d74b8; }
    .dcard small { display: block; font: 600 10.5px/1.2 Consolas, monospace; letter-spacing: .14em; text-transform: uppercase; color: #6b6450; }
    .dcard b { display: block; font: 600 26px/1.15 var(--head); letter-spacing: .03em; margin: 5px 0 2px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .dcard span { font-size: 12.5px; color: #6b6450; }
    .dsection { margin: 4px 0 10px; }
    .dservers { display: grid; grid-template-columns: repeat(auto-fill, minmax(330px, 1fr)); gap: 12px; margin-bottom: 18px; }
    .dsv { background: var(--card); border: 1px solid var(--line); padding: 14px 16px; border-radius: 2px; }
    .dsv-head { display: flex; align-items: center; gap: 9px; margin-bottom: 6px; } .dsv-head b { font: 600 19px var(--head); letter-spacing: .04em; flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .ddot { width: 10px; height: 10px; border-radius: 50%; background: #5d5d55; flex: none; } .ddot.running { background: #7dc24a; box-shadow: 0 0 0 3px rgba(125,194,74,.18); } .ddot.starting, .ddot.waiting, .ddot.restarting { background: #d1a03a; }
    .dpill { font: 600 10.5px/1 var(--body); letter-spacing: .12em; text-transform: uppercase; padding: 5px 8px; background: #3a4030; color: #b9ad7c; border-radius: 2px; }
    .dpill.green { background: #6e9a3c; color: #fff; } .dpill.amber { background: #d1a03a; color: #201a08; }
    .dfacts { display: flex; flex-wrap: wrap; gap: 4px 16px; color: #b9ad7c; font-size: 13.5px; } .dfacts b { color: var(--cream); font-weight: 600; }
    .dsmall { font-size: 12px; } .dmuted { color: #9d9578; } .dmono { font-family: Consolas, "DejaVu Sans Mono", monospace; font-size: 12.5px; white-space: nowrap; }
    .dround { margin-top: 9px; font-size: 12.5px; color: #b9ad7c; } .dround .dbar-track { display: block; margin-top: 4px; }
    .dchips { display: flex; flex-wrap: wrap; gap: 5px; margin-top: 10px; } .dchip { background: #14170f; color: var(--cream); padding: 3px 8px; font-size: 12.5px; border-radius: 2px; }
    .dchip small.ok { color: #a9d08a; } .dchip small.warn { color: #d1a03a; } .dchip small.bad { color: #f0b9ae; }
    .dgrid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; } .dwide { grid-column: 1 / -1; }
    .dpanel { background: var(--paper); color: var(--ink); padding: 16px 18px; border-radius: 2px; min-width: 0; }
    .dpanel h3 { color: var(--ink); font-size: 18px; margin-bottom: 8px; } .dpanel .dmuted { color: #6b6450; }
    .dpanel-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
    .dtabs button { background: #e3dabd; border: 0; padding: 5px 11px; margin-left: 4px; font: 600 12px var(--body); cursor: pointer; border-radius: 2px; color: var(--ink); }
    .dtabs button.on { background: #24251f; color: #f1ead6; }
    .dchart { width: 100%; height: auto; display: block; } .dgrid { stroke: #ddd3b4; stroke-width: 1; } .daxis { fill: #7a7259; font: 10.5px Consolas, monospace; }
    /* Blue lines (TekRantGaming, 20 Nov 2026: green was hard to see on the green page); a page's own accent may replace it. */
    .darea { fill: var(--chart-fill, rgba(61,116,184,.20)); } .dline { fill: none; stroke: var(--chart, #2f66a8); stroke-width: 2.4; stroke-linejoin: round; }
    .dpeak { fill: #ce3e30; } .dhit { fill: transparent; }
    .dpanel .dmuted.dsmall { margin: -4px 0 8px; }
    .dbars { display: grid; gap: 7px; } .dbar { display: grid; grid-template-columns: 120px 1fr 64px; gap: 10px; align-items: center; font-size: 13.5px; }
    .dbar-label { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; } .dbar-value { text-align: right; font: 12.5px Consolas, monospace; color: #6b6450; }
    .dbar-track { height: 10px; background: #e3dabd; border-radius: 2px; overflow: hidden; display: block; } .dbar-fill { display: block; height: 100%; }
    .dheat { display: grid; grid-template-columns: 34px repeat(24, 1fr); gap: 2px; font: 10px Consolas, monospace; color: #7a7259; }
    .dheat-c { height: 15px; background: #ebe3c9; border-radius: 1px; } .dheat-d { line-height: 15px; } .dheat-h { text-align: left; }
    .dscroll { overflow-x: auto; } .dtable { border-collapse: collapse; width: 100%; } .dtable td, .dtable th { text-align: left; padding: 6px 10px 6px 0; border-bottom: 1px solid #d9cfae; font-size: 13.5px; vertical-align: top; }
    .dtable th { font: 600 10.5px var(--body); letter-spacing: .12em; text-transform: uppercase; color: #6b6450; }
    .dtag { display: inline-block; font: 600 10px/1 var(--body); letter-spacing: .1em; padding: 3px 6px; border-radius: 2px; background: #6b6450; color: #fff; margin-left: 6px; }
    .dtable td > .dtag:first-child { margin-left: 0; }
    .dtag.ok { background: #6e9a3c; } .dtag.info { background: #3d74b8; } .dtag.warn { background: #d1a03a; color: #201a08; } .dtag.bad { background: #ce3e30; }
    @media (max-width: 860px) { .dgrid2 { grid-template-columns: 1fr; } .dservers { grid-template-columns: 1fr; } .dbar { grid-template-columns: 90px 1fr 56px; } }
  `;
  document.head.append(style);
  function h(tag, attrs, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') e.className = v; else if (k === 'text') e.textContent = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v); else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
    }
    for (const c of children.flat()) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
    return e;
  }
  function s(tag, attrs, ...children) {
    const e = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs || {})) if (v != null) e.setAttribute(k, v);
    for (const c of children.flat()) if (c != null) e.append(c.nodeType ? c : document.createTextNode(String(c)));
    return e;
  }
  const pad2 = n => String(n).padStart(2, '0');
  function dur(sec) {
    sec = Math.max(0, Math.round(sec || 0));
    const d = Math.floor(sec / 86400), hh = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
    return d ? d + 'd ' + hh + 'h' : hh ? hh + 'h ' + m + 'm' : m ? m + 'm' : sec + 's';
  }
  function clock(t) { const d = new Date(t * 1000); return pad2(d.getHours()) + ':' + pad2(d.getMinutes()); }
  function dayTime(t) { const d = new Date(t * 1000); return pad2(d.getDate()) + '/' + pad2(d.getMonth() + 1) + ', ' + clock(t); }
  function mb(bytes) { return bytes == null ? '-' : bytes >= 1 << 30 ? (bytes / (1 << 30)).toFixed(1) + ' GB' : Math.round(bytes / (1 << 20)) + ' MB'; }
  const COLORS = ['#8fb34f', '#d1a03a', '#3d74b8', '#ce3e30', '#9a6fc0', '#4fb3a5', '#c06f9a', '#b9ad7c'];

  // ---- charts
  function lineChart(points, opts) {
    const W = 640, H = opts.h || 170, L = 34, B = 22, T = 8, R = 8;
    const svg = s('svg', { viewBox: '0 0 ' + W + ' ' + H, class: 'dchart', role: 'img', 'aria-label': opts.label || 'chart' });
    if (!points.length) { svg.append(s('text', { x: W / 2, y: H / 2, 'text-anchor': 'middle', class: 'dmuted' }, 'Nothing recorded yet')); return svg; }
    const t0 = opts.from || points[0][0], t1 = opts.to || points[points.length - 1][0];
    // Whole steps on the axis: players count in ones, a percentage in fives.
    const raw = Math.max(opts.min || 4, ...points.map(p => p[1] || 0)) * 1.05;
    const step = opts.pct ? Math.max(5, Math.ceil(raw / 4 / 5) * 5) : Math.max(1, Math.ceil(raw / 4));
    const maxY = Math.min(opts.pct ? 100 : Infinity, step * 4);
    const x = t => L + (W - L - R) * (t1 > t0 ? (t - t0) / (t1 - t0) : 0), y = v => T + (H - T - B) * (1 - (v || 0) / maxY);
    for (let i = 0; i <= 4; i++) {
      const v = maxY * i / 4;
      svg.append(s('line', { x1: L, x2: W - R, y1: y(v), y2: y(v), class: 'dgrid' }));
      svg.append(s('text', { x: L - 5, y: y(v) + 4, 'text-anchor': 'end', class: 'daxis' }, Math.round(v) + (opts.pct ? '%' : '')));
    }
    for (let i = 0; i <= 5; i++) {
      const t = t0 + (t1 - t0) * i / 5;
      svg.append(s('text', { x: x(t), y: H - 6, 'text-anchor': i === 0 ? 'start' : i === 5 ? 'end' : 'middle', class: 'daxis' }, (t1 - t0) > 2 * 86400 ? dayTime(t).split(',')[0] : clock(t)));
    }
    const line = points.map((p, i) => (i ? 'L' : 'M') + x(p[0]).toFixed(1) + ' ' + y(p[1]).toFixed(1)).join(' ');
    svg.append(s('path', { d: line + ' L' + x(points[points.length - 1][0]).toFixed(1) + ' ' + y(0) + ' L' + x(points[0][0]).toFixed(1) + ' ' + y(0) + ' Z', class: 'darea' }));
    svg.append(s('path', { d: line, class: 'dline' }));
    const peak = points.reduce((a, p) => (p[1] > a[1] ? p : a), points[0]);
    if (peak[1] > 0) svg.append(s('circle', { cx: x(peak[0]), cy: y(peak[1]), r: 3.5, class: 'dpeak' }, s('title', {}, 'Peak ' + peak[1] + (opts.pct ? '%' : ' players') + ' at ' + dayTime(peak[0]))));
    points.forEach(p => svg.append(s('rect', { x: x(p[0]) - 3, y: T, width: 6, height: H - T - B, class: 'dhit' }, s('title', {}, dayTime(p[0]) + ': ' + (opts.pct ? Math.round(p[1]) + '%' : p[1] + ' players')))));
    return svg;
  }
  function bars(rows, opts) {
    if (!rows.length) return h('p', { class: 'dmuted', text: 'Nothing recorded yet.' });
    const max = Math.max(...rows.map(r => r[1]), 1);
    return h('div', { class: 'dbars' }, rows.map(([label, v], i) => h('div', { class: 'dbar' },
      h('span', { class: 'dbar-label', text: label }),
      h('span', { class: 'dbar-track' }, h('span', { class: 'dbar-fill', style: 'width:' + (100 * v / max).toFixed(1) + '%;background:' + COLORS[i % COLORS.length] })),
      h('span', { class: 'dbar-value', text: opts.fmt ? opts.fmt(v) : v }))));
  }
  function heat(grid) {
    const days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    const max = Math.max(1, ...grid.flat().filter(v => v != null));
    const table = h('div', { class: 'dheat' }, h('span'), Array.from({ length: 24 }, (_, i) => h('span', { class: 'dheat-h', text: i % 3 ? '' : pad2(i) })));
    grid.forEach((row, w) => {
      table.append(h('span', { class: 'dheat-d', text: days[w] }));
      row.forEach((v, hr) => table.append(h('span', { class: 'dheat-c', title: days[w] + ' ' + pad2(hr) + ':00 UTC: ' + (v == null ? 'no data' : v + ' players on average'),
        style: v == null ? '' : 'background:rgba(143,179,79,' + (0.12 + 0.88 * v / max).toFixed(2) + ')' })));
    });
    return table;
  }

  // ---- the parts
  function card(label, value, sub, accent) {
    return h('div', { class: 'dcard' + (accent ? ' ' + accent : '') }, h('small', { text: label }), h('b', { text: value }), sub ? h('span', { text: sub }) : null);
  }
  function serverCard(sv, now, isPublic) {
    const up = sv.state === 'running';
    const head = h('div', { class: 'dsv-head' }, h('span', { class: 'ddot ' + (sv.state || '') }), h('b', { text: sv.name || ('Server ' + sv.n) }),
      h('span', { class: 'dpill ' + (up ? 'green' : sv.state === 'stopped' ? '' : 'amber'), text: up ? 'Live' : sv.state === 'stopped' ? 'Stopped' : 'Starting' }));
    const facts = h('div', { class: 'dfacts' });
    if (up) {
      facts.append(h('span', {}, 'Map ', h('b', { text: sv.map || '?' }), sv.next_map && sv.next_map !== sv.map ? ' then ' + sv.next_map : ''));
      facts.append(h('span', {}, h('b', { text: (sv.players || 0) + ' / ' + (sv.max || '?') }), ' players'));
      if (sv.ping_avg != null) facts.append(h('span', {}, 'Ping ', h('b', { text: sv.ping_avg + ' ms' }), ' average'));
      if (sv.held) facts.append(h('span', {}, h('b', { text: sv.held.toFixed(1) }), ' of ' + (sv.tick || '?') + ' Hz held'));
      else if (sv.tick) facts.append(h('span', {}, h('b', { text: sv.tick + ' Hz' })));
      if (sv.since) facts.append(h('span', {}, 'Up ', h('b', { text: dur(now - sv.since) })));
    }
    const body = h('div', { class: 'dsv' }, head, facts);
    if (up && sv.round) {
      const left = Math.max(0, Math.min(1, sv.round.left));
      body.append(h('div', { class: 'dround' }, h('span', { text: 'Round ' + sv.round.round + ' · the losing side has ' + Math.round(left * 100) + '% of its tickets' }),
        h('span', { class: 'dbar-track' }, h('span', { class: 'dbar-fill', style: 'width:' + (100 * left).toFixed(1) + '%;background:' + (left < 0.25 ? '#ce3e30' : '#8fb34f') }))));
    }
    if (!isPublic && sv.procs) {
      const p = sv.procs, g = p.game || {}, ho = p.host || {};
      if (g.mem || ho.mem) body.append(h('div', { class: 'dfacts dsmall' }, h('span', {}, 'Game: CPU ', h('b', { text: g.cpu == null ? '-' : g.cpu + '%' }), ', ', h('b', { text: mb(g.mem) })),
        h('span', {}, 'Host: CPU ', h('b', { text: ho.cpu == null ? '-' : ho.cpu + '%' }), ', ', h('b', { text: mb(ho.mem) }))));
    }
    if (sv.roster && sv.roster.length) body.append(h('div', { class: 'dchips' }, sv.roster.map(r => h('span', { class: 'dchip', title: r.since ? 'On for ' + dur(now - r.since) : '' }, r.name,
      r.ping != null ? h('small', { class: r.ping > 150 ? 'bad' : r.ping > 90 ? 'warn' : 'ok', text: ' ' + r.ping + ' ms' }) : null))));
    else if (up) body.append(h('p', { class: 'dmuted', text: 'Nobody on it right now.' }));
    return body;
  }
  const EVENT = { join: ['JOIN', 'ok'], leave: ['LEAVE', ''], map: ['MAP', 'info'], restart: ['RESTART', 'bad'], server_up: ['UP', 'ok'], server_down: ['DOWN', 'warn'] };
  function feed(events, names) {
    if (!events.length) return h('p', { class: 'dmuted', text: 'Nothing has happened yet.' });
    return h('table', { class: 'dtable' }, h('tr', {}, ['Time', 'Event', 'Who', 'Where'].map(t => h('th', { text: t }))),
      events.map(e => h('tr', {}, h('td', { class: 'dmono', text: dayTime(e.t) }), h('td', {}, h('span', { class: 'dtag ' + (EVENT[e.kind] || ['', ''])[1], text: (EVENT[e.kind] || [e.kind])[0] })),
        h('td', { text: e.kind === 'join' || e.kind === 'leave' ? e.name + (e.played ? ' (' + dur(e.played) + ')' : '') :
                        e.kind === 'restart' ? 'Started again' + (e.why ? ': ' + e.why : '') : e.kind === 'map' ? 'Now ' + e.map :
                        e.kind === 'server_up' ? 'Server started' : e.kind === 'server_down' ? 'Server stopped' : e.name || '' }),
        h('td', { text: e.kind === 'map' ? names[e.server] || '' : (e.map || '') + (names[e.server] ? (e.map ? ' · ' : '') + names[e.server] : '') }))));
  }
  function leaderboard(rows, now) {
    if (!rows.length) return h('p', { class: 'dmuted', text: 'Nobody has played a minute yet.' });
    return h('div', { class: 'dscroll' }, h('table', { class: 'dtable' }, h('tr', {}, ['#', 'Player', 'Playtime', 'Sessions', 'Kills', 'Deaths', 'K/D', 'Favourite map', 'Last seen'].map(t => h('th', { text: t }))),
      rows.map((r, i) => h('tr', {}, h('td', { class: 'dmono', text: i + 1 }), h('td', {}, h('b', { text: r.name })), h('td', { text: dur(r.seconds) }), h('td', { text: r.sessions }),
        h('td', { text: r.kills || '-' }), h('td', { text: r.deaths || '-' }), h('td', { text: r.kills || r.deaths ? r.kd : '-' }), h('td', { text: r.map || '-' }),
        h('td', { class: 'dmono', text: now - r.last < 120 ? 'Playing now' : dayTime(r.last) })))));
  }
  function network(net) {
    if (!net) return h('p', { class: 'dmuted', text: 'Not on the main server list (LAN only), or it has not answered yet.' });
    return h('div', {}, h('p', { class: 'dmuted', text: net.players + ' players on ' + net.servers.length + ' listed servers, ' + net.online + ' signed in to Reborn.' }),
      h('div', { class: 'dscroll' }, h('table', { class: 'dtable' }, h('tr', {}, ['Server', 'Players', 'Map', 'Region', 'Tick'].map(t => h('th', { text: t }))),
        net.servers.map(r => h('tr', {}, h('td', {}, h('b', { text: r.name }), r.promoted ? h('span', { class: 'dtag info', text: 'TRUSTED' }) : r.official ? h('span', { class: 'dtag ok', text: 'REBORN' }) : null,
          r.description ? h('div', { class: 'dsmall dmuted', text: r.description }) : null),
          h('td', { text: r.players + ' / ' + r.max }), h('td', { text: r.map }), h('td', { text: r.region || '-' }), h('td', { text: r.tick ? r.tick + ' Hz' : '-' }))))));
  }

  // ---- the whole dashboard: render(root, data, {public, range})
  let range = '24h';
  function render(root, d, opts) {
    opts = opts || {};
    const on = k => !opts.sections || opts.sections[k] !== false;  // the community page's choice of sections
    const names = {};
    d.servers.forEach(sv => { names[sv.n] = sv.name; });
    const c = d.cards;
    const cards = h('div', { class: 'dcards' },
      card('Playing now', c.online + (c.slots ? ' / ' + c.slots : ''), d.servers.filter(x => x.state === 'running').length + ' server' + (d.servers.filter(x => x.state === 'running').length === 1 ? '' : 's') + ' live', 'green'),
      card('24 h peak', String(c.peak24), 'players at once'),
      card('Played, 24 h', c.hours24 + ' h', c.sessions24 + ' sessions'),
      card('Players tracked', String(c.tracked), 'in the last 90 days'),
      c.mvp ? card('MVP of the day', c.mvp.name, dur(c.mvp.seconds) + ' played today', 'amber') : null,
      c.top_killer ? card('Top gun, 7 days', c.top_killer.name, c.top_killer.kills + ' kills', 'red') : null,
      d.machine && d.machine.cpu != null ? card('This machine', Math.round(d.machine.cpu) + '% CPU', mb(d.machine.mem) + ' of ' + mb(d.machine.mem_total) + ' memory') : null,
      d.network ? card('Reborn network', d.network.players + ' playing', d.network.servers.length + ' servers listed', 'blue') : null);
    const servers = h('div', { class: 'dservers' }, d.servers.map(sv => serverCard(sv, d.now, opts.public)));
    const chartBox = h('div', {});
    const tabs = h('div', { class: 'dtabs' }, ['6h', '24h', '7d'].map(r => h('button', { class: r === range ? 'on' : '', onclick: () => { range = r; render(root, d, opts); } }, r)));
    chartBox.append(lineChart(d.series[range] || [], { label: 'Players over ' + range, from: d.now - { '6h': 21600, '24h': 86400, '7d': 604800 }[range], to: d.now }));
    const maps = Object.entries(d.maps || {});
    const half = on('maps') !== on('hours');  // one of the two: the whole width
    const sections = [
      on('players') ? h('div', { class: 'dpanel dwide' }, h('div', { class: 'dpanel-head' }, h('h3', { text: 'Players' }), tabs), chartBox) : null,
      on('maps') ? h('div', { class: 'dpanel' + (half ? ' dwide' : '') }, h('h3', { text: 'Maps played, 7 days' }), h('p', { class: 'dmuted dsmall', text: 'Player-minutes on each map.' }), bars(maps, { fmt: v => dur(v * 60) })) : null,
      on('hours') ? h('div', { class: 'dpanel' + (half ? ' dwide' : '') }, h('h3', { text: 'Busiest hours, 30 days (UTC)' }), heat(d.heatmap)) : null,
      d.health && d.health.length && !opts.public ? h('div', { class: 'dpanel dwide' }, h('h3', { text: 'This machine, 24 h: CPU' }),
        lineChart(d.health.map(p => [p[0], p[1] || 0]), { pct: true, min: 10, h: 130, from: d.now - 86400, to: d.now, label: 'CPU over 24 hours' })) : null,
      on('leaderboard') && (!opts.public || d.leaderboard.length || !d.page || d.public_page.names) ? h('div', { class: 'dpanel dwide' }, h('h3', { text: 'Veterans: the most playtime, 90 days' }), leaderboard(d.leaderboard, d.now)) : null,
      on('activity') ? h('div', { class: 'dpanel dwide' }, h('h3', { text: 'Recent activity' }), feed(d.events, names)) : null,
      on('network') ? h('div', { class: 'dpanel dwide' }, h('h3', { text: 'Reborn network now' }), network(d.network)) : null];
    root.replaceChildren(on('cards') ? cards : '', on('servers') ? h('h3', { class: 'dsection', text: 'Servers' }) : '', on('servers') ? servers : '',
                         h('div', { class: 'dgrid2' }, sections));
  }
  window.Dash = { render: render, h: h, dur: dur };
})();
