#!/usr/bin/env bash
# Build the phone debug tools into out/op2tools; --install copies them to /usr/local/bin over SSH.
set -euo pipefail
cd "$(dirname "$0")"; source ../../tools/env.sh
CC="$XCODE_ROOT/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang -arch arm64 -isysroot $IPHONEOS_SDK -miphoneos-version-min=12.0 -O1 -Wall"
O=../../out/op2tools; mkdir -p $O
$CC -o $O/op2stack op2stack.c && codesign -s - --entitlements op2stack.ent --force $O/op2stack
$CC -o $O/op2net op2net.c && codesign -s - --force $O/op2net
$CC -o $O/op2reboot op2reboot.c && codesign -s - --force $O/op2reboot
$CC -o $O/op2kill op2kill.c && codesign -s - --force $O/op2kill
ls -l $O
if [ "${1:-}" = "--install" ]; then
  scp -O $O/op2stack $O/op2net $O/op2reboot $O/op2kill root@192.168.7.2:/usr/local/bin/
fi
