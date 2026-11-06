# The official servers (Hetzner, Windows Server)

Step by step, for TekRantGaming's Hetzner machine: the main backend everyone logs in to, and the official servers.

## Once

1. **Python**: install [Python 3.12](https://www.python.org/downloads/windows/) on the server, with "Add python.exe
   to PATH" ticked.
2. **Ports**: in the Hetzner Cloud Console, the server's firewall: allow inbound TCP 18241, 18246, 18250 and UDP
   18300. If the server has no Hetzner firewall attached, nothing to do there.
3. **The package**: copy `BF1943-Reborn-Server-<version>-windows.zip` and your Battlefield 1943 package file to the
   server (Remote Desktop: copy and paste works), and unzip it, for example to `C:\BF1943`.
4. Run **Open firewall ports.bat** (it asks for administrator rights): Windows Firewall lets the players in.

## Set up the official server

5. Open **BF1943 Server.bat**:
   - **Install the game from your package**: select the package file.
   - **Server name**: for example "Battlefield 1943 Reborn | Official".
   - **Map rotation**: Wake Island, Guadalcanal, Iwo Jima, Coral Sea.
   - **Main backend**: **This PC runs it (the official servers)**.
   - **Public address**: press **Detect**: it shows the server's public IPv4 address (Hetzner gives it to the machine
     itself). Untick **Open the ports on my router** (there is no router).
6. **Start servers**. The status line says Running.

## Point the game at it

7. Players' games find the main backend through one setting, `bf_online_host`, in `bin\bf1943.toml`. Set it to the
   server's public address (or a domain name pointing at it):

   ```toml
   bf_online_host = "203.0.113.10"
   ```

   The release packages are built with it already set (`tools\make_packages.ps1 -OnlineHost <address>`).
8. Start the game on your own PC: the server shows in the server browser with its name, map and players.

## Starting by itself

To start the official server when the machine restarts: Task Scheduler, a task "At system startup" (run whether
the user is logged on or not) running `C:\BF1943\Start servers (no window).bat`.

## Updating

Stop servers, unzip the new package over the old folder (your `game` folder and `server.json` stay), Start servers.
