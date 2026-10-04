Soundee
-------

A C++ speaker and headphone equalization application. Start with a flat response
or import .swproj calibration, add draggable parametric EQ, monitor stereo output
and clipping, and follow Windows output changes with per-device profiles.
Automatic headroom follows the actual combined EQ response. Add a headroom
offset, an optional stereo limiter, and true-peak metering in the system EQ path.
Undo/redo, A/B snapshots, portable calibration backups and a recording comparison
tool help refine and verify your presets. Soundee can stay in the Windows tray
and start in the background at sign-in.

Application stack: C++20, JUCE 9.0.3, CMake and MSVC. Windows-wide processing
uses Equalizer APO with
device-specific activation, automatic configuration updates, and restoration.
Keep Soundee.exe, SoundeeDSP.dll, SoundeeBackendProbe.exe and
SoundeeEndpointSetup.exe together when moving the portable application.
See docs/user-guide.txt for setup, supported formats and DSP behavior.

Project layout
--------------
assets/profiles/raw/                 Original calibration profiles
assets/profiles/decoded/yamaha-hs8/   Decoded JSON, XML, CSV and graph exports
tools/profiles/                      Profile decoding and plotting utilities
docs/swproj-format.txt               Observed binary format and DSP caveats
docs/user-guide.txt                  Usage and development instructions
src/                                Native importer, DSP, UI and diagnostics
tests/fixtures/                     Public synthetic calibration and baseline
third_party/equalizerapo/            Pinned endpoint setup sources and notices
scripts/                            Build, run and portable-package actions

Build:    pwsh -NoProfile -File scripts/Build.ps1
Test:     ctest --preset release
Run:      pwsh -NoProfile -File scripts/Start.ps1
Package:  pwsh -NoProfile -File scripts/Package.ps1

Package automatically increments the patch version. No publishing is configured.
Personal profiles are excluded from Git and portable packages.
Required tests use committed synthetic profiles and run without personal files.
See tests/fixtures/README.txt for regeneration and optional local Yamaha checks.

The Yamaha HS8 profile is calibration data for a specific speaker/room setup.
It is a local development fixture, not a generic Yamaha HS8 factory profile.

Reproduce decoded data from the project root
-------------------------------------------
python tools/profiles/decode_swproj.py

Plot the decoded data (requires ReportLab)
----------------------------------------
python tools/profiles/plot_decoded.py

The plotting utility generates SVG and PDF. The existing PNG preview is also
preserved in assets/profiles/decoded/yamaha-hs8/.

Validation performed during decoding
-----------------------------------
All 1420 exported point triples were compared directly with source bytes.
The two inverse curve pairs were confirmed numerically.
SoundID's exact runtime phase, gain, limits and delay behavior remains to be
validated before claiming equivalent playback.
