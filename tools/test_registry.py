"""The server registry's protocol without the game (SERVERS-PLAN.md sections 5 to 7): a local backend with an official
key; an official server, a community one, an unlisted one and one with a wrong key register through server/gmhost.py.
Checks the list (official flags, descriptions, next maps, unlisted left out), Direct connect's find, the refusal, the
reachability probe and the hosts' ping answer, then stops everything it started.

    python -I tools/test_registry.py
"""
import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
REG = 18350  # ports apart from the usual ones, so a running backend is not touched


def run(args, log):
    return subprocess.Popen([PY, '-I'] + args, cwd=ROOT, stdout=open(log, 'w'), stderr=subprocess.STDOUT)


def ask(obj):
    with socket.create_connection(('127.0.0.1', REG), timeout=5) as s:
        s.sendall((json.dumps(obj) + '\n').encode())
        return json.loads(s.makefile().readline())


def ping(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        t = time.perf_counter()
        s.sendto(b'BF43PING' + b'12345678', ('127.0.0.1', port))
        data, _ = s.recvfrom(64)
        return data[:8] == b'BF43PONG' and data[8:16] == b'12345678', (time.perf_counter() - t) * 1000


def main():
    tmp = tempfile.mkdtemp(prefix='bfreg_')
    key = os.path.join(tmp, 'official.key')
    bad = os.path.join(tmp, 'wrong.key')
    open(key, 'w').write(os.urandom(32).hex())
    open(bad, 'w').write(os.urandom(32).hex())
    procs = [run(['server/plasma.py', '--port', '18341', '--theater-port', '18346', '--registry-port', str(REG),
                  '--official-key-file', key, '--log', os.path.join(tmp, 'plasma.log')], os.path.join(tmp, 'p.out'))]
    time.sleep(1.5)
    servers = [('Official one', 18360, ['--official-key-file', key, '--maps', 'Iwo Jima,Wake Island', '--map',
                                        'Iwo Jima', '--description', 'Run by TRG Studios']),
               ('Community one', 18361, ['--description', 'No base camping', '--mode', 'modern']),
               ('Hidden one', 18362, ['--unlisted']),
               ('Fake official', 18363, ['--official-key-file', bad])]
    for name, port, extra in servers:
        procs.append(run(['server/gmhost.py', '--backend', '127.0.0.1:%d' % REG, '--listen', '0.0.0.0:%d' % port,
                          '--game-server', '127.0.0.1:1', '--name', name, '--status-file',
                          os.path.join(tmp, 'status_%d.json' % port)] + extra, os.path.join(tmp, 'h%d.out' % port)))
    time.sleep(8)
    failures = []

    def check(what, ok):
        print(('ok    ' if ok else 'FAIL  ') + what)
        if not ok:
            failures.append(what)

    try:
        listed = ask({'op': 'list'})['servers']
        names = {s['name']: s for s in listed}
        check('the official and the community server are listed', {'Official one', 'Community one'} <= set(names))
        check('the unlisted server is not', 'Hidden one' not in names)
        check('the wrong key is refused, not listed', 'Fake official' not in names)
        check('official flag set only for the official server',
              names.get('Official one', {}).get('official') is True and names.get('Community one', {}).get('official') is False)
        check('descriptions arrive', names.get('Community one', {}).get('description') == 'No base camping')
        check('the next map follows the rotation', names.get('Official one', {}).get('next_map') == 'Wake Island')
        check('the address is the connection\'s (no address sent)',
              names.get('Community one', {}).get('address') == ['127.0.0.1', 18361])
        found = ask({'op': 'find', 'address': '127.0.0.1', 'port': 18362})
        check('Direct connect finds the unlisted server', (found.get('server') or {}).get('name') == 'Hidden one')
        check('Direct connect finds nothing at a wrong port', ask({'op': 'find', 'address': '127.0.0.1', 'port': 1})
              .get('server') is None)
        for port in (18360, 18361):
            ok, ms = ping(port)
            check('ping answered by the host on %d (%.1f ms)' % (port, ms), ok)
        st = json.load(open(os.path.join(tmp, 'status_18360.json')))
        check('status file: registered, official, reachable', st.get('state') == 'registered' and st.get('official')
              is True and st.get('reachable') is True)
        st = json.load(open(os.path.join(tmp, 'status_18363.json')))
        check('status file of the wrong key says refused', st.get('state') == 'refused')
    finally:
        for p in procs:
            p.kill()
    print('plasma log:', os.path.join(tmp, 'plasma.log'))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
