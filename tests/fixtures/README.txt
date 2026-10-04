Public synthetic stereo calibration
==================================

raw/stereo-v3.swproj and raw/expected.json contain smooth, deliberately
different left/right EQ responses and their inverse pairs. They contain no
room measurements or personal data. All required CTest checks use these files.

Regenerate with Python's standard library:
  python tools/tests/generate-profile-fixture.py

Optional personal-profile validation:
  cmake --preset windows "-DSOUNDEE_PERSONAL_PROFILE=C:/path/to/Yamaha HS8.swproj"
  ctest --preset release -R Soundee.PersonalProfile

The optional profile's decoded baseline must be at
  ../decoded/yamaha-hs8/profile.json
relative to its raw/ directory, or beside it as expected.json.
Leave SOUNDEE_PERSONAL_PROFILE empty for reproducible public tests/packages.
