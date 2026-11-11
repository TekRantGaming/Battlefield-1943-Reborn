"""Group a REX_DEV_FRAME_LOG frame into runs of similar draws (same targets, depth mode, scissor) and resolves."""
import re
import sys

lines = [l.split('Frame log: ', 1)[1].strip() for l in open(sys.argv[1], encoding='utf-8', errors='replace')
         if 'Frame log: ' in l]
run_key, run_n, run_first = None, 0, None


def flush():
    if run_key:
        print('  draws %4d..%-4d x%-4d %s' % (run_first, run_first + run_n - 1, run_n, run_key))


for l in lines:
    m = re.match(r'draw (\d+) ps (\w+) vs (\w+) pitch (\d+) msaa (\d+) z (\S+) ?(.*?) @(\S+) off \S+ scissor (\S+)(.*?) count (\d+)', l)
    if m:
        key = 'pitch %s z %s %s @%s scissor %s%s%s' % (m.group(4), m.group(6), m.group(7), m.group(8), m.group(9),
                                                      m.group(10), ' ps none' if m.group(2) == '0' * 16 else '')
        if key != run_key:
            flush()
            run_key, run_n, run_first = key, 0, int(m.group(1))
        run_n += 1
        continue
    flush()
    run_key = None
    print(l[:170])
flush()
