"""Decode the EA tunnel traffic in a run log (BF_NET_TRACE=1 and BF_NET_DUMP=1): every datagram to or from a
GameManager port, split into tunnel sub packets, and the link (CommUDP style) packets on port index 1 split into
header, payloads and trailing sizes.

    python -I tools/tunnel_dump.py runs/cap1/peer/bf1943.log [--port 1000] [--max 200]

Tunnel datagram: u16 length (the VDP header), then one u16 header per sub packet (length << 4 | port index), then
the sub packets' data in the same order. Port index 7 is control: u8 0, u32 sender id, u8 bound.
"""
import argparse
import re

LINE = re.compile(r'^\[[\d-]+ ([\d:.]+)\].*BF NET (sendto|WSARecvFrom) socket \w+ (?:length \d+ to|\d+ bytes from) '
                  r'([\d.]+:\d+).*? data ([0-9A-F]+)')


def split_tunnel(data):
    total = int.from_bytes(data[:2], 'big')
    heads, at, used = [], 2, 2
    while used < len(data):
        h = int.from_bytes(data[at:at + 2], 'big')
        heads.append((h >> 4, h & 15))
        at += 2
        used = at + sum(n for n, _ in heads)
        if used >= len(data):
            break
    subs = []
    for n, port in heads:
        subs.append((port, data[at:at + n]))
        at += n
    return total, subs, data[at:]


def link(data):
    """seq, ack and the payloads of a link packet. The seq's top nibble is the number of extra payloads, packed
    after the main one as [payload][size byte] each; read from the end."""
    seq = int.from_bytes(data[0:4], 'big')
    ack = int.from_bytes(data[4:8], 'big')
    body = data[8:]
    extras = []
    for _ in range(seq >> 28):
        if not body:
            break
        n = body[-1]
        extras.insert(0, body[-1 - n:-1])
        body = body[:-1 - n]
    return seq & 0x0FFFFFFF, ack, [body] + extras


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--port', type=int, default=1000)
    ap.add_argument('--max', type=int, default=200)
    a = ap.parse_args()
    shown = 0
    for line in open(a.log, encoding='latin-1'):
        m = LINE.match(line)
        if not m or not m.group(3).endswith(':%d' % a.port):
            continue
        t, kind, peer, hexdata = m.groups()
        data = bytes.fromhex(hexdata)
        arrow = '->' if kind == 'sendto' else '<-'
        total, subs, rest = split_tunnel(data)
        print('%s %s %s len %d' % (t, arrow, peer, total))
        for port, sub in subs:
            if port == 7:
                print('    ctl id %08X bound %d' % (int.from_bytes(sub[1:5], 'big'), sub[5]))
            elif port == 1 and len(sub) >= 8:
                seq, ack, parts = link(sub)
                print('    link seq %X ack %X' % (seq, ack))
                for p in parts:
                    print('        [%d] %s' % (len(p), p.hex().upper()))
            else:
                print('    port %d [%d] %s' % (port, len(sub), sub.hex().upper()[:160]))
        if rest:
            print('    rest %s' % rest.hex().upper())
        shown += 1
        if shown >= a.max:
            break


if __name__ == '__main__':
    main()
