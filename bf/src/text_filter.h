// Our profanity filter (TekRantGaming, 11 Nov 2026: "When it comes to text chat, we should just censor the profanity
// word with stars. But when it comes to soldier name, we can't allow them to have the name at all. They'll have to
// select a new name."). Text chat: comms.cpp shows each word holding profanity as stars, as many as its characters.
// Soldier names: game_menu.cpp refuses them on the name page, and a saved one at the start (the player then chooses
// a new name before the game signs in). Header only, no game or SDK: bf/tests/text_filter_tests.cpp builds it alone.
//
// A word is compared after folding: lower case, the usual stand ins inside it (0 o, 1 i, 3 e, 4 a, 5 s, 7 t, @ a,
// $ s, ! i; a second reading drops every digit, so "Ass69" and "a55" are both read), and a letter held longer is still
// that letter ("fuuuck"). A word with no letter at all ("455 points") is never touched. The lists:
// - kAnywhere: found inside any word ("bullshit"), unless inside one of kInnocent ("scunthorpe", "therapist");
// - kWords: a whole word only ("ass", "asses"; never "class", "assault", "pass" or "assassin");
// - kNameOnly: names only, anywhere in them (hate words a chat may still name in passing in a war game);
// - kInnocent: real words holding a kAnywhere or kNameOnly entry.
// Chat words spelled out one letter at a time ("f u c k", "f.u.c.k") are read as one word.
// A name is read whole (letters run together across spaces and underscores: "F_U_C_K") and word by word, split at
// spaces, underscores and a capital after a small letter ("BigAss" is Big and Ass, "AssaultKing" Assault and King).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bf1943::text_filter {
namespace detail {

inline constexpr std::string_view kAnywhere[] = {
    "fuck",     "phuck",    "phuk",      "shit",     "cunt",      "nigger",   "nigga",    "faggot",   "whore",
    "slut",     "bitch",    "biatch",    "bastard",  "asshole",   "arsehole", "dickhead", "cocksuck", "penis",
    "vagina",   "pussy",    "dildo",     "blowjob",  "handjob",   "rimjob",   "jizz",     "cumshot",  "cumslut",
    "cumdump",  "wank",     "twat",      "porn",     "rapist",    "molest",   "pedophil", "paedophil", "incest",
    "wetback",  "raghead",  "towelhead", "bollock",  "bellend",   "clit",     "orgasm",   "milf",     "douchebag",
    "jerkoff",
};
inline constexpr std::string_view kWords[] = {
    "ass",     "asses",    "arse",    "arses",    "dumbass", "jackass", "fatass",  "asshat",  "asswipe",  "assface",
    "smartass", "tits",    "titty",   "titties",  "boob",    "boobs",   "boobies", "dick",    "dicks",    "cock",
    "cocks",   "cum",      "cums",    "cumming",  "fag",     "fags",    "fagot",   "fuk",     "fuks",     "fuker",
    "fukin",   "fuking",   "fukker",  "fck",      "fcking",  "fckin",   "fcker",   "prick",   "pricks",   "spic",
    "spics",   "chink",    "chinks",  "gook",     "gooks",   "coon",    "coons",   "kike",    "kikes",    "beaner",
    "beaners", "dyke",     "dykes",   "tranny",   "trannies", "retard", "retards", "retarded", "spaz",    "spastic",
    "paki",    "pakis",    "jap",     "japs",     "rape",    "rapes",   "raped",   "raping",  "slag",     "skank",
    "skanks",  "douche",   "anal",    "anus",     "pedo",    "pedos",   "paedo",   "kys",     "horny",    "boner",
};
inline constexpr std::string_view kNameOnly[] = {"nazi", "hitler", "heil", "kkk"};
inline constexpr std::string_view kInnocent[] = {
    "scunthorpe", "shiitake", "shitake", "snigger", "niggard", "therapist", "swank", "phuket", "nazir",
};

// A letter as it is compared: lower case; a digit or symbol standing in for one (leet true), or with leet false only
// the symbols (digits dropped); 0 when it reads as no letter (other digits, apostrophes).
inline char Fold(char c, bool leet) {
  if (c >= 'A' && c <= 'Z') return char(c - 'A' + 'a');
  if (c >= 'a' && c <= 'z') return c;
  switch (c) {
    case '@': return 'a';
    case '$': return 's';
    case '!': return 'i';
    default: break;
  }
  if (!leet) return 0;
  switch (c) {
    case '0': return 'o';
    case '1': return 'i';
    case '3': return 'e';
    case '4': return 'a';
    case '5': return 's';
    case '7': return 't';
    default: return 0;
  }
}

inline std::string FoldWord(std::string_view w, bool leet) {
  std::string out;
  for (char c : w)
    if (const char f = Fold(c, leet)) out += f;
  return out;
}

inline bool HasLetter(std::string_view w) {
  for (char c : w)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return false;
}

// A word as runs of one letter ("fuuuck": f1 u3 c1 k1): an entry matches where the same letters come in the same
// order, each run at least as long as the entry's ("ass" needs two s, so "as" is not it).
struct Run {
  char c;
  int n;
};
inline std::vector<Run> Runs(std::string_view s) {
  std::vector<Run> out;
  for (char c : s) {
    if (!out.empty() && out.back().c == c) ++out.back().n;
    else out.push_back({c, 1});
  }
  return out;
}
inline bool RunsAt(const std::vector<Run>& text, size_t at, const std::vector<Run>& entry) {
  if (entry.empty() || at + entry.size() > text.size()) return false;
  for (size_t j = 0; j < entry.size(); ++j)
    if (text[at + j].c != entry[j].c || text[at + j].n < entry[j].n) return false;
  return true;
}
inline bool Contains(const std::vector<Run>& text, std::string_view entry) {
  const std::vector<Run> e = Runs(entry);
  for (size_t at = 0; at < text.size(); ++at)
    if (RunsAt(text, at, e)) return true;
  return false;
}
inline bool Same(const std::vector<Run>& text, std::string_view entry) {
  const std::vector<Run> e = Runs(entry);
  return text.size() == e.size() && RunsAt(text, 0, e);
}

// A folded word: whether it is one of kWords, or holds a kAnywhere entry (kNameOnly too for a name) outside the
// innocent words.
inline bool BadFolded(const std::string& folded, bool name) {
  if (folded.empty()) return false;
  const std::vector<Run> runs = Runs(folded);
  for (std::string_view e : kWords)
    if (Same(runs, e)) return true;
  std::string blanked = folded;
  for (std::string_view ok : kInnocent)
    for (size_t at = blanked.find(ok); at != std::string::npos; at = blanked.find(ok, at + 1))
      blanked.replace(at, ok.size(), ok.size(), '-');
  const std::vector<Run> blanked_runs = Runs(blanked);
  for (std::string_view e : kAnywhere)
    if (Contains(blanked_runs, e)) return true;
  if (name)
    for (std::string_view e : kNameOnly)
      if (Contains(blanked_runs, e)) return true;
  return false;
}

// A word as typed: read with the stand ins, and again with its digits dropped.
inline bool BadWord(std::string_view word, bool name) {
  if (!HasLetter(word)) return false;
  return BadFolded(FoldWord(word, true), name) || BadFolded(FoldWord(word, false), name);
}

// The characters a chat word is made of: ASCII letters and digits, and the stand ins @ $ ! and the apostrophe.
inline bool WordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '$' ||
         c == '!' || c == '\'';
}

}  // namespace detail

// The chat text as shown: each word holding profanity replaced by stars, as many as its characters; everything else
// (spaces, punctuation, other languages' letters) as typed.
inline std::string Censor(const std::string& text) {
  using namespace detail;
  struct Word {
    size_t begin, end;
  };
  std::vector<Word> words;
  for (size_t i = 0; i < text.size();) {
    if (!WordChar(text[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && WordChar(text[j])) ++j;
    size_t b = i, e = j;  // a ! or ' before or after the word is punctuation ("shit!", "'fuck'")
    while (b < e && (text[b] == '!' || text[b] == '\'')) ++b;
    while (e > b && (text[e - 1] == '!' || text[e - 1] == '\'')) --e;
    if (b < e) words.push_back({b, e});
    i = j;
  }
  std::string out = text;
  auto stars = [&out](const Word& w) {
    for (size_t k = w.begin; k < w.end; ++k) out[k] = '*';
  };
  for (const Word& w : words)
    if (BadWord(std::string_view(text).substr(w.begin, w.end - w.begin), false)) stars(w);
  // Spelled out one letter at a time: three or more one character words in a row, read as one.
  for (size_t a = 0; a < words.size();) {
    size_t z = a;
    std::string joined;
    while (z < words.size() && words[z].end - words[z].begin == 1) joined += text[words[z].begin], ++z;
    if (z - a >= 3 && BadWord(joined, false))
      for (size_t k = a; k < z; ++k) stars(words[k]);
    a = z > a ? z : a + 1;
  }
  return out;
}

// Whether a soldier name may be used: no profanity in it, read whole and word by word.
inline bool NameAllowed(const std::string& name) {
  using namespace detail;
  if (name.find("1488") != std::string::npos) return false;  // a hate number, as digits
  std::string joined;
  for (char c : name)
    if (c != ' ' && c != '_') joined += c;
  if (BadWord(joined, true)) return false;
  std::string word;
  bool bad = false;
  auto flush = [&] {
    if (!word.empty() && BadWord(word, true)) bad = true;
    word.clear();
  };
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == ' ' || c == '_') {
      flush();
      continue;
    }
    if (i > 0 && c >= 'A' && c <= 'Z' && name[i - 1] >= 'a' && name[i - 1] <= 'z') flush();
    word += c;
  }
  flush();
  return !bad;
}

}  // namespace bf1943::text_filter
