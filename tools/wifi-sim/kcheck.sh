#!/usr/bin/env bash
# Compile-only check of the Wi-Fi kernel files with XNU's C++ warning flags (-Weverything -Werror minus XNU's own
# exceptions), against the stand-in headers in kstub/. Any clang++. With and without the generated firmware and
# network headers (made here from a fixed test network if firmware/qca6174/ has the firmware).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ARM="$REPO/xnu/xnu-4903.270.47/iokit/Kernel/arm"
FW="${OP2_WIFI_FW_DIR:-$REPO/firmware/qca6174}"
FLAGS="-std=gnu++1z -fsyntax-only -nostdinc -nostdinc++ --target=arm64-apple-ios12.0 -Weverything -Wno-pedantic -Werror
 -Wno-assign-enum -Wno-bad-function-cast -Wno-c++98-compat -Wno-c++-compat -Wno-conditional-uninitialized -Wno-conversion
 -Wnull-conversion -Wstring-conversion -Wliteral-conversion -Wnon-literal-null-conversion -Wint-conversion -Wenum-conversion
 -Wfloat-conversion -Wconstant-conversion -Wpointer-bool-conversion -Wno-covered-switch-default -Wno-disabled-macro-expansion
 -Wno-documentation-unknown-command -Wno-format-non-iso -Wno-format-nonliteral -Wno-reserved-id-macro
 -Wno-language-extension-token -Wno-missing-variable-declarations -Wno-packed -Wno-padded -Wno-partial-availability
 -Wno-shift-sign-overflow -Wno-switch-enum -Wno-undef -Wno-unused-macros -Wno-used-but-marked-unused -Wno-variadic-macros
 -Wno-vla -Wno-zero-length-array -Wno-unknown-warning-option -Wno-error=atomic-implicit-seq-cst -Wno-c++98-compat-pedantic
 -Wno-exit-time-destructors -Wno-global-constructors -Wno-old-style-cast -Wno-zero-as-null-pointer-constant
 -Wno-reserved-identifier -Wno-unsafe-buffer-usage -Wno-declaration-after-statement -Wno-missing-include-dirs
 -Wno-cast-function-type-strict -Wno-c++20-compat"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/wifi-kcheck.XXXXXX")"
mkdir -p "$OUT/kernel" "$OUT/gen" "$OUT/src"
cp "$ARM/OP2Ath10k.cpp" "$ARM/OP2Wifi.cpp" "$ARM/op2_ath10k.h" "$OUT/kernel/"
rc=0
clang++ $FLAGS -I"$HERE/kstub" -I"$OUT/kernel" "$OUT/kernel/OP2Ath10k.cpp" && echo "OP2Ath10k.cpp, no firmware: OK" || rc=1
if [ -f "$FW/firmware-6.bin" ] && [ -f "$FW/bdwlan30.bin" ]; then
	cp "$FW/firmware-6.bin" "$FW/bdwlan30.bin" "$OUT/src/"
	printf 'iPhone\nsimulated-ap\n' > "$OUT/src/wifi-network.txt"
	OP2_WIFI_FW_DIR="$OUT/src" OP2_WIFI_FW_OUT_DIR="$OUT/gen" python3 "$REPO/tools/gen-wifi-fw.py" >/dev/null
	clang++ $FLAGS -I"$HERE/kstub" -I"$OUT/gen" -I"$OUT/kernel" "$OUT/kernel/OP2Ath10k.cpp" &&
	    echo "OP2Ath10k.cpp, firmware + network: OK" || rc=1
else
	echo "OP2Ath10k.cpp, firmware + network: skipped (no firmware in $FW)"
fi
clang++ $FLAGS -I"$HERE/kstub" -I"$OUT/kernel" "$OUT/kernel/OP2Wifi.cpp" && echo "OP2Wifi.cpp: OK" || rc=1
rm -rf "$OUT"
exit $rc
