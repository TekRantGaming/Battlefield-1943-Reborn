"""Signing in to the server manager's page (a throwaway manager on loopback, no game, no backend, no router).

    python -I tools/test_manager_sign_in.py

Checks: without a session the API answers 401; this machine's browser signs itself in with a single use code; the
command line's code signs in; the password signs in; a password change keeps the page that made it and ends the
others; the token of the command line works.
"""
import http.client
import json
import os
import sys
import tempfile
import threading
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = tempfile.mkdtemp(prefix='bf-signin-test-')
os.environ['BF_SERVER_DATA'] = DATA
sys.path.insert(0, os.path.join(ROOT, 'server'))
import manager  # noqa: E402

FAILS = []


def check(what, ok, detail=''):
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', what, '' if ok else '   ' + str(detail)[:300]))
    if not ok:
        FAILS.append(what)


def call(port, path, data=None, cookie='', host='127.0.0.1', headers=()):
    c = http.client.HTTPConnection(host, port, timeout=15)
    h = dict(headers)
    if cookie:
        h['Cookie'] = cookie
    if data is not None:
        h['X-Requested-With'] = 'bf1943'
    c.request('GET' if data is None else 'POST', path, None if data is None else json.dumps(data), h)
    r = c.getresponse()
    raw = r.read()
    got = (r.getheader('Set-Cookie') or '').split(';')[0]
    c.close()
    return r.status, json.loads(raw or b'{}'), got


def main():
    m = manager.Manager()
    m.cfg['upnp'], m.cfg['lan_only'] = False, True
    manager.Handler.manager = m
    server = manager.Server(('127.0.0.1', 0), manager.Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    check('no session: 401', call(port, '/api/state')[0] == 401)
    check('no session: no settings saved', call(port, '/api/config', {'config': {}})[0] == 401)
    where = urllib.parse.urlsplit(call(port, '/api/session')[1].get('local', ''))
    origin = {'Origin': 'http://127.0.0.1:%d' % port}
    code, got, _ = call(where.port, where.path, host=where.hostname, headers=origin)
    check('the page gets a code', code == 200 and got.get('code'), (code, got))
    code, _, cookie = call(port, '/api/login', {'password': got.get('code', '')})
    check('and signs in with it', code == 200 and call(port, '/api/state', cookie=cookie)[0] == 200, code)
    check('a code works once', call(port, '/api/login', {'password': got.get('code', '')})[0] == 401)
    code, _, second = call(port, '/api/login', {'password': m.code})
    check('the command line\'s code signs in', code == 200 and bool(second), code)
    token = {'X-BF-Token': m.token}
    check('the token signs the command line in', call(port, '/api/state', headers=token)[0] == 200)
    check('the token gets the code', call(port, '/api/code', headers=token)[1].get('code') == m.code)
    code, _, kept = call(port, '/api/password', {'password': 'a test password'}, cookie)
    check('a password is set, and the page stays signed in', code == 200 and call(port, '/api/state', cookie=kept)[0] == 200, code)
    check('the other session ended', call(port, '/api/state', cookie=second)[0] == 401)
    check('the code no longer signs in', call(port, '/api/login', {'password': m.code})[0] == 401)
    code, _, signed = call(port, '/api/login', {'password': 'a test password'})
    check('the password signs in', code == 200 and call(port, '/api/state', cookie=signed)[0] == 200, code)
    m.close_local_page()
    server.shutdown()
    print('%d failed' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
