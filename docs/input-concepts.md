# Battlefield 1943 input concepts (research, 12 Nov 2026)

How the game turns the pad into the per-seat input that is simulated and sent to the server, for the 0.4.0 native mouse
and keyboard work. Static study of the recompiled code (`bf/generated/default`); nothing here was run yet. Concept
meanings are inferred from what reads them.

## Pipeline, once a game tick for the local player

`sub_82264278` (the local player's input update, called through `sub_8226E1C0`), in order (return addresses):

| Return | Step |
|---|---|
| | dt, tick and flag set on `state = player+324` |
| | settings read; `sub_821D09E0(ctrl, InputType)` picks the scheme's mapping (rebuilt with `sub_822CEB78` on a change) |
| **0x822644B0** | **`sub_822BDB90(mapper, state)`: pad to concepts** (mapper = `controllable+268`, vtable 0x8200E4A0, slot +12) |
| 0x822644D4 | event 31/41 broadcast with the state |
| 0x822644DC | `sub_8224C298`: concepts disabled by `player+96` (analog) / `+100` (digital) cleared |
| 0x822644F4 | FlipY: `sub_822BA5F0(state, 5)` inverts concept 5 |
| 0x82264504 | `sub_822532C8`: local prediction; sets bit 40 and the absolute aim yaw/pitch at +136/+140 (Sensitivity is its f2) |
| 0x82264510 | `sub_822C15D0(player+320, state)`: into the player's EntryInput |

The client tick `sub_82380600` then sends it: `sub_82378138(clientConn, dt)` copies `player+324` into a 192-byte message
(`sub_822BA8C0`), concepts 13 and 14 cleared when `+2404 == 0 && +2120`, queued through `sub_82371D00`. Serialized by
`sub_822BADF8` (tick 32 bits, entry 25, value 6, digital bits 8..33 and 40..43, the 8 analog floats, the aim angles when
bit 40 is set); read by `sub_822BA928`; the server feeds it to its EntryInput in `sub_823AF4A0`. **Digital concepts
34..39 are never sent.**

**Hook point for native input:** `sub_822BDB90` when `lr == 0x822644B0` (save r4, the state, before the call): after the
game's mapping, before its post-processing, local prediction and the send. Mind FlipY (pre-invert concept 5) and the
disabled masks.

Why writes in a turret's step (`sub_822ADE38`) were lost: the vehicle entry code works on a stack copy
(`sub_822C1518`), with input curves (`sub_822CBC10`) and scales (`sub_821D3C98`) applied to it, and the soldier's
prediction ticks a copy too.

## EntryInputState (176 bytes, vtable 0x8200E48C, made by `sub_822C1478`)

| Offset | Contents |
|---|---|
| +8..+0x24 | 8 analog floats: concept c at +8+4c (two-sided -1..1: 0, 1, 4, 5, 6; one-sided 0..1: 2, 3, 7) |
| +40 | 41 half floats: how long each concept has been held |
| +120 | u64 of digital concepts, big endian: bit i in byte +120+7-i/8, mask 1 << (i % 8); bit 40 aim valid, 43 from the client |
| +128 / +132 | dt / a float |
| +136 / +140 | absolute aim yaw / pitch (bit 40) |
| +152 / +156 / +160 | tick / entry (seat) / `player+316` |

EntryInput (360 bytes): states at +0 and +176; **+0x160 the current state**, +0x164 the previous.
API: value `sub_822BD248(ei, c)`, down `sub_822BD2A8`, pressed `sub_822BD2B0`, released `sub_822BD308`, held longer than
f1 `sub_822C1818`; on a state: set digital `sub_822BA578(state, c, bool)`, set analog clamped `sub_822BD518(state, c, f)`,
invert `sub_822BA5F0(state, c)`.

## Pad inputs (`sub_821C7FD8(pad, id)`)

0/1 LX+/-, 2/3 LY+/-, 4/5 RX+/-, 6/7 RY+/-, 8 Start, 9 Back, 10 LS, 11 RS, 12 A, 13 B, 14 X, 15 Y, 18 LB, 19 RB,
20 LT, 21 RT, 22-25 d-pad up/down/left/right. A concept's bindings: `sub_822BAF90(mapper, c)` (8-byte entries,
`buttonData+12` the pad input). EntryInputActionEnum to concept: `sub_821C8760` (identity but 17->29, 18..29->17..28,
40->128 none).

## Concepts (inferred)

| c | Meaning | Read by |
|---|---|---|
| 0 | throttle / move forward-back | soldier `sub_8220C100`, aim input +4; vehicle vfunc84 |
| 1 | strafe / steer | soldier aim input +8; vehicle vfunc112 |
| 2 | brake | vehicle vfunc92 |
| 3 | (handbrake?) | no reader found |
| 4 | yaw | soldier look x; plane rudder (vfunc104, negated) |
| 5 | pitch | soldier look y; vehicle vfunc108; turret y; FlipY |
| 6 | roll | vehicle vfunc116 (negated); turret x |
| 7 | fire (analog trigger) | weapons `sub_821F57B0` and others; cancels sprint |
| 8, 9 | fire related (alt fire, zoom?) | cleared with 7 when the weapon can't fire |
| 11 | jump? | `sub_822001A0` |
| 12, 15, 29 | soldier actions (pose, interact) | `sub_82201DC8`, `sub_82308268` |
| 16 | scoreboard / map? | `sub_82398E08`, HUD `sub_824534F0` |
| 17 / 18 / 19 | next / previous weapon / quick switch | `sub_822DD670` |
| 20 | weapon pressed (reload?) | weapons |
| 21, 35 | vehicle actions (35 not sent) | `sub_821D9108`, `sub_822E84E0` |
| 22 | sprint (likely); vehicle boost? | flag +71 |
| 13, 14 | cleared at send | `sub_823A1F80` |
| 10, 24, 25, 28 | unknown | `sub_822D8CE0`, `sub_82231CB8`, `sub_822D7D80`, `sub_821FE890` |

To confirm at run time: the u32 at 0x8200E4A0+12 is 0x822BDB90; log the state after the map call per seat while
pressing each control.
