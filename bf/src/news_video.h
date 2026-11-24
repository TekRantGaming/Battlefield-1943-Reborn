// The news window's video: YouTube's own embedded player (never a downloaded or extracted stream: YouTube's terms),
// in a WebView2 view over the game's window on Windows (bf/third_party/webview2: Microsoft's signed loader, loaded at
// run time from the game's folder; the runtime itself is Windows 10's and 11's own). The view is a child window over
// the menu at the lead story's place; nothing is drawn over the player.
//
// Autoplay with sound: an embedded browser follows Chromium's rule (sound only after a click in the page), so the
// view starts with --autoplay-policy=no-user-gesture-required, which lets the player start with its sound.
// The player page is ours (cache/news/web/player.html, served under a name of its own,
// https://news.bf1943reborn.example/, so YouTube gets the page's address it asks embeds for; a page without one gets
// YouTube's error 153). Links out of it (YouTube's logo, a title) open in the player's browser instead.
//
// Linux, the Steam Deck, and Windows without the loader or the runtime: Available() is false and the news window
// shows the video's thumbnail; A or a click opens the video on YouTube in the browser.
#pragma once

#include <filesystem>
#include <string>

namespace bf1943::news {

// True while an embedded player can be tried (Windows, the loader and a runtime there, no failure yet this run).
bool VideoAvailable();
// Shows the player for `youtube_id` at x, y, w, h (the menu's coordinates, on a display_w x display_h screen) over
// `parent` (the game's HWND), creating the view the first time (it appears a moment later). Called every frame the
// video shows: a move or a new id is applied. `muted`: the player starts muted.
void VideoShow(void* parent, const std::filesystem::path& cache_dir, const std::string& youtube_id, float x, float y,
               float w, float h, float display_w, float display_h, bool muted);
// Hides and stops the player (the window closed, or no video item in the lead).
void VideoHide();
void VideoSetMuted(bool muted);
// True while the player is showing (created and visible).
bool VideoShowing();
// True while the player shows and its video has not ended, paused or failed (the menu music waits meanwhile).
bool VideoPlaying();
// True once the player said it failed (no internet or YouTube out of reach, or YouTube refused the video), until
// the next video or VideoHide.
bool VideoFailed();
// Esc pressed while the player had the keyboard (the view takes it on a click): the window's Close. Taken once.
bool VideoTakeEscape();

// The news window's storm (TekRantGaming, 23 Nov 2026, the Storm look for 0.4.8: "when it first shows rain wind and the
// lightning sounds ... with a slow fade out until mute", so the trailer's sound is not covered): the weather's own
// recordings (weather_audio.cpp, data\sounds\weather) on a sound stream of their own, as the menus' sounds are. Every
// frame of our menus with `open` true while the window shows, at the master, effects and weather volumes (the
// Weather volume row). An opening starts the rain and the wind at once, holds them a few seconds and fades them out until they are
// silent; a close fades them out in half a second.
void StormSound(bool open);
// A strike's thunder (0 a close strike, 1 a distant one, 2 lightning in the clouds), as its flash shows: each comes
// after its own time on the way, under the same fade.
void StormThunder(int kind);
// Readies the recordings (decoded on threads of their own), so the window's first second already has them.
void StormPrepare();

}  // namespace bf1943::news
