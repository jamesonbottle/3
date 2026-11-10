@echo off
REM HANDOFF [TODO] build.bat (step 5) - call CMake instead of the two cl lines.
REM Keep the contract: run from an x64 Native Tools prompt, outputs build\ddr5ramtest.exe and build\ddr5test_cli.exe (build.yml uploads exactly these).
REM In CMake set RUNTIME_OUTPUT_DIRECTORY(_<CONFIG>) so the exes land in build\ whatever the generator/config. Keep /O2 /std:c++20 /arch:AVX2 /EHsc /utf-8 /DNDEBUG
REM and /SUBSYSTEM:WINDOWS /MANIFEST:NO for the GUI + src\app.rc. Keep the REM line about `python tools\gen_suite.py`.
REM Delete this block when done.
REM Builds ddr5ramtest.exe (GUI) and ddr5test_cli.exe. Run from a "x64 Native Tools Command Prompt for VS".
REM Optional: python tools\gen_suite.py   (only after editing suite.cfg)
setlocal
if not exist build mkdir build
rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl /nologo /O2 /std:c++20 /arch:AVX2 /EHsc /utf-8 /DNDEBUG /Fo:build\ /Fe:build\ddr5ramtest.exe src\main_gui.cpp build\app.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || exit /b 1
cl /nologo /O2 /std:c++20 /arch:AVX2 /EHsc /utf-8 /DNDEBUG /Fo:build\ /Fe:build\ddr5test_cli.exe src\main_cli.cpp || exit /b 1
echo.
echo Built build\ddr5ramtest.exe and build\ddr5test_cli.exe
