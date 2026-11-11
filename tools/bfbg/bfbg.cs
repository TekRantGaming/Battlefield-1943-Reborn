// bfbg: keeps the test runs' Battlefield 1943 windows out of the user's way.
// Copied from King Kong Recompiled's tools/kkbg (same author); only the program name changed.
//
// Watches for windows of bf1943.exe processes whose program lives under one of the
// given folders (the agents' build folders, never the user's own copies). When such a
// window is shown it goes to the bottom of the z-order without being activated; if it
// ever becomes the foreground window, it goes to the bottom and the window that was in
// the foreground before (for example a full-screen Plex) gets the foreground back.
//
// Usage: bfbg.exe <folder> [<folder> ...]   (runs until killed; logs to bfbg.log beside it)

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

static class BfBg {
  delegate void WinEventProc(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time);

  [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr mod, WinEventProc proc, uint pid, uint tid, uint flags);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
  [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG msg);
  [DllImport("user32.dll")] static extern IntPtr SetTimer(IntPtr hwnd, IntPtr id, uint ms, IntPtr proc);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr h, uint flags, StringBuilder name, ref int size);
  delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);

  [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }

  const uint EVENT_SYSTEM_FOREGROUND = 0x0003, EVENT_OBJECT_SHOW = 0x8002;
  const uint WINEVENT_OUTOFCONTEXT = 0, WINEVENT_SKIPOWNPROCESS = 2;
  const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_NOOWNERZORDER = 0x200;
  static readonly IntPtr HWND_BOTTOM = new IntPtr(1);

  static string[] folders;
  static readonly Dictionary<string, bool> isTestByKey = new Dictionary<string, bool>();
  static IntPtr lastUserForeground = IntPtr.Zero;
  static WinEventProc keep;  // kept alive for the hook
  static string logPath;

  static void Log(string s) {
    try { File.AppendAllText(logPath, DateTime.Now.ToString("HH:mm:ss ") + s + Environment.NewLine); } catch { }
  }

  [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out long creation, out long exit, out long kernel, out long user);

  // Keyed by process id AND creation time: Windows reuses process ids, so an id cached for an
  // agent's test game must never match a later process (for example the user's own game).
  static bool IsTestProcess(uint pid) {
    bool v = false;
    IntPtr h = OpenProcess(0x1000, false, pid);  // PROCESS_QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return false;
    long created, exited, kernel, user;
    string key = pid + ":" + (GetProcessTimes(h, out created, out exited, out kernel, out user) ? created : 0);
    if (isTestByKey.TryGetValue(key, out v)) { CloseHandle(h); return v; }
    var sb = new StringBuilder(1024);
    int n = sb.Capacity;
    if (QueryFullProcessImageName(h, 0, sb, ref n)) {
      string path = sb.ToString();
      if (Path.GetFileName(path).Equals("bf1943.exe", StringComparison.OrdinalIgnoreCase))
        foreach (var f in folders)
          if (path.StartsWith(f, StringComparison.OrdinalIgnoreCase)) { v = true; break; }
    }
    CloseHandle(h);
    if (isTestByKey.Count > 4096) isTestByKey.Clear();
    isTestByKey[key] = v;
    return v;
  }

  static bool IsTestWindow(IntPtr hwnd) {
    if (hwnd == IntPtr.Zero) return false;
    uint pid;
    GetWindowThreadProcessId(hwnd, out pid);
    return pid != 0 && IsTestProcess(pid);
  }

  static void Bottom(IntPtr hwnd) {
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }

  static void GiveBackForeground() {
    if (lastUserForeground == IntPtr.Zero || !IsWindow(lastUserForeground)) return;
    // A process may only take the foreground for another window right after input; a
    // released ALT counts and does nothing else.
    keybd_event(0x12, 0, 0, UIntPtr.Zero);
    keybd_event(0x12, 0, 2, UIntPtr.Zero);
    SetForegroundWindow(lastUserForeground);
  }

  static void OnEvent(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time) {
    if (idObject != 0 || idChild != 0 || hwnd == IntPtr.Zero) return;
    IntPtr top = GetAncestor(hwnd, 2);  // GA_ROOT
    if (top != IntPtr.Zero) hwnd = top;
    if (ev == EVENT_OBJECT_SHOW) {
      if (IsTestWindow(hwnd)) { Bottom(hwnd); Log("shown, sent to the back: " + hwnd); }
    } else if (ev == EVENT_SYSTEM_FOREGROUND) {
      if (IsTestWindow(hwnd)) {
        Bottom(hwnd);
        GiveBackForeground();
        Log("took the foreground, gave it back: " + hwnd);
      } else {
        lastUserForeground = hwnd;
      }
    }
  }

  static void Sweep() {
    // In case a window was raised without an event we saw: keep test windows at the back.
    EnumWindows((h, l) => {
      if (IsWindowVisible(h) && IsTestWindow(h)) Bottom(h);
      return true;
    }, IntPtr.Zero);
    if (IsTestWindow(GetForegroundWindow())) GiveBackForeground();
  }

  static int Main(string[] args) {
    if (args.Length == 0) { Console.Error.WriteLine("usage: bfbg <folder> [<folder> ...]"); return 2; }
    folders = args;
    logPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bfbg.log");
    Log("watching bf1943.exe under: " + string.Join("; ", folders));
    IntPtr fg = GetForegroundWindow();
    if (!IsTestWindow(fg)) lastUserForeground = fg;
    keep = OnEvent;
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, IntPtr.Zero, keep, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, IntPtr.Zero, keep, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(IntPtr.Zero, IntPtr.Zero, 1000, IntPtr.Zero);
    MSG msg;
    while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) {
      if (msg.message == 0x0113) Sweep();  // WM_TIMER
      TranslateMessage(ref msg);
      DispatchMessage(ref msg);
    }
    return 0;
  }
}
