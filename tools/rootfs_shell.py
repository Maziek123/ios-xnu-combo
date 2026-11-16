#!/usr/bin/env python3
"""USB root shell + SSH on a mounted iOS rootfs: rootfs_shell.py <mountpoint>

Installs the launcher built by tools/build-op2shell.sh (dash + toybox + dropbear + scp + your
authorized_keys, all embedded) and registers its launchd job, so a freshly built rootfs comes up with
the serial shell on /dev/op2usb and dropbear on ue0 (192.168.7.2) with no manual steps:
   /usr/local/bin/op2-air-run      out/shell/build/op2shell, zero-padded to a 16 MiB slot so later
                                   builds can be written in place (tools/build-op2shell.sh --install)
   xpcd_cache.dylib                job com.op2.shell (tools/op2shell-job.plist) added in place
                                   (iOS 12 launchd reads jobs only from this cache; launchd_unsecure_cache=1)
Skips with a message when the launcher has not been built.
"""
import os
import subprocess
import sys

MiB = 1 << 20
root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'out', 'shell', 'build', 'op2shell')
if not os.path.exists(src):
    print('  [shell] out/shell/build/op2shell not built (tools/build-op2shell.sh): skipped')
    sys.exit(0)

data = open(src, 'rb').read()
SLOT = 16 * MiB
if len(data) > SLOT:
    sys.exit('op2shell is %d bytes; slot is %d' % (len(data), SLOT))
dst = os.path.join(root, 'usr/local/bin/op2-air-run')
os.makedirs(os.path.dirname(dst), exist_ok=True)
with open(dst, 'wb') as f:
    f.write(data)
    f.write(b'\0' * (SLOT - len(data)))
os.chown(dst, 0, 0)
os.chmod(dst, 0o755)
print('  [shell] /usr/local/bin/op2-air-run: %d bytes in a %d-byte slot' % (len(data), SLOT))

cache = os.path.join(root, 'System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib')
subprocess.check_call([sys.executable, '-I', os.path.join(here, 'xpcd-cache-add.py'), cache, cache,
                       os.path.join(here, 'op2shell-job.plist')])
print('  [shell] launchd job com.op2.shell added')
