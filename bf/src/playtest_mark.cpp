// Play test marks: with BF_MARK_FILE=<file> (the test copy's watched start), each press of R on the keyboard in the
// game window appends "<local time>  mark <n>" to that file and logs "BF mark <n>", so a moment TekRantGaming saw (a flicker,
// a stutter) can be found in the logs; tools\playtest_watch.ps1 takes a screenshot when the file grows. TekRantGaming: "make
// the key press required for the mark to be the letter R". The key still reaches the game.
// (A watcher polling the keyboard from its own process saw none of his presses in the first watched test.)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace {

class MarkListener : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (e.virtual_key() != rex::ui::VirtualKey::kR || e.prev_state()) return;  // a new press, not a repeat
    static std::mutex mutex;
    static int count = 0;
    std::lock_guard lock(mutex);
    ++count;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, ms);
    REXLOG_INFO("BF mark {}", count);
    if (FILE* f = std::fopen(file_.c_str(), "a")) {
      std::fprintf(f, "%s  mark %d\n", stamp, count);
      std::fclose(f);
    }
  }
  std::string file_;
};
MarkListener g_mark;

}  // namespace

namespace bf1943 {
void InstallPlaytestMark(rex::ui::Window* window) {
  const char* file = std::getenv("BF_MARK_FILE");
  if (!window || !file || !*file) return;
  g_mark.file_ = file;
  window->AddInputListener(&g_mark, 999);
  REXLOG_INFO("BF play test marks: R writes to {}", file);
}
}  // namespace bf1943
