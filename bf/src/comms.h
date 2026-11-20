// Voice and text chat (comms.cpp, comms_ui.cpp; server/gmhost.py relays; docs/COMMS-PLAN.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf1943::comms {

enum Channel : uint8_t { kAll = 0, kTeam = 1, kSquad = 2 };

// From the game's hooks.
void NoteCamera(const float pos[3], const float right[3], const float back[3]);  // fov.cpp: the rendered camera
// The rendered camera as last noted (the pause menu's map: where the player is and looks); false before the first.
bool Camera(float pos[3], float right[3], float back[3]);
void NoteInWorld(bool in_world);                                                 // auto_join.cpp: on foot or seated
bool InWorld();  // the local soldier is in the level (on foot or seated), as auto_join.cpp last noted it
void NoteJoined(const std::string& address);                                     // game_menu.cpp: the server joined
void NoteTeam(int team, int squad);  // the local player's team (1, 2; 0 not known) and squad (0 none)
void ReadTeam(const uint8_t* base, bool level_loaded);  // auto_join.cpp's pad read: NoteTeam from the game's player
int Team();   // 0 while not known
int Squad();  // 0 none, else the game's squad number + 1

// For the overlay (comms_ui.cpp).
struct ChatLine {
  double time = 0;  // seconds (steady clock)
  Channel channel = kAll;
  std::string name, text;
  bool own = false;
  bool notice = false;  // ours, not a player's (a refusal, the chat being unavailable)
};
std::vector<ChatLine> RecentChat(size_t max);
void SendChat(Channel channel, const std::string& text);
// A server broadcast (the host's message to every player: a game update, a restart, an admin's own words), for the
// box at the top of the screen (comms_ui.cpp). False when none is up. `kind`: 0 an admin's message, 1 a game
// update, 2 a server restart; `age` seconds since it arrived, `seconds` how long it stays.
bool CurrentBroadcast(std::string* text, int* kind, double* age, double* seconds);
std::vector<std::string> Speaking();  // the players heard now
// Moderation (server/gmhost.py C_KICKED, docs/MODERATION.md): the server took this player off it (a kick, a ban of
// that server or of the Reborn network). Its title and message once, then false until it happens again.
bool TakeRemoved(std::string* title, std::string* message);
// Per player voice mute (TekRantGaming, 17 Nov 2026: "allows users to mute individual users so that their comms dont
// come across on voice chat anymore"). On this game only: a muted player's voice is dropped as it arrives, before it
// is decoded or mixed; nothing is sent, the server and the muted player are not told. Keyed on the player's name
// (the account's, without a clan tag: "[TAG]Name" and "Name" are the same player) and kept in the user folder's
// muted-players.txt, so a mute lasts across matches, servers and restarts. Never the player's own name.
void SetUserDir(const std::filesystem::path& user_dir);  // game_menu.cpp at the start: where muted-players.txt is
std::string PlainName(const std::string& name);  // "[TAG]Name" as "Name" (a name as the game's own lists show it)
bool Muted(const std::string& name);
void SetMuted(const std::string& name, bool muted);  // saved at once; the player's own name is refused
bool ToggleMuted(const std::string& name);           // the new state
bool IsMe(const std::string& name);                  // the name played under (any clan tag ignored)
bool Talking(const std::string& name);               // heard now (in Speaking()); a muted player never is
bool Transmitting();                  // the local player's voice is going out
void SetPushToTalk(bool down);
void SetTyping(bool typing);
bool Typing();          // the chat box is open: the game gets no input
bool TypingOrJustClosed();  // and for 0.4 s after, so the Enter or Esc that closed it is not the game's
bool Connected();       // the server's relay answered
double Now();

}  // namespace bf1943::comms
