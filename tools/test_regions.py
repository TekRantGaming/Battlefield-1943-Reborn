"""The server regions are one list in two places: the game's (bf/src/regions.h kRegions: what the server browser can
show, with its flag) and the server manager's (server/manager.py REGIONS: what a host can pick). This checks that
they are the same codes, names and countries in the same order, and that the game has a flag for each country.

    python -I tools/test_regions.py
"""
import ast
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def game_regions():
    text = open(os.path.join(ROOT, 'bf', 'src', 'regions.h'), encoding='utf-8').read()
    table = text[text.index('kRegions[] = {'):]
    table = table[:table.index('};')]
    return re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}', table), text


def manager_regions():
    tree = ast.parse(open(os.path.join(ROOT, 'server', 'manager.py'), encoding='utf-8').read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', '') == 'REGIONS' for t in node.targets):
            return [tuple(x) for x in ast.literal_eval(node.value)]
    raise SystemExit('server/manager.py has no REGIONS')


def main():
    game, header = game_regions()
    manager = manager_regions()
    failures = []

    def check(what, ok):
        print(('ok    ' if ok else 'FAIL  ') + what)
        if not ok:
            failures.append(what)

    check('the game knows %d regions, the manager %d' % (len(game), len(manager)), len(game) == len(manager) and len(game) > 0)
    check('the same codes, names and countries in the same order', game == manager)
    for a, b in zip(game, manager):
        if a != b:
            print('      the game has %s, the manager %s' % (a, b))
    check('no code twice', len({c for c, _, _ in game}) == len(game))
    check('every code is short, lower case letters with dashes (what the backend keeps)',
          all(re.fullmatch(r'[a-z0-9-]{1,16}', c) for c, _, _ in game))
    flags = set(re.findall(r'c == "([a-z]{2})"', header)) | set(re.findall(r'country == "([a-z]{2})"', header))
    missing = sorted({country for _, _, country in game} - flags)
    check('the game draws a flag for every country%s' % (' (none for: %s)' % ', '.join(missing) if missing else ''), not missing)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
