"""Our profanity filter for the backend: soldier names and clan tags, the same rules as the game's bf/src/text_filter.h
(NameAllowed, CheckTag). The game checks first, to say why at once; the backend (plasma.py) decides, so a changed
game cannot wear a refused tag. tools/test_clan_tags.py keeps the two equal: the lists here against the header's, and
every answer against the header's own test program (bf/tests/text_filter_tests.cpp --tags).

Clan tags (TekRantGaming, 16 Nov 2026: "can be left blank or can have custom text ... Ban the use of Devs and Dev clan
tags and profanity like NOG"): 2 to 4 letters or digits; Dev and its lookalikes are the developers' alone."""

# ---- soldier names (text_filter.h detail::kAnywhere, kWords, kNameOnly, kInnocent)
ANYWHERE = (
    "fuck", "phuck", "phuk", "shit", "cunt", "nigger", "nigga", "faggot", "whore",
    "slut", "bitch", "biatch", "bastard", "asshole", "arsehole", "dickhead", "cocksuck", "penis",
    "vagina", "pussy", "dildo", "blowjob", "handjob", "rimjob", "jizz", "cumshot", "cumslut",
    "cumdump", "wank", "twat", "porn", "rapist", "molest", "pedophil", "paedophil", "incest",
    "wetback", "raghead", "towelhead", "bollock", "bellend", "clit", "orgasm", "milf", "douchebag",
    "jerkoff",
)
WORDS = (
    "ass", "asses", "arse", "arses", "dumbass", "jackass", "fatass", "asshat", "asswipe", "assface",
    "smartass", "tits", "titty", "titties", "boob", "boobs", "boobies", "dick", "dicks", "cock",
    "cocks", "cum", "cums", "cumming", "fag", "fags", "fagot", "fuk", "fuks", "fuker",
    "fukin", "fuking", "fukker", "fck", "fcking", "fckin", "fcker", "prick", "pricks", "spic",
    "spics", "chink", "chinks", "gook", "gooks", "coon", "coons", "kike", "kikes", "beaner",
    "beaners", "dyke", "dykes", "tranny", "trannies", "retard", "retards", "retarded", "spaz", "spastic",
    "paki", "pakis", "jap", "japs", "rape", "rapes", "raped", "raping", "slag", "skank",
    "skanks", "douche", "anal", "anus", "pedo", "pedos", "paedo", "kys", "horny", "boner",
)
NAME_ONLY = ("nazi", "hitler", "heil", "kkk")
INNOCENT = ("scunthorpe", "shiitake", "shitake", "snigger", "niggard", "therapist", "swank", "phuket", "nazir")

# ---- clan tags (text_filter.h kTagMin, kTagMax, detail::kTag...)
TAG_MIN, TAG_MAX = 2, 4
TAG_RESERVED_ANYWHERE = ("dev", "adm", "bot")  # bot: the bots' own tag (bots.cpp "[BOT]"), 20 Nov 2026
TAG_RESERVED_WHOLE = ("dv", "dvs", "dvz", "mod", "mods", "modz", "staf", "stff", "ea", "dice")
TAG_ANYWHERE = ("nig", "nog", "ngr", "niga", "fag", "fgt", "kkk", "cunt",
                "fuck", "fuk", "fck", "shit", "jizz", "nazi", "heil", "porn",
                "rape", "kike", "spic", "gook", "coon", "paki", "wank", "twat")
TAG_WHOLE = (
    "fu", "fk", "fuc", "fux", "fcuk", "stfu", "gtfo", "kys", "ss", "hh", "wp", "wpww", "isis", "cum",
    "tit", "tits", "dik", "dic", "dick", "cok", "coc", "cock", "azz", "ass", "arse", "pus", "puss", "sex",
    "sexy", "hoe", "hoes", "thot", "fap", "rtrd", "tard", "sieg", "ped", "pedo", "cnt", "sht", "btch", "bich",
    "anal", "slut", "hore", "jap", "japs", "dyke", "homo", "lezz", "smd", "kms", "nga", "ngga",
)
TAG_DIGITS_WHOLE = ("88", "h88", "1488")
TAG_DIGITS_ANYWHERE = ("1488",)

# The player's words for each problem code (the game shows its own, the same: TagProblemText).
TAG_PROBLEM_TEXT = {
    'length': "A clan tag needs 2 to 4 letters or numbers, or leave it blank.",
    'chars': "A clan tag needs 2 to 4 letters or numbers, or leave it blank.",
    'reserved': "That tag is kept for the game's developers and bots. Choose another one.",
    'profanity': "That clan tag is not allowed. Choose another one.",
    'with_name': "That clan tag and your name together are not allowed.",
}


def _fold(c, leet):
    if 'A' <= c <= 'Z':
        return c.lower()
    if 'a' <= c <= 'z':
        return c
    if c in '@$!':
        return {'@': 'a', '$': 's', '!': 'i'}[c]
    if not leet:
        return ''
    return {'0': 'o', '1': 'i', '3': 'e', '4': 'a', '5': 's', '7': 't'}.get(c, '')


def _fold_word(w, leet):
    return ''.join(_fold(c, leet) for c in w)


def _has_letter(w):
    return any(('a' <= c <= 'z') or ('A' <= c <= 'Z') for c in w)


def _runs(s):
    out = []
    for c in s:
        if out and out[-1][0] == c:
            out[-1][1] += 1
        else:
            out.append([c, 1])
    return out


def _runs_at(text, at, entry):
    if not entry or at + len(entry) > len(text):
        return False
    return all(text[at + j][0] == entry[j][0] and text[at + j][1] >= entry[j][1] for j in range(len(entry)))


def _contains(text, entry):
    e = _runs(entry)
    return any(_runs_at(text, at, e) for at in range(len(text)))


def _same(text, entry):
    e = _runs(entry)
    return len(text) == len(e) and _runs_at(text, 0, e)


def _bad_folded(folded, name):
    if not folded:
        return False
    runs = _runs(folded)
    if any(_same(runs, e) for e in WORDS):
        return True
    blanked = folded
    for ok in INNOCENT:
        at = blanked.find(ok)
        while at != -1:
            blanked = blanked[:at] + '-' * len(ok) + blanked[at + len(ok):]
            at = blanked.find(ok, at + 1)
    blanked_runs = _runs(blanked)
    if any(_contains(blanked_runs, e) for e in ANYWHERE):
        return True
    return name and any(_contains(blanked_runs, e) for e in NAME_ONLY)


def _bad_word(word, name):
    if not _has_letter(word):
        return False
    return _bad_folded(_fold_word(word, True), name) or _bad_folded(_fold_word(word, False), name)


def name_allowed(name):
    """Whether a soldier name may be used: no profanity in it, read whole and word by word (text_filter.h)."""
    if '1488' in name:
        return False
    if _bad_word(''.join(c for c in name if c not in ' _'), True):
        return False
    word, bad = '', False
    for i, c in enumerate(name):
        if c in ' _':
            bad = bad or (bool(word) and _bad_word(word, True))
            word = ''
            continue
        if i > 0 and 'A' <= c <= 'Z' and 'a' <= name[i - 1] <= 'z':
            bad = bad or (bool(word) and _bad_word(word, True))
            word = ''
        word += c
    bad = bad or (bool(word) and _bad_word(word, True))
    return not bad


_TAG_LEET = {'0': 'o', '2': 'z', '3': 'e', '4': 'a', '5': 's', '6': 'g', '7': 't', '8': 'b', '9': 'g'}


def _tag_fold(c, one_is_i):
    c = c.lower() if 'A' <= c <= 'Z' else c
    if one_is_i and c == 'l':
        return 'i'
    if c == '1':
        return 'i' if one_is_i else 'l'
    return _TAG_LEET.get(c, c)


def _collapse(s):
    out = ''
    for c in s:
        if not out or out[-1] != c:
            out += c
    return out


def _strip(s):
    if len(s) < 3 or s[0] not in 'xz' or s[-1] not in 'xz':
        return s
    return s.strip('xz')


def tag_readings(tag):
    leet = ''.join(_tag_fold(c, False) for c in tag)
    leet_i = ''.join(_tag_fold(c, True) for c in tag)
    letters = ''.join(_tag_fold(c, False) for c in tag if not c.isdigit())
    out = []
    for r in (leet, leet_i, letters):
        for v in (r, _collapse(r), _strip(r), _strip(_collapse(r)), _collapse(_strip(r))):
            if v:
                out.append(v)
    return out


def _tag_hits(readings, anywhere, whole):
    return any(any(e in r for e in anywhere) or r in whole for r in readings)


def tag_problem(tag, name=''):
    """What is wrong with a clan tag for this soldier name: '' when it may be worn (an empty tag is no tag), else
    'length', 'chars', 'reserved' (Dev and its lookalikes, staff tags), 'profanity' or 'with_name'."""
    if not tag:
        return ''
    if not isinstance(tag, str) or not TAG_MIN <= len(tag) <= TAG_MAX:
        return 'length'
    if not all(c.isascii() and c.isalnum() for c in tag):
        return 'chars'
    readings = tag_readings(tag)
    if _tag_hits(readings, TAG_RESERVED_ANYWHERE, TAG_RESERVED_WHOLE):
        return 'reserved'
    lower = tag.lower()
    if lower in TAG_DIGITS_WHOLE or any(d in lower for d in TAG_DIGITS_ANYWHERE):
        return 'profanity'
    if not name_allowed(tag) or _tag_hits(readings, TAG_ANYWHERE, TAG_WHOLE):
        return 'profanity'
    if name and not name_allowed(tag + name):
        return 'with_name'
    return ''
