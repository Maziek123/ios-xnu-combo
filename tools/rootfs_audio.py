#!/usr/bin/env python3
"""Audio on a mounted iOS rootfs: rootfs_audio.py <mountpoint>   (2026-11-17)

mediaserverd (all audio/video playback) needs a built-in audio device. There is no audio driver yet, so:
   /Library/Audio/Plug-Ins/HAL/OP2NullAudio.driver   silent 48 kHz stereo output (userland/op2audio/build.sh)
   /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin   back from HAL.disabled: CoreMedia's CMSession waits for its
                                                     devices ("vadInitialization")
   HAL.op2-off/                                      AirPlay/CarPlay/Octavia Halogen, BTAudio, TimeSync and
                                                     NetworkUplinkClock: they block startup on absent daemons
   /private/var/db/com.apple.xpc.launchd/disabled.plist   com.apple.mediaserverd enabled
Skips with a message when OP2NullAudio has not been built.
"""
import os
import plistlib
import shutil
import sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'out', 'op2audio', 'OP2NullAudio.driver')
if not os.path.isdir(src):
    print('  [audio] out/op2audio/OP2NullAudio.driver not built (userland/op2audio/build.sh): skipped')
    sys.exit(0)

plug = os.path.join(root, 'Library/Audio/Plug-Ins')
hal, off, disabled = (os.path.join(plug, d) for d in ('HAL', 'HAL.op2-off', 'HAL.disabled'))
os.makedirs(hal, exist_ok=True)
os.makedirs(off, exist_ok=True)
dst = os.path.join(hal, 'OP2NullAudio.driver')
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(src, dst)
va = os.path.join(disabled, 'VirtualAudio.plugin')
if os.path.isdir(va):
    shutil.move(va, os.path.join(hal, 'VirtualAudio.plugin'))
for p in ('AirPlayHalogen', 'AppleTimeSyncAudioClock', 'BTAudioHALPlugin', 'CarPlayHalogen', 'NetworkUplinkClock',
          'OctaviaHalogen'):
    d = os.path.join(hal, p + '.driver')
    if os.path.isdir(d):
        shutil.move(d, os.path.join(off, p + '.driver'))
for dirpath, dirs, files in os.walk(dst):
    for n in dirs + files:
        os.chown(os.path.join(dirpath, n), 0, 0)
os.chown(dst, 0, 0)

dp = os.path.join(root, 'private/var/db/com.apple.xpc.launchd/disabled.plist')
if os.path.exists(dp):
    raw = open(dp, 'rb').read()
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    d = plistlib.loads(raw)
    d['com.apple.mediaserverd'] = False
    open(dp, 'wb').write(plistlib.dumps(d, fmt=fmt))
print('  [audio] OP2NullAudio + VirtualAudio in HAL, blocking plug-ins in HAL.op2-off, mediaserverd enabled')
