// The bots' text chat (the user, 19 Nov 2026: "give basic quips and 'shit talk' in the ALL channel, give helpful
// teamplay info in TEAM text channel, and more personal squad based banter in the SQUAD text channel", praise for kills
// in TEAM and SQUAD, good sport banter in ALL, teasing their own teammates, the enemy joking about hacks during a
// player's long infantry kill streak, and simple replies to a player's own chat; "level 1": rules that know the match,
// lines of our own).
//
// Everything runs on the server: bots.cpp tells it the kills (Kill), the flags as they change hands (Flags, from its
// tick) and hands it the bots each tick (Tick); the GameManager host (gm_host.cpp) tells it a player's chat line
// (BotsHeardChat, the host's thread: queued) and sends the bots' lines (GmHostChat) to those they are for, as a
// player's. A line waits a moment before it is sent (people take a second to type), and the chat stays readable: at
// most one line a few seconds a channel, a bot at most once in 15 s, and a share of the events only.
// Every line here is our own writing (the task's rule: no lines from other games). bf_bot_chat turns them off.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "capture_points.h"

REXCVAR_DEFINE_BOOL(bf_bot_chat, true, "BF1943/Server",
                    "Bots talk in the text chat: banter in All, teamplay in Team, squad talk in Squad, and replies");

namespace bf1943 {

void GmHostChat(const std::string& name, int team, int squad, int channel, const std::string& text);  // gm_host.cpp

namespace botchat {

struct Who {
  std::string name;
  int team = 0, squad = 0;  // squad as the chat counts it: 0 none, else the game's + 1
  bool bot = false;
  bool leader = false;
  std::string goal;  // the flag it is heading for, as players call it ("B (Village)"), or ""
};

namespace {

using Clock = std::chrono::steady_clock;
enum Channel { kAll = 0, kTeam = 1, kSquad = 2 };

std::mutex g_mutex;
std::mt19937 g_rng{std::random_device{}()};
struct Pending {
  Clock::time_point due;
  Who who;
  int channel = 0;
  std::string text;
};
std::vector<Pending> g_pending;
std::map<std::string, Clock::time_point> g_bot_said;       // bot name -> when it last spoke
std::array<Clock::time_point, 3> g_channel_said{};          // channel -> when anyone last spoke there
std::map<std::string, int> g_streak;                         // a human's infantry kills since its last death
std::map<std::string, Clock::time_point> g_spawned;          // a player's last death (a quick second death is teased)
struct Heard {
  Who who;
  int channel = 0;
  std::string text;
};
std::deque<Heard> g_heard;  // players' lines from the host's thread, answered in Tick
std::map<std::string, int> g_owners;  // flag title -> owner last seen

float Rand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng); }
template <size_t N>
const char* Pick(const char* const (&lines)[N]) {
  return lines[std::uniform_int_distribution<size_t>(0, N - 1)(g_rng)];
}
// The bot's own temper, the same for a name all match long: 0 a good sport, 1 cheeky, 2 salty.
int Temper(const std::string& name) {
  uint32_t h = 2166136261u;
  for (char c : name) h = (h ^ uint8_t(c)) * 16777619u;
  return int(h % 3);
}
std::string First(const std::string& name) {  // "Earl Morgan" -> "Earl"; a Japanese name is family first: kept whole
  const size_t space = name.find(' ');
  return space == std::string::npos ? name : name.substr(0, space);
}
// One pass from left to right: a value put in is never read again, so a name with braces in it stays as it is.
std::string Fill(const std::string& line, const std::map<std::string, std::string>& values) {
  std::string out;
  out.reserve(line.size() + 32);
  for (size_t at = 0; at < line.size();) {
    const size_t open = line.find('{', at);
    if (open == std::string::npos) {
      out.append(line, at, std::string::npos);
      break;
    }
    out.append(line, at, open - at);
    const size_t close = line.find('}', open + 1);
    const auto it = close == std::string::npos ? values.end() : values.find(line.substr(open + 1, close - open - 1));
    if (it == values.end()) {
      out += '{';
      at = open + 1;
    } else {
      out += it->second;
      at = close + 1;
    }
  }
  return out;
}

// A line for a bot, in a channel, a few seconds on, if the bot and the channel have been quiet long enough.
bool Say(const Who& who, int channel, const std::string& text, float delay_min = 1.2f, float delay_max = 3.5f) {
  if (!REXCVAR_GET(bf_bot_chat) || !who.bot || text.empty()) return false;
  if (channel == kTeam && !who.team) return false;
  if (channel == kSquad && !who.squad) return false;
  const auto now = Clock::now();
  static constexpr std::array<int, 3> kChannelGap = {5, 4, 6};  // seconds between any two lines of a channel
  if (now - g_channel_said[channel] < std::chrono::seconds(kChannelGap[channel])) return false;
  if (const auto it = g_bot_said.find(who.name); it != g_bot_said.end() && now - it->second < std::chrono::seconds(15))
    return false;
  for (const Pending& p : g_pending)
    if (p.who.name == who.name) return false;
  g_channel_said[channel] = now, g_bot_said[who.name] = now;
  const float delay = delay_min + (delay_max - delay_min) * Rand();
  g_pending.push_back({now + std::chrono::milliseconds(int(delay * 1000)), who, channel, text});
  return true;
}

// ---- the lines (all ours)
constexpr const char* kKillAll[] = {
    "{victim}, you dropped this. It was your rifle.", "Sit down, {victim}.", "Too slow, {victim}!",
    "That one's going on the fridge.", "Should've stayed at the base, {victim}.", "Ha! Next.",
    "{victim}, hope you enjoyed the view from there.", "Get back to the boat, {victim}.",
    "Thanks for the free points, {victim}.", "And stay down!"};
constexpr const char* kKillAllSport[] = {"Sorry {victim}, nothing personal.", "Good fight, {victim}.",
                                         "Close one, {victim}!", "You almost had me there, {victim}."};
constexpr const char* kLongShot[] = {"{dist} metres. Somebody write that down.", "Long distance call for {victim}!",
                                     "{dist} m, and I didn't even hold my breath."};
constexpr const char* kDeathSalty[] = {"Lucky shot.", "Oh come ON.", "Lag!", "Really? From there?",
                                       "Enjoy it while it lasts, {killer}.", "I tripped. That didn't count.",
                                       "My rifle jammed. Swear."};
constexpr const char* kDeathSport[] = {"Nice shot, {killer}.", "Fair enough, {killer}. Got me.", "Well played, {killer}.",
                                       "Ha, good one {killer}.", "Okay, that was clean.", "Respect, {killer}."};
constexpr const char* kDeathToVehicle[] = {"Get out of the tank and fight me, {killer}!", "Rolled over again...",
                                           "Someone take care of that tank!", "Big gun, small courage, {killer}."};
constexpr const char* kPraiseTeam[] = {"Nice one {killer}!", "Good kill, {killer}.", "{killer} on fire today!",
                                       "That's how it's done, {killer}.", "Keep it up {killer}!"};
constexpr const char* kPraiseSquad[] = {"Nice work {first}!", "Good shot, {first}!", "{first} carrying us again.",
                                        "That's my squadmate!", "Saw that, {first}. Beauty.", "Teach me that one, {first}."};
constexpr const char* kTeaseTeam[] = {"{first}, that was quick. Even for you.", "Did you even leave the spawn, {first}?",
                                      "Speedrun to the respawn screen, {first}?", "{first}, the enemy is the OTHER way.",
                                      "Somebody give {first} a map.", "{first}, that wasn't the plan, was it?"};
constexpr const char* kHacks1[] = {"{player} is getting lucky...", "Okay {player}, who's paying you?",
                                   "Calm down {player}, it's not the finals."};
constexpr const char* kHacks2[] = {"{player} has to be hacking.", "{streak} in a row? Reported, {player}.",
                                   "Check {player}'s PC, something's off.", "{player} using wallhacks, guaranteed."};
constexpr const char* kHacks3[] = {"{player}, turn the aimbot off, please!", "{streak} kills?! That's it, I'm calling an admin.",
                                   "{player} downloaded more RAM and an aimbot.", "Nobody is that good. NOBODY, {player}.",
                                   "Someone unplug {player}'s computer."};
constexpr const char* kFlagTakenTeam[] = {"{flag} is ours! Hold it.", "Got {flag}. Moving on.", "{flag} captured, nice work team.",
                                          "{flag} secure. Someone stay and watch it."};
constexpr const char* kFlagLostTeam[] = {"We lost {flag}! Need people there.", "{flag} fell. Retake it!",
                                         "They took {flag}, heads up.", "{flag} is gone. Counter-attack, anyone?"};
constexpr const char* kFlagLostAll[] = {"Enjoy {flag} while you can.", "Borrowing {flag}? We'll want it back.",
                                        "Fine, have {flag}. For now."};
constexpr const char* kFlagTakenAll[] = {"{flag} is ours now, thanks for keeping it warm.", "Knock knock. {flag} is ours.",
                                         "Thank you for the flag at {flag}!"};
constexpr const char* kLeaderTeam[] = {"My squad's taking {goal}.", "Moving my squad to {goal}.",
                                       "{goal} is ours to deal with, my squad's on it.", "Squad heading for {goal}, cover us."};
constexpr const char* kLeaderGoal[] = {"Squad, we're heading for {goal}.", "Squad, on me. Target {goal}.",
                                       "Taking the squad to {goal}.", "Squad: {goal}. Let's go."};

// Replies to a player's line, by what it says.
struct Kind {
  const char* const* words;
  size_t count;
};
constexpr const char* kGreetWords[] = {"hi", "hello", "hey", "yo", "sup", "howdy", "greetings", "o7"};
constexpr const char* kGreet[] = {"Hey {first}!", "o7 {first}", "Hello {first}.", "Yo {first}, welcome in.",
                                  "Hey hey {first}."};
constexpr const char* kThanksWords[] = {"thanks", "thx", "ty", "thank", "cheers"};
constexpr const char* kThanks[] = {"Anytime, {first}.", "No problem!", "Got your back, {first}.", "np"};
constexpr const char* kGgWords[] = {"gg", "good game", "ggwp", "gg wp"};
constexpr const char* kGg[] = {"gg!", "gg wp {first}", "Good game everyone.", "gg, that was a fun one."};
constexpr const char* kNiceWords[] = {"nice shot", "ns", "nice", "good shot", "great shot", "well played", "wp"};
constexpr const char* kNice[] = {"Thanks {first}!", "I try.", "Practice, {first}. Lots of practice.", "Haha, thanks!"};
constexpr const char* kTauntWords[] = {"noob", "n00b", "trash", "ez", "bad", "rekt", "garbage", "scrub", "lol", "loser"};
constexpr const char* kTaunt[] = {"Big words, {first}.", "Says the one who keeps respawning.", "Come say that to my face, {first}.",
                                  "Talk is cheap, {first}. Points aren't.", "We'll see about that, {first}.",
                                  "Cute, {first}."};
constexpr const char* kHackWords[] = {"hack", "hacker", "hacks", "cheat", "cheater", "aimbot", "wallhack"};
constexpr const char* kHackReply[] = {"It's called skill, {first}.", "Not hacking, just good.",
                                      "Maybe aim at me next time, {first}?", "Report me to my mom, {first}."};
constexpr const char* kBotWords[] = {"are you a bot", "you a bot", "bots", "robot", "ai"};
constexpr const char* kBotReply[] = {"Beep boop. I mean... no.", "A bot? Me? Never heard of them.",
                                     "I'm as real as this island, {first}.", "01101110 01101111. That means no."};
constexpr const char* kHelpWords[] = {"help", "backup", "need", "where", "anyone", "support", "assist"};
constexpr const char* kHelpReply[] = {"On my way to {goal}, {first}.", "Heading to {goal}. Meet me there.",
                                      "I'm going for {goal}. Coming?"};
constexpr const char* kHelpNoGoal[] = {"Coming, {first}!", "Hang on {first}, almost there.", "Where are you, {first}?"};
constexpr const char* kFollowWords[] = {"follow", "on me", "with me", "attack", "defend", "go to", "push", "take"};
constexpr const char* kFollowReply[] = {"Roger that, {first}.", "Copy, {first}.", "On it.", "You got it, {first}.",
                                        "Right behind you."};
constexpr const char* kQuestion[] = {"No idea, {first}.", "Ask me after the match, {first}.", "Hmm, good question.",
                                     "Busy shooting, {first}!"};

bool Has(const std::string& lower, const char* const* words, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const std::string w = words[i];
    for (size_t at = lower.find(w); at != std::string::npos; at = lower.find(w, at + 1)) {
      const bool start = at == 0 || !std::isalnum(static_cast<unsigned char>(lower[at - 1]));
      const size_t end = at + w.size();
      const bool stop = end >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[end]));
      if (start && stop) return true;
    }
  }
  return false;
}
#define HAS(words) Has(lower, words, std::size(words))

}  // namespace

// A kill (bots.cpp, from the game's own death scoring): the killer and the victim, how far, whether from a vehicle.
void Kill(const Who& killer, const Who& victim, float distance, bool from_vehicle, const std::vector<Who>& bots) {
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  // A quick second death is teased by a teammate; the streak of the one who died ends.
  const bool quick = g_spawned.count(victim.name) && now - g_spawned[victim.name] < std::chrono::seconds(25);
  g_spawned[victim.name] = now;
  g_streak[victim.name] = 0;
  if (killer.name.empty() || killer.name == victim.name || killer.team == victim.team) return;
  const int streak = from_vehicle ? g_streak[killer.name] : ++g_streak[killer.name];
  const std::map<std::string, std::string> v = {{"killer", killer.name}, {"victim", victim.name},
                                                 {"first", First(killer.name)},
                                                 {"dist", std::to_string(int(distance + 0.5f))}};
  // The victim, a bot: a word in All now and then, as its temper is.
  if (victim.bot && Rand() < 0.35f) {
    const int t = Temper(victim.name);
    const char* line = from_vehicle && Rand() < 0.6f ? Pick(kDeathToVehicle) : t == 0 ? Pick(kDeathSport)
                                                                             : t == 1 ? (Rand() < 0.5f ? Pick(kDeathSport) : Pick(kDeathSalty))
                                                                                      : Pick(kDeathSalty);
    Say(victim, kAll, Fill(line, v));
  }
  // The killer, a bot: trash talk or a good sport's word in All, a long shot noted.
  if (killer.bot && Rand() < 0.25f) {
    const char* line = distance > 120.0f && !from_vehicle ? Pick(kLongShot)
                       : Temper(killer.name) == 0         ? Pick(kKillAllSport)
                                                          : Pick(kKillAll);
    Say(killer, kAll, Fill(line, v));
  }
  // The killer's teammates: praise in Squad (its squad's bots) or Team.
  if (Rand() < 0.3f) {
    std::vector<const Who*> squad, team;
    for (const Who& b : bots) {
      if (b.name == killer.name || b.team != killer.team) continue;
      (killer.squad && b.squad == killer.squad ? squad : team).push_back(&b);
    }
    if (!squad.empty()) Say(*squad[size_t(Rand() * squad.size()) % squad.size()], kSquad, Fill(Pick(kPraiseSquad), v));
    else if (!team.empty() && (streak >= 3 || distance > 100.0f))
      Say(*team[size_t(Rand() * team.size()) % team.size()], kTeam, Fill(Pick(kPraiseTeam), v));
  }
  // The victim's teammates: a quick second death teased (bots tease their own too).
  if (quick && Rand() < 0.35f) {
    std::vector<const Who*> mates;
    for (const Who& b : bots)
      if (b.name != victim.name && b.team == victim.team) mates.push_back(&b);
    if (!mates.empty()) {
      const Who& m = *mates[size_t(Rand() * mates.size()) % mates.size()];
      const std::map<std::string, std::string> t = {{"first", First(victim.name)}};
      Say(m, victim.squad && m.squad == victim.squad ? kSquad : kTeam, Fill(Pick(kTeaseTeam), t));
    }
  }
  // A human's long infantry streak: the other side's bots joke about hacks (5, 8, 12 and on).
  if (!killer.bot && !from_vehicle && (streak == 5 || streak == 8 || streak >= 12) && (streak < 12 || streak % 3 == 0)) {
    std::vector<const Who*> enemies;
    for (const Who& b : bots)
      if (b.team && b.team != killer.team) enemies.push_back(&b);
    if (!enemies.empty()) {
      const std::map<std::string, std::string> h = {{"player", killer.name}, {"streak", std::to_string(streak)}};
      const char* line = streak >= 12 ? Pick(kHacks3) : streak >= 8 ? Pick(kHacks2) : Pick(kHacks1);
      Say(*enemies[size_t(Rand() * enemies.size()) % enemies.size()], kAll, Fill(line, h), 0.8f, 2.0f);
    }
  }
}

// The flags (bots.cpp's tick): each one's title and owner now (0 none, 1 US, 2 Japan). A change of hands is told in
// Team by a squad leader of each side (else any bot), and now and then in All.
void Flags(const std::vector<std::pair<std::string, int>>& flags, const std::vector<Who>& bots) {
  std::lock_guard lock(g_mutex);
  for (const auto& [title, owner] : flags) {
    const auto it = g_owners.find(title);
    const int before = it == g_owners.end() ? -1 : it->second;
    g_owners[title] = owner;
    if (before < 0 || before == owner) continue;
    const std::map<std::string, std::string> v = {{"flag", title}};
    for (int side = 1; side <= 2; ++side) {
      const bool won = owner == side, lost = before == side;
      if (!won && !lost) continue;
      const Who* speaker = nullptr;
      for (const Who& b : bots)
        if (b.team == side && (!speaker || (b.leader && !speaker->leader))) speaker = &b;
      if (!speaker) continue;
      if (Rand() < 0.8f) Say(*speaker, kTeam, Fill(won ? Pick(kFlagTakenTeam) : Pick(kFlagLostTeam), v));
      if (Rand() < 0.2f) {
        for (const Who& b : bots)
          if (b.team == side && &b != speaker) {
            Say(b, kAll, Fill(won ? Pick(kFlagTakenAll) : Pick(kFlagLostAll), v));
            break;
          }
      }
    }
  }
}

// A bot getting out of its vehicle before it goes up (bots.cpp's BailCheck): a word to its squad, else its team.
void Bail(const Who& who, bool plane) {
  static constexpr const char* kBailLines[] = {"Bailing out!", "She's going up, I'm out!", "I'm out, she's burning!",
                                               "Getting out before she blows!", "That's it for this one. I'm out."};
  static constexpr const char* kBailPlane[] = {"Hit bad, I'm jumping!", "Going over the side!", "Chute time!",
                                               "She's done. Bailing out!"};
  std::lock_guard lock(g_mutex);
  if (Rand() > 0.6f) return;
  Say(who, who.squad ? kSquad : kTeam, plane ? Pick(kBailPlane) : Pick(kBailLines), 0.3f, 1.0f);
}

// A bot squad leader's call to its team when the players get well ahead (bots.cpp's Pressure, at half).
void Rally(const Who& leader) {
  static constexpr const char* kRallyGoal[] = {"They're running us over. All squads, push {goal} together!",
                                               "Enough of this. Everyone on {goal}, now!",
                                               "We're losing this. Stick with your squad and hit {goal} hard.",
                                               "Regroup on your leaders and take {goal}!"};
  static constexpr const char* kRally[] = {"They're running us over. Stick together and push!",
                                           "Squads, group up. We hit them together from here on.",
                                           "Enough of this. Watch each other's backs and push!"};
  std::lock_guard lock(g_mutex);
  g_channel_said[kTeam] = {};
  Say(leader, kTeam, leader.goal.empty() ? Pick(kRally) : Fill(Pick(kRallyGoal), {{"goal", leader.goal}}), 0.5f, 1.5f);
}

// Every server tick (bots.cpp): the replies to players' lines, a squad leader's word on its goal now and then, and
// the lines whose moment has come, sent through the host.
void Tick(const std::vector<Who>& bots) {
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  while (!g_heard.empty()) {
    const Heard h = g_heard.front();
    g_heard.pop_front();
    std::string lower = h.text;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    // Who may answer: a bot that reads that channel (All: anyone; Team: its team; Squad: its squad).
    std::vector<const Who*> readers;
    for (const Who& b : bots) {
      if (h.channel == kTeam && b.team != h.who.team) continue;
      if (h.channel == kSquad && (b.team != h.who.team || b.squad != h.who.squad || !h.who.squad)) continue;
      readers.push_back(&b);
    }
    if (readers.empty()) continue;
    std::shuffle(readers.begin(), readers.end(), g_rng);
    const Who& r = *readers.front();
    std::map<std::string, std::string> v = {{"first", First(h.who.name)}, {"goal", r.goal}};
    const char* line = nullptr;
    int replies = 1;
    if (HAS(kBotWords)) line = Pick(kBotReply);
    else if (HAS(kHackWords)) line = Pick(kHackReply);
    else if (HAS(kGgWords)) line = Pick(kGg), replies = 2;
    else if (HAS(kThanksWords)) line = Pick(kThanks);
    else if (HAS(kNiceWords)) line = Pick(kNice);
    else if (HAS(kTauntWords)) line = Pick(kTaunt);
    else if (HAS(kGreetWords)) line = Pick(kGreet), replies = 2;
    else if (HAS(kHelpWords)) line = r.goal.empty() ? Pick(kHelpNoGoal) : Pick(kHelpReply);
    else if (HAS(kFollowWords)) line = Pick(kFollowReply);
    else if (lower.find('?') != std::string::npos && Rand() < 0.5f) line = Pick(kQuestion);
    if (!line) continue;  // nothing it knows: no answer
    // An answer skips the channel's quiet time (it was spoken to), not the bot's own.
    for (int i = 0; i < replies && i < int(readers.size()); ++i) {
      const Who& who = *readers[size_t(i)];
      if (i) {
        v["goal"] = who.goal;
        line = HAS(kGgWords) ? Pick(kGg) : Pick(kGreet);
      }
      g_channel_said[h.channel] = {};
      Say(who, h.channel, Fill(line, v), 1.5f + 1.5f * float(i), 4.0f + 2.0f * float(i));
    }
  }
  // A squad leader names its goal to its squad when it picks a new one (now and then).
  static std::map<std::string, std::string> told_goal;
  for (const Who& b : bots) {
    if (!b.leader || !b.squad || b.goal.empty() || told_goal[b.name] == b.goal) continue;
    told_goal[b.name] = b.goal;
    if (Rand() < 0.4f) Say(b, kSquad, Fill(Pick(kLeaderGoal), {{"goal", b.goal}}), 0.5f, 1.5f);
    else if (Rand() < 0.4f) Say(b, kTeam, Fill(Pick(kLeaderTeam), {{"goal", b.goal}}), 0.5f, 1.5f);
  }
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (it->due > now) {
      ++it;
      continue;
    }
    if (REXCVAR_GET(bf_bot_chat)) GmHostChat(it->who.name, it->who.team, it->who.squad, it->channel, it->text);
    it = g_pending.erase(it);
  }
}

}  // namespace botchat

// A player's chat line as the host relayed it (gm_host.cpp, its own thread): answered at the bots' next tick.
void BotsHeardChat(const std::string& name, int team, int squad, int channel, const std::string& text) {
  if (!REXCVAR_GET(bf_bot_chat)) return;
  std::lock_guard lock(botchat::g_mutex);
  if (botchat::g_heard.size() < 16) botchat::g_heard.push_back({{name, team, squad, false, false, ""}, channel, text});
}

}  // namespace bf1943
