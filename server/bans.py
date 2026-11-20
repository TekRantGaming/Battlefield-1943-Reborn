"""Server moderation (TekRantGaming, 20 Nov 2026: "community servers to have their own independent ban lists, their own
moderation tools, and of course, for our official servers, we're going to need our own separate ban list ... anyone
that has been banned from a community server, is only banned from that community server. But if they're banned on the
official networks, that means they're banned from joining any official server and community server, but can still join
via direct IP as long as the direct IP doesn't belong to one of the community servers or an official server that's
hooked up to our backend"). docs/MODERATION.md has the whole design; this is the part every program shares.

A ban list is a JSON file: {"version": 1, "bans": [entry, ...]}. One entry:
  id        "b-" and 8 hex digits, made here
  kind      "ban" (no joining), "kick" (no joining for KICK_SECONDS; made by a kick) or "mute" (plays, but its voice and
            text chat reach nobody)
  ids       player ids, 8 hex digits as the game shows them (the XUID's low word, made at a game's first start)
  accounts  account keys (Protect your name: the Ed25519 public key, 64 hex digits)
  ips       the addresses the player was seen at; matched only until "ip_until" (IP_DAYS at most), then dropped
  names     the names the player was seen under: shown to admins, never matched (anyone can take a name)
  reason    shown to the player (REASON_CHARS, at most REASON_MAX)
  by        who did it (the admin's own words, e.g. a name; "manager" for the page without one)
  at        when (UTC seconds); until: when it ends (0: permanent)
  servers   the manager's server numbers it holds on ([]: all of them); the network list has none
A player matches an entry when its player id, its account or its address is in it (TekRantGaming's answer: "id +
account + IP"). The list never holds anything else about anyone, and an entry that ended is dropped at the next save.

Every program reads the file again when it changes (reload), so the manager's page or command line changes it once
and the hosts and the backend follow within a second. Only the manager writes it (and the official manager the
network list): one writer, written whole to a .tmp and renamed.

The moderation log (ModerationLog): one JSON object a line, what was done and by whom, kept LOG_DAYS (the privacy
page's 90 days for logs). No addresses in it: an address lives only in the ban that holds it, for as long as it holds.
"""
import datetime
import ipaddress
import json
import os
import time

KINDS = ('ban', 'kick', 'mute')
KICK_SECONDS = 300            # a kick keeps the player out this long (TekRantGaming: "Out, back after 5 minutes")
IP_DAYS = 30                  # an address is matched (and kept) at most this long, whatever the ban's length: a home's
                              # address moves on to someone else in time
LOG_DAYS = 90
REASON_MAX = 120
MOST = 5000                   # entries in one list
# What a reason may hold: characters every game font has, and no quotes, braces or backslashes (the games read the
# backend's replies with a small parser, server_list.h JsonField).
REASON_CHARS = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;!?\'()/&+%#@-')
LENGTHS = {'1h': 3600, '1d': 86400, '7d': 7 * 86400, '30d': 30 * 86400, 'permanent': 0}
APPEAL_NETWORK = 'Appeal on the Reborn Discord.'
APPEAL_SERVER = "To appeal, contact this server's admin."


def clean_id(value):
    """A player id as the game shows it (8 hex digits, upper case), or ''."""
    v = ''.join(c for c in str(value or '').strip() if c not in ' -').upper()
    if v.startswith('0X'):
        v = v[2:]
    if not v or len(v) > 8 or any(c not in '0123456789ABCDEF' for c in v):
        return ''
    v = v.rjust(8, '0')
    return '' if v == '00000000' else v


def id_of_xuid(xuid):
    """The player id in a XUID (the backend's and the hosts' decimal number): its low word."""
    try:
        return '%08X' % (int(xuid) & 0xFFFFFFFF) if int(xuid) & 0xFFFFFFFF else ''
    except (TypeError, ValueError):
        return ''


def clean_account(value):
    v = str(value or '').strip().lower()
    return v if len(v) == 64 and all(c in '0123456789abcdef' for c in v) else ''


def clean_ip(value):
    try:
        a = ipaddress.ip_address(str(value or '').strip().strip('[]'))
    except ValueError:
        return ''
    if a.version == 6 and a.ipv4_mapped:
        a = a.ipv4_mapped
    return str(a)


def clean_name(value):
    return ''.join(c for c in str(value or '') if c.isalnum() or c in ' _')[:15].strip()


def reason_problem(text):
    """Why a reason cannot be shown to a player, or ''."""
    if len(text) > REASON_MAX:
        return 'a reason is %d characters at most' % REASON_MAX
    bad = sorted(set(c for c in text if c not in REASON_CHARS))
    if bad:
        return 'the reason has characters that cannot be shown: %s' % ' '.join(repr(c) for c in bad)
    if 'reserved' in text.lower():  # a game from before the version gate takes a reserve answer holding it as a yes
        return 'the reason may not hold the word "reserved"'
    return ''


def clean_reason(text):
    return ' '.join(str(text or '').split())


def when_text(until):
    if not until:
        return 'Permanent'
    return datetime.datetime.fromtimestamp(int(until), datetime.timezone.utc).strftime('%d %b %Y %H:%M UTC').lstrip('0')


def length_seconds(value):
    """'1h', '1d', '7d', '30d', 'permanent', or a number of minutes/hours/days ('90m', '12h', '3d'): seconds (0:
    permanent). Raises ValueError for anything else."""
    v = str(value or '').strip().lower()
    if v in LENGTHS:
        return LENGTHS[v]
    if v and v[:-1].isdigit() and v[-1] in 'mhd' and 0 < int(v[:-1]) <= 3650 * 24 * 60:
        return int(v[:-1]) * {'m': 60, 'h': 3600, 'd': 86400}[v[-1]]
    raise ValueError('a length is 1h, 1d, 7d, 30d, permanent, or a number with m, h or d (like 12h)')


def refusal(entry, network, now=None):
    """(title, message) for a player refused by this entry: the game's notice pop-up shows both (server_list.h, any
    refusal of "reserve" with a message), and a kicked player's message box the message."""
    now = time.time() if now is None else now
    reason = entry.get('reason') or 'no reason given'
    if not reason.endswith(('.', '!', '?')):
        reason += '.'
    if entry.get('kind') == 'kick':
        minutes = max(1, int(round((entry.get('until', now) - now) / 60.0)))
        return ('KICKED FROM THIS SERVER', 'You were kicked from this server: %s You can join again in %d minute%s.' % (
            reason, minutes, '' if minutes == 1 else 's'))
    ends = 'Ends: %s.' % when_text(entry.get('until')) if entry.get('until') else 'This ban is permanent.'
    if network:
        return ('BANNED FROM THE REBORN NETWORK', 'You are banned from the Reborn servers and the community servers on '
                'the Reborn list. Reason: %s %s %s' % (reason, ends, APPEAL_NETWORK))
    return ('BANNED FROM THIS SERVER', 'You are banned from this server. Reason: %s %s %s' % (reason, ends, APPEAL_SERVER))


def mute_text(entry):
    reason = entry.get('reason') or 'no reason given'
    if not reason.endswith(('.', '!', '?')):
        reason += '.'
    ends = ' Ends: %s.' % when_text(entry['until']) if entry.get('until') else ''
    return 'You are muted on this server: %s Your voice and text chat reach nobody.%s' % (reason, ends)


def write_atomic(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, path)


class BanList:
    """One ban list file (the format above), read again when it changes."""

    def __init__(self, path, note=None, network=False):
        self.path, self.note, self.network = path, note or (lambda text: None), network
        self.entries = []
        self.stamp = None
        self.looked = 0.0
        self.reload(force=True)

    # ---- reading
    def reload(self, force=False, every=1.0):
        """True when the file changed and was read again (at most once each `every` seconds)."""
        now = time.monotonic()
        if not force and now - self.looked < every:
            return False
        self.looked = now
        if not self.path:
            return False
        try:
            st = os.stat(self.path)
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        if stamp == self.stamp:
            return False
        entries = []
        if stamp is not None:
            try:
                with open(self.path, encoding='utf-8-sig') as f:
                    raw = json.load(f)
                entries = [e for e in (self.clean(x) for x in raw.get('bans', [])[:MOST]) if e]
            except (OSError, ValueError, AttributeError) as e:
                self.note('moderation: %s not read (%s): the last list stays' % (self.path, e))
                self.stamp = stamp
                return False
        self.stamp, self.entries = stamp, entries
        return True

    @staticmethod
    def clean(e):
        if not isinstance(e, dict) or e.get('kind') not in KINDS:
            return None
        out = {'id': str(e.get('id', ''))[:16], 'kind': e['kind'],
               'ids': [x for x in (clean_id(v) for v in (e.get('ids') or [])[:16]) if x],
               'accounts': [x for x in (clean_account(v) for v in (e.get('accounts') or [])[:8]) if x],
               'ips': [x for x in (clean_ip(v) for v in (e.get('ips') or [])[:8]) if x],
               'names': [x for x in (clean_name(v) for v in (e.get('names') or [])[:8]) if x],
               'reason': clean_reason(e.get('reason'))[:REASON_MAX], 'by': clean_reason(e.get('by'))[:32]}
        try:
            out['at'] = int(e.get('at') or 0)
            out['until'] = int(e.get('until') or 0)
            out['ip_until'] = int(e.get('ip_until') or 0)
            out['servers'] = sorted({int(n) for n in (e.get('servers') or []) if 0 < int(n) <= 64})
        except (TypeError, ValueError):
            return None
        if reason_problem(out['reason']):
            out['reason'] = ''
        if not (out['ids'] or out['accounts'] or out['ips']):
            return None
        return out

    # ---- asking
    def active(self, now=None):
        now = time.time() if now is None else now
        return [e for e in self.entries if not e['until'] or e['until'] > now]

    def match(self, player_id='', account='', ip='', server=0, kinds=('ban', 'kick'), now=None):
        """The entry that holds against this player (the longest one), or None. An address counts until its
        ip_until."""
        now = time.time() if now is None else now
        player_id, account, ip = clean_id(player_id), clean_account(account), clean_ip(ip)
        best = None
        for e in self.entries:
            if e['kind'] not in kinds or (e['until'] and e['until'] <= now):
                continue
            if server and e['servers'] and server not in e['servers']:
                continue
            if not ((player_id and player_id in e['ids']) or (account and account in e['accounts']) or
                    (ip and e['ips'] and (not e['ip_until'] or e['ip_until'] > now) and ip in e['ips'])):
                continue
            if best is None or (best['until'] and (not e['until'] or e['until'] > best['until'])):
                best = e
        return best

    def match_any(self, ids=(), accounts=(), ips=(), server=0, kinds=('ban', 'kick'), now=None):
        for pid in list(ids) or ['']:
            for acct in list(accounts) or ['']:
                for ip in list(ips) or ['']:
                    e = self.match(pid, acct, ip, server, kinds, now)
                    if e:
                        return e
        return None

    def find(self, ban_id):
        return next((e for e in self.entries if e['id'] == ban_id), None)

    # ---- changing (the manager only)
    def save(self, now=None):
        now = time.time() if now is None else now
        kept = []
        for e in self.entries:
            if e['until'] and e['until'] <= now:
                continue  # ended: nothing of it is kept
            if e['ips'] and e['ip_until'] and e['ip_until'] <= now:
                e['ips'], e['ip_until'] = [], 0  # its addresses held their time: dropped
                if not (e['ids'] or e['accounts']):
                    continue
            kept.append(e)
        self.entries = kept[-MOST:]
        write_atomic(self.path, json.dumps({'version': 1, 'note': 'server moderation (server/bans.py); written by the '
                                            'server manager', 'bans': self.entries}, indent=1))
        self.reload(force=True)

    def add(self, kind, ids=(), accounts=(), ips=(), names=(), reason='', by='', seconds=0, servers=(), now=None):
        """A new entry (ValueError for one that does not check). A kick lasts KICK_SECONDS whatever was asked. An
        entry for a player already in one of the same kind replaces that one."""
        now = int(time.time() if now is None else now)
        if kind not in KINDS:
            raise ValueError('kind is ban, kick or mute')
        reason = clean_reason(reason)
        why = reason_problem(reason)
        if why:
            raise ValueError(why)
        ids = sorted({x for x in (clean_id(v) for v in ids) if x})
        accounts = sorted({x for x in (clean_account(v) for v in accounts) if x})
        ips = sorted({x for x in (clean_ip(v) for v in ips) if x})
        if not (ids or accounts or ips):
            raise ValueError('a player id, an account or an address is needed')
        seconds = KICK_SECONDS if kind == 'kick' else int(seconds)
        if seconds < 0:
            raise ValueError('a bad length')
        until = now + seconds if seconds else 0
        self.reload(force=True)
        for old in [e for e in self.entries if e['kind'] == kind and
                    (set(e['ids']) & set(ids) or set(e['accounts']) & set(accounts))]:
            self.entries.remove(old)
        e = {'id': 'b-' + os.urandom(4).hex(), 'kind': kind, 'ids': ids, 'accounts': accounts, 'ips': ips,
             'names': sorted({x for x in (clean_name(v) for v in names) if x})[:8], 'reason': reason,
             'by': clean_reason(by)[:32] or 'manager', 'at': now, 'until': until,
             'ip_until': (min(until, now + IP_DAYS * 86400) if until else now + IP_DAYS * 86400) if ips else 0,
             'servers': sorted({int(n) for n in servers if 0 < int(n) <= 64})}
        if len(self.entries) >= MOST:
            raise ValueError('the list is full (%d entries)' % MOST)
        self.entries.append(e)
        self.save(now)
        return e

    def remove(self, ban_id, now=None):
        self.reload(force=True)
        e = self.find(ban_id)
        if not e:
            raise ValueError('no such entry: %s' % ban_id)
        self.entries.remove(e)
        self.save(now)
        return e

    def public(self, e, with_ips=True):
        """An entry as the manager's page shows it."""
        out = dict(e, ends=when_text(e['until']))
        if not with_ips:
            out['ips'] = ['(%d)' % len(e['ips'])] if e['ips'] else []
        return out


class ModerationLog:
    """What was done, one JSON object a line (the manager's moderation page reads it). Lines older than LOG_DAYS go at
    the first write of a day."""

    def __init__(self, path):
        self.path = path
        self.trimmed = 0.0

    def write(self, **fields):
        if not self.path:
            return
        fields = {k: v for k, v in fields.items() if v not in (None, '', [])}
        fields.setdefault('at', int(time.time()))
        line = json.dumps(fields, separators=(',', ':')) + '\n'
        try:
            with open(self.path, 'a', encoding='utf-8', newline='\n') as f:
                f.write(line)
        except OSError:
            return
        if time.time() - self.trimmed > 86400:
            self.trim()

    def trim(self, now=None):
        now = time.time() if now is None else now
        self.trimmed = now
        try:
            with open(self.path, encoding='utf-8') as f:
                lines = f.readlines()
        except OSError:
            return
        keep = []
        for line in lines:
            try:
                if now - int(json.loads(line).get('at', 0)) <= LOG_DAYS * 86400:
                    keep.append(line)
            except (ValueError, AttributeError, TypeError):
                pass
        if len(keep) != len(lines):
            try:
                write_atomic(self.path, ''.join(keep))
            except OSError:
                pass

    def read(self, most=300):
        try:
            with open(self.path, encoding='utf-8') as f:
                lines = f.readlines()[-most:]
        except OSError:
            return []
        out = []
        for line in lines:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
        return out[::-1]


def hashed(salt, value):
    """A value as a host tells the backend its ban list (server_bans): salted, so the backend holds no list of a
    community server's banned addresses in the clear."""
    import hashlib
    return hashlib.sha256(('%s|%s' % (salt, value)).encode('utf-8')).hexdigest()[:32] if value else ''
