// Voice and text chat (comms.cpp, comms_ui.cpp; server/gmhost.py relays; docs/COMMS-PLAN.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf1943::comms {

enum Channel : uint8_t { kAll = 0, kTeam = 1, kSquad = 2 };

// From the game's hooks.
void NoteCamera(const float pos[3], const float right[3], const float back[3]);  // fov.cpp: the rendered camera
void NoteInWorld(bool in_world);                                                 // auto_join.cpp: on foot or seated
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
std::vector<std::string> Speaking();  // the players heard now
bool Transmitting();                  // the local player's voice is going out
void SetPushToTalk(bool down);
void SetTyping(bool typing);
bool Typing();          // the chat box is open: the game gets no input
bool TypingOrJustClosed();  // and for 0.4 s after, so the Enter or Esc that closed it is not the game's
bool Connected();       // the server's relay answered
double Now();

}  // namespace bf1943::comms
