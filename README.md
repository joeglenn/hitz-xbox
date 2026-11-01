# hitz-xbox

Static recompilation of NHL Hitz Pro (original Xbox) to Windows x64 with xboxrecomp.

- hitz_pro/: the project (hand overrides, D3D11 GPU back end, profiler, build scripts)
- xboxrecomp-hitz.patch: toolkit changes against the xboxrecomp commit in hitz_pro/toolkit/BASE_COMMIT
- tools/: VM build/run, profiling and GPU passthrough helpers
- NOTES.md / CLAUDE.md: progress log and phase plan

No game data is included. You need your own copy of the game; generated code (hitz_pro/src/recomp/gen) is rebuilt from it (see hitz_pro/toolkit/README).
