"""Import an editable test fixture into a separate game profile and local leaderboard."""

import argparse
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import tomllib


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "bf/out/build/bf-relwithdebinfo"
USER = BUILD / "test-user"
SDK = ROOT / "sdk/src-bf/out/install/win-amd64/bin/rexglue.exe"
GAME = ROOT / "bf/assets"
FESL_PORT = 18241
RANK_MINUTES = (0, 10, 25, 45, 70, 100, 140, 190, 250, 320, 400, 500, 620, 760, 920,
                1100, 1300, 1550, 1850, 2200, 2600, 3100, 3700)


def read_settings(path):
    if not path.is_file():
        return {}
    rows = (line.partition("=") for line in path.read_text(encoding="utf-8-sig").splitlines())
    return {key.strip(): value.strip() for key, sep, value in rows if sep and not key.lstrip().startswith("#")}


def write_save(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def import_progression(fixture_path):
    if not fixture_path.is_file():
        raise ValueError(f"Missing test fixture: {fixture_path}")
    fixture = read_settings(fixture_path)
    settings_path = USER / "bf1943-settings.txt"
    settings = read_settings(settings_path)
    name = fixture.get("player_name", "Progression QA")
    if not 1 <= len(name) <= 15 or name != name.strip() or any(
            not (char.isascii() and (char.isalnum() or char in " _")) for char in name):
        raise ValueError("player_name must use 1–15 letters, digits, spaces or underscores")
    seconds = int(fixture.get("match_seconds", "0"))
    if not 0 <= seconds <= 2_147_483_647:
        raise ValueError("match_seconds must be between 0 and 2147483647")
    requested_rank = fixture.get("rank", "")
    rank = int(requested_rank) if requested_rank else sum(seconds >= minutes * 60 for minutes in RANK_MINUTES)
    if not 1 <= rank <= 23:
        raise ValueError("rank must be between 1 and 23")

    stats = {}
    score = 0.0
    captures = 0
    for key, value in fixture.items():
        if not key.startswith("stat."):
            continue
        count, points = value.split(",")
        count, points = int(count), float(points)
        if not 0 <= count <= 2_147_483_647 or not math.isfinite(points):
            raise ValueError(f"Invalid count or points for {key}")
        stats[key] = f"{count},{points:.15g}"
        score += points
        if key == "stat.CQ_StringForFlagCapture":
            captures = count
    if not math.isfinite(score) or abs(score) > 2_147_483_647:
        raise ValueError("Total score is outside the supported range")

    metadata = USER / "achievement-metadata"
    metadata.mkdir(parents=True, exist_ok=True)
    xex = str(GAME / "default.xex")
    subprocess.run([str(SDK), "init", "--project-name", "bf1943-test", "--xex-path", xex,
                    "achievements", xex, str(metadata)], cwd=metadata, check=True, stdout=subprocess.DEVNULL)
    definitions = tomllib.loads((metadata / "achievements.toml").read_text(encoding="utf-8"))
    known_ids = {entry["id"] for entry in definitions["achievements"]}
    achievement_path = USER / "achievements/5841097E.toml"
    old_achievements = {}
    if achievement_path.is_file():
        existing = tomllib.loads(achievement_path.read_text(encoding="utf-8")).get("unlocked", {})
        old_achievements = ({int(key): entry.get("filetime", 0) for key, entry in existing.items()}
                            if isinstance(existing, dict) else dict.fromkeys(existing, 0))
    default = fixture.get("achievements", "actual")
    if default not in ("actual", "locked", "unlocked"):
        raise ValueError("achievements must be actual, locked or unlocked")
    overrides = {}
    for key, state in fixture.items():
        if key.startswith("achievement."):
            achievement_id = int(key.removeprefix("achievement."))
            if achievement_id not in known_ids or state not in ("actual", "locked", "unlocked"):
                raise ValueError(f"Invalid achievement override: {key} = {state}")
            overrides[achievement_id] = state
    now = time.time_ns() // 100 + 116444736000000000
    unlocked = {}
    for achievement_id in sorted(known_ids):
        state = overrides.get(achievement_id, default)
        if state == "unlocked" or (state == "actual" and achievement_id in old_achievements):
            unlocked[achievement_id] = old_achievements.get(achievement_id) or now

    settings = {key: value for key, value in settings.items() if not key.startswith("stat.")}
    settings.update(stats, player_name=name, match_seconds=str(seconds), test_progression="1")
    settings.pop("rank", None)
    if requested_rank:
        settings["rank"] = str(rank)
    write_save(settings_path, "".join(f"{key} = {value}\n" for key, value in sorted(settings.items())))
    write_save(achievement_path, "".join(f"[unlocked.{key}]\nfiletime = {value}\n\n"
                                        for key, value in unlocked.items()))
    return dict(op="stats_put", name=name, score=round(score), seconds=seconds,
                captures=captures, rank=rank, achievements=list(unlocked))


def submit(progression, launch):
    leaderboard = USER / "leaderboard.json"
    log_path = USER / "plasma.log"
    error_path = USER / "plasma-startup-errors.log"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        registry_port = probe.getsockname()[1]
    marker = f"server registry on 127.0.0.1:{registry_port}"
    log_path.write_text("", encoding="utf-8")
    backend_args = [
        sys.executable, "-B", str(ROOT / "server/plasma.py"), "--host", "127.0.0.1", "--test-progression",
        "--registry-port", str(registry_port), "--leaderboard", str(leaderboard), "--log", str(log_path),
        "--port", str(FESL_PORT if launch else 0), "--stop-on-stdin-eof",
        *(["--theater-port", "0"] if not launch else []),
    ]
    with error_path.open("wb") as errors:
        server = subprocess.Popen(backend_args, stdin=subprocess.PIPE, stderr=errors,
                                  creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
    try:
        for _ in range(100):
            if server.poll() is not None:
                details = error_path.read_text(encoding="utf-8", errors="replace").strip().splitlines()
                reason = details[-1] if details else f"exit code {server.returncode}"
                raise RuntimeError(f"The local backend exited: {reason}\nDetails: {error_path}")
            if marker in log_path.read_text(encoding="utf-8"):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"The local backend did not become ready. Check {log_path}")
        with socket.create_connection(("127.0.0.1", registry_port), timeout=3) as connection:
            connection.sendall((json.dumps(progression) + "\n").encode())
            with connection.makefile("rb") as replies:
                reply = json.loads(replies.readline())
        if reply != {"op": "stats_put", "ok": True}:
            raise RuntimeError(f"The local leaderboard rejected the profile: {reply}")
        saved = next((row for row in json.loads(leaderboard.read_text(encoding="utf-8"))
                      if row["name"] == progression["name"]), {})
        if any(saved.get(key) != value for key, value in progression.items() if key != "op"):
            raise RuntimeError(f"The local leaderboard did not persist the submitted values: {leaderboard}")
        print(f"Saved progression in {USER}\nSubmitted and verified {progression['name']} in {leaderboard}", flush=True)
        if launch:
            # The registry port serves leaderboards. The guest needs the FESL port separately for sign-in.
            client_env = dict(os.environ, BF_ONLINE_HOST="127.0.0.1", BF_ONLINE_PORT_45410004=str(FESL_PORT),
                              BF_LOOPBACK_ONLY="1")
            subprocess.run([str(BUILD / "bf1943.exe"), f"--game_data_root={GAME}", f"--user_data_root={USER}",
                            "--bf_online_host=127.0.0.1", f"--bf_backend_registry=127.0.0.1:{registry_port}",
                            f"--log_file={USER / 'bf1943.log'}",
                            f"--bf_player_name={progression['name']}", "--bf_share_pipelines=false"],
                           cwd=BUILD, env=client_env, check=True)
    finally:
        server.stdin.close()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.terminate()
            server.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=BUILD / "user/bf1943-test-profile.txt")
    parser.add_argument("--launch", action="store_true", help="start the game with the isolated profile and local backend")
    args = parser.parse_args()
    try:
        submit(import_progression(args.fixture), args.launch)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
