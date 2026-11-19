"""XXH3 64-bit (seed 0) in plain Python, for inputs of 17 bytes or more: the hash the runtime gives a shader's
microcode, so the tools can name shaders found in the game's files without the xxhash module."""
import struct

_SECRET = bytes.fromhex(
    'b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671fcb79e64eccc0e578825ad07dccff7221b8084674f743248e'
    'e03590e6813a264c3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8a8fa763fe39c343ff9dcbbc7c70b4f1d'
    '8a51e04bcdb45931c89f7ec9d9787364eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e2b16be587d47a1fc'
    '8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e')
_M = (1 << 64) - 1
P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3, P64_4, P64_5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63,
                                     0x27D4EB2F165667C5)


def _r64(b, i):
    return struct.unpack_from('<Q', b, i)[0]


def _fold(a, b):
    p = a * b
    return (p & _M) ^ (p >> 64)


def _avalanche(h):
    h ^= h >> 37
    h = (h * 0x165667919E3779F9) & _M
    return h ^ (h >> 32)


def _mix16(b, i, s):
    return _fold(_r64(b, i) ^ _r64(_SECRET, s), _r64(b, i + 8) ^ _r64(_SECRET, s + 8))


def xxh3_64(b):
    n = len(b)
    if n < 17:
        raise ValueError('xxh3_64 here takes 17 bytes or more')
    if n <= 128:
        acc = (n * P64_1) & _M
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(b, 48, 96) + _mix16(b, n - 64, 112)
                acc += _mix16(b, 32, 64) + _mix16(b, n - 48, 80)
            acc += _mix16(b, 16, 32) + _mix16(b, n - 32, 48)
        acc += _mix16(b, 0, 0) + _mix16(b, n - 16, 16)
        return _avalanche(acc & _M)
    if n <= 240:
        acc = (n * P64_1) & _M
        for i in range(8):
            acc += _mix16(b, 16 * i, 16 * i)
        acc = _avalanche(acc & _M)
        for i in range(8, n // 16):
            acc += _mix16(b, 16 * i, 16 * (i - 8) + 3)
        acc += _mix16(b, n - 16, 136 - 17)
        return _avalanche(acc & _M)
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    keys = [struct.unpack_from('<8Q', _SECRET, 8 * k) for k in range(17)]  # a stripe's keys, by its place in a block

    def stripe(at, key):
        d = struct.unpack_from('<8Q', b, at)
        for i in range(8):
            k = d[i] ^ key[i]
            acc[i ^ 1] = (acc[i ^ 1] + d[i]) & _M
            acc[i] = (acc[i] + (k & 0xFFFFFFFF) * (k >> 32)) & _M

    per_block = (len(_SECRET) - 64) // 8
    block = 64 * per_block
    blocks = (n - 1) // block
    scramble = struct.unpack_from('<8Q', _SECRET, len(_SECRET) - 64)
    for k in range(blocks):
        for s in range(per_block):
            stripe(k * block + 64 * s, keys[s])
        for i in range(8):
            a = acc[i]
            a ^= a >> 47
            a ^= scramble[i]
            acc[i] = (a * P32_1) & _M
    for s in range(((n - 1) - block * blocks) // 64):
        stripe(blocks * block + 64 * s, keys[s])
    stripe(n - 64, struct.unpack_from('<8Q', _SECRET, len(_SECRET) - 64 - 7))
    result = (n * P64_1) & _M
    for i in range(4):
        result += _fold(acc[2 * i] ^ _r64(_SECRET, 11 + 16 * i), acc[2 * i + 1] ^ _r64(_SECRET, 11 + 16 * i + 8))
    return _avalanche(result & _M)
