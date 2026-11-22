"""Exploratory EA Plasma backend for the Xbox 360 build of Battlefield 1943: FESL (login, accounts) on 18241 and
Theater (game lists, joining) on 18246. Our own code.

The 360 client talks plain TCP (no ProtoSSL: on the console the Xbox security gateway encrypted the link) to the
address the port gives XOnlineGetServiceInfo (bf/src/online_services.cpp), port 18241. Packets:

    4 bytes   type ("fsys", "acct", ...)
    4 bytes   big endian: transmission type in the top byte (0xC0 request, 0x80 response, 0xF0 and 0xB0 multi
              part, 0x00 ping), packet id in the low 24 bits
    4 bytes   big endian total length, header included
    payload   "key=value" lines joined by "\\n", values with spaces in double quotes, a zero byte at the end

Theater uses the same framing; the 4 letters are the command (CONN, USER, ...), requests carry 0x40000000 and
replies 0, the request's TID comes back in the reply.

Every packet in and out is logged. Unknown transactions are logged and answered with their TXN (FESL) or TID
(Theater) only, so the next step the client asks for shows up in the log.

    python -I server/plasma.py [--theater-host 127.0.0.1] [--log runs/plasma.log] [--seconds N]
"""
import argparse
import asyncio
import base64
import datetime
import itertools
import json
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out
import bans  # noqa: E402
import gmhost  # noqa: E402
import player_stats  # noqa: E402
import stats  # noqa: E402
import text_filter  # noqa: E402

HEADER = 12
MULTI_LIMIT = 8064   # packets larger than this go as multi part (the client says fragmentSize=8096)
MULTI_CHUNK = 6000   # raw payload bytes per part
MULTI = (0xB0000000, 0xF0000000)  # multi part response, request
# One address can be several players (a household, a LAN party), so a join's state goes by who: (address, the player id
# the game says with "reserve"; '' for a game that says none, which shares that one). An address keeps its newest
# ADDRESS_RESERVATIONS picks, ADDRESS_PICKS of them counted as on their way, and ADDRESS_QUEUED places waiting in queues.
ADDRESS_RESERVATIONS = 64
ADDRESS_PICKS = 4
ADDRESS_QUEUED = 8
RESERVATIONS = {}  # who -> (GID, time): the server picked in our menu, for the next Quick Match
# A full server refuses new joins (TekRantGaming, 15 Nov 2026: players joining a full server; the game itself lets in up
# to 64, and a join over the game's own cap crashed a test server). A server is full when the players its host last
# reported (every 5 s) and the players who picked it in the last JOIN_WINDOW seconds and are still on their way reach
# its MAX-PLAYERS. PICKS keeps each player's first pick of a server (a renewal of the same pick keeps its place).
PICKS = {}  # who -> (GID, time of the first pick, the server's players then, seconds it counts as on the way)
JOIN_WINDOW = 30
# A pick whose game got the server's address (Theater EGAM) is in the host's player count from the host's next status
# on (its link is up within 2 s of the address: 424 joins on Hetzner, 17 Nov 2026). Until 0.4.7.4 it was counted as on
# its way for the whole JOIN_WINDOW: when another player left meanwhile, the count did not rise and the slot stayed
# held for nobody (99 of the 144 picks counted on their way in a "full" refusal there had their address already).
ARRIVED = {}  # who -> (GID, time its game got the server's address)
ARRIVED_COUNTED = 3  # a host's status this long after the address counts the player
# The join queue (TekRantGaming, 18 Nov 2026: "a server que system with a popup showing you your position in the que
# that updates there should also be a close button"). A game that says "queue": 1 in its "reserve" gets a place in the
# full server's queue instead of the refusal ({"op": "queued", "token", "position", "total"}) and asks again with
# "queue" every few seconds: its place, or "reserved" once one is free for it (its next Quick Match joins, as after
# "reserve"). A free slot goes to the queue's first player: nobody gets in past a waiting one (a game without the queue
# is refused as full while players wait, a pick of a server with room for everyone waiting and one more goes in).
# A place whose game stops asking for QUEUE_TIMEOUT seconds is given up, "queue_leave" gives it up at once (the pop-up's
# Close), and a pick of another server leaves it. A queue outlives its server's lost link for SERVER_GONE_GRACE
# seconds (the host registers again, as a new game: the queue follows it by the server's address).
QUEUE = []  # the waiting players, oldest first: {'token', 'gid', 'address', 'id', 'server', 'since', 'seen'}
QUEUE_TIMEOUT = 15
QUEUE_MOST = 64        # a server's queue holds at most this many
MOST_PLAYERS = 128     # lists of a server's players are cut at this
BIGGEST_SERVER = 64    # server sizes: 24 (the game's own) and less, 32, 64 (docs/SERVER-SIZES.md)
ADMIT_WINDOW = 90      # a player let in from the queue counts as on the way this long (from Home, the game starts first)
SERVER_GONE_GRACE = 30
GONE = {}  # GID -> when its server's link ended
NAMES = {}  # a name, lower case -> (player id, held until: None while its player is online), see free_name
LEADERBOARD = {}  # a name, lower case -> {'name', 'score', 'seconds', 'captures', 'at'}: the players' own totals
MATCH_SCORES = {}  # a game's GID -> {name, lower case -> {'name', 'score', 'at'}}: the scoreboard, from the players' games
LEADERBOARD_FILE = None  # where it is kept (--leaderboard), loaded at the start
# The website's totals (server/stats.py): players who joined a match, this month's, hours played. Kept in --stats.
STATS = stats.Stats()
PLAYER_STATS = player_stats.PlayerStats()  # each player's stats as the servers count them (player_stats.py)

# ---- Protect your name (TekRantGaming, 15 Nov 2026: "a secure your username option ... prevents anyone else from
# taking that name ... store long term progression tracking for the user", with no email and no setup from players).
# An account is an Ed25519 public key (the game makes the key from the player's recovery code, bf/src/account_key.cpp)
# with a name and the player's progress. Every account request is signed over a one time challenge from here
# ({"op": "challenge"}), so a request seen on the network cannot be used again and the code never leaves the player's
# PC. A protected name is the account's alone: a name check or a login under it from anyone who did not sign in for
# it gets a number (Name2), and stats for it are only taken signed. A name unused for NAME_UNUSED_DAYS is free again.
ACCOUNTS = {}       # public key (hex) -> {'key', 'name', 'created', 'seen', 'progress': {'stats', 'seconds',
                    #                      'score', 'captures', 'achievements'}}
ACCOUNT_NAMES = {}  # a protected name, lower case -> its account's key
ACCOUNTS_FILE = None  # --accounts: kept here (written within --accounts-save-every seconds of a change)
ACCOUNTS_DIRTY = [False]
CHALLENGES = {}     # nonce -> time given (a minute and a half to use it, once)
CHALLENGES_BY_IP = {}  # address -> [times]: at most 120 a minute
VERIFIED = {}       # key -> (player id, address, until): who signed in for the account's name, and from where
CLAIMS = {}         # address -> [times]: new and renamed names, at most CLAIMS_AN_HOUR
CLAIMS_AN_HOUR = 5
NAME_UNUSED_DAYS = 180
VERIFIED_FOR = 12 * 3600  # a sign in holds the name for the game's FESL login for this long (same id and address)

# ---- Team names (TekRantGaming, 16 Nov 2026: "permanently secure TekRantGaming and ZALIAS names and all variations of
# these names ... and provide a key for both users to keep that will allow them to use their names in the launcher").
# Each owner's account is the public half of the key their recovery code makes (the codes are only with them). It holds
# the owner's name, every name below and every lookalike of them (team_owner): nobody else can protect, sign in for or
# log in under one (they get Soldier with a number), stats for them come only signed by the owner, they never expire
# (account_active), and the owner plays under any of them with the same code (signed_name, claim). Lookalikes: any case,
# underscores, leetspeak (0 o, 1 i, l i, 3 e, 4 a, @ a, 5 s, 7 t, 2 z), doubled letters, anything before or after a
# "contains" stem, up to "typos" edits of a stem once prefixes and suffixes (xX, The, Real, Its, digits, TV...) are
# off, and "parts": a name made of a "before" word (or none), a "middle" word and an "after" word, with a slip in each
# (one edit each, two in all; with no "before" the middle exactly as written; the middle starting with the middle
# word's letter, or for a Z its lookalike S or its neighbour X; no letter of the middle taken as an "after" word's
# extra first letter), so
# ThatZalisGai, ThatSaliasGuy and ZaliaGai are ZALIAS's while ThatTallGuy, ThatSaladGuy and ZaidGuy are not. TekRantGaming's watchlist of variations is in "names", each entry with spaces,
# dots and hyphens left out (names are 3 to 15 letters, digits and underscores). --team-names FILE replaces this list.
TEAM_OWNERS = [
    {'owner': 'TekRantGaming', 'key': 'a183dac812d9732ea9982188b799feb4eb2f8e074977568b3f8f690a9c63b18a',
     'contains': ['TekRantGaming', 'TekRant', 'TekRG'], 'typos': {'TekRantGaming': 2},
     'names': ['TekRantGaming', 'TekRant', 'TekRG', 'TekRantG', 'TekRantGame', 'TekRantGames', 'TekRantGamer',
               'TekRantGamers', 'TekRantGamingTV', 'TekRantGamingOfficial', 'TekRantGamingUK', 'TekRantGamingDev',
               'TekRantGamingProject', 'TekRantGamingTeam', 'TekRantGamingCommunity', 'TekRantGamingNetwork',
               'TekRantGamingServer', 'TekRantGamingServers', 'TekRantGamingOrg', 'TekRantGamingCom',
               'Tek_Rant_Gaming', 'TekRant_Gaming', 'TekRantGaming_', '_TekRantGaming', 'TekRantGaming1',
               'TekRantGaming2', 'TekRantGaming01', 'TekRantGaming123', 'TekRantGamingX', 'xTekRantGaming',
               'TheTekRantGaming', 'RealTekRantGaming', 'ItsTekRantGaming', 'ImTekRantGaming', 'OfficialTekRantGaming',
               'TechRantGaming', 'TekRankGaming', 'TekRantGamng', 'TekRantGming', 'TekRantGamin', 'TekRantGamming',
               'TekRantGamimg', 'TekRantGamig', 'TekRantGaiming', 'TekRantGmaing', 'TekRantGamingg', 'TekRanttGaming',
               'TekkRantGaming', 'TekRanntGaming', 'TekRantGam1ng', 'TekRantG4ming', 'T3kRantGaming', 'TekR4ntGaming',
               'TekR@ntGaming', 'T3kR4ntG4ming', 'TekRantG4m1ng', 'TekRantGm', 'TekRGaming', 'TekRantGamez',
               'TekRantGmz', 'TekRantGng', 'TekRant_G', 'tekrantgamingcom', 'wwwtekrantgamingcom',
               'chattekrantgamingcom', 'tek_rant']},
    {'owner': 'ZALIAS', 'key': 'da315ef804e4b95c28113f41e930b9a9227a88b227f480dd43b52505193f3bfb',
     'contains': ['ZALIAS', 'ThatZalia', 'ZaliaGuy'],
     'typos': {'ZALIAS': 1},
     # ThatZaliaSGuy (16 Nov 2026, TekRantGaming: "do the same for the following name too ThatZaliaSGuy"): ZALIAS's
     # own other name, so under this key too; its 25073 entry variation list is caught by these rules but for the
     # generic short forms in TEAM_NAMES_LEFT_FREE (tools/test_accounts.py team checks a sample)
     'parts': {'before': ['That', 'Dat', 'Tha', 'Da', 'The', 'Tht', ''], 'middle': ['Zalias', 'Zalia'],
               'after': ['Guy', 'Gai', 'Gui', 'Guey', 'Guys', 'Dude', 'Boy', 'Man']},
     'names': ['ZALIAS', 'ZAlias', 'ZALIAS_', '_ZALIAS', 'ZALIAS1', 'ZALIAS01', 'ZALIAS123', 'ZALIASX', 'TheZALIAS',
               'ItsZALIAS', 'ImZALIAS', 'RealZALIAS', 'OfficialZALIAS', 'ZALIASOfficial', 'ZALIASGaming',
               'ZALIASGamer', 'ZALIASDev', 'ZALIASTeam', 'ZALIASProject', 'ZALIASNetwork', 'Z_A_L_I_A_S', 'Zaliass',
               'Zaliasss', 'Zallias', 'Zalais', 'Zaliasz', 'Zalials', 'Zalius', 'Zaleas', 'Zaleis', 'Zalies', 'Zalyas',
               'Zalyass', 'Zaliash', 'Zalias2', 'Zalias3', 'ZaliasYT', 'ZaliasTV', 'ZaliasUK', 'ZaliasGames',
               'ZaliasG', 'ZAL1AS', 'ZALlAS', 'Z4LIAS', 'Z@LIAS', 'Z4L1AS', 'ZALl4S', '2ALIAS', 'ZALIA5', 'ZALlA5',
               'ZALIASSSS', 'ZALIAScom', 'wwwzaliascom', 'z_alias', 'ThatZaliaSGuy', 'ThatZaliaGuy', 'ThatZalia',
               'TZalia', 'TZaliaGuy', 'ZaliaGuy', 'ThatZalGuy', 'ThatZaliGuy', 'DatZaliaGuy', 'ThatxZaliaxSxGu']},
]
# The watchlist's generic entries, left free for everyone (they would block normal players): TekRantGaming decides.
TEAM_NAMES_LEFT_FREE = ['TRG', 'TRGaming', 'TRGGaming', 'TR_Gaming', 'TekGaming', 'Tek_Gaming', 'RantGaming', 'Rant',
                        'Gaming', 'TechRant', 'Tech_Rant',
                        # ThatZaliaSGuy's short forms
                        'TZG', 'TZSG', 'TZAG', 'ThatZ', 'ThatZG', 'ThtZG', 'ThatZGuy', 'ZGuy', 'ZSGuy', 'TZGuy', 'TZSGuy',
                        'Zal', 'Zali', 'Zals', 'Zalia', 'ZalGuy', 'ZalsGuy', 'ZaliGuy', 'TZalGuy', 'ThatSGuy', 'ThatGuy']
TEAM = {'owners': [], 'by_key': {}, 'exact': {}, 'free': set(), 'cache': {}}  # set_team_names, below the accounts

# ---- The version players need (TekRantGaming, 13 Nov 2026: "server version check for latest version and only to allow
# latest version on and reject and prompt outdated clicnt connections give them a popup advising and a button to update
# now or close"). The newest release, from the same feed the game's own updater reads (--version-feed, asked every few
# minutes), or a version given by hand (--required-version). With neither there is no gate, as before v0.4.0.
#   - A game older than it is refused at "reserve", the step every join from our menus takes: {"op": "outdated",
#     "required", "yours"}. The game (v0.4.0 and newer) shows its update pop-up with Update now and Close.
#   - The server list says the version ("required_version"), so the game can ask before it tries.
#   - Quick Match gives a server only to a game our menu reserved one for, so a game that never asked is not let in.
#   - A game from before the gate says no version and has no pop-up: its server list is one line telling the player to
#     restart the game, which updates itself at the start.
# A release without a download for a platform (the game's updater looks for a file ending in windows-x64.zip or
# linux-x86_64.AppImage) asks nothing of that platform's players: they could not update to it.
VERSION_GATE = {'required': '', 'platforms': None, 'source': ''}  # platforms None: every platform
PLATFORM_FILES = (('windows', 'windows-x64.zip'), ('linux', 'linux-x86_64.AppImage'))


def version_parts(v):
    """'v0.2.0-beta.3' -> ([0, 2, 0], 3); the second is -1 without a suffix, 0 for a suffix without a number (the
    game's VersionParts, bf/src/update_files.cpp). A hotfix is the fourth number: '0.4.5.10' -> ([0, 4, 5, 10], -1)."""
    v = str(v).strip()
    if v[:1] in ('v', 'V'):
        v = v[1:]
    pre = -1
    if '-' in v:
        v, suffix = v.split('-', 1)
        digits = ''
        for c in suffix:
            if c.isdigit():
                digits += c
            elif digits:
                break
        pre = int(digits) if digits else 0
    # A hotfix is a fourth number (14 Nov 2026): 0.4.5 < 0.4.5.1 < 0.4.5.10 < 0.4.6, as the game.
    parts = []
    for piece in v.split('.'):
        digits = ''
        for c in piece.strip():
            if not c.isdigit():
                break
            digits += c
        parts.append(int(digits) if digits else 0)
    return parts, pre


def is_newer(a, b):
    """True when version a is newer than b, as the game's updater decides it (IsNewerVersion): the numbers first, a
    version with a suffix before the same numbers without one, suffixes with numbers counting up."""
    pa, pre_a = version_parts(a)
    pb, pre_b = version_parts(b)
    for i in range(max(len(pa), len(pb))):
        x = pa[i] if i < len(pa) else 0
        y = pb[i] if i < len(pb) else 0
        if x != y:
            return x > y
    if pre_a == pre_b:
        return False
    if pre_a < 0 or pre_b < 0:
        return pre_a < 0
    return pre_a > pre_b


def required_version(platform=''):
    """The version a player's game on `platform` ('windows', 'linux'; '' when it did not say) needs, or ''."""
    need = VERSION_GATE['required']
    platforms = VERSION_GATE['platforms']
    if need and platforms is not None and platform and platform not in platforms:
        return ''
    return need


def outdated(version, platform=''):
    """True when a game of `version` (None or '' when it did not say: one from before the gate) may not join."""
    need = required_version(str(platform or ''))
    return bool(need) and (not version or is_newer(need, str(version)))


def read_release(body):
    """A releases feed's answer (GitHub's releases/latest) as (version, platforms with a download)."""
    release = json.loads(body)
    version = str(release.get('tag_name', '')).strip()
    if version[:1] in ('v', 'V'):
        version = version[1:]
    names = [str(a.get('name', '')) for a in release.get('assets', []) if isinstance(a, dict)]
    platforms = {name for name, tail in PLATFORM_FILES if any(n.endswith(tail) for n in names)}
    return version, platforms


def fetch_feed(url):
    import urllib.request
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'bf1943-backend'})
    with urllib.request.urlopen(request, timeout=15) as reply:
        return reply.read(1 << 20)


async def watch_feed(url, every, note):
    """Keeps VERSION_GATE at the feed's newest release. An answer that cannot be read changes nothing (the last
    version known stays; none known yet: no gate), so a feed that is down never locks the players out."""
    while True:
        try:
            body = await asyncio.get_running_loop().run_in_executor(None, fetch_feed, url)
            version, platforms = read_release(body)
            if version and platforms:
                if version != VERSION_GATE['required'] or platforms != VERSION_GATE['platforms']:
                    note('version gate: players need v%s (%s) from %s' % (version, ', '.join(sorted(platforms)), url))
                VERSION_GATE.update(required=version, platforms=platforms, source=url)
            else:
                note('version gate: the feed names no release with a download (%r): unchanged' % version)
        except Exception as e:  # the network, the feed's answer: tried again at the next turn
            note('version gate: no answer from %s (%s: %s): unchanged' % (url, type(e).__name__, e))
        await asyncio.sleep(every)


def update_notice(mode):
    """A server list line for a game from before the gate, which has no update pop-up: its browser shows this."""
    need = VERSION_GATE['required']
    return {'gid': 'update-' + mode, 'name': 'UPDATE NEEDED: RESTART THE GAME', 'address': ['0.0.0.0', 0],
            'map': 'Wake Island', 'players': 0, 'max_players': 0, 'mode': mode, 'official': True, 'next_map': '',
            'maps': [], 'night': False,
            'description': 'This version can no longer join: version %s is out. Close the game and start it again, or '
                           'choose Update on the main menu. It updates itself.' % need}


# ---- Maintenance mode (TekRantGaming, 15 Nov 2026: maintenance periods that players learn about in the game, not only
# on Discord). A file the server manager writes (manager.py maintenance on|off, --maintenance-file here), read again
# when it changes: {"on", "kind", "text" (custom only), "until" (UTC seconds, 0: none), "allow" [addresses]}. While it
# is on, new connections are refused with its text: "reserve" (every join from our menus) answers {"op": "closed",
# "kind", "title", "message"}, Quick Match finds no server, and the list carries the notice (notice_kind, notice_title,
# notice_text, notice_seconds; a game from before it gets one row in place of the servers). Players in a match never
# ask again and are not touched, nor are the servers' registrations, logins and Direct connect's find. The addresses in
# "allow" (admins) are let in. With an end time the mode switches itself off then.
NOTICE_PRESETS = {
    'deploy': ('MAINTENANCE', 'Unable to connect: maintenance period. Check Discord for details and try again in a few '
                              'minutes.'),
    'update': ('UPDATE IN PROGRESS', 'Unable to connect: the servers are being updated to a new version. Check Discord '
                                     'for details and try again in a few minutes.'),
    'restart': ('SERVERS RESTARTING', 'Unable to connect: the servers are restarting. Try again in a few minutes.'),
    'emergency': ('EMERGENCY FIX', 'Unable to connect: the servers are down for an urgent fix. Check Discord for '
                                   'details and try again soon.'),
    'custom': ('MAINTENANCE', ''),
}
NOTICE_TEXT_MAX = 200
NOTICE_CHARS = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;!?\'()/&+%#@-')
MAINTENANCE = {'file': '', 'mtime': None, 'looked': 0.0, 'state': None, 'note': None}


def notice_text_problem(text):
    """Why a maintenance text cannot be shown to every player, or '' when it can. Characters every game font has, no
    quotes or braces (the games read the replies with a small parser), and never the word "reserved": a game from
    before this takes any answer to "reserve" that holds it as a yes."""
    if not text or len(text) > NOTICE_TEXT_MAX:
        return 'the text must be 1 to %d characters' % NOTICE_TEXT_MAX
    bad = sorted(set(c for c in text if c not in NOTICE_CHARS))
    if bad:
        return 'the text has characters that cannot be shown: %s' % ' '.join(repr(c) for c in bad)
    if 'reserved' in text.lower():
        return 'the text may not hold the word "reserved"'
    return ''


def clean_maintenance(raw):
    """A maintenance file's contents as the backend uses them, or None (off, or a file that does not check)."""
    if not isinstance(raw, dict) or raw.get('on') is not True:
        return None
    kind = str(raw.get('kind', 'deploy'))
    if kind not in NOTICE_PRESETS:
        return None
    title, text = NOTICE_PRESETS[kind]
    if kind == 'custom':
        text = ' '.join(str(raw.get('text', '')).split())
    if notice_text_problem(text):
        return None
    try:
        until = float(raw.get('until') or 0)
    except (TypeError, ValueError):
        until = 0.0
    allow = raw.get('allow', [])
    allow = {str(a)[:45] for a in allow if isinstance(a, str)} if isinstance(allow, list) else set()
    return {'kind': kind, 'title': title, 'text': text, 'until': until, 'allow': allow}


def maintenance_now(address=None):
    """The maintenance notice for a player at `address` now ({'kind', 'title', 'text', 'seconds'}), or None: off, past
    its end time, or the player is one of its allowed addresses. The file is looked at at most once a second."""
    path, now = MAINTENANCE['file'], time.time()
    if path and now - MAINTENANCE['looked'] >= 1.0:
        MAINTENANCE['looked'] = now
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            mtime = None
        if mtime != MAINTENANCE['mtime']:
            MAINTENANCE['mtime'] = mtime
            try:
                with open(path, encoding='utf-8') as f:
                    MAINTENANCE['state'] = clean_maintenance(json.load(f)) if mtime is not None else None
            except (OSError, ValueError):
                MAINTENANCE['state'] = None
    state = MAINTENANCE['state']
    note = MAINTENANCE['note']
    if state and state['until'] and now >= state['until']:
        state = None
    if note:
        word = ('on (%s%s)' % (state['kind'], ', until %s UTC' % time.strftime('%H:%M', time.gmtime(state['until']))
                               if state['until'] else '') if state else 'off')
        if word != note.get('said'):
            note['said'] = word
            note['say']('maintenance: %s' % word)
    if not state or (address and address in state['allow']):
        return None
    text, seconds = state['text'], 0
    if state['until']:
        seconds = max(1, int(state['until'] - now))
        minutes = (seconds + 59) // 60
        text += ' Expected back in about %d minute%s.' % (minutes, '' if minutes == 1 else 's')
    return {'kind': state['kind'], 'title': state['title'], 'text': text, 'seconds': seconds}


def notice_row(notice):
    """The server list's one row for a game from before maintenance mode (as update_notice): its browser shows it."""
    return {'gid': 'notice-' + notice['kind'], 'name': 'SERVERS CLOSED: ' + notice['title'], 'address': ['0.0.0.0', 0],
            'map': 'Wake Island', 'players': 0, 'max_players': 0, 'mode': 'classic', 'official': True, 'next_map': '',
            'maps': [], 'night': False, 'region': '', 'tick': 60, 'description': notice['text']}


def leaderboard_put(m, test_progression=False):
    """A player's totals from their game (our Stats & Awards): kept by name, the newest wins, at most 5,000 names."""
    name = player_stats.clean_name(m.get('name', ''))
    if not name:
        return False
    try:
        entry = {'name': name, 'score': int(float(m.get('score', 0))), 'seconds': int(float(m.get('seconds', 0))),
                 'captures': int(float(m.get('captures', 0))), 'at': int(time.time())}
    except (TypeError, ValueError, OverflowError):
        return False
    if test_progression and ('rank' in m or 'achievements' in m):
        rank, achievements = m.get('rank'), m.get('achievements')
        if (type(rank) is not int or not 1 <= rank <= 23 or not isinstance(achievements, list)
                or len(achievements) > 256
                or any(type(i) is not int or not 0 < i <= 0xffffffff for i in achievements)):
            return False
        entry.update(rank=rank, achievements=sorted(set(achievements)))
    if len(LEADERBOARD) >= 5000 and name.lower() not in LEADERBOARD:
        return False
    LEADERBOARD[name.lower()] = entry
    save_leaderboard()
    return True


def save_leaderboard():
    if LEADERBOARD_FILE:
        try:
            with open(LEADERBOARD_FILE + '.tmp', 'w', encoding='utf-8') as f:
                json.dump(list(LEADERBOARD.values()), f)
            os.replace(LEADERBOARD_FILE + '.tmp', LEADERBOARD_FILE)
        except OSError:
            pass


def leaderboard_top(count=50):
    rows = sorted(LEADERBOARD.values(), key=lambda e: (-e['score'], e['name'].lower()))
    return rows[:count], len(rows)


def who_asks(m, address):
    """The key of the player asking with "reserve" or "queue" (RESERVATIONS and the rest)."""
    return (address, bans.clean_id(m.get('id')))


def who_signed_in(table, address, xuid):
    """The key a signed in game's state is under: its player id's, else the one of a game that said no id."""
    who = (address, player_id_of(xuid))
    return who if who in table else (address, '')


def keep(table, who, value, most):
    """table[who] = value, with the newest `most` of that address kept."""
    table.pop(who, None)
    table[who] = value
    for old in [w for w in table if w[0] == who[0]][:-most]:
        del table[old]


def server_full(gid, who, ahead=None):
    """(full, players, arriving, most): whether one more player would go over the server's MAX-PLAYERS. The players
    waiting in its queue take their places first: `ahead` is how many wait before this one (None: every one of them,
    as for a player not in the queue)."""
    g = GAMES.get(gid, {})
    try:
        most = int(g.get('MAX-PLAYERS') or 24)
        players = int(g.get('AP') or 0)
    except (TypeError, ValueError):
        return False, 0, 0, 0
    now = time.time()
    # The picks still on their way: those of their window (JOIN_WINDOW, a queue's ADMIT_WINDOW) whose game has not got
    # the server's address and been counted by the host since (ARRIVED), less the players who arrived since the oldest
    # of them (each pick keeps the player count it was made at), so an arrived player is not counted twice.
    status_at = g.get('status_at', 0)
    pending = []
    for a, (pg, at, ap, window) in PICKS.items():
        if pg != gid or a == who or now - at >= window:
            continue
        got = ARRIVED.get(a)
        if got and got[0] == gid and got[1] >= at and status_at >= got[1] + ARRIVED_COUNTED:
            continue
        pending.append(ap)
    arriving = max(0, len(pending) - max(0, players - min(pending))) if pending else 0
    waiting = len([e for e in queue_line(gid) if (e['address'], e['id']) != who]) if ahead is None else ahead
    return players + arriving + waiting >= most, players, arriving, most


def queue_line(gid):
    """The players waiting for this server, oldest first."""
    return [e for e in QUEUE if e['gid'] == gid]


def queue_sweep(now=None):
    """The places given up: their game stopped asking (QUEUE_TIMEOUT), or their server is gone for good. Returns them
    with the reason."""
    now = now or time.time()
    out = []
    for e in list(QUEUE):
        why = 'its game stopped asking' if now - e['seen'] > QUEUE_TIMEOUT else \
            'the server is gone' if e['gid'] not in GAMES and now - GONE.get(e['gid'], now) > SERVER_GONE_GRACE else ''
        if why:
            QUEUE.remove(e)
            out.append((e, why))
    for gid in [k for k, at in GONE.items() if now - at > 600]:
        del GONE[gid]
    return out


def queue_entry(who):
    return next((e for e in QUEUE if (e['address'], e['id']) == who), None)


def queue_reply(e):
    line = queue_line(e['gid'])
    return {'op': 'queued', 'gid': e['gid'], 'token': e['token'], 'position': line.index(e) + 1, 'total': len(line)}


def let_in(gid, who, players, window=JOIN_WINDOW, renewal=False):
    """A player's reservation of a server: its next Quick Match joins it (and counts on its way meanwhile)."""
    now = time.time()
    if not renewal:
        keep(PICKS, who, (gid, now, players, window), ADDRESS_PICKS)
    keep(RESERVATIONS, who, (gid, now), ADDRESS_RESERVATIONS)


def give_up_own(who):
    """A refused player's reservation and place in a queue. Only their own: others may share the address."""
    RESERVATIONS.pop(who, None)
    mine = queue_entry(who)
    if mine:
        QUEUE.remove(mine)


def forget_old():
    """What the tables keep for players gone a while: a server that runs for weeks keeps no list of every login, every
    browser pick and every name it has seen. Run at each login."""
    now = time.time()
    for name in [n for n, held in NAMES.items() if held[1] is not None and held[1] <= now]:
        del NAMES[name]
    for who in [w for w, (_, at) in RESERVATIONS.items() if now - at > 300]:
        del RESERVATIONS[who]
    for who in [w for w, (_, at, _, _) in PICKS.items() if now - at > 300]:
        del PICKS[who]
    for who in [w for w, (_, at) in ARRIVED.items() if now - at > 300]:
        del ARRIVED[who]
    # A login's key: kept 10 minutes after its FESL connection closed (Theater reads it when the game links).
    for lkey in [k for k, at in Client.closed.items() if now - at > 600]:
        for table in (Client.sessions, Client.user_ids, Client.xuids, Client.addresses, Client.closed):
            table.pop(lkey, None)


def name_holder(name):
    """The player id holding a name now, or None."""
    held = NAMES.get(name.lower())
    if held and (held[1] is None or held[1] > time.time()):
        return held[0]
    NAMES.pop(name.lower(), None)
    return None


def free_name(name, pid, address=None):
    """The player's name, or it with the smallest number nobody else holds: TekRantGaming, TekRantGaming2, TekRantGaming3 (TekRantGaming, 7 Nov 2026:
    two players with the same name in a server). Names are 15 characters at most, as gamertags; a name differing only
    in case counts as the same one. A protected name (an account's) is free only to the player who signed in for it
    from this address."""
    name = name[:15]
    if name_free_for(name, pid, address):
        return name
    owner = team_owner(name)
    if owner and not signed_in_for(team_account(owner), pid, address):
        name = 'Soldier'  # a team name with a number is the team's too (team_owner)
        if name_free_for(name, pid, address):
            return name
    for n in range(2, 1000):
        suffix = str(n)
        candidate = name[:15 - len(suffix)] + suffix
        if name_free_for(candidate, pid, address):
            return candidate
    return name[:9] + str(random.randint(100000, 999999))


# ---- accounts (Protect your name, above)
def account_active(acct, now=None):
    if acct.get('key') in TEAM['by_key']:
        return True  # team names never expire
    return (now or time.time()) - acct.get('seen', 0) < NAME_UNUSED_DAYS * 86400


def protected_by(name):
    """The account whose protected name this is, or None (none, or unused for NAME_UNUSED_DAYS). A team name (any of an
    owner's, team_owner) is always its owner's account."""
    owner = team_owner(name)
    if owner:
        return team_account(owner)
    acct = ACCOUNTS.get(ACCOUNT_NAMES.get(name.lower(), ''))
    if acct and acct.get('name', '').lower() == name.lower() and account_active(acct):
        return acct
    return None


def signed_in_for(acct, pid, address):
    v = VERIFIED.get(acct['key'])
    return bool(v) and v[0] == pid and v[1] == address and v[2] > time.time()


def name_free_for(name, pid, address=None):
    acct = protected_by(name)
    if acct and not signed_in_for(acct, pid, address):
        return False
    holder = name_holder(name)
    if acct and holder not in (None, pid):
        # The owner signed in (from another PC, say): a 10 minute hold from an earlier name check gives way; a player
        # online under the name now does not (two players with one name break joining, Client.handle).
        return NAMES[name.lower()][1] is not None
    return holder is None or holder == pid


def valid_name(name):
    """A name the game's name page allows: 3 to 15 letters, digits and underscores, and since v0.4.7.4 one trademark
    sign at its end, all within 15 bytes of UTF-8 (bf/src/player_name.h)."""
    if not isinstance(name, str):
        return False
    core = name[:-1] if name.endswith(player_stats.TM) else name
    return (3 <= len(core) <= 15 and len(name.encode('utf-8')) <= 15 and
            all(c.isascii() and (c.isalnum() or c == '_') for c in core))


# ---- team names (TEAM_OWNERS, above)
_LOOKALIKE = str.maketrans({'0': 'o', '1': 'i', 'l': 'i', '|': 'i', '!': 'i', '3': 'e', '4': 'a', '@': 'a', '5': 's',
                            '$': 's', '7': 't', '2': 'z', '6': 'g', '9': 'g', '8': 'b'})
_PREFIXES = ('official', 'real', 'the', 'its', 'iam', 'im', 'mr', 'og', 'ttv', 'yt', 'xx', 'x')
_SUFFIXES = ('official', 'gaming', 'gamers', 'gamer', 'games', 'game', 'team', 'live', 'dev', 'ttv', 'tv', 'yt', 'uk',
             'us', 'hd', 'pro', 'xx', 'x', 'g')


def name_skeleton(text):
    """What a name looks like with case, separators, leetspeak and doubled letters taken out: T3kRantGaminggg and
    tek_rant_gaming are both tekrantgaming; Z4L1AS and Zallias are both zaias (l and i count as one letter)."""
    s = ''.join(c for c in str(text).lower() if c.isalnum() or c in '@$!|').translate(_LOOKALIKE)
    out = []
    for c in s:
        if not out or out[-1] != c:
            out.append(c)
    return ''.join(out)


def _cores(raw):
    """The name (lower case, separators out) with prefixes, suffixes and digits at either end taken off, in every
    order: xXTheZaliusTVXx gives zalius among others. As skeletons."""
    seen, todo = set(), [raw]
    while todo:
        s = todo.pop()
        if not s or s in seen or len(seen) > 64:
            continue
        seen.add(s)
        todo.append(s.strip('0123456789'))
        todo.extend(s[len(p):] for p in _PREFIXES if s.startswith(p))
        todo.extend(s[:-len(q)] for q in _SUFFIXES if s.endswith(q))
    return {name_skeleton(s) for s in seen}


def _edits(a, b, most):
    """The edits (a letter added, dropped, changed, or two side by side swapped) from a to b, or most + 1 when more."""
    if abs(len(a) - len(b)) > most:
        return most + 1
    prev2, prev = None, list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        cur = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1]))
            if i > 1 and j > 1 and a[i - 1] == b[j - 2] and a[i - 2] == b[j - 1]:
                cur[j] = min(cur[j], prev2[j - 2] + 1)
        if min(cur) > most:
            return most + 1
        prev2, prev = prev, cur
    return prev[-1]


def set_team_names(owners, left_free):
    """The team names in use: TEAM_OWNERS (or --team-names) indexed by skeleton."""
    TEAM.update(owners=[], by_key={}, exact={}, free={name_skeleton(n) for n in left_free}, cache={})
    for o in owners:
        key = str(o['key']).lower()
        if len(key) != 64:
            raise ValueError('a team name owner needs a 64 hex digit key: %s' % o.get('owner'))
        int(key, 16)
        e = {'owner': str(o['owner']), 'key': key,
             'contains': [name_skeleton(s) for s in o.get('contains', [])],
             'typos': [(name_skeleton(s), int(n)) for s, n in o.get('typos', {}).items()],
             'parts': {k: sorted({name_skeleton(w) for w in o['parts'].get(k, [])})
                       for k in ('before', 'middle', 'after')} if o.get('parts') else None}
        TEAM['owners'].append(e)
        TEAM['by_key'][key] = e
        for n in [o['owner']] + list(o.get('names', [])):
            TEAM['exact'].setdefault(name_skeleton(n), e)


def team_owner(name):
    """The team owner (TEAM_OWNERS) whose name this is or looks like, or None."""
    if not isinstance(name, str) or not name:
        return None
    raw = ''.join(c for c in name.lower()[:32] if c.isalnum() or c in '@$!|')
    cache = TEAM['cache']
    if raw in cache:
        return cache[raw]
    if len(cache) > 20000:
        cache.clear()
    cache[raw] = found = _team_owner(raw)
    return found


def _team_owner(raw):
    skel = name_skeleton(raw)
    if not skel or skel in TEAM['free']:
        return None
    if skel in TEAM['exact']:
        return TEAM['exact'][skel]
    for o in TEAM['owners']:
        if any(stem in skel for stem in o['contains']):
            return o
    cores = None
    for o in TEAM['owners']:
        for stem, most in o['typos']:
            if len(skel) < len(stem) - most:
                continue
            if cores is None:
                cores = _cores(raw)
            looks = set(cores)
            if len(stem) >= 8:  # a long name: also with anything around it (MyTekRankGamingFan)
                looks.update(skel[i:i + n] for n in range(len(stem) - most, len(stem) + most + 1)
                             for i in range(0, len(skel) - n + 1))
            # (Same first letter: Alias is not ZALIAS with a letter dropped. A short stem with no letter dropped
            # either: Zalia is a name of its own.)
            shortest = len(stem) - most if len(stem) >= 8 else len(stem)
            if any(c[:1] == stem[:1] and len(c) >= shortest and _edits(c, stem, most) <= most for c in looks):
                return o
    for o in TEAM['owners']:
        if o['parts'] and _in_parts(skel, o['parts']):
            return o
    return None


def _near(text, words):
    '''The fewest edits from text to one of the words (2 when more than one).'''
    return min([_edits(text, w, 1) for w in words] or [2])


def _in_parts(skel, parts):
    '''Whether the name is a "before" word (or none), a "middle" word and an "after" word, a slip in each (team_owner).'''
    middles = parts['middle']
    first = {w[:1] for w in middles} | ({'s', 'x'} if any(w[:1] == 'z' for w in middles) else set())  # S, 5, X for Z
    for j in range(len(skel) - 1, 0, -1):  # where the after word starts
        after = _near(skel[j:], parts['after'])
        if after > 1 or skel[j + 1:] in parts['after']:  # (ThatSaladGuy: the D is Salad's, not a slip of Guy)
            continue
        for i in range(0, j):  # where the middle word starts
            if skel[i] not in first or j - i < min(len(w) for w in middles) - 1:
                continue
            before = 0 if i == 0 and '' in parts['before'] else _near(skel[:i], [w for w in parts['before'] if w])
            middle = _near(skel[i:j], middles)
            if before <= 1 and middle <= (1 if i else 0) and before + middle + after <= 2:
                return True
    return False


def team_account(owner):
    """The owner's account, made the first time (it never expires: account_active)."""
    acct = ACCOUNTS.get(owner['key'])
    if acct is None:
        now = int(time.time())
        acct = ACCOUNTS[owner['key']] = {'key': owner['key'], 'name': owner['owner'], 'created': now, 'seen': now,
                                         'progress': {}}
        accounts_changed()
    return acct


def keep_team_names():
    """At the start: each owner has an account under one of their names, and no other account keeps a team name (one
    protected before the names were kept for the team loses it)."""
    for o in TEAM['owners']:
        acct = team_account(o)
        if team_owner(acct.get('name', '')) is not o:
            acct['name'] = o['owner']
            accounts_changed()
        ACCOUNT_NAMES[acct['name'].lower()] = o['key']
    for key, acct in ACCOUNTS.items():
        owner = team_owner(acct.get('name', ''))
        if owner and owner['key'] != key:
            if ACCOUNT_NAMES.get(acct['name'].lower()) == key:
                del ACCOUNT_NAMES[acct['name'].lower()]
            acct['name'] = ''
            accounts_changed()


set_team_names(TEAM_OWNERS, TEAM_NAMES_LEFT_FREE)


def accounts_changed():
    ACCOUNTS_DIRTY[0] = True


def save_accounts():
    if not ACCOUNTS_FILE or not ACCOUNTS_DIRTY[0]:
        return
    ACCOUNTS_DIRTY[0] = False
    try:
        with open(ACCOUNTS_FILE + '.tmp', 'w', encoding='utf-8') as f:
            json.dump({'version': 1, 'accounts': list(ACCOUNTS.values())}, f)
        os.replace(ACCOUNTS_FILE + '.tmp', ACCOUNTS_FILE)
    except OSError:
        ACCOUNTS_DIRTY[0] = True


def load_accounts(path):
    try:
        data = json.load(open(path, encoding='utf-8'))
    except (OSError, ValueError):
        return 0
    for a in data.get('accounts', []) if isinstance(data, dict) else []:
        try:
            key = str(a['key'])
            if len(key) != 64:
                continue
            int(key, 16)
        except (KeyError, TypeError, ValueError):
            continue
        ACCOUNTS[key] = a
        if a.get('name'):
            other = ACCOUNTS.get(ACCOUNT_NAMES.get(a['name'].lower(), ''))
            if not other or other.get('seen', 0) < a.get('seen', 0):
                ACCOUNT_NAMES[a['name'].lower()] = key
    return len(ACCOUNTS)


async def keep_accounts_saved(every):
    while True:
        await asyncio.sleep(every)
        save_accounts()


async def keep_stats_saved(every):
    """The website's totals (--stats), written when they changed: a backend stopped hard loses at most this long."""
    while True:
        await asyncio.sleep(every)
        STATS.save()


def challenge(address):
    """A one time nonce for a signed request; at most 120 a minute for one address."""
    now = time.time()
    times = [t for t in CHALLENGES_BY_IP.get(address, []) if now - t < 60]
    if len(times) >= 120:
        CHALLENGES_BY_IP[address] = times
        return ''
    times.append(now)
    CHALLENGES_BY_IP[address] = times
    if len(CHALLENGES) > 5000:
        for n in [n for n, t in CHALLENGES.items() if now - t > 90]:
            del CHALLENGES[n]
        for a in [a for a, ts in CHALLENGES_BY_IP.items() if not ts or now - ts[-1] > 60]:
            del CHALLENGES_BY_IP[a]
    nonce = os.urandom(16).hex()
    CHALLENGES[nonce] = now
    return nonce


def check_signed(m, op):
    """The signed request's key and fields after "op|nonce|", or None: a signature by that key over the text, on a
    challenge this backend gave in the last 90 seconds and never used before."""
    import ed25519
    key, sig, text = m.get('key'), m.get('sig'), m.get('signed')
    if not (isinstance(key, str) and isinstance(sig, str) and isinstance(text, str)) or len(text) > 8192:
        return None
    parts = text.split('|')
    if len(parts) < 2 or parts[0] != op:
        return None
    given = CHALLENGES.pop(parts[1], None)
    if given is None or time.time() - given > 90:
        return None
    try:
        public, signature = bytes.fromhex(key), bytes.fromhex(sig)
    except ValueError:
        return None
    if len(public) != 32 or not ed25519.verify(public, text.encode('utf-8'), signature):
        return None
    return key.lower(), parts[2:]


def parse_progress(fields):
    """A signed stats_put's fields: name, score, seconds, captures, stats ("Label=events,points;..."), achievements
    ("1,7,..."). None when any is not as the game sends it."""
    if len(fields) != 6:
        return None
    name, score, seconds, captures, stats_text, ach_text = fields
    try:
        score, seconds, captures = int(score), int(seconds), int(captures)
    except ValueError:
        return None
    if not (0 <= seconds < 10 ** 10 and 0 <= captures < 10 ** 9 and abs(score) < 10 ** 12):
        return None
    stats = {}
    for item in stats_text.split(';') if stats_text else []:
        label, _, value = item.partition('=')
        events, _, points = value.partition(',')
        if (not 1 <= len(label) <= 48 or not all(c.isascii() and (c.isalnum() or c == '_') for c in label)
                or not events.isdigit() or len(events) > 9 or not points.lstrip('-').isdigit() or len(points) > 13):
            return None
        stats[label] = '%d,%d' % (int(events), int(points))
    if len(stats) > 128:
        return None
    achievements = []
    for item in ach_text.split(',') if ach_text else []:
        if not item.isdigit() or not 0 < int(item) <= 0xFFFFFFFF:
            return None
        achievements.append(int(item))
    if len(achievements) > 256:
        return None
    return {'name': name, 'score': score, 'seconds': seconds, 'captures': captures, 'stats': stats,
            'achievements': sorted(set(achievements))}


def progress_text(p):
    """The account's progress for the game, as one line it reads easily: score|seconds|captures|stats|achievements,
    the stats and awards as in a signed stats_put."""
    return '%d|%d|%d|%s|%s' % (int(p.get('score', 0)), int(p.get('seconds', 0)), int(p.get('captures', 0)),
                               ';'.join('%s=%s' % kv for kv in sorted(p.get('stats', {}).items())),
                               ','.join(str(a) for a in p.get('achievements', [])))


def merge_progress(acct, got):
    """The account's progress and the totals a game sent: the larger of each (totals only grow; a new PC that has not
    yet taken the account's progress back must not lower it), the awards together."""
    p = acct.setdefault('progress', {})
    p['seconds'] = max(int(p.get('seconds', 0)), got['seconds'])
    p['captures'] = max(int(p.get('captures', 0)), got['captures'])
    p['score'] = max(int(p.get('score', 0)), got['score'])
    stats = p.setdefault('stats', {})
    for label, value in got['stats'].items():
        n, pts = (int(x) for x in value.split(','))
        if label in stats:
            on, opts = (int(x) for x in stats[label].split(','))
            n, pts = max(n, on), max(pts, opts)
        elif len(stats) >= 256:
            continue
        stats[label] = '%d,%d' % (n, pts)
    p['achievements'] = sorted(set(p.get('achievements', [])) | set(got['achievements']))
    return p


GAMES = {}  # GID -> game fields: --game (a dedicated server started with --server), later Theater CGAM
# Game wide stats (owner type 11). global_kills unlocks Coral Sea at 1,000,000 (Arcadia's notes). pm_minplayers
# is read with it: 0.0 hangs the game's main thread right after the reply (run qm3), so it is left empty.
STAT_DEFAULTS = {'global_kills': os.environ.get('BF_GLOBAL_KILLS', '0.0'), 'pm_minplayers': os.environ.get('BF_PM_MINPLAYERS', '')}
REQUEST, RESPONSE = 0xC0000000, 0x80000000


def from_wire(text_):
    """A text field the game sent as UTF-8 bytes (read as Latin-1 by parse_payload) as the text it is: a soldier name's
    trademark sign (since v0.4.7.4). Anything that is not UTF-8 stays as it was read."""
    try:
        return text_.encode('latin-1').decode('utf-8')
    except UnicodeError:
        return text_


def wire(text_):
    """Text as the game's bytes: a character Latin-1 has as its one byte (what parse_payload read), any other (a
    soldier name's trademark sign) as UTF-8, as the game holds it."""
    try:
        return text_.encode('latin-1')
    except UnicodeEncodeError:
        return b''.join(c.encode('latin-1') if ord(c) < 256 else c.encode('utf-8') for c in text_)


def parse_payload(data):
    out = {}
    for line in data.rstrip(b'\0').decode('latin-1').split('\n'):
        if '=' in line:
            k, v = line.split('=', 1)
            if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                v = v[1:-1]
            out[k] = v
    return out


def build(kind, transmission, packet_id, fields):
    lines = ['%s=%s' % (k, '"%s"' % v if ' ' in str(v) else v) for k, v in fields.items()]
    body = wire('\n'.join(lines) + '\0')
    return kind.encode() + struct.pack('>II', (transmission & 0xFF000000) | (packet_id & 0xFFFFFF), HEADER + len(body)) + body


class Client:
    """One FESL connection."""
    ids = itertools.count(11)  # user ids; distinct from PIDs and LIDs, so logs show which is which
    sessions = {}  # lkey -> persona name, for Theater's USER
    user_ids = {}  # lkey -> user id
    xuids = {}  # lkey -> the console's XUID (decimal, from NuXBL360Login)
    addresses = {}  # lkey -> the address it logged in from (moderation: asker checks a "reserve"'s id against it)
    closed = {}  # lkey -> when its FESL connection closed (forget_old drops its entries 10 minutes later)
    online = set()  # the personas of the FESL connections open now (a name is free again when its player leaves)
    name_grace = 60  # seconds a player's name stays theirs after their connection closes (a reconnect keeps it)
    pnow_ids = itertools.count(1)

    def __init__(self, reader, writer, args, log):
        self.reader, self.writer, self.args, self.log = reader, writer, args, log
        self.peer = '%s:%d' % writer.get_extra_info('peername')[:2]
        self.user_id = next(Client.ids)
        self.persona = None
        self.holds_name = False
        self.lkey = None
        self.multi_id, self.multi_data, self.multi_b64 = None, b'', 0

    def note(self, text):
        line = '%s %s %s' % (time.strftime('%H:%M:%S'), self.peer, text)
        print(line, flush=True)
        self.log.write(line + '\n')
        self.log.flush()

    async def send(self, kind, transmission, packet_id, fields):
        self.note('<- %s %08X id %d %s' % (kind, transmission, packet_id, fields))
        packet = build(kind, transmission, packet_id, fields)
        if len(packet) <= MULTI_LIMIT:
            self.writer.write(packet)
        else:
            # Multi part: each part carries a base64 chunk of the payload ("=" written as %3d), the total base64
            # size and the decoded payload size.
            payload = packet[HEADER:]
            chunks = [base64.b64encode(payload[i:i + MULTI_CHUNK]).decode().replace('=', '%3d')
                      for i in range(0, len(payload), MULTI_CHUNK)]
            total = sum(len(c) for c in chunks)
            for c in chunks:
                self.writer.write(build(kind, MULTI[0], packet_id, {'data': c, 'size': total,
                                                                    'decodedSize': len(payload)}))
            self.note('   (sent as %d parts)' % len(chunks))
        await self.writer.drain()

    # Keepalive: the client sends none of its own and drops a connection that stays silent for 4 minutes (TekRantGaming, kicked
    # 4:00 after joining every time: "BF GM conn err -206", "TheaterConn broken", then a goodbye; run kick2). EA's
    # servers pinged it: FESL with an fsys Ping request, Theater with a PING.
    KEEPALIVE_SECONDS = 30

    async def keepalive(self):
        await self.send('fsys', REQUEST, 0, {'TXN': 'Ping'})

    async def keep_alive(self):
        try:
            while True:
                await asyncio.sleep(self.KEEPALIVE_SECONDS)
                await self.keepalive()
                await self.writer.drain()
        except (ConnectionError, asyncio.CancelledError):
            pass

    async def run(self):
        self.note('connected')
        pinger = asyncio.get_running_loop().create_task(self.keep_alive())
        try:
            while True:
                head = await self.reader.readexactly(HEADER)
                kind = head[:4].decode('latin-1')
                word, length = struct.unpack('>II', head[4:])
                body = await self.reader.readexactly(length - HEADER) if length > HEADER else b''
                transmission, packet_id = word & 0xFF000000, word & 0xFFFFFF
                fields = parse_payload(body)
                if transmission in MULTI:
                    part = fields.get('data', '').replace('%3d', '=')
                    if self.multi_id != packet_id:
                        self.multi_id, self.multi_data, self.multi_b64 = packet_id, b'', 0
                    self.multi_data += base64.b64decode(part)
                    self.multi_b64 += len(part)
                    if self.multi_b64 < int(fields.get('size', '0')):
                        continue
                    self.multi_id = None
                    fields = parse_payload(self.multi_data)
                self.note('-> %s %08X id %d %s' % (kind, transmission, packet_id, fields))
                await self.handle(kind, transmission, packet_id, fields)
        except (asyncio.IncompleteReadError, ConnectionError) as e:
            self.note('closed (%s)' % type(e).__name__)
        finally:
            pinger.cancel()
            self.writer.close()
            if self.lkey:
                Client.closed[self.lkey] = time.time()
            if self.holds_name:
                Client.online.discard(self.persona)
                held = NAMES.get(self.persona.lower())
                if held and held[1] is None:
                    NAMES[self.persona.lower()] = (held[0], time.time() + Client.name_grace)

    async def handle(self, kind, transmission, packet_id, f):
        txn = f.get('TXN', '')
        key = '%s/%s' % (kind, txn)
        if key == 'fsys/Hello':
            now = datetime.datetime.now(datetime.timezone.utc).strftime('%b-%d-%Y %H:%M:%S UTC')
            # Where Theater is, for this player (LAN only mode: the address it reached us on, own_address).
            theater = (own_address(self.writer, self.args.theater_host) if getattr(self.args, 'lan', False)
                       else self.args.theater_host)
            await self.send('fsys', RESPONSE, packet_id, {
                'domainPartition.domain': f.get('sku', 'xenon'),
                'messengerIp': theater,
                'messengerPort': 0,
                'domainPartition.subDomain': f.get('clientString', 'beach').split('-')[0].upper(),
                'TXN': 'Hello',
                'activityTimeoutSecs': 0,
                'curTime': now,
                'theaterIp': theater,
                'theaterPort': self.args.theater_port,
            })
            await self.send('fsys', REQUEST, 0, {'TXN': 'MemCheck', 'memcheck.[]': 0, 'type': 0,
                                                 'salt': random.randint(100000000, 999999999)})
        elif key in ('fsys/MemCheck', 'fsys/Ping'):
            pass
        elif key == 'fsys/Goodbye':
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn})
            self.writer.close()
        elif key == 'acct/NuXBL360Login':
            # The menu opens its "Connecting" dialog through a queued command but hears "connected" directly; an
            # instant login reply closes the dialog before it opens and it then stays up. Real EA servers took
            # seconds, so wait a little (docs/01-online-flow.md).
            await asyncio.sleep(self.args.login_delay)
            self.persona = from_wire(f.get('gamertag') or f.get('name') or '') or 'Player%d' % self.user_id
            # Personas must differ: a host finds a joiner's user by name (sub_82507450), and two consoles with the
            # same gamertag (both "User" in our runs) made the host link the joiner under its own id (run duo10).
            # Only the players online now count: a player who left (or whose game closed) gets their name back.
            # A renamed player can't join a dedicated server (their game finds itself in the roster by its own
            # gamertag: an access violation at stage 5, TekRantGaming 6 Nov, run hetzner-rename2), so each game now has its
            # own name (bf_player_name, bf\src\live_layers.cpp) and this is the last resort.
            # The player id: the XUID's low word (the game's player_id, bf\src\game_menu.cpp). Our games ask the
            # registry for a free name before they log in ({"op": "name"}), so this rename is the last resort.
            try:
                pid = int(f.get('xuid', '0')) & 0xFFFFFFFF
            except ValueError:
                pid = 0
            if not pid:
                pid = 0x80000000 | self.user_id
            # A protected name (an account's) logs in only from the player id and address that signed in for it.
            address = self.writer.get_extra_info('peername')[0]
            if not name_free_for(self.persona, pid, address):
                renamed = free_name(self.persona, pid, address)
                if renamed == self.persona:
                    renamed = '%s%d' % ('Soldier' if team_owner(self.persona) else self.persona[:12], self.user_id)
                self.note('the name %s is in use: this player is renamed %s' % (self.persona, renamed))
                self.persona = renamed
            NAMES[self.persona.lower()] = (pid, None)
            Client.online.add(self.persona)
            self.holds_name = True
            lkey = 'BF1943LKEY%08d' % self.user_id
            forget_old()
            self.lkey = lkey
            Client.sessions[lkey] = self.persona
            Client.user_ids[lkey] = self.user_id
            Client.xuids[lkey] = f.get('xuid', '0')
            Client.addresses[lkey] = address
            seen(self.persona, player_id_of(f.get('xuid', '0')), address,
                 account_of(player_id_of(f.get('xuid', '0')), address))
            await self.send(kind, RESPONSE, packet_id, {
                'TXN': txn, 'lkey': lkey, 'userId': self.user_id, 'personaName': self.persona,
            })
        elif key == 'fsys/GetPingSites':
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn, 'pingSite.[]': 0, 'minPingSitesToPing': 0})
        elif key == 'asso/GetAssociations':
            await self.send(kind, RESPONSE, packet_id, {
                'TXN': txn, 'domainPartition.domain': f.get('domainPartition.domain', 'xenon'),
                'domainPartition.subDomain': f.get('domainPartition.subDomain', 'BEACH'),
                'owner.id': self.user_id, 'owner.type': 1, 'type': f.get('type', ''),
                'owner.name': self.persona or 'Player', 'maxListSize': 100, 'members.[]': 0,
            })
        elif key == 'acct/GetTelemetryToken':
            # "host,port,locale," then an opaque part; nothing listens there, telemetry stays off.
            token = base64.b64encode(b'127.0.0.1,9946,enUS,bf1943-no-telemetry').decode()
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn, 'filters': '', 'enabled': '', 'disabled': '',
                                                        'telemetryToken': token})
        elif key == 'rank/GetStats':
            count = int(f.get('keys.[]', '0') or 0)
            out = {'TXN': txn, 'stats.[]': count}
            for i in range(count):
                stat = f.get('keys.%d' % i, '')
                out['stats.%d.key' % i] = stat
                out['stats.%d.value' % i] = STAT_DEFAULTS.get(stat, '0.0')
            await self.send(kind, RESPONSE, packet_id, out)
        elif key == 'pnow/Start':
            # Play now (Quick Match): the session id first, then the result. resultType is one of NOMATCH,
            # NOSERVER, JOIN, LIST, CREATE, CREATE_PROXY (the client's strings at 0x8202CE4C). The client asks with
            # sessionType findServer, and after NOSERVER again with resetServer: then CREATE makes it host a
            # public game itself (Theater CGAM), which the next player's findServer joins.
            session_id = next(Client.pnow_ids)
            partition = f.get('partition.partition', '/xenon/BEACH')
            session = f.get('players.0.props.{sessionType}', 'findServer')
            await self.send(kind, RESPONSE, packet_id, {'TXN': 'Start', 'id.id': session_id, 'id.partition': partition})
            status = {'TXN': 'Status', 'id.id': session_id, 'id.partition': partition, 'sessionState': 'COMPLETE'}
            # An unlisted server (a private match, Direct connect only) never takes a Quick Match the player did not
            # pick in our menu (its reservation below still joins it).
            games = [g for g in GAMES.values()
                     if (g.get('TYPE') != 'P' or self.args.list_private) and g.get('listed', True)]
            # A server picked in our menu's server browser (Registry "reserve"): this player's own, by its sign in.
            address = self.peer.split(':')[0]
            xuid = Client.xuids.get(self.lkey, '0')
            reserved = RESERVATIONS.get(who_signed_in(RESERVATIONS, address, xuid))
            gated = False
            if maintenance_now(address):
                # Maintenance mode: no server for a new connection, and no hosting either.
                games, gated = [], True
                self.note('Quick Match during maintenance: no server')
            elif reserved and time.time() - reserved[1] < 300 and reserved[0] in GAMES:
                games = [GAMES[reserved[0]]]
            elif VERSION_GATE['required']:
                # The version gate: every join from our menus reserves first, and "reserve" is where a game's version
                # is checked. A game that did not reserve (one from before the gate, pressing the game's own Quick
                # Match) gets no server, and is not asked to host one.
                games, gated = [], True
                self.note('Quick Match without a reservation: no server (players need v%s)' % VERSION_GATE['required'])
            else:
                # No reservation and no gate (a LAN or community backend): the full servers are left out (see PICKS).
                who = (address, player_id_of(xuid))
                full = [g for g in games if server_full(g['GID'], who)[0]]
                if full:
                    games = [g for g in games if g not in full]
                    self.note('Quick Match: %d full server(s) left out' % len(full))
                if games:
                    keep(PICKS, who, (games[0]['GID'], time.time(), int(games[0].get('AP') or 0), JOIN_WINDOW),
                         ADDRESS_PICKS)
            if games:
                status.update({'props.{}': 3, 'props.{resultType}': 'JOIN', 'props.{avgFit}': '1.0',
                               'props.{games}.[]': len(games)})
                for i, g in enumerate(games):
                    status['props.{games}.%d.gid' % i] = g['GID']
                    status['props.{games}.%d.lid' % i] = g['LID']
            elif session == 'resetServer' and not self.args.no_create and not gated:
                # The game's attributes (sub_824F3F78 reads props.{attrib}.N.name/value: "version", then U-*
                # attributes) from what the player asked for: filter-X values and pref-X values with their
                # prefToGame-X names.
                pre = 'players.0.props.{'
                attrib = [('version', f.get(pre + 'filter-version}', ''))]
                for k, v in f.items():
                    if k.startswith(pre + 'filterToGame-') and v.startswith('U-'):
                        attrib.append((v, f.get(pre + 'filter-' + k[len(pre + 'filterToGame-'):], '')))
                    elif k.startswith(pre + 'prefToGame-'):
                        attrib.append((v, f.get(pre + 'pref-' + k[len(pre + 'prefToGame-'):], '')))
                status.update({'props.{}': 4, 'props.{resultType}': 'CREATE',
                               'props.{maxGameSize}': f.get(pre + 'poolMaxPlayers}', '24'),
                               'props.{resetTag}': 'bf1943-%d' % session_id, 'props.{attrib}.[]': len(attrib)})
                for i, (name, value) in enumerate(attrib):
                    status['props.{attrib}.%d.name' % i] = name
                    status['props.{attrib}.%d.value' % i] = value
            else:
                status.update({'props.{}': 1, 'props.{resultType}': 'NOSERVER'})
            await self.send(kind, RESPONSE, packet_id, status)
        else:
            self.note('unknown transaction %s, answering with its TXN only' % key)
            await self.send(kind, RESPONSE, packet_id, {'TXN': txn})


class TheaterClient(Client):
    """One Theater connection.

    Games: --game servers (dedicated, not connected to Theater) and games a client hosts (Create Private Match:
    CGAM with RESERVE-HOST=1, then the host enters its own game with EGAM). Entering a hosted game: Theater asks
    the host (EGRQ), the host answers (EGRS, ALLOWED), and the joiner gets the host's address and Xbox secure
    address (EGEG: XNADDR, XNKID session id, XNKEY key). The field names are the ones the client reads (its tables
    at 0x8202543C to 0x8202589C).
    """
    gids = itertools.count(1)
    pids = itertools.count(7)

    async def keepalive(self):
        await self.send('PING', 0, 0, {'TID': '0'})

    def __init__(self, *a):
        super().__init__(*a)
        self.name, self.uid, self.xuid, self.xnaddr = 'Player', 0, '0', ''
        self.joins = {}  # PID -> (joiner, game) waiting for this host's EGRS

    async def handle(self, kind, transmission, packet_id, f):
        tid = f.get('TID', '')
        if kind == 'CONN':
            await self.send('CONN', 0, 0, {'TIME': int(time.time()), 'TID': tid, 'activityTimeoutSecs': 0,
                                           'PROT': f.get('PROT', '2')})
        elif kind == 'USER':
            lkey = f.get('LKEY', '')
            self.name, self.uid = Client.sessions.get(lkey, 'Player'), Client.user_ids.get(lkey, 0)
            self.xuid = Client.xuids.get(lkey, '0')
            await self.send('USER', 0, 0, {'NAME': self.name, 'TID': tid})
        elif kind == 'PING':
            pass
        elif kind == 'CGAM':
            gid = str(next(TheaterClient.gids) + 100)
            g = {'GID': gid, 'LID': '5', 'I': self.peer.split(':')[0], 'P': int(f.get('PORT', '1000')),
                 'INT-IP': f.get('INT-IP', ''), 'INT-PORT': f.get('INT-PORT', f.get('PORT', '1000')),
                 'HU': self.uid, 'HN': self.name, 'N': f.get('NAME', self.name), 'TYPE': f.get('TYPE', 'P'),
                 'JOIN': f.get('JOIN', 'O'), 'MAX-PLAYERS': f.get('MAX-PLAYERS', '16'), 'AP': 0,
                 'UGID': f.get('UGID') or 'NOGUID', 'SECRET': f.get('SECRET') or 'NOSECRET',
                 'B-version': f.get('B-version', ''), 'host': self,
                 'XNKID': base64.b64encode(os.urandom(8)).decode(), 'XNKEY': base64.b64encode(os.urandom(16)).decode()}
            for k, v in f.items():
                if k.startswith('B-') and k not in g:
                    g[k] = v
            GAMES[gid] = g
            await self.send('CGAM', 0, 0, {'TID': tid, 'LID': g['LID'], 'GID': gid, 'MAX-PLAYERS': g['MAX-PLAYERS'],
                                           'EKEY': self.args.ekey, 'UGID': g['UGID'], 'JOIN': g['JOIN'],
                                           'SECRET': g['SECRET'], 'J': g['JOIN']})
        elif kind == 'GDAT':
            g = GAMES.get(f.get('GID', ''))
            await self.send('GDAT', 0, 0, dict(public(for_player(g, self.writer, self.args)), TID=tid) if g
                            else {'TID': tid})
        elif kind == 'EGAM':
            g = GAMES.get(f.get('GID', ''))
            await self.send('EGAM', 0, 0, {'TID': tid, 'LID': f.get('LID', ''), 'GID': f.get('GID', '')})
            if not g:
                return
            # Moderation (join_refusal): a banned player gets no address. Our menus asked at "reserve" first and showed
            # the pop-up; this is for a game that did not.
            address = self.peer.split(':')[0]
            pid = player_id_of(self.xuid)
            account = account_of(pid, address) if pid else ''
            refused = join_refusal(g, [(self.name, pid, account)], address)
            if refused:
                self.note('EGAM refused: %s (%s) is %s' % (self.name, pid or 'no id', 'banned on the Reborn network' if
                                                          refused[3] else 'in game %s\'s ban list (%s)' % (
                                                              g['GID'], refused[4].get('id', ''))))
                tell_refused(g, refused[2], refused[3], refused[4])
                return
            if pid:
                JOINS.setdefault(g['GID'], {})[pid] = (self.name, address, account, time.time())
                seen(self.name, pid, address, account, g.get('N', ''))
            STATS.joined(self.xuid)  # the website's player counts (server/stats.py)
            # in the host's count soon (server_full)
            keep(ARRIVED, who_signed_in(PICKS, address, self.xuid), (g['GID'], time.time()), ADDRESS_RESERVATIONS)
            self.xnaddr = f.get('R-XNADDR', '')
            pid = next(TheaterClient.pids)
            # One ticket per join: the joiner shows the one from EGEG to the host, which compares it with the one
            # from EGRQ and drops the player when they differ (sub_824BDDF0).
            ticket = '0x%x' % random.getrandbits(31)
            host = g.get('host')
            if host is self:
                g['XNADDR'] = self.xnaddr
            if host:
                # Ask the host, also when the host enters its own game (it adds players to its list from EGRQ;
                # without that its PENT reply finds no player). EGEG goes to the joiner when the host allows it.
                host.joins[str(pid)] = (self, g, ticket)
                await host.send('EGRQ', 0, 0, {
                    # The host reads NAME, UID, PID, TICKET, PORT, R-XNADDR, XUID, R-U-*, R-XUID, R-UID,
                    # R-USER, GROUP.[], R-GROUPID, PTYPE (sub_824D8580).
                    'XUID': self.xuid, 'R-XUID': self.xuid, 'R-USER': self.name,
                    'R-NAME': self.name, 'R-UID': self.uid, 'R-U-accid': self.uid, 'R-XNADDR': self.xnaddr,
                    'R-INT-IP': f.get('R-INT-IP', self.peer.split(':')[0]), 'R-INT-PORT': f.get('PORT', '1000'),
                    'NAME': self.name, 'UID': self.uid, 'PID': pid, 'TICKET': ticket,
                    'IP': self.peer.split(':')[0], 'PORT': f.get('PORT', '1000'), 'PTYPE': f.get('PTYPE', 'P'),
                    'XNADDR': self.xnaddr, 'LID': g['LID'], 'GID': g['GID']})
            else:
                # A --game or --dedicated server: not connected to Theater, the address goes out at once. Our
                # GameManager host for a --dedicated server learns the player first; the client's hello names
                # the ticket.
                if g.get('gmhost'):
                    g['gmhost'].expect(ticket, pid, self.name, self.xuid, self.xnaddr, account)
                await self.send_egeg(g, pid, ticket)
        elif kind in ('PENT', 'PLVT'):
            # Player entered (or left) the host's game; the host looks the player up by the PID in the reply
            # (without it, sub_824B6CD0 writes through a null player).
            await self.send(kind, 0, 0, {'TID': tid, 'PID': f.get('PID', ''), 'LID': f.get('LID', ''),
                                         'GID': f.get('GID', '')})
        elif kind == 'EGRS':
            # The host's EGRS is a request: without the reply (sub_824BDD70 handles it) the host never starts its
            # link to the joiner, and the joiner's tunnel packets find no tunnel there.
            await self.send('EGRS', 0, 0, {'TID': tid, 'LID': f.get('LID', ''), 'GID': f.get('GID', ''),
                                           'PID': f.get('PID', '')})
            joiner, g, ticket = self.joins.pop(f.get('PID', ''), (None, None, None))
            if joiner and f.get('ALLOWED', '1') == '1':
                await joiner.send_egeg(g, f.get('PID', '0'), ticket)
            elif joiner:
                self.note('host refused PID %s: %s' % (f.get('PID'), f.get('REASON', '')))
        elif transmission == 0:
            pass  # not a request (UGAM, the host's game updates): no reply, or the client logs an unknown txn
        else:
            self.note('unknown Theater command %s, answering with its TID only' % kind)
            await self.send(kind, 0, 0, {'TID': tid})

    async def send_egeg(self, g, pid, ticket):
        g = for_player(g, self.writer, self.args)  # LAN only mode: the server's address this player can reach
        out = {'PL': 'xenon', 'TICKET': ticket, 'PID': pid, 'HUID': g['HU'],
               'I': g['I'], 'P': g['P'], 'INT-IP': g.get('INT-IP') or g['I'], 'INT-PORT': g.get('INT-PORT', g['P']),
               'EKEY': self.args.ekey, 'UGID': g['UGID'], 'LID': g['LID'], 'GID': g['GID']}
        for k in ('XNADDR', 'XNKID', 'XNKEY'):
            if g.get(k):
                out[k] = g[k]
        await self.send('EGEG', 0, 0, out)


class RemoteHost:
    """A registered server's GameManager host, reached over its registry connection."""

    def __init__(self, writer):
        self.writer = writer

    def expect(self, ticket, pid, name, xuid, xnaddr_b64, account=''):
        m = {'op': 'join', 'ticket': ticket, 'pid': int(pid), 'name': name, 'xuid': str(xuid), 'xnaddr': xnaddr_b64}
        if account:
            m['account'] = account  # the player's Protect your name key (public): its host's ban list matches it too
        self.writer.write((json.dumps(m) + '\n').encode())


def signed_host_key(m, name, digest):
    """A registration's host key when its signature checks (over this name and digest), else ''."""
    import ed25519
    h = m.get('host')
    if not isinstance(h, dict):
        return ''
    key, text, sig = str(h.get('key', '')), str(h.get('text', '')), str(h.get('sig', ''))
    parts = text.split('|')
    if len(text) > 512 or len(parts) < 4 or parts[0] != 'bf1943-register' or parts[2] != digest or \
            '|'.join(parts[3:]) != name:
        return ''
    try:
        public, signature = bytes.fromhex(key), bytes.fromhex(sig)
    except ValueError:
        return ''
    if len(public) != 32 or not ed25519.verify(public, text.encode('utf-8'), signature):
        return ''
    return key.lower()


class ServerCheck:
    """Which community servers the server list shows (TekRantGaming, 16 Nov 2026: a community server listed through our
    backend ran a modified build with bots; "go with option B": a server that is not an official build is hidden).
    On with --check-servers (the official backend); LAN and private backends list every server as before.

    A server registering (gmhost.py) says its package's version, platform and build digest (gmhost.build_report) and
    signs that with its host key (gmhost.host_proof). It is listed only when the digest is the official one for that
    version and platform: the manifests in the --check-servers folder (tools/build_manifest.py, one per release
    package). Anything else (an older package with no report, a changed file, an unknown version, a blocked host) is
    registered but not listed: Direct connect only, never in the list or Quick Match. Official servers (official.key)
    are not checked.

    --blocked-file: {"keys": [host keys], "addresses": [IP addresses], "names": [lower case parts of a name],
    "digests": [build digests]}, read again when it changes; a blocked server is taken off the list within a minute,
    even one listed before."""

    def __init__(self, folder, blocked_file, note):
        self.folder, self.blocked_file, self.note = folder, blocked_file, note
        self.seen, self.official, self.blocked = None, {}, {}
        self.blocked_stamp = None

    def reload(self):
        """The manifests and the block list again when a file of theirs changed."""
        try:
            names = sorted(n for n in os.listdir(self.folder) if n.endswith('.json'))
        except OSError:
            names = []
        stamps = {}
        for n in names:
            try:
                stamps[n] = os.path.getmtime(os.path.join(self.folder, n))
            except OSError:
                pass
        if stamps != self.seen:
            official = {}
            for n in stamps:
                try:
                    with open(os.path.join(self.folder, n), encoding='utf-8') as f:
                        m = json.load(f)
                    files = {str(k): str(v) for k, v in m['files'].items()}
                    official[(str(m['version']), str(m['platform']))] = gmhost.build_digest(files)
                except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
                    self.note('server check: manifest %s not read (%s)' % (n, e))
            self.seen, self.official = stamps, official
            self.note('server check: official builds %s' % (', '.join('%s %s' % k for k in sorted(official)) or 'none'))
        try:
            stamp = os.path.getmtime(self.blocked_file) if self.blocked_file else None
        except OSError:
            stamp = None
        if stamp != self.blocked_stamp:
            blocked = {}
            if stamp is not None:
                try:
                    with open(self.blocked_file, encoding='utf-8') as f:
                        raw = json.load(f)
                    for k in ('keys', 'addresses', 'names', 'digests'):
                        blocked[k] = [str(x).strip().lower() for x in raw.get(k, []) if str(x).strip()]
                except (OSError, ValueError, AttributeError) as e:
                    self.note('server check: block list not read (%s): the last one stays' % e)
                    self.blocked_stamp = stamp
                    return
            self.blocked_stamp, self.blocked = stamp, blocked
            self.note('server check: block list %s' % (', '.join('%d %s' % (len(v), k) for k, v in blocked.items())
                                                       or 'empty'))

    def host_key(self, m, name, digest):
        return signed_host_key(m, name, digest)

    def block_reason(self, name, address, key, digest):
        b = self.blocked
        if key and key in b.get('keys', []):
            return 'its host is blocked'
        if address and address.lower() in b.get('addresses', []):
            return 'its address is blocked'
        if any(part in name.lower() for part in b.get('names', [])):
            return 'its name is blocked'
        if digest and digest in b.get('digests', []):
            return 'its build is blocked'
        return ''

    def verdict(self, m, name, raw_name, address):
        """(why it is hidden, or '' when it may be listed; its host key). raw_name: the name as the host signed it."""
        self.reload()
        build = m.get('build') if isinstance(m.get('build'), dict) else {}
        digest = str(build.get('digest', '')).lower()
        key = self.host_key(m, raw_name, digest) if digest else ''
        blocked = self.block_reason(name, address, key, digest)
        if blocked:
            return blocked, key
        if not build:
            return 'an older server package with no build check', key
        version, platform = str(build.get('version', ''))[:24], str(build.get('platform', ''))[:16]
        want = self.official.get((version, platform))
        if want is None:
            return 'version %s (%s) is not an official server package' % (version, platform), key
        if digest != want:
            return 'its files are not those of the official %s package' % version, key
        if not key:
            return 'no valid host key', key
        return '', key

    async def watch(self, every=60):
        """Takes a server that became blocked (the block list changed) off the list."""
        while True:
            await asyncio.sleep(every)
            try:
                self.reload()
                for gid, g in list(GAMES.items()):
                    if g.get('TYPE') != 'G' or g.get('official') or not g.get('listed', True):
                        continue
                    why = self.block_reason(g.get('N', ''), g.get('from', ''), g.get('host_key', ''),
                                            g.get('build_digest', ''))
                    if why:
                        g['listed'], g['hidden'] = False, why
                        self.note('server check: game %s "%s" taken off the list: %s' % (gid, g.get('N', ''), why))
            except Exception as e:  # never let the watch end
                self.note('server check: %r' % e)


class Promoted:
    """Community servers TekRantGaming shows with the official ones (16 Nov 2026: "promote some community servers at my
    leisure to become official, or at least show in the official. And at any time I can revoke that, and they can just
    simply go back to showing on the community tab"). Known by their host key (gmhost.host_proof, checked at their
    registration): an address or a name can be faked or change, the key cannot without the host's own key file. Every
    server of that host is promoted.

    --promoted-file: {"servers": [{"key": host key (64 hex), "note": "what it is", "since": "UTC time"}]}, written by
    the manager (manager.py promote, demote), read again when it changes (looked at every few seconds). A promoted
    server still has to pass the build check: one that is hidden or blocked is never listed, promoted or not. A
    promoted server is listed with "official": true (every game puts it on the Reborn tab) and "promoted": true (a game
    since v0.4.7 marks it PROMOTED, never as run by TRG Studios); inside the backend it stays a community server: its
    players' stats do not count, and the official network broadcasts do not go to it."""

    def __init__(self, path, note):
        self.path, self.note = path, note
        self.keys, self.stamp, self.looked = {}, None, 0.0

    def reload(self, force=False):
        now = time.monotonic()
        if not force and now - self.looked < 5:
            return
        self.looked = now
        try:
            stamp = os.path.getmtime(self.path)
        except OSError:
            stamp = None
        if stamp == self.stamp:
            return
        keys = {}
        if stamp is not None:
            try:
                with open(self.path, encoding='utf-8') as f:
                    raw = json.load(f)
                for e in raw.get('servers', []):
                    k = str(e.get('key', '')).strip().lower() if isinstance(e, dict) else ''
                    if len(k) == 64 and all(c in '0123456789abcdef' for c in k):
                        keys[k] = str(e.get('note', ''))[:80]
            except (OSError, ValueError, AttributeError) as err:
                self.note('promoted servers: %s not read (%s): the last list stays' % (self.path, err))
                self.stamp = stamp
                return
        self.stamp, self.keys = stamp, keys
        self.note('promoted servers: %d host%s%s' % (len(keys), '' if len(keys) == 1 else 's',
                                                     ' (%s)' % ', '.join(k[:16] for k in keys) if keys else ''))

    def has(self, key):
        return bool(key) and key in self.keys


class Registry:
    """Dedicated servers register here (server/gmhost.py run by the server package) and stay listed while their
    connection lasts. One JSON object per line:
      server:  {"op": "register", "name", "port", "map", "max_players", "mode", "description", "maps", "listed",
                "official"?, "address"?, "night"?, "region"?, "tick"?, "version"?}
               region: where the server is, one of the game's region codes (bf/src/regions.h: "gb", "us-east"...);
               the browser shows its name and flag. tick: the server's simulation rate in Hz (30 today)
               mode: "classic" or "modern", the movement and aiming the server plays (default classic)
               night: true for a Night Ops server (its maps drawn at night in players' games; default false)
               the address: the one the registration came from (a loopback one: this backend's --theater-host);
               "address" [host, port] only from older packages and tests (SERVERS-PLAN.md section 6)
               listed false: not in the list, joined by Direct connect ("find") only
               official: {"stamp", "nonce", "mac"} from the official key (gmhost.official_code); checked against
               --official-key-file, else refused
               "resume" {"uid", "pid", "pids"}?: a host whose link to the backend dropped asks to keep its user id
               (its tunnel's id) and player id; pids: its players' ids (resume below)
      backend: {"op": "registered", "gid", "uid", "pid", "official", "listed", "address"}
      backend: {"op": "reachable", "ok", "address", "local"} after a UDP probe of the server's port; for a server on
               this backend's own machine ("local"), ok is null when it answers: a probe from here never passes
               the machine's incoming firewall, so whether players outside reach it cannot be told from here
      backend: {"op": "refused", "why"}                     an official code that does not check
      server:  {"op": "status", "players", "bots", "map", "next_map"}   every few seconds (bots: 21 Nov 2026)
      backend: {"op": "join", "ticket", "pid", "name", "xuid", "xnaddr"}   before the joiner gets EGEG
      anyone:  {"op": "list"} -> {"op": "servers", "servers": [...]}   for a server browser (listed servers only)
               with the version gate (VERSION_GATE): "required_version" too, and for a game that says no "version"
               (one from before the gate) the servers are one line telling the player to restart the game
      anyone:  {"op": "find", "address", "port"} -> {"op": "found", "server"|null}   Direct connect
      anyone:  {"op": "stats"} -> {"op": "stats", "players", "players_month", "month", "hours", "since"}   the
               website's totals (server/stats.py; its feed asks): counts only
      player:  {"op": "reserve", "gid", "id"?, "version"?, "platform"?} -> {"op": "reserved"}  the player's next
               Quick Match joins that game ("id": the player id, believed only where it signed in: asker)
               with the version gate a game older than the newest release (or one that says no version) gets
               {"op": "outdated", "required", "yours"} and no reservation
               a v0.4.0 or newer game says its "version" and "platform" ("windows", "linux") in list, find and reserve
               a full server (or one with players waiting): {"op": "full", "title", "message"}; with "queue": 1
               (v0.4.7.4) a place in its queue instead (QUEUE): {"op": "queued", "gid", "token", "position", "total"}
      player:  {"op": "queue", "gid", "token", "id"?} -> {"op": "queued", ...} | {"op": "reserved", "gid"} (its turn:
               as after reserve) | {"op": "queue_lost"} (no such place: reserve again) | {"op": "gone"|"closed", "title",
               "message"}   every few seconds while it waits; a place not asked for in QUEUE_TIMEOUT is given up
      player:  {"op": "queue_leave", "token"} -> {"op": "queue_left"}   the queue pop-up's Close
      player:  {"op": "tag", "name", "id", "tag"} -> {"op": "tag", "ok", "tag", "why"?, "text"?, "dev"?}  the
               player's clan tag (put_tag): "" for none; why "not_yours" (the name is not held for this player),
               "length", "chars", "reserved", "profanity", "with_name" (text_filter.py), with the player's words
      player:  {"op": "tags", "gid"?, "names"?, "me"?} -> {"op": "tags", "tags": {name: tag}}  the clan tags of a
               server's players and of the names asked about (tags_reply); scoreboard, players and leaderboard rows
               carry "tag" too
      player:  {"op": "name", "name", "id"} -> {"op": "name", "name"}  before logging in: the player's name, or it
               with a number when another player has it (free_name); held for that player id for 10 minutes, and
               while they are online. "protected_by_other": true when the name is another player's protected name
    Protect your name (accounts, above): "key" (the account's public key, hex), "signed" (the text signed: the op, a
    nonce from "challenge", then the fields, joined by "|") and "sig" (its Ed25519 signature, hex):
      player:  {"op": "challenge"} -> {"op": "challenge", "nonce"}   one use, within 90 seconds
      player:  {"op": "claim", signed "claim|nonce|name|player id"} -> {"op": "claim", "ok", "name"|"why"}  protect
               a name (or move the account to a new one): why "taken" (another account's), "in_use" (another player
               plays under it now), "too_many" (5 new names an hour from one address), "bad"
      player:  {"op": "name", signed "name|nonce|name|player id"} -> {"op": "name", "name", "protected": true,
               "progress"}  the name check of a player with an account: their name, held for them
      player:  {"op": "recover", signed "recover|nonce"} -> {"op": "recover", "ok", "name", "progress"}  a recovery
               code typed on a new PC: the account's name and progress
      player:  {"op": "stats_put", signed "stats_put|nonce|name|score|seconds|captures|Label=events,points;...|1,7"}
               -> {"op": "stats_put", "ok"}  totals for the leaderboard and the account (unsigned stats for a protected
               name are refused)
    Each player's stats as the servers count them (player_stats.py, server_stats.cpp):
      server:  {"op": "player_stats", "rows": [{"name", "kills", "deaths", ...}]}  each player's new counts; kept only
               from official servers (or every server with --player-stats-from all)
      anyone:  {"op": "leaderboard", "sort", "period", "me"} -> {"op": "leaderboard", "rows", "total", "me"?}  a board
               by score, kills, kd, captures, seconds, ... for all time, this month or this week (without sort, period
               and me: the old reply, by score)
      anyone:  {"op": "player_card", "name"} -> {"op": "player_card", "found", every count, "maps", "places", ...}
      anyone:  {"op": "achievement_share"} -> {"op": "achievement_share", "players", "have": {id: count}}
    """
    ids = itertools.count(1)

    @staticmethod
    def resume(asked, address, m):
        """(uid, pid) for a host that asks to keep its ids, or None (17 Nov 2026: gmhost.py keeps its players through
        a lost link to the backend, which needs the same tunnel id, a hash of its user id, and the same player id). A
        user id no listed game has, ids in the ranges this backend gives, and its counters moved past them and the
        players' ids named, so nobody is given them again (a backend that restarted counts from the start). A game
        that still holds the user id from the same address and port is this server's own old registration, whose
        link died without this side seeing it (nothing times out here): it is let go."""
        if not isinstance(asked, dict):
            return None
        try:
            uid, pid = int(asked.get('uid')), int(asked.get('pid'))
            pids = [int(x) for x in asked.get('pids') or []][:MOST_PLAYERS]
        except (TypeError, ValueError):
            return None
        top = 10 ** 7
        if not 901 <= uid < top or not 7 <= pid < top or any(not 7 <= x < top for x in pids):
            return None
        try:
            port = int(m['address'][1]) if 'address' in m else int(m.get('port', 18300))
        except (TypeError, ValueError, IndexError, KeyError):
            return None
        for gid, g in list(GAMES.items()):
            if g.get('HU') != uid:
                continue
            if g.get('from') != address or int(g.get('P', 0)) != port:
                return None
            GAMES.pop(gid, None)  # its connection's own end still notes it gone
            try:
                g['gmhost'].writer.close()
            except Exception:  # a connection already gone
                pass
        Registry.ids = itertools.count(max(next(Registry.ids), uid - 900 + 1))
        TheaterClient.pids = itertools.count(max([next(TheaterClient.pids), pid + 1] + [x + 1 for x in pids]))
        return uid, pid

    def __init__(self, log, args=None):
        self.log = log
        self.public = getattr(args, 'theater_host', '127.0.0.1') if args else '127.0.0.1'
        self.args = args
        self.test_progression = getattr(args, 'test_progression', False)
        self.key = None
        if args and getattr(args, 'official_key_file', ''):
            self.key = gmhost.read_key(args.official_key_file)
        self.used = {}  # official codes taken (nonce -> time)
        # The build check (ServerCheck): the official backend lists only official builds' community servers.
        self.check = ServerCheck(args.check_servers, getattr(args, 'blocked_file', ''), self.note) \
            if args and getattr(args, 'check_servers', '') else None
        # Promoted community servers (Promoted) and network broadcasts (gmhost.sign_network): both need official.key.
        self.promoted = Promoted(args.promoted_file, self.note) if args and getattr(args, 'promoted_file', '') else None
        trusted = gmhost.trusted_broadcast_keys(self.key) if self.key else []
        self.network = gmhost.NetworkCheck(trusted)
        self.admin = gmhost.NetworkCheck(trusted, per_minute=30)

    def refresh_promoted(self):
        """Each listed community server's promotion, from the promoted list as it is now (said in the log when it
        changes)."""
        if not self.promoted:
            return
        self.promoted.reload()
        for gid, g in list(GAMES.items()):
            if g.get('TYPE') != 'G':
                continue
            now = bool(not g.get('official') and g.get('listed', True) and not g.get('hidden')
                       and self.promoted.has(g.get('host_key', '')))
            if now != bool(g.get('promoted')):
                g['promoted'] = now
                self.note('game %s "%s" %s' % (gid, g.get('N', ''), 'promoted: listed with the Reborn servers (host %s)'
                                               % g.get('host_key', '')[:16] if now else
                                               'no longer promoted: back with the community servers'))

    def relay(self, m, peer):
        """A network broadcast from the official manager (gmhost.sign_network): checked, then passed to every
        server registered here that it is for, over the server's own registry connection."""
        why, b = self.network.broadcast(m)
        if why:
            self.note('%s network broadcast refused: %s' % (peer, why))
            return {'op': 'network_broadcast', 'ok': False, 'why': why}
        line = (json.dumps({'op': 'network_broadcast', 'signed': m['signed'], 'sig': m['sig']}) + '\n').encode()
        sent = {'official': 0, 'community': 0}
        for g in list(GAMES.values()):
            host = g.get('gmhost')
            if g.get('TYPE') != 'G' or not isinstance(host, RemoteHost):
                continue
            if b['scope'] == 'official' and not g.get('official'):
                continue
            try:
                host.writer.write(line)
            except (ConnectionError, RuntimeError, OSError):
                continue
            sent['official' if g.get('official') else 'community'] += 1
        self.note('network broadcast %s (%s, to %s): %d official and %d community servers: %s' % (
            b['id'], b['kind'], b['scope'], sent['official'], sent['community'], b['text']))
        return {'op': 'network_broadcast', 'ok': True, 'id': b['id'], 'sent': sent}

    def hosts(self):
        """The community servers registered now, with their host keys (only for the official manager)."""
        self.refresh_promoted()
        rows = []
        for g in GAMES.values():
            if g.get('TYPE') != 'G' or g.get('official') or not isinstance(g.get('gmhost'), RemoteHost):
                continue
            rows.append({'gid': g['GID'], 'name': g.get('N', ''), 'host_key': g.get('host_key', ''),
                         'from': g.get('from', ''), 'port': g.get('P', 0), 'version': g.get('server_version', ''),
                         'players': g.get('AP', 0), 'bots': g.get('bots', 0),
                         'max_players': g.get('MAX-PLAYERS', 0), 'listed': bool(g.get('listed', True)), 'hidden': g.get('hidden', ''),
                         'promoted': bool(g.get('promoted')),
                         'promoted_key': bool(self.promoted and self.promoted.has(g.get('host_key', ''))),
                         'region': g.get('region', '')})
        rows.sort(key=lambda r: (r['host_key'] or '~', r['name'].lower()))
        return rows

    def official(self, code, name):
        """An official code that checks: made with the official key, within two minutes, never seen before."""
        import hmac
        if not self.key or not isinstance(code, dict):
            return False
        try:
            stamp, nonce = int(code['stamp']), str(code['nonce'])[:32]
        except (KeyError, ValueError, TypeError):
            return False
        now = time.time()
        self.used = {n: t for n, t in self.used.items() if now - t < 300}
        if abs(now - stamp) > 120 or nonce in self.used:
            return False
        want = gmhost.official_code(self.key, name, stamp, nonce)['mac']
        if not hmac.compare_digest(want, str(code.get('mac', ''))):
            return False
        self.used[nonce] = now
        return True

    async def probe(self, writer, host, port, local=False):
        """Whether players can reach a registered server: a UDP probe its host answers (gmhost PROBE, PONG). For a
        server on this machine (local) an answer only says it runs: the probe stays inside the machine and skips its
        incoming firewall (Hetzner, 8 Nov 2026: all five official servers read reachable while the firewall blocked
        four), so the answer is then "not known" (ok null), and no answer still means it cannot be reached."""
        loop = asyncio.get_running_loop()
        got = loop.create_future()

        class Probe(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                if data.startswith(gmhost.PONG) and not got.done():
                    got.set_result(True)

        ok = False
        try:
            # A socket that ignores Windows' port unreachable reports (gmhost.udp_socket): a probe that came before the
            # host had bound its port left asyncio's Proactor loop reading nothing more, so the answers to the next
            # two were never read (rejoin tests of 9 Nov 2026).
            sock = gmhost.udp_socket(('::' if ':' in host else '0.0.0.0', 0))
            transport, _ = await loop.create_datagram_endpoint(Probe, sock=sock)
            try:
                for _ in range(3):
                    transport.sendto(gmhost.PROBE + os.urandom(8), (host, port))
                    try:
                        ok = await asyncio.wait_for(asyncio.shield(got), 1.0)
                        break
                    except asyncio.TimeoutError:
                        pass
            finally:
                transport.close()
        except OSError:
            ok = False
        verdict = None if (ok and local) else bool(ok)
        try:
            writer.write((json.dumps({'op': 'reachable', 'ok': verdict, 'local': bool(local),
                                      'address': '%s:%d' % (host, port)}) + '\n').encode())
            await writer.drain()
        except ConnectionError:
            pass
        self.note('%s:%d %s' % (host, port, 'NOT reachable by UDP (closed port?)' if not ok else
                                'answers on this machine; from outside not checked (a probe from here skips the '
                                'firewall): try the game\'s server browser from another network' if local else
                                'reachable'))

    def note(self, text):
        line = '%s REGISTRY %s' % (time.strftime('%H:%M:%S'), text)
        print(line, flush=True)
        self.log.write(line + '\n')
        self.log.flush()

    def note_given_up(self, given_up):
        for e, why in given_up:
            self.note('%s gave up its place in the queue of game %s after %.0f s: %s' % (
                e['address'], e['gid'], time.time() - e['since'], why))

    # ---- Protect your name (accounts, above)
    def name_reply(self, asked, pid, address, extra=None):
        """The name check: the name, or it with a number (free_name), held for this player for 10 minutes."""
        name = free_name(asked, pid, address)
        if name_holder(name) != pid or NAMES[name.lower()][1] is not None:
            NAMES[name.lower()] = (pid, time.time() + 600)
        reply = {'op': 'name', 'name': name}
        if name.lower() != asked.lower() and protected_by(asked):
            reply['protected_by_other'] = True
            if team_owner(asked):
                reply['team_name'] = True  # kept for the team (TEAM_OWNERS): the game says so
        reply.update(extra or {})
        self.note('%s asked for the name %s: %s%s' % (address, asked, name, ' (protected)' if
                                                      reply.get('protected') else ''))
        return reply

    def signed_name(self, m, address):
        got = check_signed(m, 'name')
        if not got or len(got[1]) != 2:
            return {'op': 'name', 'name': '', 'why': 'bad'}
        key, (asked, pid_text) = got
        try:
            pid = int(pid_text, 16) & 0xFFFFFFFF
        except ValueError:
            return {'op': 'name', 'name': '', 'why': 'bad'}
        acct = ACCOUNTS.get(key)
        now = time.time()
        owner = team_owner(asked)
        if acct and owner and owner['key'] == key and valid_name(asked) and acct.get('name', '').lower() != asked.lower():
            self.move_name(acct, asked, address)  # a team owner plays under another of their names: the account follows
        if acct and not acct.get('name') and valid_name(asked) and protected_by(asked) is None \
                and name_holder(asked) in (None, pid):
            acct['name'] = asked  # its name was freed after a long time unused: protected again if still free
            ACCOUNT_NAMES[asked.lower()] = key
        if acct and acct.get('name', '').lower() == asked.lower() and protected_by(acct['name']) in (None, acct):
            acct['seen'] = int(now)
            ACCOUNT_NAMES[acct['name'].lower()] = key
            accounts_changed()
            VERIFIED[key] = (pid, address, now + VERIFIED_FOR)
            return self.name_reply(acct['name'], pid, address,
                                   {'protected': True, 'progress': acct.get('progress', {}),
                                    'progress_text': progress_text(acct.get('progress', {}))})
        if pid:
            return self.name_reply(player_stats.clean_name(asked) or 'Soldier',
                                   pid, address, {'protected': False})
        return {'op': 'name', 'name': asked}

    def claim(self, m, address):
        got = check_signed(m, 'claim')
        if not got or len(got[1]) != 2 or not valid_name(got[1][0]):
            return {'op': 'claim', 'ok': False, 'why': 'bad'}
        key, (name, pid_text) = got
        try:
            pid = int(pid_text, 16) & 0xFFFFFFFF
        except ValueError:
            return {'op': 'claim', 'ok': False, 'why': 'bad'}
        now = time.time()
        acct = ACCOUNTS.get(key)
        owner = protected_by(name)
        if owner and owner is not acct:
            # (A team name says "team_name", never "reserved": a game from before the version gate reads that word.)
            return {'op': 'claim', 'ok': False, 'why': 'team_name' if team_owner(name) else 'taken'}
        if not (acct and acct.get('name', '').lower() == name.lower()):
            if name_holder(name) not in (None, pid):
                return {'op': 'claim', 'ok': False, 'why': 'in_use'}
            times = [t for t in CLAIMS.get(address, []) if now - t < 3600]
            if len(times) >= CLAIMS_AN_HOUR and key not in TEAM['by_key']:  # a team owner moves among their names
                CLAIMS[address] = times
                return {'op': 'claim', 'ok': False, 'why': 'too_many'}
            CLAIMS[address] = times + [now]
            if len(ACCOUNTS) >= 200000 and not acct:
                return {'op': 'claim', 'ok': False, 'why': 'too_many'}
            if acct:
                self.move_name(acct, name, address)
            else:
                old = ACCOUNT_NAMES.get(name.lower())
                if old and old != key and old in ACCOUNTS:
                    ACCOUNTS[old]['name'] = ''  # an account unused too long loses the name it no longer protects
                acct = ACCOUNTS[key] = {'key': key, 'name': name, 'created': int(now), 'seen': int(now), 'progress': {}}
                self.note('%s protected the name %s' % (address, name))
        acct['seen'] = int(now)
        ACCOUNT_NAMES[name.lower()] = key
        VERIFIED[key] = (pid, address, now + VERIFIED_FOR)
        NAMES[name.lower()] = (pid, now + 600)
        accounts_changed()
        return {'op': 'claim', 'ok': True, 'name': acct['name']}

    def move_name(self, acct, name, address):
        """The account moves to another name (a claim, or a team owner's name check under another of their names): its
        old name is free at once, its leaderboard row moves with it."""
        key = acct['key']
        old = ACCOUNT_NAMES.get(name.lower())
        if old and old != key and old in ACCOUNTS:
            ACCOUNTS[old]['name'] = ''  # an account unused too long loses the name it no longer protects
        if acct.get('name') and ACCOUNT_NAMES.get(acct['name'].lower()) == key:
            del ACCOUNT_NAMES[acct['name'].lower()]
        held = NAMES.get(acct.get('name', '').lower())
        if held and held[1] is not None:  # the old name's hold from its name checks: free at once
            del NAMES[acct['name'].lower()]
        row = LEADERBOARD.pop(acct.get('name', '').lower(), None)  # the leaderboard row moves with it
        if row:
            other = LEADERBOARD.get(name.lower())
            if other and other.get('score', 0) > row.get('score', 0):  # a team owner back on a name with its own row
                row = other
            row['name'] = name
            LEADERBOARD[name.lower()] = row
            save_leaderboard()
        if acct.get('name'):
            PLAYER_STATS.rename(acct['name'], name)  # and the stats the servers counted
        self.note('%s moved the account of %s to %s' % (address, acct.get('name') or '(no name)', name))
        acct['name'] = name
        ACCOUNT_NAMES[name.lower()] = key
        accounts_changed()

    def signed_stats(self, m):
        got = check_signed(m, 'stats_put')
        progress = parse_progress(got[1]) if got else None
        acct = ACCOUNTS.get(got[0]) if got else None
        if not progress or not acct or acct.get('name', '').lower() != progress['name'].lower() \
                or protected_by(acct['name']) is not acct:
            return {'op': 'stats_put', 'ok': False, 'why': 'bad' if not progress else 'not_yours'}
        p = merge_progress(acct, progress)
        acct['seen'] = int(time.time())
        accounts_changed()
        ok = leaderboard_put({'name': acct['name'], 'score': p['score'], 'seconds': p['seconds'],
                              'captures': p['captures']})
        return {'op': 'stats_put', 'ok': ok}

    async def client(self, reader, writer):
        gid = None
        peer = '%s:%d' % writer.get_extra_info('peername')[:2]
        address = writer.get_extra_info('peername')[0]
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                m = json.loads(line)
                op = m.get('op')
                if op == 'register' and not gid:
                    name = ''.join(c for c in str(m.get('name', 'Server')) if c.isprintable())[:48] or 'Server'
                    official = 'official' in m and self.official(m['official'], m.get('name', 'Server'))
                    if 'official' in m and not official:
                        writer.write((json.dumps({'op': 'refused', 'why': 'the official code does not check'}) +
                                      '\n').encode())
                        self.note('%s refused: an official code that does not check' % peer)
                        await writer.drain()
                        break
                    # The build check: a community server that is not an official build is not listed.
                    hidden, host_key = '', ''
                    if self.check and not official:
                        hidden, host_key = self.check.verdict(m, name, str(m.get('name', 'Server')),
                                                              writer.get_extra_info('peername')[0])
                    elif self.promoted and not official and isinstance(m.get('build'), dict):
                        host_key = signed_host_key(m, str(m.get('name', 'Server')),
                                                   str(m['build'].get('digest', '')).lower())
                    kept = Registry.resume(m.get('resume'), writer.get_extra_info('peername')[0], m)
                    n = next(Registry.ids)
                    gid, uid, pid = str(200 + n), 900 + n, next(TheaterClient.pids)
                    if kept:
                        uid, pid = kept
                    local = False
                    if 'address' in m:
                        host, port = m['address']
                    else:
                        host, port = writer.get_extra_info('peername')[0], int(m.get('port', 18300))
                        # A server on this backend's own machine: it registers from a loopback address, or
                        # from the very address of ours it reached.
                        if host.startswith('127.') or host == '::1' or host == writer.get_extra_info('sockname')[0]:
                            host, local = self.public, True
                    GAMES[gid] = {'GID': gid, 'LID': '1', 'I': host, 'P': int(port), 'HU': uid,
                                  'HN': name, 'N': name, 'TYPE': 'G',
                                  'JOIN': 'O', 'MAX-PLAYERS': max(2, min(BIGGEST_SERVER, int(m.get('max_players', 24)))), 'AP': 0,
                                  'UGID': 'NOGUID', 'B-version': 'RETAIL429432', 'B-U-type': 'RANKED',
                                  'gmhost': RemoteHost(writer), 'map': m.get('map', ''),
                                  'local': local,  # on this backend's own machine (for_player)
                                  'mode': 'modern' if m.get('mode') == 'modern' else 'classic',
                                  'night': m.get('night') is True,  # Night Ops: its maps at night
                                  'region': ''.join(c for c in str(m.get('region', '')).lower()
                                                    if c.isalnum() or c == '-')[:16],
                                  'tick': tick_of(m.get('tick')),
                                  'server_version': ''.join(c for c in str(m.get('version', ''))
                                                            if c.isalnum() or c in '.-')[:24],
                                  'description': ''.join(c for c in str(m.get('description', ''))
                                                         if c.isprintable())[:200],
                                  'maps': [str(x)[:32] for x in m.get('maps', []) if isinstance(x, str)][:8],
                                  'next_map': '', 'official': official,
                                  'listed': bool(m.get('listed', True)) and not hidden,
                                  'hidden': hidden if m.get('listed', True) else '',
                                  'from': writer.get_extra_info('peername')[0], 'host_key': host_key,
                                  'build_digest': str((m.get('build') or {}).get('digest', ''))[:64].lower()
                                  if isinstance(m.get('build'), dict) else '',
                                  'XNADDR': base64.b64encode(gmhost.xnaddr(host)).decode(),
                                  'XNKID': base64.b64encode(os.urandom(8)).decode(),
                                  'XNKEY': base64.b64encode(os.urandom(16)).decode()}
                    reply = {'op': 'registered', 'gid': gid, 'uid': uid, 'pid': pid, 'official': official,
                             'listed': GAMES[gid]['listed'], 'address': '%s:%d' % (host, int(port))}
                    if GAMES[gid]['hidden']:
                        reply['hidden'] = GAMES[gid]['hidden']
                    writer.write((json.dumps(reply) + '\n').encode())
                    self.note('%s registered "%s" at %s:%s as game %s%s%s%s' % (
                        peer, name, host, port, gid, ' (official)' if official else '',
                        '' if GAMES[gid]['listed'] else ' (not listed)',
                        ': hidden, %s' % GAMES[gid]['hidden'] if GAMES[gid]['hidden'] else
                        ' (host %s)' % host_key[:16] if host_key else ''))
                    if kept:
                        self.note('%s game %s kept the ids of its host (user %d, player %d): its players stay' % (
                            peer, gid, uid, pid))
                    # A server registering again (its link to here dropped): its queue waits on for it.
                    moved = [e for e in QUEUE if e['gid'] not in GAMES and e['server'] == (host, int(port))]
                    for e in moved:
                        e['gid'] = gid
                    if moved:
                        self.note('%s game %s: its server\'s %d waiting players wait on for it' % (peer, gid, len(moved)))
                    asyncio.get_running_loop().create_task(self.probe(writer, host, int(port), local))
                    self.refresh_promoted()
                elif op == 'server_bans' and gid in GAMES:
                    # Moderation: this server's own ban list (gmhost.py bans_summary), for this server only.
                    GAMES[gid]['bans'] = clean_server_bans(m)
                    self.note('game %s: its ban list has %d entr%s' % (
                        gid, len((GAMES[gid]['bans'] or {}).get('rows', [])),
                        'y' if len((GAMES[gid]['bans'] or {}).get('rows', [])) == 1 else 'ies'))
                elif op == 'status' and gid in GAMES:
                    GAMES[gid]['AP'] = int(m.get('players', 0))
                    # Its bots (a host from 21 Nov 2026; none from an older one): the browser shows "(4) 0 / 24".
                    # They are not players: AP, the full check and the website's hours count the people only.
                    try:
                        GAMES[gid]['bots'] = max(0, min(BIGGEST_SERVER, int(m.get('bots') or 0)))
                    except (TypeError, ValueError):
                        GAMES[gid]['bots'] = 0
                    GAMES[gid]['status_at'] = time.time()  # the players whose game got its address since are in it
                    STATS.status(gid, GAMES[gid]['AP'])  # the website's hours played (server/stats.py)
                    GAMES[gid]['map'] = m.get('map', GAMES[gid]['map'])
                    GAMES[gid]['next_map'] = str(m.get('next_map', ''))[:32]
                    names = m.get('names', [])
                    GAMES[gid]['names'] = [str(n)[:15] for n in names][:MOST_PLAYERS] if isinstance(names, list) else []
                elif op == 'players':
                    # Join Match With Friends (our main menu): the players signed in now and the listed server each
                    # plays on (no address), the servers' own names left out. A player in a private match (a server
                    # not listed) shows as there, with no way in from this list: its host gives friends the address
                    # for Direct connect.
                    hosts = {g.get('HN') for g in GAMES.values()}
                    where = {}
                    for g in GAMES.values():
                        if g.get('TYPE') == 'G':
                            for n in g.get('names', []):
                                where[n.lower()] = g
                    rows = []
                    for persona in sorted(Client.online):
                        if persona in hosts:
                            continue
                        g = where.get(persona.lower())
                        if g and not g.get('listed', True):
                            rows.append({'name': persona, 'gid': '', 'server': 'Private match', 'map': ''})
                            continue
                        rows.append({'name': persona, 'gid': g['GID'] if g else '', 'server': g.get('N', '') if g else '',
                                     'map': g.get('map', '') if g else ''})
                    writer.write((json.dumps({'op': 'players', 'rows': with_tags(rows[:200])}) + '\n').encode())
                elif op == 'match_score' and str(m.get('gid', '')) in GAMES:
                    # The pause's Scoreboard: each player's game sends its score in this match.
                    name = player_stats.clean_name(m.get('name', ''))
                    try:
                        score = int(float(m.get('score', 0)))
                    except (TypeError, ValueError):
                        score = None
                    if name and score is not None:
                        table = MATCH_SCORES.setdefault(str(m['gid']), {})
                        if len(table) < MOST_PLAYERS or name.lower() in table:
                            table[name.lower()] = {'name': name, 'score': score, 'at': int(time.time())}
                    writer.write((json.dumps({'op': 'match_score', 'ok': bool(name)}) + '\n').encode())
                elif op == 'scoreboard':
                    g = GAMES.get(str(m.get('gid', '')))
                    joined = {n.lower() for n in g.get('names', [])} if g else set()
                    table = MATCH_SCORES.get(str(m.get('gid', '')), {})
                    rows = [e for k, e in table.items() if not joined or k in joined]
                    for n in (g.get('names', []) if g else []):  # players with no score sent yet
                        if n.lower() not in table:
                            rows.append({'name': n, 'score': 0, 'at': 0})
                    rows.sort(key=lambda e: (-e['score'], e['name'].lower()))
                    writer.write((json.dumps({'op': 'scoreboard', 'rows': with_tags(rows[:MOST_PLAYERS])}) + '\n').encode())
                elif op == 'tag':  # clan tags (tag_for, above)
                    reply = put_tag(m, address)
                    self.note('%s %s the clan tag "%s" for %s%s' % (
                        address, 'set' if reply.get('ok') else 'was refused', str(m.get('tag', ''))[:16],
                        str(m.get('name', ''))[:15], (': wears "%s"' % reply.get('tag')) if reply.get('ok') and
                        reply.get('tag') != str(m.get('tag', '')) else '' if reply.get('ok') else
                        ' (%s)' % reply.get('why')))
                    writer.write((json.dumps(reply) + '\n').encode())
                elif op == 'tags':
                    writer.write((json.dumps(tags_reply(m, address)) + '\n').encode())
                elif op == 'stats_put' and 'signed' in m:
                    reply = self.signed_stats(m)
                    writer.write((json.dumps(reply) + '\n').encode())
                elif op == 'stats_put':
                    local_test = (self.test_progression and
                                  writer.get_extra_info('peername')[0].startswith('127.'))
                    name = player_stats.clean_name(m.get('name', ''))
                    if name and protected_by(name):  # a protected name's stats come signed by its account only
                        writer.write((json.dumps({'op': 'stats_put', 'ok': False, 'why': 'protected'}) + '\n').encode())
                    else:
                        ok = leaderboard_put(m, test_progression=local_test)
                        writer.write((json.dumps({'op': 'stats_put', 'ok': ok}) + '\n').encode())
                elif op == 'challenge':
                    nonce = challenge(address)
                    writer.write((json.dumps({'op': 'challenge', 'nonce': nonce} if nonce else
                                             {'op': 'challenge', 'why': 'too_many'}) + '\n').encode())
                elif op == 'claim':
                    reply = self.claim(m, address)
                    writer.write((json.dumps(reply) + '\n').encode())
                elif op == 'recover':
                    got = check_signed(m, 'recover')
                    acct = ACCOUNTS.get(got[0]) if got else None
                    if acct:
                        acct['seen'] = int(time.time())
                        if acct.get('name') and protected_by(acct['name']) is not acct:
                            acct['name'] = ''  # unused too long, and another player has it now
                        accounts_changed()
                        writer.write((json.dumps({'op': 'recover', 'ok': True, 'name': acct.get('name', ''),
                                                  'progress': acct.get('progress', {}),
                                                  'progress_text': progress_text(acct.get('progress', {}))}) +
                                      '\n').encode())
                        self.note('%s recovered the account of %s' % (peer, acct.get('name') or '(no name)'))
                    else:
                        writer.write((json.dumps({'op': 'recover', 'ok': False}) + '\n').encode())
                elif op == 'name' and 'signed' in m:
                    reply = self.signed_name(m, address)
                    note_name_check(reply, str(m.get('signed', '')).rsplit('|', 1)[-1], address)  # clan tags
                    writer.write((json.dumps(reply) + '\n').encode())
                elif op == 'network_broadcast' and not gid:
                    # The official manager's network broadcast (gmhost.sign_network), passed on to the servers.
                    writer.write((json.dumps(self.relay(m, peer)) + '\n').encode())
                elif op == 'admin' and not gid:
                    # The official manager's questions (gmhost.sign_admin): the community servers' host keys, so
                    # TekRantGaming can pick one to promote. Signed: the rows hold the hosts' addresses.
                    why, what = self.admin.admin(m)
                    if not why and (what == 'players' or str(what).startswith('lookup:')):
                        # Moderation: the official manager's player lookup (who is online, names seen this week).
                        rows = admin_players(what)
                        self.note('%s admin question: %s (%d rows)' % (peer, what[:40], len(rows)))
                        writer.write((json.dumps({'op': 'admin', 'ok': True, 'players': rows}) + '\n').encode())
                    elif why or what != 'hosts':
                        self.note('%s admin question refused: %s' % (peer, why or 'unknown'))
                        writer.write((json.dumps({'op': 'admin', 'ok': False, 'why': why or 'unknown'}) +
                                      '\n').encode())
                    else:
                        writer.write((json.dumps({'op': 'admin', 'ok': True, 'hosts': self.hosts()}) + '\n').encode())
                elif op == 'stats':
                    # The website's totals (server/stats.py, read by its feed): counts only, nothing about anyone.
                    writer.write((json.dumps(dict(STATS.totals(), op='stats')) + '\n').encode())
                elif op == 'player_stats' and gid in GAMES:
                    # A dedicated server's counts (server_stats.cpp): only a server we trust counts (player_stats.py).
                    trusted = GAMES[gid].get('official') or getattr(self.args, 'player_stats_from', '') == 'all'
                    if trusted:
                        PLAYER_STATS.add(m.get('rows'))
                elif op == 'leaderboard' and ('sort' in m or 'period' in m or 'me' in m):
                    # Leaderboards (game_menu_progress.inc): a board by any stat and period, with the asker's own row.
                    board = PLAYER_STATS.board(str(m.get('sort', 'score')), str(m.get('period', 'all')), 50,
                                               str(m.get('me', ''))[:15], LEADERBOARD)
                    board['rows'] = with_tags(board['rows'])
                    if 'me' in board:
                        board['me'] = with_tags([board['me']])[0]
                    writer.write((json.dumps(dict(board, op='leaderboard')) + '\n').encode())
                elif op == 'achievement_share':
                    # Achievements (game_menu_progress.inc): how many players with an account have each one. Counts
                    # only: the accounts' progress (Protect your name), nothing about anyone.
                    have, players = {}, 0
                    for acct in ACCOUNTS.values():
                        ids = (acct.get('progress') or {}).get('achievements') or []
                        if not acct.get('name'):
                            continue
                        players += 1
                        for i in set(ids):
                            if type(i) is int and 0 < i < 1000:
                                have[str(i)] = have.get(str(i), 0) + 1
                    writer.write((json.dumps({'op': 'achievement_share', 'players': players, 'have': have}) +
                                  '\n').encode())
                elif op == 'player_card':
                    # Stats & Rewards, and a leaderboard row's card: one player's record (counts only, by name).
                    card = PLAYER_STATS.card(str(m.get('name', ''))[:15], LEADERBOARD)
                    writer.write((json.dumps(dict(card, op='player_card')) + '\n').encode())
                elif op == 'leaderboard':
                    rows, total = leaderboard_top()
                    writer.write((json.dumps({'op': 'leaderboard', 'rows': with_tags(rows), 'total': total}) +
                                  '\n').encode())
                elif op == 'reserve' and maintenance_now(address):
                    # Maintenance mode (maintenance_now): every join refused with its text, whatever server was picked.
                    notice = maintenance_now(address)
                    RESERVATIONS.pop(who_asks(m, address), None)
                    writer.write((json.dumps({'op': 'closed', 'kind': notice['kind'], 'title': notice['title'],
                                              'message': notice['text']}) + '\n').encode())
                    self.note('%s refused game %s: maintenance (%s)' % (peer, str(m.get('gid', ''))[:16], notice['kind']))
                elif op == 'reserve' and m.get('gid') in GAMES and \
                        join_refusal(GAMES[m['gid']], asker(m, address), address):
                    # Moderation (join_refusal): the pop-up, with the reason and when it ends. (Never the word
                    # "reserved" in it: bans.reason_problem.)
                    title, message, who, network, entry = join_refusal(GAMES[m['gid']], asker(m, address), address)
                    give_up_own(who_asks(m, address))
                    writer.write((json.dumps({'op': 'banned', 'gid': m['gid'], 'network': network, 'title': title,
                                              'message': message}) + '\n').encode())
                    self.note('%s refused game %s: %s%s' % (peer, m['gid'], who[0] or 'a player', ' is banned on the '
                                                            'Reborn network (%s)' % entry.get('id') if network else
                                                            ' is in the server\'s ban list (%s %s)' % (
                                                                entry.get('kind'), entry.get('id'))))
                    tell_refused(GAMES[m['gid']], who, network, entry)
                elif op == 'reserve' and m.get('gid') in GAMES:
                    yours, platform = str(m.get('version', ''))[:24], str(m.get('platform', ''))[:16]
                    if outdated(yours, platform):
                        # (No word of the reply may hold "reserved": a game from before the gate looks for it.)
                        need = required_version(platform)
                        RESERVATIONS.pop(who_asks(m, address), None)
                        writer.write((json.dumps({'op': 'outdated', 'required': need, 'yours': yours}) + '\n').encode())
                        self.note('%s refused game %s: its version %s, players need v%s' %
                                  (peer, m['gid'], 'v' + yours if yours else 'not said (before the gate)', need))
                    else:
                        who = who_asks(m, address)
                        self.note_given_up(queue_sweep())
                        renewal = RESERVATIONS.get(who, (None, 0))[0] == m['gid']
                        # A player waiting for another server who picks this one leaves that queue; one waiting for
                        # this one keeps its place.
                        mine = queue_entry(who)
                        if mine and mine['gid'] != m['gid']:
                            QUEUE.remove(mine)
                            self.note('%s left the queue of game %s: picked game %s' % (peer, mine['gid'], m['gid']))
                            mine = None
                        line = queue_line(m['gid'])
                        full, players, arriving, most = server_full(m['gid'], who, line.index(mine) if mine else None)
                        waiting = len(line) - (1 if mine else 0)
                        may_wait = mine or (len(line) < QUEUE_MOST and
                                            sum(e['address'] == address for e in QUEUE) < ADDRESS_QUEUED)
                        if full and not renewal and m.get('queue') == 1 and may_wait:
                            # A game with the queue: a place in it (the same one when it asks again).
                            if not mine:
                                mine = {'token': os.urandom(8).hex(), 'gid': m['gid'], 'address': address,
                                        'id': who[1],
                                        'server': (GAMES[m['gid']]['I'], int(GAMES[m['gid']]['P'])),
                                        'since': time.time()}
                                QUEUE.append(mine)
                            mine['seen'] = time.time()
                            RESERVATIONS.pop(who, None)
                            reply = queue_reply(mine)
                            writer.write((json.dumps(reply) + '\n').encode())
                            self.note('%s queued for game %s: %d of %d (%d players, %d on their way, %d at most)' %
                                      (peer, m['gid'], reply['position'], reply['total'], players, arriving, most))
                        elif full and not renewal:
                            # (A game that does not know "full" says the list did not answer, and joins nothing.)
                            RESERVATIONS.pop(who, None)
                            # (Never the server's name in "message": a name holding "reserved" would read as a yes.)
                            text = 'This server has %d of %d players. Pick another server, or try again in a moment.' % (
                                min(players + arriving, most), most)
                            if waiting:
                                text = 'This server has %d of %d players, and %d %s waiting for a place. Pick another ' \
                                       'server, or try again in a moment.' % (min(players + arriving, most), most,
                                                                               waiting, 'player is' if waiting == 1
                                                                               else 'players are')
                            writer.write((json.dumps({'op': 'full', 'gid': m['gid'], 'players': players,
                                                      'max_players': most, 'title': 'SERVER FULL',
                                                      'message': text}) + '\n').encode())
                            self.note('%s refused game %s: full (%d players, %d on their way, %d waiting, %d at most)' %
                                      (peer, m['gid'], players, arriving, waiting, most))
                        else:
                            if mine:
                                QUEUE.remove(mine)
                            let_in(m['gid'], who, players, renewal=renewal)
                            writer.write((json.dumps({'op': 'reserved', 'gid': m['gid']}) + '\n').encode())
                            self.note('%s picked game %s%s' % (peer, m['gid'], ' (v%s)' % yours if yours else ''))
                elif op == 'queue':
                    # A queued game asking for its place (every few seconds): its place, or its turn ("reserved").
                    address = peer.split(':')[0]
                    self.note_given_up(queue_sweep())
                    token = str(m.get('token', ''))[:32]
                    mine = next((e for e in QUEUE if e['token'] == token and e['address'] == address), None)
                    notice = maintenance_now(address)
                    if notice:
                        if mine:
                            QUEUE.remove(mine)
                        writer.write((json.dumps({'op': 'closed', 'kind': notice['kind'], 'title': notice['title'],
                                                  'message': notice['text']}) + '\n').encode())
                    elif not mine and str(m.get('gid', '')) not in GAMES:
                        writer.write((json.dumps({'op': 'gone', 'gid': str(m.get('gid', ''))[:16], 'title': 'SERVER GONE',
                                                  'message': 'The server stopped while you waited for a place. Pick '
                                                             'another server.'}) + '\n').encode())
                        self.note('%s asked for its place in the queue of game %s, which is gone' % (
                            peer, str(m.get('gid', ''))[:16]))
                    elif not mine:
                        # Not waiting (given up after QUEUE_TIMEOUT, or this backend started again): the game asks
                        # with "reserve" again, and waits at the end.
                        writer.write((json.dumps({'op': 'queue_lost', 'gid': str(m.get('gid', ''))[:16]}) +
                                      '\n').encode())
                        self.note('%s asked for a place it does not have (game %s)' % (peer, str(m.get('gid', ''))[:16]))
                    else:
                        mine['seen'] = time.time()
                        line = queue_line(mine['gid'])
                        waiter = (address, mine['id'])
                        full, players, arriving, most = server_full(mine['gid'], waiter, line.index(mine)) \
                            if mine['gid'] in GAMES else (True, 0, 0, 0)
                        refused = join_refusal(GAMES.get(mine['gid']), asker(m, address), address) \
                            if mine['gid'] in GAMES else None
                        if refused:
                            if mine['id'] in ('', bans.clean_id(m.get('id'))):  # (as give_up_own: their own place)
                                QUEUE.remove(mine)
                            writer.write((json.dumps({'op': 'banned', 'gid': mine['gid'], 'network': refused[3],
                                                      'title': refused[0], 'message': refused[1]}) + '\n').encode())
                            self.note('%s left the queue of game %s: banned' % (peer, mine['gid']))
                        elif not full:
                            QUEUE.remove(mine)
                            let_in(mine['gid'], waiter, players, ADMIT_WINDOW)
                            writer.write((json.dumps({'op': 'reserved', 'gid': mine['gid']}) + '\n').encode())
                            self.note('%s let in to game %s from its queue after %.0f s (%d players, %d on their way, '
                                      '%d at most; %d still waiting)' % (
                                          peer, mine['gid'], time.time() - mine['since'], players, arriving, most,
                                          len(line) - 1))
                        else:
                            writer.write((json.dumps(queue_reply(mine)) + '\n').encode())
                elif op == 'queue_leave':
                    # The pop-up's Close: the place is given up at once.
                    address = peer.split(':')[0]
                    token = str(m.get('token', ''))[:32]
                    mine = next((e for e in QUEUE if e['token'] == token and e['address'] == address), None)
                    if mine:
                        QUEUE.remove(mine)
                        self.note('%s left the queue of game %s after %.0f s (%d still waiting)' % (
                            peer, mine['gid'], time.time() - mine['since'], len(queue_line(mine['gid']))))
                    writer.write((json.dumps({'op': 'queue_left'}) + '\n').encode())
                elif op == 'name':
                    asked = player_stats.clean_name(m.get('name', ''))
                    try:
                        pid = int(str(m.get('id', '0')), 16) & 0xFFFFFFFF
                    except ValueError:
                        pid = 0
                    if not asked or not pid:
                        writer.write((json.dumps({'op': 'name', 'name': asked}) + '\n').encode())
                    else:
                        reply = note_name_check(self.name_reply(asked, pid, address), pid, address)  # clan tags
                        writer.write((json.dumps(reply) + '\n').encode())
                elif op == 'find':
                    self.refresh_promoted()
                    want = (str(m.get('address', '')), int(m.get('port', 18300) or 18300))
                    games = [for_player(g, writer, self.args) for g in GAMES.values() if g.get('TYPE') == 'G']
                    hit = [g for g in games if (g['I'], g['P']) == want]
                    writer.write((json.dumps({'op': 'found', 'server': listing(hit[0]) if hit else None}) +
                                  '\n').encode())
                elif op == 'list':
                    self.refresh_promoted()
                    self.note_given_up(queue_sweep())  # each server's waiting players, as the list says them
                    # The address goes to the game (its ping and its join need it); the browser never shows it.
                    servers = [listing(for_player(g, writer, self.args)) for g in GAMES.values()
                               if g.get('TYPE') == 'G' and g.get('listed', True)]
                    # The players signed in now, for the launcher's Home (PLAYERS ONLINE): every open FESL persona but
                    # the servers' own.
                    hosts = {g.get('HN') for g in GAMES.values()}
                    online = len([p for p in Client.online if p not in hosts])
                    reply = {'op': 'servers'}
                    notice = maintenance_now(address)
                    if notice:  # before "servers": the games read the first field of each name
                        reply.update(notice_kind=notice['kind'], notice_title=notice['title'],
                                     notice_text=notice['text'], notice_seconds=notice['seconds'])
                        if 'notice' not in m:  # a game from before maintenance mode: one row says it
                            servers = [notice_row(notice)]
                    reply.update(servers=servers, players_online=online)
                    need = required_version(str(m.get('platform', '')))
                    if need:
                        reply['required_version'] = need
                        if 'version' not in m and not notice:  # a game from before the gate: it has no pop-up, so
                            reply['servers'] = [update_notice('classic'), update_notice('modern')]  # its list says it
                    writer.write((json.dumps(reply) + '\n').encode())
                await writer.drain()
        except (ConnectionError, ValueError, KeyError) as e:
            self.note('%s %s' % (peer, type(e).__name__))
        finally:
            if gid:
                GAMES.pop(gid, None)
                GONE[gid] = time.time()  # its queue waits SERVER_GONE_GRACE for it to register again
                STATS.gone(gid)
                MATCH_SCORES.pop(gid, None)
                self.note('game %s gone (server disconnected)' % gid)
            writer.close()


# ---- Moderation (TekRantGaming, 20 Nov 2026; server/bans.py, docs/MODERATION.md). Two kinds of list, enforced here
# before a join and again by each host (gmhost.py), so a modified game cannot walk past either:
#   - the Reborn network's list (--network-bans: the official manager's network_bans.json on this machine only): a player
#     in it joins no server registered here, official or community, Direct connect to their addresses included. A server
#     on its own backend (LAN only) never asks here, so it stays open to them.
#   - each server's own list (its manager's bans.json): its host tells this backend a salted, hashed copy over its own
#     registry link ("server_bans"), so the player gets the pop-up before joining, and it holds on that server only.
# A refusal is checked at "reserve" (every join from our menus: the pop-up, {"op": "banned", "title", "message"}), at a
# queued player's turn, and at Theater's EGAM (the step that gives the server's address: a game that skipped "reserve"
# gets no address). A player is matched by its player id (the XUID's low word), its account (Protect your name) and its
# address. A player on a server when a network ban is made is removed: its host gets {"op": "remove", "player_id"}.
NETWORK_BANS = [None]   # the BanList, with --network-bans
JOINS = {}   # GID -> {player id: (name, address, account, when)}: who joined each server (EGAM), for "remove"
SEEN = {}    # a name, lower case -> {'name', 'id', 'ip', 'account', 'at', 'server'}: the official manager's lookup;
             # in memory only, a week at most
SEEN_DAYS = 7


def player_id_of(xuid):
    return bans.id_of_xuid(xuid)


def account_of(pid_hex, address):
    """The account key a player signed in for (Protect your name), by its player id and address, or ''."""
    now = time.time()
    for key, (pid, at, until) in list(VERIFIED.items()):
        if until > now and '%08X' % (int(pid) & 0xFFFFFFFF) == pid_hex and at == address:
            return key
    return ''


def asker(m, address):
    """Who asks with a "reserve" or "queue": [(name, player id, account)], or [] when only the address is known. One
    address can be a household's or a LAN party's, so a join is never checked as the others signed in from it. The
    game says its id, matched with that id's own sign in from the same address. Without one (an older game, or before
    the sign in) the address is what is known here; EGAM and the host check the player as signed in."""
    said = bans.clean_id(m.get('id'))
    if not said:
        return []
    for lkey, name in reversed(list(Client.sessions.items())):  # the newest sign in, for its name
        if Client.addresses.get(lkey) == address and player_id_of(Client.xuids.get(lkey, '0')) == said:
            return [(name, said, account_of(said, address))]
    return []


def seen(name, pid, address, account, server=''):
    if not name:
        return
    now = int(time.time())
    SEEN[name.lower()] = {'name': name, 'id': pid, 'ip': address, 'account': account, 'at': now,
                          'server': server or SEEN.get(name.lower(), {}).get('server', '')}
    if len(SEEN) > 20000:
        for k in sorted(SEEN, key=lambda k: SEEN[k]['at'])[:2000]:
            del SEEN[k]


def network_bans():
    nb = NETWORK_BANS[0]
    if nb:
        nb.reload()
    return nb


def server_ban(g, pid, account, address, now=None):
    """The entry of the server's own list (its host's server_bans) that holds against this player, or None."""
    now = time.time() if now is None else now
    table = g.get('bans') if g else None
    if not table:
        return None
    salt = table['salt']
    hp, ha, hi = bans.hashed(salt, pid), bans.hashed(salt, account), bans.hashed(salt, bans.clean_ip(address))
    for row in table['rows']:
        if row['until'] and row['until'] <= now:
            continue
        if (hp and hp in row['ids']) or (ha and ha in row['accounts']) or \
                (hi and hi in row['ips'] and (not row['ip_until'] or row['ip_until'] > now)):
            return row
    return None


def join_refusal(g, players, address):
    """Why this join is refused, or None: (title, message, the player (name, id), network: bool, the entry). players:
    [(name, player id, account)], the one player joining (asker, or EGAM's own sign in), or [] for the address alone."""
    nb = network_bans()
    for name, pid, account in (players or [('', '', '')]):
        e = nb.match(pid, account, address) if nb else None
        if e:
            title, message = bans.refusal(e, True)
            return title, message, (name, pid), True, e
        row = server_ban(g, pid, account, address)
        if row:
            if row.get('kind') == 'kick':  # its minutes counted from now (the host made the text when it sent it)
                minutes = max(1, int(round((row['until'] - time.time()) / 60.0)))
                message = row['message'].split(' You can join again in ')[0] + ' You can join again in %d minute%s.' % (
                    minutes, '' if minutes == 1 else 's')
                return row['title'], message, (name, pid), False, row
            return row['title'], row['message'], (name, pid), False, row
    return None


def tell_refused(g, who, network, entry):
    """The server's host is told a join was refused (its moderation log: only "the network's" for a network ban)."""
    host = g.get('gmhost') if g else None
    if not isinstance(host, RemoteHost):
        return
    m = {'op': 'refused_join', 'name': who[0], 'player_id': who[1], 'network': bool(network)}
    if not network:
        m.update(kind=entry.get('kind', ''), ban=entry.get('id', ''))
    try:
        host.writer.write((json.dumps(m) + '\n').encode())
    except (ConnectionError, RuntimeError, OSError):
        pass


def clean_server_bans(m):
    """A host's server_bans, as kept: hex digests only, a reason's text as the host made it (checked as a notice)."""
    salt = str(m.get('salt', ''))[:32]
    rows = []
    for r in (m.get('bans') or [])[:2000]:
        if not isinstance(r, dict) or r.get('kind') not in ('ban', 'kick'):
            continue
        hexes = lambda k: [str(x)[:32] for x in (r.get(k) or [])[:16] if all(c in '0123456789abcdef' for c in str(x))]
        try:
            until, ip_until = int(r.get('until') or 0), int(r.get('ip_until') or 0)
        except (TypeError, ValueError):
            continue
        title = ' '.join(str(r.get('title', '')).split())[:60]
        message = ' '.join(str(r.get('message', '')).split())[:400]
        if not title or not message or 'reserved' in (title + message).lower() or any(c in title + message for c in '"\\{}'):
            continue
        rows.append({'id': str(r.get('id', ''))[:16], 'kind': r['kind'], 'ids': hexes('ids'),
                     'accounts': hexes('accounts'), 'ips': hexes('ips'), 'until': until, 'ip_until': ip_until,
                     'title': title, 'message': message})
    return {'salt': salt, 'rows': rows} if salt else None


async def watch_network_bans(note):
    """A new network ban takes its player off the server they are on: every host whose joins (JOINS) match is told
    to remove them, by player id only."""
    told = {}  # (gid, player id) -> the ban's id it was told for
    while True:
        await asyncio.sleep(2)
        try:
            nb = network_bans()
            if not nb:
                continue
            now = time.time()
            for gid, joined in list(JOINS.items()):
                g = GAMES.get(gid)
                if not g:
                    JOINS.pop(gid, None)
                    continue
                names = {n.lower() for n in g.get('names', [])}
                for pid, (name, address, account, at) in list(joined.items()):
                    if now - at > 6 * 3600 and name.lower() not in names:
                        del joined[pid]
                        continue
                    e = nb.match(pid, account, address)
                    if not e or told.get((gid, pid)) == e['id'] or name.lower() not in names:
                        continue
                    told[(gid, pid)] = e['id']
                    title, message = bans.refusal(e, True)
                    host = g.get('gmhost')
                    if isinstance(host, RemoteHost):
                        host.writer.write((json.dumps({'op': 'remove', 'player_id': pid, 'title': title,
                                                       'message': message}) + '\n').encode())
                    elif host is not None and hasattr(host, 'network_out'):  # a --dedicated host in this process
                        host.network_out[pid] = {'kind': 'network', 'title': title, 'message': message,
                                                 'until': now + 600}
                        host.enforce()
                    note('network ban %s: %s (%s) taken off game %s "%s"' % (e['id'], name, pid, gid, g.get('N', '')))
            for k in [k for k in told if k[0] not in GAMES]:
                del told[k]
            for k in [k for k, v in SEEN.items() if now - v['at'] > SEEN_DAYS * 86400]:
                del SEEN[k]
        except Exception as e:  # never stop watching for one bad turn
            note('network bans: %s: %s' % (type(e).__name__, e))


def admin_players(what):
    """The official manager's moderation questions (signed, Registry admin): "players" (everyone logged in now and
    where they play) or "lookup:<text>" (names seen in the last week holding the text, or that player id)."""
    where = {}
    for g in GAMES.values():
        for n in g.get('names', []):
            where[n.lower()] = g
    if what == 'players':
        rows = []
        for lkey, name in list(Client.sessions.items()):
            if lkey in Client.closed:
                continue
            address = Client.addresses.get(lkey, '')
            pid = player_id_of(Client.xuids.get(lkey, '0'))
            g = where.get(name.lower())
            rows.append({'name': name, 'id': pid, 'ip': address, 'account': account_of(pid, address),
                         'server': g.get('N', '') if g else '', 'gid': g['GID'] if g else '',
                         'official': bool(g.get('official')) if g else False})
        return sorted(rows, key=lambda r: r['name'].lower())[:500]
    text = what.split(':', 1)[1].strip().lower()[:32] if ':' in what else ''
    if not text:
        return []
    rows = [dict(v, online=k in where) for k, v in SEEN.items() if text in k or text.upper() == v.get('id')]
    return sorted(rows, key=lambda r: -r['at'])[:100]


def own_address(writer, public):
    """LAN only mode (--lan; TekRantGaming, 13 Nov 2026: "lan only so that they don't need to connect to the backend
    and they can just use local connections or allow it online themselves and get external users to connect via
    direct ip"): the address ONE player should use for this machine's own services (Theater, and the servers that
    registered from this machine).

    A player on this machine or this network (a loopback, private, link local or carrier address) gets the address
    it reached this backend on: that one works for it by definition, whichever of this machine's addresses it is.
    A player from the internet gets the public address the host set (--theater-host), since a router's port
    forwarding hides it from this machine; without one, again the address it reached (a hosted machine's own).
    So players at home and players outside join the same server at the same time."""
    import ipaddress
    try:
        peer = ipaddress.ip_address(writer.get_extra_info('peername')[0])
        local = writer.get_extra_info('sockname')[0]
    except (ValueError, TypeError, IndexError):
        return public
    if not peer.is_global or not public or public.startswith('127.'):
        return local
    return public


def for_player(g, writer, args):
    """A game as one player gets it: in LAN only mode a server on this machine has the address that player can
    reach (own_address), with its XNADDR to match; else the game as it is."""
    if not (args and getattr(args, 'lan', False) and g.get('local')):
        return g
    host = own_address(writer, args.theater_host)
    if host == g['I']:
        return g
    return dict(g, I=host, XNADDR=base64.b64encode(gmhost.xnaddr(host)).decode())


# ---- Clan tags (TekRantGaming, 16 Nov 2026: "can be left blank or can have custom text, shows up next to a username in
# launcher, in game and in scoreboard like this [clan]Username. Ban the use of Devs and Dev clan tags and profanity
# ... Developers TekRantGaming and ZALIAS are devs of this project and should have the clan tag of Dev."). The backend
# decides who wears what: each game sends its player's tag here ({"op": "tag"}) and draws everyone's from what this
# says ({"op": "tags"}, and "tag" in the scoreboard, players and leaderboard rows), never from what another game
# claims. A tag is 2 to 4 letters or digits, checked by server/text_filter.py (the game's text_filter.h rules: Dev and
# its lookalikes, staff tags, profanity and slurs, and the tag and name read together). It is worn while its player
# holds the name (name_holder: the name check's hold, then the FESL login), and every game sends it again at its start
# and with its match's tag requests, so nothing needs keeping on disk. The team names (TEAM_OWNERS, the developers'
# accounts, which only their owners' signed games can hold) always wear Dev, whatever their game sends.
TAGS = {}  # a name, lower case -> (tag, player id)
NAME_FROM = {}  # a name, lower case -> (player id, address): the last name check that gave it (put_tag)
DEV_TAG = 'Dev'


def note_name_check(reply, pid, address):
    """A name check's answer: which player and address it gave the name to, so only they set its tag."""
    if isinstance(pid, str):
        try:
            pid = int(pid, 16) & 0xFFFFFFFF
        except ValueError:
            pid = 0
    if isinstance(reply, dict) and reply.get('name') and pid:
        if len(NAME_FROM) > 100000:
            NAME_FROM.clear()
        NAME_FROM[reply['name'].lower()] = (pid, address)
    return reply


def dev_name(name):
    """Whether this is one of the developers' names (a team name of TEAM_OWNERS)."""
    return bool(name) and team_owner(name) is not None


def tag_for(name):
    """The clan tag worn by the player under this name now ('' for none)."""
    if not isinstance(name, str) or not name:
        return ''
    if dev_name(name):
        return DEV_TAG
    held = TAGS.get(name.lower())
    if not held:
        return ''
    if name_holder(name) != held[1]:  # the name's hold ended, or another player has it now
        TAGS.pop(name.lower(), None)
        return ''
    if text_filter.tag_problem(held[0], name) == 'reserved':  # taken before it was reserved (BOT, 20 Nov 2026)
        TAGS.pop(name.lower(), None)
        return ''
    return held[0]


def with_tags(rows):
    """Rows of names ({'name': ...}) with each one's tag added where there is one (copies: the tables stay as kept)."""
    out = []
    for r in rows:
        tag = tag_for(r.get('name', ''))
        out.append(dict(r, tag=tag) if tag else r)
    return out


def put_tag(m, address):
    """A game's {"op": "tag", "name", "id", "tag"}: its player's tag, worn from now on. Only the player holding the name
    may set its tag (the same player id; for a protected name, the one signed in for it from this address)."""
    name = str(m.get('name', ''))[:15]
    tag = str(m.get('tag', '')).strip()[:16]
    try:
        pid = int(str(m.get('id', '')), 16) & 0xFFFFFFFF
    except ValueError:
        pid = 0
    acct = protected_by(name) if name else None
    if not name or not pid or name_holder(name) != pid or NAME_FROM.get(name.lower()) != (pid, address) or \
            (acct and not signed_in_for(acct, pid, address)):
        return {'op': 'tag', 'ok': False, 'why': 'not_yours', 'tag': tag_for(name),
                'text': 'Your name is not signed in yet: your clan tag is used from your next start.'}
    if dev_name(name):
        return {'op': 'tag', 'ok': True, 'tag': DEV_TAG, 'dev': True}
    why = text_filter.tag_problem(tag, name)
    if why:
        return {'op': 'tag', 'ok': False, 'why': why, 'text': text_filter.TAG_PROBLEM_TEXT[why], 'tag': tag_for(name)}
    if len(TAGS) > 50000:
        for k in [k for k, (_, p) in TAGS.items() if name_holder(k) != p]:
            del TAGS[k]
    if tag:
        TAGS[name.lower()] = (tag, pid)
    else:
        TAGS.pop(name.lower(), None)
    return {'op': 'tag', 'ok': True, 'tag': tag}


def tags_reply(m, address):
    """A game's {"op": "tags", "gid"?, "names"?, "me"?}: the tags of a server's players and of the names asked about
    (64 at most), {name: tag} for those wearing one. "me" ({"name", "id", "tag"}) sets the asker's own first (put_tag),
    so a tag is back at once after this backend restarts."""
    if isinstance(m.get('me'), dict):
        put_tag(m['me'], address)
    names = []
    g = GAMES.get(str(m.get('gid', '')))
    if g:
        names += g.get('names', [])
    if isinstance(m.get('names'), list):
        names += [str(n)[:15] for n in m['names'][:64]]
    tags = {}
    for n in names[:128]:
        tag = tag_for(n)
        if tag:
            tags[n] = tag
    return {'op': 'tags', 'tags': tags}


def tick_of(value):
    """A server's tick rate in Hz as it registered it: a whole number from 10 to 240, else 0 (not said)."""
    try:
        tick = int(value)
    except (TypeError, ValueError):
        return 0
    return tick if 10 <= tick <= 240 else 0


def listing(g):
    """A game as the server browser gets it."""
    return {'gid': g['GID'], 'name': g['N'], 'address': [g['I'], g['P']], 'map': g.get('map', ''),
            'players': g['AP'], 'bots': g.get('bots', 0), 'max_players': g['MAX-PLAYERS'],
            'mode': g.get('mode', 'classic'),
            'description': g.get('description', ''), 'official': bool(g.get('official') or g.get('promoted')),
            'promoted': bool(g.get('promoted') and not g.get('official')),  # Promoted: on the Reborn tab, marked
            'next_map': g.get('next_map', ''), 'maps': g.get('maps', []), 'night': bool(g.get('night')),
            'region': g.get('region', ''), 'tick': g.get('tick', 0),
            'queue': len(queue_line(g['GID']))}  # players waiting for a place (0.4.7.4)


def public(g):
    """A game's fields without our own bookkeeping."""
    return {k: v for k, v in g.items()
            if k not in ('host', 'gmhost', 'XNKID', 'XNKEY', 'XNADDR', 'map', 'mode', 'night', 'region', 'tick',
                         'server_version', 'local', 'hidden', 'from', 'host_key', 'build_digest', 'promoted',
                         'status_at', 'bots')}


async def http_client(reader, writer, log):
    """The game's HTTP requests (EA's editorial news, easo.ea.com, reached at the second LIVE service's address)."""
    peer = '%s:%d' % writer.get_extra_info('peername')[:2]
    try:
        head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 10)
        text = head.decode('latin-1').strip()
        line = '%s %s HTTP %s' % (time.strftime('%H:%M:%S'), peer, text.replace('\r\n', ' | '))
        log.write(line + '\n')
        log.flush()
        path = text.split(' ')[1] if ' ' in text else ''
        if path.endswith('/ticker-xenon.xml'):
            # The client checks for a UTF-8 byte order mark and "<?xml" in the first 8 bytes (else it retries in
            # 60 s), then reads <ticker><lang id="..."><item>text</item>...</lang></ticker> for its language.
            status, kind = b'200 OK', b'text/xml'
            body = (b'\xef\xbb\xbf<?xml version="1.0" encoding="UTF-8"?>\n<ticker>\n  <lang id="en-US">\n'
                    b'    <item>Local test server</item>\n  </lang>\n</ticker>\n')
        else:
            status, kind, body = b'404 Not Found', b'text/plain', b'Not found'
        style = os.environ.get('BF_HTTP_STYLE', '11')  # experiments with the reply format
        if style == '10':  # HTTP/1.0, no Connection header
            head_out = b'HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n' % (status, kind, len(body))
        elif style == 'nolen':  # HTTP/1.0, the close ends the body
            head_out = b'HTTP/1.0 %s\r\nContent-Type: %s\r\n\r\n' % (status, kind)
        else:
            head_out = (b'HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
                        % (status, kind, len(body)))
        writer.write(head_out + body)
        await writer.drain()
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, asyncio.LimitOverrunError):
        pass
    finally:
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=18241, help='FESL port')
    ap.add_argument('--theater-host', default='127.0.0.1')
    ap.add_argument('--lan', action='store_true',
                    help="LAN only mode (a server package's own backend): each player gets the address of this "
                         "machine it can reach: the one it connected to when it is on this machine or network, "
                         "else --theater-host (the public address)")
    ap.add_argument('--theater-port', type=int, default=18246)
    ap.add_argument('--check-servers', default='',
                    help="the official builds' manifests (a folder of *.json): list only community servers that run "
                         'one (ServerCheck); the others are Direct connect only')
    ap.add_argument('--blocked-file', default='', help='with --check-servers: hosts never listed (ServerCheck)')
    ap.add_argument('--promoted-file', default='',
                    help='community servers listed with the official ones, by host key (Promoted; manager.py promote)')
    ap.add_argument('--login-delay', type=float, default=2.0, help='seconds before the login reply')
    # Off by default: with our HTTP reply the client never finishes the ticker request (docs/01-online-flow.md).
    ap.add_argument('--http-port', type=int, default=0, help='HTTP port for the news ticker (0: off)')
    ap.add_argument('--game', action='append', default=[], metavar='HOST:PORT',
                    help='list a dedicated server (bf1943.exe --server --cl=-dedicated) for Quick Match')
    # The client's own local EGEG (sub_824B03F8) sends these two: no tunnel key, no game UID.
    ap.add_argument('--ekey', default='NOENCYRPTIONKEY', help="EGEG's tunnel key (the client's spelling)")
    ap.add_argument('--list-private', action='store_true',
                    help='Quick Match also offers private games a client hosts (join tests)')
    ap.add_argument('--no-create', action='store_true',
                    help='Quick Match never asks a client to host (only --game servers and hosted games)')
    ap.add_argument('--dedicated', action='append', default=[], metavar='GWHOST:GWPORT=SERVERHOST:SERVERPORT',
                    help='list a dedicated server behind our GameManager host (server/gmhost.py): joiners link to '
                         'GWHOST:GWPORT, game packets go on to the server at SERVERHOST:SERVERPORT (UDP 1003)')
    ap.add_argument('--registry-host', default='', help='where dedicated servers register (default: --host)')
    ap.add_argument('--registry-port', type=int, default=18250, help='server registry port (0: off)')
    ap.add_argument('--official-key-file', default='',
                    help='official.key: servers registering with a code from it are official (TekRantGaming only)')
    ap.add_argument('--version-feed', default='',
                    help="the version gate: players need the newest release of this feed (a GitHub releases/latest "
                         "API address, the one the game's updater reads)")
    ap.add_argument('--version-feed-every', type=float, default=300.0, help='seconds between two looks at the feed')
    ap.add_argument('--required-version', default='',
                    help='the version gate: players need this version (in place of --version-feed)')
    ap.add_argument('--maintenance-file', default='',
                    help='maintenance mode: a file the server manager writes (manager.py maintenance); while it is on, '
                         'new joins are refused with its text (maintenance_now)')
    ap.add_argument('--network-bans', default='',
                    help="the Reborn network's ban list (the official manager's network_bans.json; server/bans.py): "
                         'read again when it changes')
    ap.add_argument('--log', default='runs/plasma.log')
    ap.add_argument('--leaderboard', default='', help="a file keeping the players' totals (our Leaderboards)")
    ap.add_argument('--player-stats', default='',
                    help="a file keeping each player's stats as the servers count them (server/player_stats.py)")
    ap.add_argument('--player-stats-from', default='official', choices=('official', 'all'),
                    help="whose counts are kept: official servers only, or every server (a test backend)")
    ap.add_argument('--accounts', default='',
                    help="a file keeping the players' accounts (Protect your name: protected names and progress)")
    ap.add_argument('--team-names', default='',
                    help='a JSON file of team names ({"owners": [...], "left_free": [...]}, as TEAM_OWNERS) in place of '
                         'the built in ones (tests)')
    ap.add_argument('--accounts-save-every', type=float, default=30.0,
                    help='seconds between two writes of the accounts file (only when something changed)')
    ap.add_argument('--stats', default='',
                    help="a file keeping the website's totals (server/stats.py: players who joined a match, by month, "
                         "and hours played; totals only, ids kept hashed)")
    ap.add_argument('--stats-save-every', type=float, default=30.0,
                    help='seconds between two writes of the stats file (only when something changed)')
    ap.add_argument('--test-progression', action='store_true',
                    help='store rank and achievement IDs submitted by local test clients')
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0: run until stopped)')
    ap.add_argument('--stop-on-stdin-eof', action='store_true', help='stop when the launcher closes its input pipe')
    args = ap.parse_args()
    if args.test_progression and (args.host != '127.0.0.1' or
                                  args.registry_host not in ('', '127.0.0.1')):
        ap.error('--test-progression requires a loopback backend and registry')
    global LEADERBOARD_FILE
    LEADERBOARD_FILE = args.leaderboard or None
    if LEADERBOARD_FILE and os.path.exists(LEADERBOARD_FILE):
        try:
            for e in json.load(open(LEADERBOARD_FILE, encoding='utf-8')):
                LEADERBOARD[str(e['name']).lower()] = e
        except (OSError, ValueError, KeyError, TypeError):
            pass
    global ACCOUNTS_FILE
    ACCOUNTS_FILE = args.accounts or None
    if ACCOUNTS_FILE and os.path.exists(ACCOUNTS_FILE):
        load_accounts(ACCOUNTS_FILE)
    if args.team_names:
        with open(args.team_names, encoding='utf-8') as f:
            team = json.load(f)
        set_team_names(team.get('owners', []), team.get('left_free', TEAM_NAMES_LEFT_FREE))
    keep_team_names()
    STATS.path = args.stats or None
    STATS.load()
    global PLAYER_STATS
    PLAYER_STATS = player_stats.PlayerStats(args.player_stats or None)
    for n, address in enumerate(args.game, 1):
        host, port = address.rsplit(':', 1)
        GAMES[str(n)] = {'GID': str(n), 'LID': '1', 'I': host, 'P': int(port), 'HU': n, 'HN': 'bf1943-server-%d' % n,
                         'N': 'Local server %d' % n, 'TYPE': 'G', 'JOIN': 'O', 'MAX-PLAYERS': 24, 'AP': 0,
                         'UGID': 'NOGUID', 'B-version': 'RETAIL429432', 'B-U-type': 'RANKED'}
    with open(args.log, 'a') as log:
        for n, spec in enumerate(args.dedicated, len(args.game) + 1):
            gw, srv = spec.split('=')
            gw_host, gw_port = gw.rsplit(':', 1)
            srv_host, srv_port = srv.rsplit(':', 1)
            uid, name = 900 + n, 'Dedicated server %d' % n
            host = await gmhost.start((gw_host, int(gw_port)), (srv_host, int(srv_port)), uid,
                                      next(TheaterClient.pids), name, log)
            GAMES[str(n)] = {'GID': str(n), 'LID': '1', 'I': gw_host, 'P': int(gw_port), 'HU': uid, 'HN': name,
                             'N': name, 'TYPE': 'G', 'JOIN': 'O', 'MAX-PLAYERS': 24, 'AP': 0, 'UGID': 'NOGUID',
                             'B-version': 'RETAIL429432', 'B-U-type': 'RANKED', 'gmhost': host,
                             'XNADDR': base64.b64encode(gmhost.xnaddr(gw_host)).decode(),
                             'XNKID': base64.b64encode(os.urandom(8)).decode(),
                             'XNKEY': base64.b64encode(os.urandom(16)).decode()}
        fesl = await asyncio.start_server(lambda r, w: Client(r, w, args, log).run(), args.host, args.port)
        theater = await asyncio.start_server(lambda r, w: TheaterClient(r, w, args, log).run(), args.host,
                                             args.theater_port)
        log.write('%s FESL on %s:%d, Theater on %s:%d\n' % (time.strftime('%H:%M:%S'), args.host, args.port,
                                                          args.host, args.theater_port))
        log.flush()
        servers = [fesl, theater]
        if args.registry_port:
            registry = Registry(log, args)
            servers.append(await asyncio.start_server(registry.client, args.registry_host or args.host,
                                                      args.registry_port))
            log.write('%s server registry on %s:%d\n' % (time.strftime('%H:%M:%S'), args.registry_host or args.host,
                                                          args.registry_port))
            if registry.check:
                registry.check.reload()
                asyncio.get_running_loop().create_task(registry.check.watch())
        if args.http_port:
            servers.append(await asyncio.start_server(lambda r, w: http_client(r, w, log), args.host, args.http_port))
            log.write('%s HTTP on %s:%d\n' % (time.strftime('%H:%M:%S'), args.host, args.http_port))
        def gate_note(text):
            log.write('%s %s\n' % (time.strftime('%H:%M:%S'), text))
            log.flush()
        feed_task = None
        if args.maintenance_file:
            MAINTENANCE.update(file=args.maintenance_file, note={'said': None, 'say': gate_note})
            maintenance_now()
        bans_task = None
        if args.network_bans:
            NETWORK_BANS[0] = bans.BanList(args.network_bans, gate_note, network=True)
            gate_note('network bans: %d entries (%s)' % (len(NETWORK_BANS[0].entries), args.network_bans))
            bans_task = asyncio.ensure_future(watch_network_bans(gate_note))
        accounts_task = asyncio.ensure_future(keep_accounts_saved(max(0.5, args.accounts_save_every)))
        stats_task = asyncio.ensure_future(keep_stats_saved(max(0.5, args.stats_save_every)))
        async def keep_player_stats_saved():  # each player's stats (player_stats.py), when something changed
            while True:
                await asyncio.sleep(10)
                PLAYER_STATS.save(force=True)
        player_stats_task = asyncio.ensure_future(keep_player_stats_saved())
        log.write('%s accounts: %d%s\n' % (time.strftime('%H:%M:%S'), len(ACCOUNTS),
                                             ' (%s)' % ACCOUNTS_FILE if ACCOUNTS_FILE else ' (kept in memory only)'))
        log.write('%s team names: %s\n' % (time.strftime('%H:%M:%S'),
                                           ', '.join(o['owner'] for o in TEAM['owners']) or 'none'))
        t = STATS.totals()
        log.write('%s stats: %d players, %d this month, %d hours%s\n' % (
            time.strftime('%H:%M:%S'), t['players'], t['players_month'], t['hours'],
            ' (%s)' % STATS.path if STATS.path else ' (kept in memory only)'))
        if args.required_version:
            VERSION_GATE.update(required=args.required_version.lstrip('vV'), platforms=None, source='--required-version')
            gate_note('version gate: players need v%s (--required-version)' % VERSION_GATE['required'])
        elif args.version_feed:
            feed_task = asyncio.ensure_future(watch_feed(args.version_feed, max(5.0, args.version_feed_every), gate_note))
        else:
            gate_note('version gate: off (no --version-feed, no --required-version)')
        if args.seconds:
            await asyncio.sleep(args.seconds)
        elif args.stop_on_stdin_eof:
            await asyncio.to_thread(sys.stdin.buffer.read)
        else:
            await asyncio.gather(*(srv.serve_forever() for srv in servers))
        if feed_task:
            feed_task.cancel()
        if bans_task:
            bans_task.cancel()
        accounts_task.cancel()
        stats_task.cancel()
        player_stats_task.cancel()
        save_accounts()
        STATS.save()
        PLAYER_STATS.save(force=True)


if __name__ == '__main__':
    asyncio.run(main())
