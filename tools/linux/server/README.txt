Battlefield 1943 Reborn Server @VERSION@ for Linux (github.com/TekRantGaming/Battlefield-1943-Reborn)

Run your own Battlefield 1943 servers. You need your own Battlefield 1943 (Xbox 360) package, installed once.

  ./bf1943-server                   starts the server manager and opens its page (http://127.0.0.1:18280/)
  ./bf1943-server run --no-browser  on a machine without a desktop; reach the page through an SSH tunnel:
                                    ssh -L 18280:127.0.0.1:18280 you@server, then open http://127.0.0.1:18280/
                                    (with no password set, sign in there with what ./bf1943-server code prints)
  ./bf1943-server install <file>    the game files from your package (or use Game files on the page)
  ./bf1943-server status            what runs

On the page: your servers (name, maps, players, port), the region players see, LAN only mode, messages to the
players, updates. Players reach a server on its UDP port (18300 and on): open it in your firewall and router.

On a VPS with no desktop, run it as a service that starts at boot and comes back if it stops: the file
bf1943-reborn-server.service in this folder says how (a user of its own, its paths, systemctl enable --now). A server
that crashes is started again within seconds; its stack is kept in <data>/logs/server<n>/game.out.

This archive needs python3 (3.9 or newer) and a 64 bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Linux Mint 21,
Debian 12 and newer). The AppImage carries its own Python and needs the same glibc.
