# Reading a player's crash dump without WinDbg

A player's `crash-<time>.dmp` (our crash reporter's minidump, `<user>\crashes\`) can be read on this PC with Python
alone; no debugger is installed here. Written by a side agent on 12 Nov 2026 for the dump
`crash-20261112-233309.dmp` (a shutdown double free in the SDK, not an overlay: STATE.md, 12 Nov 22:20).

Treat a dump as an untrusted download: copy it into an empty folder of its own and run these scripts from
elsewhere with `python -I`, passing the dump's path. Never run anything from inside it.

Needs (`pip install --user minidump capstone pefile`):

- `parse_dump.py <dump>`: header, system info, the module list with versions, timestamps and PDB ids, the
  exception, the threads, the crashing thread's context, and a scan of its stack for return addresses inside
  loaded modules (a plain scan, since there is no unwinder).
- `grep_dump.py <dump>`: strings of interest in the raw bytes: the reporter's own text (it is in the dump's
  memory even when only the .dmp was sent), overlay and Vulkan layer names, our modules.
- `inspect_dump2.py <dump>`: the exception stream's own context, a deep stack scan from it, the memory the
  exception parameters point at (a heap failure's record), ntdll's segments, every layer and overlay string.
- `common_dump.py`: the module list loader the others share.
- `match_syms.py rva...`: names a released bf1943.exe's RVAs with a later build's PDB by matching the function's
  code bytes (the release PDB is not kept); its paths at the top are the 0.3.0 exe and the current
  bf-relwithdebinfo build and llvm-symbolizer: edit them for another pair.

Microsoft's frames (ntdll, ucrtbase) are named with their PDBs from the Microsoft symbol server by the PDB id
the module list gives; rexruntime.dll ships without a PDB, its frames are named from its export table and a
disassembly (capstone). The known overlay and hook DLLs are listed in `bf/src/overlay_hooks.inc`.
