// Diagnostics for players' logs: Settings > Diagnostics (game_menu.cpp DiagnosticsRows). Each area adds detail to
// bf1943.log for issue reports; all are on by default (TekRantGaming, 26 Nov 2026), and the player can turn each off.
// Each is cheap: counts kept where things happen, summary lines every few seconds, and the log file written and
// anonymized on a thread of its own (the SDK's SetLogFileFilter), so the game's threads never wait for the disk.
//
//   Audio      the SDK's audio_diagnostics (rex/audio/diagnostics.h): underruns, starved or unreadable XMA voices,
//              the game's mixer late, every 5 s when one happened, an all-clear line every minute
//   Rendering  frame statistics every 5 s (frame_stats.cpp), the renderer's slow-frame lines from 50 ms, the game's
//              own frames over 100 ms, the graphics settings at the start
//   Network    in a match, every 10 s: packets and bytes from each server, the longest gap between its packets, gaps
//              over 150 ms, the ping; each gap over 400 ms as it ends (NoteReceive, socket_overlapped.cpp)
//   Input      the controls' settings and bindings at the start (controllers' connections are always logged)
//   System     the system at the start; every minute the process's and the PC's memory, the game's own memory, the
//              CPU, the game's file reads, power and the CPU's clock; as it happens the power source, a low clock
//              while playing, memory nearly full, the window's focus (diagnostics_health.cpp)
//   Network    also every online request (the backend's, http.cpp's): failed or slower than 2 s each, a count a minute
//
// The log is anonymized as it is written, whatever is on (TekRantGaming, 26 Nov 2026): the report's privacy rules
// (report.cpp: user and PC names, home folders, keys, the recovery code, the install id, mail addresses) and more:
// the soldier name is [you], other players' names [player N], addresses [ip N] (numbered per run, so lines about one
// player or server still match up), and chat text is left out. A dedicated server's log is left as it is (its
// admins moderate from it).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace bf1943::diagnostics {

enum class Area : int { kAudio, kRender, kNetwork, kInput, kSystem, kBlackBox, kCount };

// How much each area logs (Settings > Diagnostics: each area's row, 27 Nov 2026: "make it modular so we can increase
// or decrease the level of verbosity"). Every feature asks for the level it needs (Want); its per-minute line limits
// scale with the level (Scaled). The settings file's value: "0" off, "quiet", "1" or "normal" (the default), "detailed",
// "verbose".
//   Quiet     summaries only: the scene lines, the minute's health line, the run's summary, the black box's worst
//             frames with their causes
//   Normal    as 27 Nov 2026's first round: the above, each event as it happens (gaps, slow requests, rubber banding,
//             ghosts, devices, settings), the black box's line a second
//   Detailed  more event lines a minute (4x), the renderer's lines from lower thresholds, the full time accounting of
//             each slow frame (every part of the renderer's thread and of our own hooks)
//   Verbose   everything the logs can say (20x the lines), the black box's last 2 s frame by frame with its accounting
enum class Level : int { kOff = 0, kQuiet = 1, kNormal = 2, kDetailed = 3, kVerbose = 4 };

// The settings file's key of an area ("diag_audio").
const char* Key(Area a);
const char* Name(Area a);  // "audio"
Level LevelOf(Area a);
const char* LevelName(Level l);  // "normal"
// The area is on at `at_least` or more.
bool Want(Area a, Level at_least);
// A per-minute line limit for the area's level: 0 off or quiet, `normal` at Normal, 4x Detailed, 20x Verbose.
int Scaled(Area a, int normal);
// The log file's size: "auto" (5 MB, 25 MB with an area on), "small" (5 MB) or "large" (200 MB). At the next start.
constexpr const char* kLogSizeKey = "diag_log_size";
// How much the areas that are on write (Settings > Diagnostics > Logging verbosity): "normal" (the default and the
// lowest a player can choose), "detailed", "verbose". An area's own value overrides it when it names a level
// ("quiet", "detailed", "verbose": dev and older settings files).
constexpr const char* kVerbosityKey = "diag_verbosity";

// The areas as this run started (they apply at the next start when changed).
bool On(Area a);
bool AnyOn();

// Before the log opens (Bf1943App::OnConfigurePaths): the player's choices from bf1943-settings.txt, the runtime
// settings they turn on, the log's size and its anonymizing filter. Not for a dedicated server.
void ApplyAtStart(const std::filesystem::path& user_dir);
// A Diagnostics row changed in the game (game_menu.cpp): the areas and Logging verbosity read again and applied at
// once, the parts an area turns on started (27 Nov 2026: "it shouldn't need a restart"). The log's size stays as the
// start set it.
void ApplyNow();
void StartParts();  // the parts with threads of their own, for the areas on (once each)
// After the log opens: the header (what is on, the log's size); the network monitor.
void AfterLogOpened();
// The game launched, the player's saved settings applied (ApplyStartupDisplaySettings): the settings not at their
// defaults, per area.
void AtGameStart();

// The log is anonymized (a player's game).
bool Anonymized();
// The soldier name (it changes at the name page): shown as [you].
void NoteOwnName(const std::string& name);
// Another player's name, as the game learned it (the host's player list, a chat line): [player N] in the log.
void NotePlayerName(const std::string& name);
// A line as the log file gets it (the SDK's file filter; never logs).
std::string AnonymizeLine(const std::string& line);

// For the report's system table: "audio, rendering, network, input", or "off".
std::string Summary();

// A packet the game received (socket_overlapped.cpp ReceiveNow): address and port in network order.
void NoteReceive(uint32_t ip, uint16_t port, int bytes);

// Where the player is (the user, 27 Nov 2026: "track what screen they're on so we can differentiate loading vs menus
// vs gameplay"): the menus' NotePresence says it 4 times a second. A change is logged ("BF scene: now playing (Iwo
// Jima)"), the scene that ended with what happened in it (its time, frames over 50 ms, the longest frame, audio
// underruns, network gaps), and the summary lines carry it ("BF FPS [playing] ..."). A scene must hold half a second
// (the deploy screen a second: getting in or out of a vehicle passes through it) before it counts.
enum class Scene : int { kStarting, kMenus, kLoading, kDeploy, kPlaying, kPaused, kTutorial, kRoundEnd, kCount };
void NoteScene(Scene scene, const std::string& map);
const char* SceneName();      // "playing"; "" before the first
std::string SceneTag();       // " [playing]", or "" before the first
void NoteFrameTime(float ms);  // frame_stats.cpp: each game frame (VdSwap to VdSwap)

// System logging (diagnostics_health.cpp).
void StartHealth();                                     // AfterLogOpened: the health thread
void LogSystem(const std::filesystem::path& user_dir);  // AtGameStart: the system line
void NoteFileRead(uint64_t bytes, double ms, uint32_t handle = 0);  // dev_io_stats.cpp: each NtReadFile of the game
void NoteWindow(const char* what);                      // bf1943_app.h: minimised, restored
// The window's focus (bf1943_app.h): logged when it comes back, with how long it was away (alt-tab flickers of a
// fraction of a second are left out); away over 10 s is said then (the health thread), in case nothing follows.
void NoteFocus(bool focused);
int64_t TakeRecentSlowFrames();  // frames over 33 ms since the last call (the health thread's low clock check)

// An online request (Network logging): the backend's (server_list.h RegistryRequest: `what` its op) or an HTTP one
// (http.cpp: the address without its query). A failed or slow (over 2 s) one is logged as it happens (at most 20 a
// minute), and a count a minute while there were any.
void NoteRequest(const char* kind, const std::string& what, bool ok, double ms, const char* failure = nullptr);
// The round's end (auto_join.cpp: the game's WinningTeamIsSet) until the next level loads (playerRoleReady, or a new
// join): the end of round screen.
void NoteRoundOver(bool over);
bool RoundOver();
int SceneIndex();    // the Scene now, -1 before the first
bool SceneIsPlay();  // playing, the deploy screen or the tutorial: where a slow frame is the player's problem

// ---- The black box (diagnostics_blackbox.cpp; on with any area): the last 30 s of every game frame and every
// renderer frame (native_x_renderer.h FrameDiagnostics) in memory, written to the log on a trigger: a game frame over
// 100 ms in play (at most one every 2 minutes, 10 a run), the player's mark key, the hang watchdog, a crash.
enum class Trigger { kSlowFrame, kMark, kHang, kCrash };
void StartBlackBox();  // AfterLogOpened
bool BlackBoxOn();
void BlackBoxGameFrame(float ms);  // frame_stats.cpp: each game frame (VdSwap to VdSwap), on the game's thread
// `now`: written on this thread at once (the hang watchdog, a crash: a crash's never waits for the frames' locks);
// otherwise by the black box's own thread a moment later, with what followed.
void BlackBoxDump(Trigger why, const std::string& detail, bool now);
int BlackBoxDumps();
// The player's mark (keybind_mark_problem, F10 by default: Settings > Controls > Bindings; comms_ui.cpp polls it):
// "BF mark: the player marked a problem" and a black box dump. At most one in 3 s, 30 a run.
void MarkProblem();
int Marks();
int64_t LastMarkTime();  // seconds since 1970, 0 none
int64_t LastMarkUs();    // steady clock, for the notice on screen
const char* MarkKeyName();  // the key bound now ("F10"), for the lines
// Counts for the black box's frames (any thread).
void CountRead(uint64_t bytes, double ms);
void CountPacketIn(int64_t gap_ms);  // gap_ms: a gap from a steady stream that ended with this packet, else 0
void CountPacketOut();
void CountGameThreadWait(double ms);
bool OnGameThread();  // the game's main thread ("Main XThread"): its waits are the black box's and the minute's
// EA's mixer stopped (or would have stopped, bf_ea_cpu_cull off) sounds for CPU load (ea_voice_cull.cpp).
void NoteVoiceCulled();
int64_t VoicesCulled();
// EA's mixer's control step, each one's time (ea_xma_kicks.cpp: once a 256-sample tick, on EA's mixer thread).
void NoteEaStep(int64_t ns);
// A game frame's audio thread times (the black box): the scene's longest, for its line.
void NoteAudioTimes(float ea_max_ms, float callback_max_ms, float xma_ms);
// The process's threads sampled 4 times a second (diagnostics_health.cpp, off the game's threads): the busiest in
// the `span_us` around `at_us`, for a black box dump; "" when nothing was sampled.
std::string BusyThreadsAround(int64_t at_us, int64_t span_us);
// The game frame thread's code sampled (diagnostics_profiler.cpp, Detailed and up): its top functions between two
// times ("sub_822A9978 40%, ..."), "" when not sampling.
void StartProfiler();
void NoteFrameThread();  // the game frame thread (VdSwap's) names itself once
std::string TopFunctions(int64_t from_us, int64_t to_us, int n);

// A guest wait (KeWaitForSingleObject and its kin, dev_io_stats.cpp, every build): on the game's main thread, its time to
// the black box's frame and, from 1 ms, to the minute's top waits by caller (the health line).
void NoteWait(uint32_t caller, double ms);
// A packet the game sent (socket_overlapped.cpp): address and port in network order; our own send gaps in a match.
void NoteSend(uint32_t ip, uint16_t port, int bytes);

// Our own work on the game's frame thread, by name (frame_stats.cpp's VdSwap, auto_join.cpp's pad hook): with the
// main thread's waits the black box says how much of a game frame was ours and how much the game's own code.
enum class Ours : int { kFrameLimit, kSdkSwap, kLook, kTickBlend, kWatchdog, kGameOptions, kDevTools, kFrameStats,
                        kPadHook, kCount };
const char* OursName(Ours what);
void AddOurs(Ours what, double ms);
class OursTimer {
 public:
  explicit OursTimer(Ours what);
  ~OursTimer();

 private:
  Ours what_;
  int64_t start_ns_;
};

// The window is closing (Bf1943App::OnClosingWindow: a quit or the close button; the process ends right after): the
// scene's rollup, the run's summary.
void AtExit();

// ---- Added 27 Nov 2026, the second round (diagnostics_extra.cpp unless said)

// The build: version (what servers are told), commit and local changes (as of CMake's configure), dev or release,
// the SDK's version; the log's first lines.
std::string BuildLine();
// SDL's events (an event watch, from AtGameStart): audio devices added and removed, controllers connected and
// disconnected while in a match, key and mouse presses for the input timing.
void StartEventWatch();
// The game read its input (native_input.cpp, the input mapping once a game tick): presses since are timed to it.
void NoteInputRead();
// The game's display pacing (game_menu.cpp ApplyFrameRate): the screen's refresh, the frame rate cap (0 none), V-Sync.
void NoteDisplay(double refresh_hz, double cap_fps, bool vsync);
// A setting changed while the game runs (game_menu.cpp SetCvar).
void NoteSettingChanged(const std::string& name, const std::string& value);
// A file the game opened (shader_prep.cpp): sound and music streams are told apart by their handle.
void NoteOpenedHandle(uint32_t handle, const std::string& path);
bool IsSoundHandle(uint32_t handle);
// A log line at the warning or error level, as the log writer gets it (AnonymizeLine): counted by kind.
void CountWarning(const std::string& message);
// The scene's parts kept in diagnostics_extra.cpp: taken at a scene's end ("; input: ...; ghosts: ..."), and the
// run's summary lines at exit.
std::string TakeExtraRollup(int scene, double secs);
void LogRunSummary();
// The renderer's frame record (the black box's callback): late presents, the game waiting for the renderer.
void NoteRenderFrame(float interval_ms, float game_waited_ms, uint16_t frames_behind);
// Each game frame (the black box): the view's jump on foot (rubber banding), the last slow frame's time.
void NoteMovementFrame(int64_t now_us, float frame_ms);
int64_t LastSlowFrameUs();
// A black box trigger's cause, for the run's summary (diagnostics_blackbox.cpp WriteDump).
void NoteTriggerCause(const char* why, float ms, const std::string& cause);
// The memory at a round's end (diagnostics_health.cpp): the process and the game's memory, against the first round.
void LogRoundMemory(int round);
void NoteSlowStream(double ms);         // a read of a sound or music file over 50 ms (diagnostics_health.cpp)
void NoteCulledNearSlow();              // EA's mixer looked overloaded within 0.2 s of a slow frame
void NoteDeployToAlive(double secs);    // the deploy screen until playing (diagnostics.cpp NoteScene)
void NoteJoin();                        // a join began (the scene went to loading from the menus)

}  // namespace bf1943::diagnostics
