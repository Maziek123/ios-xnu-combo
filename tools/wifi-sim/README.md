# Wi-Fi host simulation

Checks for the QCA6174 Wi-Fi driver (`iokit/Kernel/arm/OP2Ath10k.cpp`) without the phone: the real driver source,
compiled for Linux with AddressSanitizer and UBSan, against a simulated chip. The chip model answers the boot loader,
checks the firmware download byte for byte, runs the HTC/WMI/HTT exchanges, delivers scan results, and from
scenario 22 on adds an access point whose side of the WPA2 handshake is done with OpenSSL (an implementation
independent of the driver's own) and, for step 7, a DHCP server, ARP and ping; for step 8 a stand-in for iOS's network stack on wl0; for the Wi-Fi manager op2netd's commands, scripted. 37 scenarios, each described at the top of `harness.cpp` and `ap.inc`: normal runs
and the failures seen on the phone (silent boot ROM, firmware crashes, missing events, a hotspot that only answers
probes, offloaded receive buffers, a stuck buffer, unconfirmed transmits, a wrong password).

    bash tools/wifi-sim/run.sh         # every scenario (Linux: g++, libssl-dev)
    bash tools/wifi-sim/run.sh 22 30   # some
    bash tools/wifi-sim/kcheck.sh      # compile the kernel files with XNU's warning flags (clang++), op2_wlan.c too

Both need the firmware in the gitignored `firmware/qca6174/` (see the main README, Wi-Fi firmware files). The
simulated network is always `iPhone` / `simulated-ap`; your own `wifi-network.txt` is not read, and the driver is copied
to a temporary folder first so headers generated for your own build are not picked up.

`host/` and `kstub/` are small stand-ins for the few kernel headers the files include (nothing from XNU itself).
A scenario passing here means the driver does what the model expects; the model is written from ath10k and from
what the phone's firmware did, so the phone still has the last word.
