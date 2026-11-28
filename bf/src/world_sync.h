// The match's world: its clock and weather, kept by the match's host and followed by every player's game
// (world_sync.cpp; the host's side is gm_host.cpp's and server/gmhost.py's "BF43WRLD").
#pragma once

#include <string>

namespace bf1943::world {

// What a match's host keeps, and what an admin sets.
struct Settings {
  bool day_night = false;      // the light, the shadows and the sky follow the clock
  float hour = 16.0f;          // the time of day, 0 to 24
  float day_minutes = 0.0f;    // minutes a whole day and night take; 0: the time stands still
  float cloud_cover = 0.35f;   // 0 to 1
  float cloud_rate = 0.33f;    // how fast the clouds form and change, 0 to 1
  float weather = 0.0f;        // 0 fair to 1 a storm: overcast, mist, far rain shafts, lightning
  float fog = 0.0f;            // the distance fog, 0 to 1
  float ground_fog = 0.0f;     // the ground fog, 0 to 1
  float ground_fog_height = 0.5f;   // the surface fog's depth, metres (21 Nov 2026)
  float ground_fog_sparse = 0.7f;   // how sparse its patches are, 0 everywhere to 1 a few
  float wind = 0.3f;           // 0 calm to 1 a gale: how far the rain leans
  float cloud_height = 450.0f; // the cloud base over the sea, metres
  bool dynamic_storm = false;  // the host builds a storm over the match and eases it (it sets the weather)
  int storm_type = 0;          // D224: 0 a thunderstorm, 1 a hurricane (NativeXRenderer's storm.h StormType)
};

// The server joined ("ip:port", its host's port; "": none). From the menu's join.
void NoteJoined(const std::string& address);
// The key of the private match this game started (private_match.cpp makes it): shown to the host, it makes this
// game an admin there. "": none.
void SetAdminKey(const std::string& key);

struct Status {
  bool answered = false;  // the joined match's host keeps a world and answered lately
  bool admin = false;     // and lets this game change it
  Settings now;           // as it is now
  // The match's rules (match_rules.cpp), from a host that tells them (19 Nov 2026):
  int tickets = 0;          // each team's tickets at a round's start; 0: the level's own
  int time_limit = 0;       // minutes a round lasts; 0: none
  float time_left = -1;     // seconds left in the round when answered; -1: no limit or no round running
  double answered_at = 0;   // when (world::Now()), for the time left since
  // A Night Ops server (24 Nov 2026; an older host never says): its schedule drives the time, clouds, weather and fog,
  // and `held`: an admin's settings hold some of them for the rest of the round.
  bool night_ops = false;
  bool held = false;
  // 26 Nov 2026: the event is Daybreak (the answer's flag 16; night_ops is then set too: an event server).
  bool daybreak = false;
  // D254 (28 Nov 2026): the host says whether the match's weather system is on (flag 64, `weather_told`), and it is
  // (flag 32): on, the renderer's sky, volumetric clouds, rain and storm; off, the game's own sky, graded for the hour.
  // An older host never says; the game then keeps its own rule.
  bool weather_told = false;
  bool weather_system = false;
  const char* EventName() const { return daybreak ? "Daybreak" : "Night Ops"; }
};
Status Current();
// Which settings an admin moved (Request's `touched`; the host's World.TOUCHED in server/gmhost.py).
enum Touched : unsigned {
  kTouchDayNight = 1, kTouchHour = 2, kTouchDayMinutes = 4, kTouchCover = 8, kTouchRate = 16, kTouchWeather = 32,
  kTouchFog = 64, kTouchGroundFog = 128, kTouchWind = 256, kTouchCloudHeight = 512, kTouchStorm = 1024,
  kTouchStormType = 2048, kTouchFogHeight = 4096, kTouchFogSparse = 8192, kTouchAll = 0x3FFF,
};
// An admin's change: sent to the host, which every player's game then follows. `touched`: the settings moved (a Night
// Ops server takes only those, and holds them for the rest of the round).
void Request(const Settings& settings, unsigned touched = kTouchAll);
// Night Ops: an admin's "Back to Night Ops": everything set this round back on the server's schedule.
void BackToNightOps();

// Seconds on world_sync's clock (Status::answered_at's).
double Now();

// Players' pings (TekRantGaming, 20 Nov 2026: "your active ping reported in the scoreboard ... how Call of Duty Black
// Ops 1 would show everyone's connection and ping, and World at War would even show you the number"). This game times
// its own round trip to the joined server's host (BF43PING, answered at once by every host), tells the host in its
// world ask, and a host that keeps them answers with every player's (world_sync.cpp). In milliseconds; -1 unknown (no
// match, an older host, a player whose game is older, or a bot).
int OwnPing();
int PingOf(const std::string& name);  // "[TAG]Name" or "Name"; the player's own name gives OwnPing()
// A bot (its scoreboard row says BOT, never a number): a name the joined server's host does not list among its joined
// players (bots wear no tag since 20 Nov 2026), or a "[BOT]" name from a server before that. False when the host
// keeps no such list (an older server).
bool IsBot(const std::string& name);

// The in-game admin panel (F9) is open: the mouse and the keys are its own.
bool AdminPanelOpen();

}  // namespace bf1943::world
