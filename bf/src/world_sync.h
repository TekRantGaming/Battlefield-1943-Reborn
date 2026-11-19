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
  float wind = 0.3f;           // 0 calm to 1 a gale: how far the rain leans
  float cloud_height = 450.0f; // the cloud base over the sea, metres
  bool dynamic_storm = false;  // the host builds a storm over the match and eases it (it sets the weather)
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
};
Status Current();
// An admin's change: sent to the host, which every player's game then follows.
void Request(const Settings& settings);

// Seconds on world_sync's clock (Status::answered_at's).
double Now();

// The in-game admin panel (F9) is open: the mouse and the keys are its own.
bool AdminPanelOpen();

}  // namespace bf1943::world
