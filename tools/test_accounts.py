"""Protect your name: checks of the account code (TekRantGaming, 15 Nov 2026).

    python -I tools/test_accounts.py crypto <kt.exe>   the game's SHA-512 and Ed25519 (bf/src/account_key.cpp, built
                                                        into a small test program) against hashlib, server/ed25519.py
                                                        and OpenSSL (openssl on the PATH, as Git for Windows has)
    python -I tools/test_accounts.py consts            the constants account_key.cpp holds, computed again
    python -I tools/test_accounts.py backend           the backend's accounts (claim, sign in, a name thief, recovery,
                                                        stats, renames, expiry, replays) on a backend of its own
                                                        (127.0.0.1, ports 28241 to 28250)
    python -I tools/test_accounts.py team              the team names (server/plasma.py TEAM_OWNERS): the built in
                                                        list and its lookalikes, then two test owners with codes of
                                                        their own on a backend of its own (127.0.0.5, 28261 to 28270)

The test program: a main that answers "sha <hex>", "pub <seed hex>", "sign <seed hex> <message hex>" and
"code <code> <message>" with hex (scratchpad acct/kt.cpp of 15 Nov 2026).
"""
import asyncio
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'server'))
import ed25519  # noqa: E402

ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
BACKEND = ['127.0.0.1']  # the backend's address (the team test runs its own on 127.0.0.5)


def seed_of(code):
    return hashlib.sha512(('BF1943 Reborn account\n' + code).encode()).digest()[:32]


def openssl_sign(seed, message):
    with tempfile.TemporaryDirectory() as d:
        der = bytes.fromhex('302e020100300506032b657004220420') + seed
        open(os.path.join(d, 'k.der'), 'wb').write(der)
        open(os.path.join(d, 'm'), 'wb').write(message)
        subprocess.run(['openssl', 'pkey', '-inform', 'DER', '-in', 'k.der', '-out', 'k.pem'], cwd=d, check=True,
                       capture_output=True)
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-inkey', 'k.pem', '-rawin', '-in', 'm', '-out', 's'], cwd=d,
                       check=True, capture_output=True)
        return open(os.path.join(d, 's'), 'rb').read()


def crypto(exe):
    def run(*args):
        return subprocess.run([exe] + list(args), capture_output=True, text=True, check=True).stdout.split()
    bad = 0
    for n in list(range(0, 260)) + [1000, 4096]:
        m = os.urandom(n)
        if run('sha', m.hex())[0] != hashlib.sha512(m).hexdigest():
            print('SHA-512 differs at %d bytes' % n)
            bad += 1
    print('SHA-512: %d lengths checked' % 262)
    for i in range(40):
        seed, m = os.urandom(32), os.urandom(random.choice([0, 1, 31, 32, 64, 100, 200, 1000]))
        pub, sig = run('pub', seed.hex())[0], run('sign', seed.hex(), m.hex())[0]
        # (OpenSSL refuses to sign an empty file: an empty message is checked against ed25519.py alone.)
        want = openssl_sign(seed, m) if m else ed25519.sign(seed, m)
        if pub != ed25519.public_key(seed).hex():
            print('public key differs from ed25519.py', seed.hex())
            bad += 1
        if sig != want.hex():
            print('signature differs from OpenSSL', seed.hex(), m.hex())
            bad += 1
        if not ed25519.verify(bytes.fromhex(pub), m, bytes.fromhex(sig)):
            print('ed25519.py does not accept the game\'s signature')
            bad += 1
        if i == 0 and ed25519.verify(bytes.fromhex(pub), m + b'x', bytes.fromhex(sig)):
            print('ed25519.py accepted a changed message')
            bad += 1
    print('Ed25519: 40 keys and signatures checked against OpenSSL and ed25519.py')
    code = ''.join(random.choice(ALPHABET) for _ in range(20))
    typed = ' '.join(code[i:i + 5] for i in range(0, 20, 5)).lower().replace('0', 'o').replace('1', 'l')
    got = run('code', typed, 'hello')
    seed = seed_of(code)
    if got[0] != code or got[1] != ed25519.public_key(seed).hex() or got[2] != ed25519.sign(seed, b'hello').hex():
        print('the code\'s key differs', got, code)
        bad += 1
    print('recovery code: typed as "%s", read back and its key checked' % typed)
    print('FAILED: %d' % bad if bad else 'ALL PASSED')
    return bad == 0


def consts():
    def limbs(v):
        return ', '.join('0x%04x' % ((v >> (16 * i)) & 0xffff) for i in range(16))

    def primes(n):
        out, k = [], 2
        while len(out) < n:
            if all(k % p for p in out if p * p <= k):
                out.append(k)
            k += 1
        return out

    def iroot(x, n):
        lo, hi = 0, 1 << ((x.bit_length() // n) + 2)
        while lo < hi:
            m = (lo + hi + 1) // 2
            lo, hi = (m, hi) if m ** n <= x else (lo, m - 1)
        return lo
    src = open(os.path.join(ROOT, 'bf', 'src', 'account_key.cpp'), encoding='utf-8').read()
    want = {
        'kD2': limbs(2 * ed25519.D % ed25519.P), 'kX': limbs(ed25519._GX), 'kY': limbs(ed25519._GY),
        'kL': ', '.join('0x%02x' % b for b in ed25519.L.to_bytes(32, 'little')),
        'kH': ', '.join('0x%016xULL' % (iroot(p << 128, 2) & (2 ** 64 - 1)) for p in primes(8)),
        'kK': ', '.join('0x%016xULL' % (iroot(p << 192, 3) & (2 ** 64 - 1)) for p in primes(80)),
    }
    ok = all(v in src for v in want.values())
    print('constants: %s' % ('all as computed' if ok else 'DIFFERENT: ' + ', '.join(k for k, v in want.items()
                                                                                   if v not in src)))
    return ok


# ---- the backend
class Player:
    def __init__(self, port, code=None, pid=None, address='127.0.0.1'):
        self.port, self.address = port, address
        self.code = code or ''.join(random.choice(ALPHABET) for _ in range(20))
        self.seed = seed_of(self.code)
        self.key = ed25519.public_key(self.seed).hex()
        self.pid = pid or '%08X' % random.randint(1, 0xFFFFFFFF)

    async def ask(self, m):
        r, w = await asyncio.open_connection(BACKEND[0], self.port, local_addr=(self.address, 0))
        w.write((json.dumps(m) + '\n').encode())
        await w.drain()
        line = await r.readline()
        w.close()
        return json.loads(line)

    async def signed(self, op, text, nonce=None, **extra):
        if nonce is None:
            nonce = (await self.ask({'op': 'challenge'}))['nonce']
        body = '%s|%s|%s' % (op, nonce, text) if text else '%s|%s' % (op, nonce)
        m = {'op': op, 'key': self.key, 'signed': body, 'sig': ed25519.sign(self.seed, body.encode()).hex()}
        m.update(extra)
        return await self.ask(m), m

    async def name(self, name, signed=True):
        if not signed:
            return await self.ask({'op': 'name', 'name': name, 'id': self.pid})
        return (await self.signed('name', '%s|%s' % (name, self.pid)))[0]


async def fesl_login(port, gamertag, pid_hex, address):
    """The game's login as its packets go (server/plasma.py's framing): the persona the backend gives."""
    import plasma
    r, w = await asyncio.open_connection(BACKEND[0], port, local_addr=(address, 0))
    w.write(plasma.build('acct', plasma.REQUEST, 1, {'TXN': 'NuXBL360Login', 'gamertag': gamertag,
                                                       'xuid': str(int(pid_hex, 16))}))
    await w.drain()
    persona = None
    for _ in range(4):
        head = await asyncio.wait_for(r.readexactly(12), 10)
        body = await r.readexactly(int.from_bytes(head[8:12], 'big') - 12)
        if head[:4] == b'acct':
            persona = plasma.parse_payload(body).get('personaName')
            break
    w.close()
    return persona


async def backend_checks(port, accounts_file, proc_log):
    bad = []

    def check(what, ok, got=None):
        print('%-62s %s' % (what, 'ok' if ok else 'FAILED %s' % (got,)))
        if not ok:
            bad.append(what)
    a, thief = Player(port), Player(port, address='127.0.0.2')  # another player: another address
    r = await a.signed('claim', 'Kestrel_7|%s' % a.pid)
    check('a player protects a free name', r[0].get('ok') is True and r[0].get('name') == 'Kestrel_7', r[0])
    check('the same request again is refused (one time challenge)', (await a.ask(r[1])).get('ok') is not True)
    r = await thief.signed('claim', 'kestrel_7|%s' % thief.pid)
    check('another key cannot protect it (any case)', r[0].get('ok') is False and r[0].get('why') == 'taken', r[0])
    r = await thief.name('Kestrel_7', signed=False)
    check('a name check without the key gets a number', r.get('name') not in ('Kestrel_7', 'kestrel_7') and
          r.get('protected_by_other') is True, r)
    r = await Player(port, pid=a.pid, address='127.0.0.3').name('Kestrel_7', signed=False)
    check('even with the owner\'s player id copied', r.get('name') != 'Kestrel_7', r)
    r = await a.name('Kestrel_7')
    check('the owner signs in and gets the name', r.get('name') == 'Kestrel_7' and r.get('protected') is True, r)
    forged = {'op': 'name', 'key': a.key, 'signed': 'name|%s|Kestrel_7|%s' % ((await a.ask({'op': 'challenge'}))['nonce'],
                                                                              thief.pid),
              'sig': ed25519.sign(thief.seed, b'x').hex()}
    r = await a.ask(forged)
    check('a signature by another key is refused', r.get('name') != 'Kestrel_7' or r.get('protected') is not True, r)
    r = await a.ask({'op': 'name', 'key': a.key, 'signed': 'name|00000000000000000000000000000000|Kestrel_7|%s' % a.pid,
                     'sig': ed25519.sign(a.seed, ('name|00000000000000000000000000000000|Kestrel_7|%s' %
                                                  a.pid).encode()).hex()})
    check('a challenge the backend never gave is refused', r.get('protected') is not True, r)
    # stats: the owner's go to the account and the leaderboard; anyone else's for that name are refused
    stats = 'Kestrel_7|1200|3600|4|CQ_StringForFlagCapture=4,400;Kill=30,800|1,7'
    r = (await a.signed('stats_put', stats))[0]
    check('the owner\'s signed stats are kept', r.get('ok') is True, r)
    r = await thief.ask({'op': 'stats_put', 'name': 'Kestrel_7', 'score': 999999, 'seconds': 1, 'captures': 0})
    check('unsigned stats for a protected name are refused', r.get('ok') is False, r)
    r = (await thief.signed('stats_put', 'Kestrel_7|999999|1|0||'))[0]
    check('signed stats by another account for it are refused', r.get('ok') is False, r)
    lb = await a.ask({'op': 'leaderboard'})
    row = [x for x in lb.get('rows', []) if x['name'] == 'Kestrel_7']
    check('the leaderboard shows the owner\'s score', bool(row) and row[0]['score'] == 1200, lb)
    # recovery on a new PC: the code alone gives the name and the progress back
    b = Player(port, code=a.code)
    r = (await b.signed('recover', ''))[0]
    prog = r.get('progress') or {}
    check('the recovery code gives the name and progress back', r.get('ok') is True and r.get('name') == 'Kestrel_7'
          and prog.get('stats', {}).get('Kill') == '30,800' and prog.get('seconds') == 3600 and
          prog.get('achievements') == [1, 7], r)
    r = (await thief.signed('recover', ''))[0]
    check('a code with no account recovers nothing', r.get('ok') is False, r)
    r = await b.name('Kestrel_7')
    check('the new PC signs in with the name (another player id)', r.get('name') == 'Kestrel_7' and
          r.get('protected') is True, r)
    # a rename: the account moves to the new name and the old one is free
    r = (await a.signed('claim', 'Kestrel_8|%s' % a.pid))[0]
    check('the owner renames the protected name', r.get('ok') is True and r.get('name') == 'Kestrel_8', r)
    r = await thief.name('Kestrel_7', signed=False)
    check('the old name is free again', r.get('name') == 'Kestrel_7', r)
    r = await thief.name('Kestrel_8', signed=False)
    check('the new one is protected', r.get('name') != 'Kestrel_8', r)
    # the game's own login (FESL): the protected name only for the player id and address that signed in for it
    persona = await fesl_login(port - 9, 'Kestrel_8', a.pid, '127.0.0.1')
    check("the owner's game logs in under the protected name", persona == 'Kestrel_8', persona)
    persona = await fesl_login(port - 9, 'Kestrel_8', a.pid, '127.0.0.2')
    check('a game logging in under it from elsewhere is renamed', persona not in ('Kestrel_8', None), persona)
    # a name someone plays under right now cannot be taken from them by a claim
    online = Player(port)
    await online.name('Busy_One', signed=False)
    r = (await thief.signed('claim', 'Busy_One|%s' % thief.pid))[0]
    check('a name held by a player now cannot be claimed by another', r.get('ok') is False and
          r.get('why') == 'in_use', r)
    # bad requests
    r = (await thief.signed('claim', 'ab|%s' % thief.pid))[0]
    check('a name of 2 characters is refused', r.get('ok') is False, r)
    r = (await thief.signed('claim', 'Bad Name!|%s' % thief.pid))[0]
    check('a name with other characters is refused', r.get('ok') is False, r)
    r = await thief.ask({'op': 'claim', 'key': 'zz', 'signed': 'claim|x|y|z', 'sig': '00'})
    check('a malformed claim is refused', r.get('ok') is False, r)
    # the rate limit: 5 claims an hour from one address
    many = [Player(port) for _ in range(5)]
    oks = [(await p.signed('claim', 'Spam_%d|%s' % (i, p.pid)))[0].get('ok') for i, p in enumerate(many)]
    r = (await Player(port).signed('claim', 'Spam_9|%s' % thief.pid))[0]
    r2 = (await Player(port, address='127.0.0.4').signed('claim', 'Spam_10|%s' % thief.pid))[0]
    check('another address can still claim', r2.get('ok') is True, r2)
    check('at most 5 new names an hour from one address (with the 2 before)', r.get('why') == 'too_many', (oks, r))
    return bad


def backend():
    plasma = os.path.join(ROOT, 'server', 'plasma.py')
    with tempfile.TemporaryDirectory() as d:
        accounts = os.path.join(d, 'accounts.json')
        log = os.path.join(d, 'plasma.log')

        def start(extra=()):
            return subprocess.Popen([sys.executable, '-I', plasma, '--host', '127.0.0.1', '--port', '28241',
                                     '--theater-port', '28246', '--registry-port', '28250', '--accounts', accounts,
                                     '--leaderboard', os.path.join(d, 'leaderboard.json'), '--log', log,
                                     '--accounts-save-every', '1', '--login-delay', '0'] + list(extra),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        p = start()
        try:
            time.sleep(1.5)
            bad = asyncio.run(backend_checks(28250, accounts, log))
            time.sleep(2.5)  # the accounts file is written within a second of a change
        finally:
            p.terminate()
            p.wait()
        saved = json.load(open(accounts, encoding='utf-8'))
        names = sorted(x.get('name', '') for x in saved.get('accounts', []))
        print('%-62s %s' % ('the accounts file keeps them (%d)' % len(names),
                            'ok' if 'Kestrel_8' in names else 'FAILED %s' % names))
        if 'Kestrel_8' not in names:
            bad.append('file')
        # a restart reads the file: the name is still protected; and one unused 181 days is free again
        for x in saved['accounts']:
            if x.get('name') == 'Spam_0':
                x['seen'] = int(time.time()) - 181 * 86400
        json.dump(saved, open(accounts, 'w', encoding='utf-8'))
        p = start()
        try:
            time.sleep(1.5)

            async def after():
                t = Player(28250)
                r1 = await t.name('Kestrel_8', signed=False)
                r2 = await t.name('Spam_0', signed=False)
                return r1, r2
            r1, r2 = asyncio.run(after())
        finally:
            p.terminate()
            p.wait()
        for what, ok, got in (('after a restart the name is still protected', r1.get('name') != 'Kestrel_8', r1),
                              ('a name unused for 181 days is free again', r2.get('name') == 'Spam_0', r2)):
            print('%-62s %s' % (what, 'ok' if ok else 'FAILED %s' % (got,)))
            if not ok:
                bad.append(what)
        print('FAILED: %s' % bad if bad else 'ALL PASSED')
        return not bad


# ---- team names (server/plasma.py TEAM_OWNERS)
# The watchlist's entries as it wrote them (spaces, dots and hyphens and all), each with its owner; and its generic ones.
WATCHLIST = {
    'TekRantGaming': [
        'TekRantGaming', 'Tek Rant Gaming', 'Tek RantGaming', 'TekRant Gaming', 'Tek Rant', 'TekRant', 'TekRG',
        'TekRantG', 'TekRantGame', 'TekRantGames', 'TekRantGamer', 'TekRantGamers', 'TekRantGamingTV',
        'TekRantGamingOfficial', 'TekRantGamingUK', 'TekRantGamingDev', 'TekRantGamingProject', 'TekRantGamingTeam',
        'TekRantGamingCommunity', 'TekRantGamingNetwork', 'TekRantGamingServer', 'TekRantGamingServers',
        'TekRantGamingOrg', 'TekRantGamingCom', 'Tek-Rant-Gaming', 'Tek_Rant_Gaming', 'Tek.Rant.Gaming',
        'Tek.RantGaming', 'TekRant-Gaming', 'TekRant_Gaming', 'TekRant.Gaming', 'TekRantGaming_', '_TekRantGaming',
        'TekRantGaming1', 'TekRantGaming2', 'TekRantGaming01', 'TekRantGaming123', 'TekRantGamingX', 'xTekRantGaming',
        'TheTekRantGaming', 'RealTekRantGaming', 'ItsTekRantGaming', 'ImTekRantGaming', 'OfficialTekRantGaming',
        'TechRantGaming', 'Tech Rant Gaming', 'TekRankGaming', 'TekRantGamng', 'TekRantGming', 'TekRantGamin',
        'TekRantGamming', 'TekRantGamimg', 'TekRantGamig', 'TekRantGaiming', 'TekRantGmaing', 'TekRantGamingg',
        'TekRanttGaming', 'TekkRantGaming', 'TekRanntGaming', 'TekRantGam1ng', 'TekRantG4ming', 'T3kRantGaming',
        'TekR4ntGaming', 'TekR@ntGaming', 'T3kR4ntG4ming', 'TekRantG4m1ng', 'TekRantGm', 'TekRGaming', 'TekRantGamez',
        'TekRantGmz', 'TekRantGng', 'TekRant_G', 'TekRant-G', 'tekrantgaming.com', 'www.tekrantgaming.com',
        'chat.tekrantgaming.com', 'tek-rant', 'tek_rant', 'tek.rant'],
    'ZALIAS': [
        'ZALIAS', 'ZALIA S', 'Z Alias', 'ZAL IAS', 'ZALIAS_', '_ZALIAS', 'ZALIAS1', 'ZALIAS01', 'ZALIAS123', 'ZALIASX',
        'TheZALIAS', 'ItsZALIAS', 'ImZALIAS', 'RealZALIAS', 'OfficialZALIAS', 'ZALIASOfficial', 'ZALIASGaming',
        'ZALIASGamer', 'ZALIASDev', 'ZALIASTeam', 'ZALIASProject', 'ZALIASNetwork', 'ZALIAS-', 'ZALIAS.',
        'Z.A.L.I.A.S.', 'Z-A-L-I-A-S', 'Z_A_L_I_A_S', 'Z A L I A S', 'Zaliass', 'Zaliasss', 'Zallias', 'Zalais',
        'Zaliasz', 'Zalials', 'Zalius', 'Zaleas', 'Zaleis', 'Zalies', 'Zalyas', 'Zalyass', 'Zaliash', 'Zalias2',
        'Zalias3', 'ZaliasYT', 'ZaliasTV', 'ZaliasUK', 'ZaliasGames', 'ZaliasG', 'ZAL1AS', 'ZALlAS', 'Z4LIAS', 'Z@LIAS',
        'Z4L1AS', 'ZALl4S', '2ALIAS', 'ZALIA5', 'ZALlA5', 'ZALIASSSS', 'ZALIAS.com', 'www.zalias.com', 'z-alias',
        'z_alias', 'z.alias'],
}
WATCHLIST_GENERIC = ['TRG', 'T.R.G.', 'T R G', 'TRGaming', 'TRG Gaming', 'TR Gaming', 'Tek Gaming', 'Rant Gaming',
                     'Rant', 'Gaming', 'TechRant', 'Tech Rant']
# Lookalikes the watchlist does not hold, and ordinary names that must stay free.
LOOKALIKES = {'TekRantGaming': ['xXTekRantGamingXx', 'T3kR4ntG4m1ng', 'TEKRANTGAMING', 'MyTekRankGamingFan',
                                'TekRantGamingFan', 'Tek_R_G', 'TekRanGaming', 'tekrantgaminggg', 'TheRealTekRant'],
              'ZALIAS': ['xXZaliusXx', 'ThatZaliaSGuy', 'Z_alias_TV', 'zalias', 'ZaliosTV', 'Za11as', 'The_Zalias_99',
                         # ThatZaliaSGuy's (its variation list's kinds: casing, separators, lookalikes, a slip in each
                         # part, two words)
                         'thatzaliaguy', 'That_Zalia_Guy', 'Th4tZ4l1aSGuy', 'ThatZalisGai', 'DatZaleasGuy',
                         'ThatSaliasGuy', 'ThatXaliaSGuy', 'ThaZaliaSGuy', 'ThatZaliaSGuyy', 'ThatZaliaSUgy',
                         'ZaliaGai', 'TZalia', 'ThatZalia', 'ZaliaSGuy', 'TheZaliaDude', 'ThatZaliaSGuy69']}
ORDINARY = ['Gamer123', 'Alias', 'Zalia', 'Zaid', 'Zeus', 'Elias', 'Salias', 'Tekken', 'TekTonic', 'TechGamer',
            'RantMaster', 'Ranger', 'TankGaming', 'Kestrel_7', 'Soldier', 'TekRanger', 'Zaliyah',
            # near ThatZaliaSGuy, and its generic short forms
            'ThatGuy', 'ThatTallGuy', 'ThatSaladGuy', 'ThatJuliaGuy', 'ThatTaliaGuy', 'ThatDaliaGuy', 'ThatAsianGuy',
            'ThatSalsaGuy', 'ZaidGuy', 'ZainGuy', 'ZaraGuy', 'TheSaladMan', 'TZG', 'ZGuy', 'ThatZGuy']
TEAM_TEST_OWNERS = []  # two test owners (codes of their own, never the real ones) in place of TEAM_OWNERS' keys


def plain(name):
    """A watchlist entry as a game name: spaces, dots and hyphens left out, @ as a (names are letters, digits and
    underscores)."""
    return ''.join(c for c in name if c not in ' .-').replace('@', 'a')


def report(bad, what, ok, got=None):
    print('%-62s %s' % (what, 'ok' if ok else 'FAILED %s' % (got,)))
    if not ok:
        bad.append(what)


def team_list_checks():
    """The built in list: the two owners' keys, every watchlist entry its owner's, the generic ones free."""
    import plasma
    bad = []
    keys = {o['owner']: o['key'] for o in plasma.TEAM['owners']}
    report(bad, 'the built in team names: TekRantGaming and ZALIAS, a key each',
           sorted(keys) == ['TekRantGaming', 'ZALIAS'] and all(len(k) == 64 for k in keys.values()), keys)
    for owner, names in WATCHLIST.items():
        wrong = [n for n in names if (plasma.team_owner(plain(n)) or {}).get('owner') != owner]
        report(bad, 'all %d watchlist entries of %s are kept for %s' % (len(names), owner, owner), not wrong, wrong)
        wrong = [n for n in LOOKALIKES[owner] if (plasma.team_owner(n) or {}).get('owner') != owner]
        report(bad, 'lookalikes beyond the watchlist are kept for %s too' % owner, not wrong, wrong)
    held = [n for n in WATCHLIST_GENERIC if plasma.team_owner(plain(n))]
    report(bad, 'the watchlist generic entries stay free (%d)' % len(WATCHLIST_GENERIC), not held, held)
    held = [n for n in ORDINARY if plasma.team_owner(n)]
    report(bad, 'ordinary names stay free (%s...)' % ', '.join(ORDINARY[:3]), not held, held)
    started = time.time()
    for i in range(3000):
        plasma.TEAM['cache'].clear()
        plasma.team_owner('TekRanqGamxr%d' % i)
    took = (time.time() - started) / 3
    report(bad, 'a name looked up in under a millisecond (%.2f ms at worst)' % took, took < 1.0)
    # The trademark sign (v0.4.7.4, ZALIAS plays as "ZALIAS™"): once, at the end, within 15 bytes of UTF-8.
    tm = '™'
    import player_stats
    report(bad, 'ZALIAS%s is a valid name and kept for ZALIAS' % tm, plasma.valid_name('ZALIAS' + tm) and
           (plasma.team_owner('ZALIAS' + tm) or {}).get('owner') == 'ZALIAS')
    wrong = [n for n in (tm * 3, 'AB' + tm, 'A' + tm + 'B', 'ABCDEFGHIJKLM' + tm, 'Gamer' + tm + tm)
             if plasma.valid_name(n)]
    report(bad, 'the sign only once, at the end, after 3 letters, within 15 bytes', not wrong, wrong)
    cleaned = [player_stats.clean_name(n) for n in ('ZAL' + tm + 'IAS', 'ABCDEFGHIJKLMNO' + tm, tm)]
    report(bad, 'names cleaned keep one sign at the end within 15 bytes', cleaned ==
           ['ZALIAS' + tm, 'ABCDEFGHIJKL' + tm, ''], cleaned)
    raw = ('ZALIAS' + tm).encode('utf-8').decode('latin-1')  # as parse_payload reads the game's gamertag
    report(bad, 'a gamertag read from the game comes back as ZALIAS%s and goes out as its bytes' % tm,
           plasma.from_wire(raw) == 'ZALIAS' + tm and plasma.wire('ZALIAS' + tm) == ('ZALIAS' + tm).encode('utf-8') and
           plasma.wire(raw) == raw.encode('latin-1') and plasma.from_wire('Gamer123') == 'Gamer123')
    return bad


async def team_checks(port):
    import plasma
    bad = []
    tek, zal = TEAM_TEST_OWNERS
    other = Player(port, address='127.0.0.2')
    # anyone else: every team name and lookalike refused, with the reason
    names = [plain(n)[:15] for o in WATCHLIST.values() for n in o if len(plain(n)) >= 3]
    names += [n for o in LOOKALIKES.values() for n in o]
    wrong = []
    for n in names:
        r = await other.name(n, signed=False)
        if r.get('name', '').lower() == n.lower() or r.get('team_name') is not True or \
                not r.get('name', '').startswith('Soldier'):
            wrong.append((n, r))
    report(bad, 'another player: all %d team names and lookalikes refused' % len(names), not wrong, wrong[:3])
    r = await other.name('TekRantGaming', signed=False)
    report(bad, '... given Soldier, with team_name for the game to say why', r.get('name', '').startswith('Soldier')
           and r.get('team_name') is True and r.get('protected_by_other') is True, r)
    r = await other.name('Gamer123', signed=False)
    report(bad, 'a normal name (Gamer123) is free', r.get('name') == 'Gamer123' and not r.get('team_name'), r)
    r = (await other.signed('claim', 'Gamer123|%s' % other.pid))[0]
    report(bad, '... and can be protected', r.get('ok') is True and r.get('name') == 'Gamer123', r)
    for n in ('Zalias2', 'TekRant', 'T3kR4ntG4ming'):
        r = (await other.signed('claim', '%s|%s' % (n, other.pid)))[0]
        report(bad, 'another account cannot protect %s (why team_name)' % n, r.get('ok') is False and
               r.get('why') == 'team_name', r)
    r = await other.name('ZaliasTV')  # signed, by an account with a name of its own
    report(bad, 'a signed name check by another account is refused too', r.get('name') != 'ZaliasTV' and
           r.get('team_name') is True and r.get('protected') is not True, r)
    r = await other.name('Gamer123')
    report(bad, 'that account still signs in for its own name', r.get('name') == 'Gamer123' and
           r.get('protected') is True, r)
    r = await other.name('Bystander9')
    report(bad, 'a signed check for another free name answers as a plain one', r.get('name') == 'Bystander9' and
           r.get('protected') is False, r)
    r = await other.name('Gamer123')
    report(bad, '... and the account keeps Gamer123', r.get('name') == 'Gamer123' and r.get('protected') is True, r)
    r = await other.ask({'op': 'stats_put', 'name': 'TekRantGaming', 'score': 999999, 'seconds': 1, 'captures': 0})
    report(bad, 'unsigned stats for a team name are refused', r.get('ok') is False, r)
    persona = await fesl_login(port - 9, 'TekRantGaming', other.pid, '127.0.0.2')
    report(bad, 'a game logging in as TekRantGaming is renamed (%s)' % persona, bool(persona) and
           plasma.team_owner(persona) is None, persona)
    # the owners: the code gives the name back, and works for any of the owner's names
    r = (await tek.signed('recover', ''))[0]
    report(bad, 'the TekRantGaming code recovers the account and its name', r.get('ok') is True and
           r.get('name') == 'TekRantGaming', r)
    r = await tek.name('TekRantGaming')
    report(bad, '... signs in for TekRantGaming', r.get('name') == 'TekRantGaming' and r.get('protected') is True, r)
    r = await tek.name('TekRantGaming2')
    report(bad, '... and for TekRantGaming2 with the same code', r.get('name') == 'TekRantGaming2' and
           r.get('protected') is True, r)
    persona = await fesl_login(port - 9, 'TekRantGaming2', tek.pid, '127.0.0.1')
    report(bad, 'the owner game logs in as TekRantGaming2', persona == 'TekRantGaming2', persona)
    r = (await tek.signed('stats_put', 'TekRantGaming2|1500|3600|2|Kill=12,600|1'))[0]
    report(bad, 'the owner signed stats under it are kept', r.get('ok') is True, r)
    r = await other.name('TekRantGaming', signed=False)
    report(bad, 'TekRantGaming stays refused to others meanwhile', r.get('name') != 'TekRantGaming', r)
    r = (await tek.signed('claim', 'ZALIAS|%s' % tek.pid))[0]
    report(bad, 'one owner cannot take the other owner name', r.get('ok') is False and r.get('why') == 'team_name', r)
    r = (await zal.signed('recover', ''))[0]
    report(bad, 'the ZALIAS code recovers the account and its name', r.get('ok') is True and r.get('name') == 'ZALIAS',
           r)
    r = (await zal.signed('claim', 'Zalias2|%s' % zal.pid))[0]
    report(bad, 'ZALIAS protects Zalias2 with the same code', r.get('ok') is True and r.get('name') == 'Zalias2', r)
    r = await zal.name('ZALIAS')
    report(bad, '... and plays as ZALIAS again', r.get('name') == 'ZALIAS' and r.get('protected') is True, r)
    r = await zal.name('ThatZaliaSGuy')
    report(bad, '... and as ThatZaliaSGuy with the same code', r.get('name') == 'ThatZaliaSGuy' and
           r.get('protected') is True, r)
    tm = '™'
    r = await zal.name('ZALIAS' + tm)
    report(bad, '... and as ZALIAS%s with the same code' % tm, r.get('name') == 'ZALIAS' + tm and
           r.get('protected') is True, r)
    persona = await fesl_login(port - 9, 'ZALIAS' + tm, zal.pid, '127.0.0.1')
    report(bad, 'the owner game logs in as ZALIAS%s (%r)' % (tm, persona), plasma.from_wire(persona or '') ==
           'ZALIAS' + tm, persona)
    r = await other.name('ZALIAS' + tm, signed=False)
    report(bad, 'ZALIAS%s is refused to others' % tm, r.get('name') != 'ZALIAS' + tm and r.get('team_name') is True, r)
    r = await other.name('Gamer12' + tm, signed=False)
    report(bad, 'another player may end a name of their own with the sign (Gamer12%s)' % tm,
           r.get('name') == 'Gamer12' + tm and not r.get('team_name'), r)
    r = await other.name('ThatZaliaSGuy', signed=False)
    report(bad, 'ThatZaliaSGuy is refused to others', r.get('name') != 'ThatZaliaSGuy' and r.get('team_name') is True, r)
    r = await other.name('Zalias2', signed=False)
    report(bad, 'Zalias2 stays refused to others after the owner moved off it', r.get('name') != 'Zalias2' and
           r.get('team_name') is True, r)
    many = [(await zal.signed('claim', 'ZaliasTV%d|%s' % (i, zal.pid)))[0].get('ok') for i in range(6)]
    report(bad, 'an owner moves among their names more than 5 times an hour', all(many), many)
    return bad


def team():
    import plasma
    bad = team_list_checks()
    plasma_py = os.path.join(ROOT, 'server', 'plasma.py')
    BACKEND[0] = '127.0.0.5'
    TEAM_TEST_OWNERS[:] = [Player(28270), Player(28270)]
    with tempfile.TemporaryDirectory() as d:
        accounts = os.path.join(d, 'accounts.json')
        team_file = os.path.join(d, 'team.json')
        owners = [dict(o, key=p.key) for o, p in zip(plasma.TEAM_OWNERS, TEAM_TEST_OWNERS)]
        with open(team_file, 'w', encoding='utf-8') as f:
            json.dump({'owners': owners}, f)
        # an account that protected a team name before the names were kept: it loses it at the start
        early = Player(28270)
        with open(accounts, 'w', encoding='utf-8') as f:
            json.dump({'version': 1, 'accounts': [{'key': early.key, 'name': 'ZaliasTV', 'created': 1,
                                                   'seen': int(time.time()), 'progress': {}}]}, f)

        def start():
            return subprocess.Popen([sys.executable, '-I', plasma_py, '--host', '127.0.0.5', '--port', '28261',
                                     '--theater-port', '28266', '--registry-port', '28270', '--accounts', accounts,
                                     '--team-names', team_file, '--leaderboard', os.path.join(d, 'leaderboard.json'),
                                     '--log', os.path.join(d, 'plasma.log'), '--accounts-save-every', '1',
                                     '--login-delay', '0'], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        async def first():
            r1 = await Player(28270, address='127.0.0.3').name('ZaliasTV', signed=False)
            r2 = (await early.signed('recover', ''))[0]
            return r1, r2
        p = start()
        try:
            time.sleep(1.5)
            r1, r2 = asyncio.run(first())
            report(bad, 'a team name protected before is no longer that account', r1.get('name') != 'ZaliasTV' and
                   r2.get('ok') is True and r2.get('name') == '', (r1, r2))
            bad += asyncio.run(team_checks(28270))
            time.sleep(2.5)  # the accounts file is written within a second of a change
        finally:
            p.terminate()
            p.wait()
        # a restart, every account unused for 400 days: the team names are still the owners' alone
        with open(accounts, encoding='utf-8') as f:
            saved = json.load(f)
        for x in saved['accounts']:
            x['seen'] = int(time.time()) - 400 * 86400
        with open(accounts, 'w', encoding='utf-8') as f:
            json.dump(saved, f)

        async def after():
            t = Player(28270, address='127.0.0.4')
            return (await t.name('TekRantGaming2', signed=False), await t.name('ZALIAS', signed=False),
                    await TEAM_TEST_OWNERS[1].name('Zalias3'), await t.name('Gamer123', signed=False))
        p = start()
        try:
            time.sleep(1.5)
            r1, r2, r3, r4 = asyncio.run(after())
        finally:
            p.terminate()
            p.wait()
        report(bad, 'after a restart and 400 days unused: TekRantGaming2 kept', r1.get('name') != 'TekRantGaming2', r1)
        report(bad, '... ZALIAS kept', r2.get('name') != 'ZALIAS', r2)
        report(bad, '... the owner code still works (Zalias3)', r3.get('name') == 'Zalias3' and
               r3.get('protected') is True, r3)
        report(bad, '... while Gamer123, unused 400 days, is free again', r4.get('name') == 'Gamer123', r4)
    print('FAILED: %s' % bad if bad else 'ALL PASSED')
    return not bad


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else ''
    if what == 'crypto':
        sys.exit(0 if crypto(sys.argv[2]) else 1)
    if what == 'consts':
        sys.exit(0 if consts() else 1)
    if what == 'backend':
        sys.exit(0 if backend() else 1)
    if what == 'team':
        sys.exit(0 if team() else 1)
    print(__doc__)
