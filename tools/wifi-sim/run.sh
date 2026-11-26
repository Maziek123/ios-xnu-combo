#!/usr/bin/env bash
# Host simulation of the QCA6174 (and, for steps 5-6, of an access point) against the real OP2Ath10k.cpp.
# Every scenario is described at the top of harness.cpp and ap.inc. Linux: g++ with ASan/UBSan, OpenSSL (libssl-dev).
# Needs the firmware the phone uses (firmware/qca6174/firmware-6.bin + bdwlan30.bin, gitignored), because the
# simulated chip checks the download byte for byte. The simulated access point's network is fixed (iPhone /
# simulated-ap); your own firmware/qca6174/wifi-network.txt is never read.
#   bash tools/wifi-sim/run.sh          all scenarios
#   bash tools/wifi-sim/run.sh 22 30    some of them (the log of each is in the printed folder)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ARM="$REPO/xnu/xnu-4903.270.47/iokit/Kernel/arm"
FW="${OP2_WIFI_FW_DIR:-$REPO/firmware/qca6174}"
if [ ! -f "$FW/firmware-6.bin" ] || [ ! -f "$FW/bdwlan30.bin" ]; then
	echo "wifi-sim: needs $FW/firmware-6.bin and bdwlan30.bin (see the README, Wi-Fi firmware files)" >&2
	exit 1
fi
OUT="$(mktemp -d "${TMPDIR:-/tmp}/wifi-sim.XXXXXX")"
mkdir -p "$OUT/src" "$OUT/gen" "$OUT/kernel"
cp "$FW/firmware-6.bin" "$FW/bdwlan30.bin" "$OUT/src/"
printf 'iPhone\nsimulated-ap\n' > "$OUT/src/wifi-network.txt"
OP2_WIFI_FW_DIR="$OUT/src" OP2_WIFI_FW_OUT_DIR="$OUT/gen" python3 "$REPO/tools/gen-wifi-fw.py" >/dev/null
# a copy of the driver, so headers generated next to it for your own build cannot be picked up
cp "$ARM/OP2Ath10k.cpp" "$ARM/op2_ath10k.h" "$OUT/kernel/"
g++ -std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -w -I"$HERE/host" -I"$OUT/gen" \
	-I"$OUT/kernel" "$HERE/harness.cpp" "$OUT/kernel/OP2Ath10k.cpp" -o "$OUT/sim" -lcrypto
list=("$@")
if [ ${#list[@]} -eq 0 ]; then
	list=($(seq 0 36))
fi
bad=0
for i in "${list[@]}"; do
	if timeout 300 "$OUT/sim" "$i" > "$OUT/scenario-$i.log" 2>&1; then
		printf '%3s  %s\n' "$i" "$(grep -a 'RESULT ok=' "$OUT/scenario-$i.log" | tail -1 | cut -c1-110)"
	else
		printf '%3s  FAILED: %s\n' "$i" "$(tail -2 "$OUT/scenario-$i.log" | tr '\n' ' ' | cut -c1-110)"
		bad=$((bad + 1))
	fi
done
echo "wifi-sim: ${#list[@]} scenarios, $bad failed; logs in $OUT"
exit $((bad != 0))
