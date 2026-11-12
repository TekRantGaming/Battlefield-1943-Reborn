"""Tests of gmhost.py's voice and text chat relay (class Comms, docs/COMMS-PLAN.md) without a game: a stand in host
with players joined from their addresses, and the packets the games send. Run: python -I server/test_comms.py"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gmhost  # noqa: E402

MAGIC = gmhost.COMMS


class Sent:
    def __init__(self):
        self.packets = []

    def sendto(self, data, addr):
        self.packets.append((addr, data))


class Player:
    def __init__(self, name):
        self.name = name


class Peer:
    def __init__(self, name, addr):
        self.state, self.player, self.addr = 'joined', Player(name), addr


class Host:
    def __init__(self, players):
        self.transport = Sent()
        self.peers = {addr: Peer(name, addr) for name, addr in players}
        self.notes = []

    def note(self, text):
        self.notes.append(text)


def text8(s):
    b = s.encode('utf-8')
    return bytes([len(b)]) + b


def hello(name, team=0, squad=0):
    return bytes([gmhost.C_HELLO, gmhost.COMMS_PROTOCOL, team, squad]) + text8(name)


def state(cid, team, squad, pos=(0.0, 0.0, 0.0), flags=1):
    return bytes([gmhost.C_STATE]) + struct.pack('>HBBBfff', cid, team, squad, flags, *pos)


def chat(cid, msg, channel, text):
    raw = text.encode('utf-8')
    return bytes([gmhost.C_CHAT]) + struct.pack('>HHBB', cid, msg, channel, len(raw)) + raw


def voice(cid, seq, pos, frame=b'\x00' * 164):
    return bytes([gmhost.C_VOICE]) + struct.pack('>HHfffBH', cid, seq, *pos, 1, len(frame)) + frame


# Game addresses (port 1000) and each game's comms socket (another port, the same IP).
A, B, C, D = ('10.0.0.1', 1000), ('10.0.0.2', 1000), ('10.0.0.3', 1000), ('10.0.0.4', 1000)
CA, CB, CC, CD = ('10.0.0.1', 50001), ('10.0.0.2', 50002), ('10.0.0.3', 50003), ('10.0.0.4', 50004)


class CommsTest(unittest.TestCase):
    def setUp(self):
        self.host = Host([('Alpha', A), ('Bravo', B), ('Charlie', C), ('Delta', D)])
        self.comms = gmhost.Comms(self.host)

    def sent(self, kind=None):
        out = [(addr, data[9:]) for addr, data in self.host.transport.packets
               if data[:8] == MAGIC and (kind is None or data[8] == kind)]
        self.host.transport.packets.clear()
        return out

    def join(self, name, addr, team, squad, pos=(0.0, 0.0, 0.0)):
        self.comms.receive(hello(name, team, squad), addr)
        welcome = self.sent(gmhost.C_WELCOME)
        self.assertEqual(len(welcome), 1)
        cid = struct.unpack_from('>H', welcome[0][1])[0]
        self.comms.receive(state(cid, team, squad, pos), addr)
        return cid

    def test_hello_needs_a_player_in_the_game_from_the_same_ip(self):
        self.comms.receive(hello('Nobody'), ('10.0.0.9', 5000))
        self.assertEqual(self.sent(gmhost.C_REFUSED)[0][1][0], 1)
        self.comms.receive(hello('Alpha'), ('10.0.0.9', 5000))  # Alpha's name from another IP
        self.assertEqual(self.sent(gmhost.C_REFUSED)[0][1][0], 1)
        self.host.peers[B].state = 'joining'
        self.comms.receive(hello('Bravo'), CB)  # not joined yet
        self.assertEqual(self.sent(gmhost.C_REFUSED)[0][1][0], 1)
        self.comms.receive(bytes([gmhost.C_HELLO, 99, 0, 0]) + text8('Alpha'), CA)  # another protocol
        self.assertEqual(self.sent(gmhost.C_REFUSED)[0][1][0], 2)
        self.comms.receive(hello('Alpha'), CA)
        self.assertEqual(len(self.sent(gmhost.C_WELCOME)), 1)

    def test_packets_before_hello_are_refused(self):
        self.comms.receive(chat(1, 1, gmhost.CHAT_ALL, 'hi'), CA)
        self.assertEqual(self.sent(gmhost.C_REFUSED)[0][1][0], 3)

    def test_ids_kept_across_a_rejoin(self):
        first = self.join('Alpha', CA, 1, 1)
        self.comms.receive(bytes([gmhost.C_BYE]) + struct.pack('>H', first), CA)
        again = self.join('Alpha', ('10.0.0.1', 50111), 1, 1)
        self.assertEqual(first, again)

    def test_chat_channels(self):
        a = self.join('Alpha', CA, 1, 1)
        self.join('Bravo', CB, 1, 1)
        self.join('Charlie', CC, 1, 2)
        self.join('Delta', CD, 2, 1)
        self.sent()

        def receivers(channel, text):
            self.comms.receive(chat(a, len(text), channel, text), CA)
            got = self.sent(gmhost.C_CHAT_RELAY)
            for addr, body in got:
                frm, msg, ch, team = struct.unpack_from('>HHBB', body)
                n = body[6]
                self.assertEqual((frm, ch, team, body[7:7 + n].decode()), (a, channel, 1, 'Alpha'))
                self.assertEqual(body[8 + n:].decode(), text)
            return sorted({addr for addr, _ in got}), len(got)

        self.assertEqual(receivers(gmhost.CHAT_ALL, 'to all'), ([CA, CB, CC, CD], 8))  # each twice
        self.comms.receive(chat(a, len('to all'), gmhost.CHAT_ALL, 'to all'), CA)  # the game's second copy
        self.assertEqual(self.sent(gmhost.C_CHAT_RELAY), [])
        self.assertEqual(receivers(gmhost.CHAT_TEAM, 'to team'), ([CA, CB, CC], 6))
        self.assertEqual(receivers(gmhost.CHAT_SQUAD, 'to squad'), ([CA, CB], 4))

    def test_team_and_squad_chat_need_them_known(self):
        a = self.join('Alpha', CA, 0, 0)
        self.join('Bravo', CB, 0, 0)
        self.join('Delta', CD, 2, 1)
        self.sent()
        for channel in (gmhost.CHAT_TEAM, gmhost.CHAT_SQUAD):
            self.comms.receive(chat(a, channel, channel, 'secret'), CA)
            self.assertEqual(self.sent(gmhost.C_CHAT_RELAY), [])
        self.comms.receive(state(a, 2, 0), CA)  # a team now, still no squad
        self.comms.receive(chat(a, 9, gmhost.CHAT_SQUAD, 'secret'), CA)
        self.assertEqual(self.sent(gmhost.C_CHAT_RELAY), [])
        self.comms.receive(chat(a, 10, gmhost.CHAT_TEAM, 'team two'), CA)
        self.assertEqual(sorted({addr for addr, _ in self.sent(gmhost.C_CHAT_RELAY)}), [CA, CD])

    def test_chat_limit_and_bad_input(self):
        a = self.join('Alpha', CA, 1, 1)
        self.sent()
        for i in range(8):
            self.comms.receive(chat(a, i, gmhost.CHAT_ALL, 'spam %d' % i), CA)
        self.assertEqual(len(self.sent(gmhost.C_CHAT_RELAY)), 5 * 2)  # 5 in 5 s
        self.comms.receive(chat(a, 50, 7, 'no such channel'), CA)
        self.comms.receive(bytes([gmhost.C_CHAT, 0]), CA)  # cut short
        self.comms.receive(chat(a, 51, gmhost.CHAT_ALL, '   '), CA)
        self.assertEqual(self.sent(gmhost.C_CHAT_RELAY), [])

    def test_voice_goes_to_both_teams_within_range_never_back(self):
        a = self.join('Alpha', CA, 1, 1, (0.0, 0.0, 0.0))
        self.join('Bravo', CB, 1, 1, (30.0, 0.0, 0.0))
        self.join('Charlie', CC, 2, 1, (0.0, 0.0, 70.0))   # the other team, in range
        self.join('Delta', CD, 2, 1, (500.0, 0.0, 0.0))    # out of range
        self.sent()
        self.comms.receive(voice(a, 1, (0.0, 0.0, 0.0)), CA)
        got = self.sent(gmhost.C_VOICE_RELAY)
        self.assertEqual(sorted(addr for addr, _ in got), [CB, CC])
        frm, seq = struct.unpack_from('>HH', got[0][1])
        self.assertEqual((frm, seq), (a, 1))
        self.assertEqual(len(got[0][1]), 2 + 17 + 164)

    def test_voice_rate_limit(self):
        a = self.join('Alpha', CA, 1, 1)
        self.join('Bravo', CB, 1, 1)
        self.sent()
        for i in range(100):
            self.comms.receive(voice(a, i, (0.0, 0.0, 0.0)), CA)
        self.assertLessEqual(len(self.sent(gmhost.C_VOICE_RELAY)), 60)

    def test_tick_drops_players_who_left_and_sends_the_roster(self):
        self.join('Alpha', CA, 1, 1)
        self.join('Bravo', CB, 2, 3)
        self.sent()
        self.comms.tick(10 ** 9)  # far later: both silent too long
        self.assertEqual(self.comms.clients, {})
        a = self.join('Alpha', CA, 1, 1)
        self.join('Bravo', CB, 2, 3)
        del self.host.peers[B]  # Bravo left the game
        self.sent()
        self.comms.roster_at = 0
        import time
        self.comms.tick(time.monotonic())
        roster = self.sent(gmhost.C_ROSTER)
        self.assertEqual([addr for addr, _ in roster], [CA])
        body = roster[0][1]
        self.assertEqual(body[0], 1)
        self.assertEqual(struct.unpack_from('>HBB', body, 1), (a, 1, 1))


if __name__ == '__main__':
    unittest.main(verbosity=2)
