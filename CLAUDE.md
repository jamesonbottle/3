# Working in this repo

- DDR5 RAM tester: C++20, AVX2, CMake. Engine in `src/` (static library), front-ends `src/main_cli.cpp` / `src/main_gui.cpp`, checks in `tools/`.
- A long-running modernization is in progress and is meant to be done in small steps across many sessions. **Read `MODERNIZE.md` first**
  (the "How to resume" section), work on branch `modernize`, do only the next open item, and keep the gate green (`tools/selftest.sh`).
- Do not change behaviour: the self-test greps exact log text and counts.
