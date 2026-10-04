Soundee endpoint helper corresponding source
============================================
This folder contains the complete corresponding source for SoundeeEndpointSetup.
Equalizer APO files retain their GPLv2-or-later copyright notices and License.txt;
the UncaughtExceptions.h dependency also retains its Apache 2.0 notice/license.

Build with Visual Studio 2022 C++ tools and CMake from this folder:
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

The helper accepts only: SoundeeEndpointSetup.exe --attach {endpoint-guid}
It must run elevated. It rejects disconnected, disabled, and capture endpoints.
Exit 0: already attached; 10: newly attached, Windows audio restart may be needed.
Exit 2: invalid argument; 3: not elevated; 4: unavailable device/backend;
5: enhancements disabled; 6: backend registration missing; 7: setup failed;
9: setup failed and the original effects could not be restored.
Use Equalizer APO Device Selector to troubleshoot or remove an attachment.
