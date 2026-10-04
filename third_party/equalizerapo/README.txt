Equalizer APO endpoint setup subset
==================================
Source: https://git.code.sf.net/p/equalizerapo/code
Commit: bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687 (1.4.2)
Copyright and GPLv2-or-later terms remain in each source file and License.txt.
These upstream files are unmodified. SoundeeEndpointSetup uses DeviceAPOInfo
to attach only a specifically selected playback endpoint and preserve the
driver's original APOs using upstream's registry backups. It does not change
Windows' default output or restart its audio service. The upstream Device
Selector remains available for installation troubleshooting and removal.
