// Our profanity filter (text_filter.h): chat words shown as stars, soldier names refused, and the everyday words and
// names it must leave alone (a war game's chat: assault, class, pass, cockpit, torpedo, therapist).
// Build (no game, no SDK): clang++ -std=c++20 -I bf/src bf/tests/text_filter_tests.cpp -o text_filter_tests
#include "text_filter.h"

#include <cstdio>
#include <string>

using bf1943::text_filter::Censor;
using bf1943::text_filter::NameAllowed;

namespace {
int failures = 0, checks = 0;
void Chat(const std::string& typed, const std::string& shown) {
  ++checks;
  const std::string got = Censor(typed);
  if (got != shown) {
    std::printf("FAIL chat \"%s\": shown \"%s\", want \"%s\"\n", typed.c_str(), got.c_str(), shown.c_str());
    ++failures;
  }
}
void Name(const std::string& name, bool allowed) {
  ++checks;
  if (NameAllowed(name) != allowed) {
    std::printf("FAIL name \"%s\": %s, want %s\n", name.c_str(), allowed ? "refused" : "allowed",
                allowed ? "allowed" : "refused");
    ++failures;
  }
}
}  // namespace

int main() {
  // Chat: the word as stars, the rest as typed.
  Chat("you fucking idiot", "you ******* idiot");
  Chat("this is bullshit!", "this is ********!");
  Chat("FUCK", "****");
  Chat("fuuuuck that", "******* that");
  Chat("sh1t happens", "**** happens");
  Chat("$hit", "****");
  Chat("kick ass", "kick ***");
  Chat("ASS!!", "***!!");
  Chat("a55 and 4ss", "*** and ***");
  Chat("f u c k you", "* * * * you");
  Chat("f.u.c.k", "*.*.*.*");
  Chat("what a b1tch", "what a *****");
  Chat("motherfucker", "************");
  Chat("'fuck'", "'****'");
  Chat("caf\xC3\xA9 shit", "caf\xC3\xA9 ****");  // other languages' letters stay as typed
  Chat("tits", "****");
  Chat("cumming in hot", "******* in hot");
  // Chat: left as typed.
  Chat("assault the flag", "assault the flag");
  Chat("nice class, pass the ammo", "nice class, pass the ammo");
  Chat("assassin on the ridge", "assassin on the ridge");
  Chat("the cockpit is hit", "the cockpit is hit");
  Chat("torpedo the carrier", "torpedo the carrier");
  Chat("grass and bass", "grass and bass");
  Chat("my therapist says hi", "my therapist says hi");
  Chat("Scunthorpe United", "Scunthorpe United");
  Chat("455 points, 50 cal", "455 points, 50 cal");
  Chat("hello world", "hello world");
  Chat("I am at A", "I am at A");
  Chat("niger delta", "niger delta");
  Chat("japan and the nazis", "japan and the nazis");  // hate words are for names, not a war game's chat
  Chat("", "");
  Chat("   ", "   ");
  Chat("good game!", "good game!");

  // Names: refused.
  Name("FuckYou", false);
  Name("xXShitXx", false);
  Name("Big_Ass", false);
  Name("BigAss", false);
  Name("Ass69", false);
  Name("B1tch", false);
  Name("F_U_C_K", false);
  Name("Fuuuck", false);
  Name("A_S_S", false);
  Name("Dick7", false);
  Name("Cum_Lord", false);
  Name("TheRapist", false);
  Name("Hitler88", false);
  Name("xXNaziXx", false);
  Name("KKKiller", false);
  Name("Sniper1488", false);
  Name("Big Ass", false);
  // Names: allowed.
  Name("testboi", true);
  Name("Soldier1234", true);
  Name("AssaultKing", true);
  Name("Classy_Sniper", true);
  Name("CockpitAce", true);
  Name("Torpedo_Joe", true);
  Name("Therapist", true);
  Name("Scunthorpe", true);
  Name("WakeFox42", true);
  Name("Assassin", true);
  Name("Nazir", true);
  Name("Dickens", true);
  Name("Cumberland", true);
  Name("Titan", true);
  Name("Japan", true);
  Name("Hitman", true);
  Name("PacificHawk10", true);
  Name("Grape", true);

  std::printf("%d of %d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
