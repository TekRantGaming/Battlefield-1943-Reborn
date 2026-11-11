"""Listen on the game's EA ports and log every connection's first bytes, to see what the client sends.

    python -I server/probe.py [--port 18241] [--seconds 120] [--log runs/probe.log]
"""
import argparse
import socket
import threading
import time


def handle(conn, addr, log, lock):
    conn.settimeout(10)
    try:
        data = b''
        while len(data) < 4096:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            with lock:
                log.write('%s %s:%d sent %d bytes\n%s\n' % (time.strftime('%H:%M:%S'), addr[0], addr[1], len(chunk),
                                                          chunk[:512].hex(' ', 16)))
                log.flush()
    except OSError as e:
        with lock:
            log.write('%s %s:%d %s\n' % (time.strftime('%H:%M:%S'), addr[0], addr[1], e))
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=18241)
    ap.add_argument('--seconds', type=int, default=120)
    ap.add_argument('--log', default='runs/probe.log')
    a = ap.parse_args()
    lock = threading.Lock()
    with open(a.log, 'a') as log, socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', a.port))
        s.listen(8)
        s.settimeout(1)
        log.write('%s listening on 127.0.0.1:%d\n' % (time.strftime('%H:%M:%S'), a.port))
        log.flush()
        end = time.time() + a.seconds
        while time.time() < end:
            try:
                conn, addr = s.accept()
            except socket.timeout:
                continue
            with lock:
                log.write('%s connection from %s:%d\n' % (time.strftime('%H:%M:%S'), addr[0], addr[1]))
                log.flush()
            threading.Thread(target=handle, args=(conn, addr, log, lock), daemon=True).start()


if __name__ == '__main__':
    main()
