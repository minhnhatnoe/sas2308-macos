# Loading SAS2308HBA.kext on the Mac mini (Apple silicon, macOS 27)

Status: these steps load the kext on the Mac mini in use since 2026-11-19 (see the
"Verified" sections of docs/design.md). Steps marked (inferred) follow Apple's documented
Apple-silicon kext flow and were written before the first load.

## One-time security change (needs someone at the Mac's keyboard)
1. Shut the Mac down. Press and hold the power button until "Loading startup options" →
   Options → Continue (Recovery).
2. Utilities → Terminal: `csrutil disable` → confirm with the admin password.
   (Permissive Security: required to load an ad-hoc-signed, non-Developer-ID kext.)
3. Utilities → Startup Security Utility → select the disk → Security Policy:
   **Reduced Security** + tick **"Allow user management of kernel extensions from identified developers"** → OK.
4. Restart into macOS.
Check afterwards: `csrutil status` → disabled. (inferred: on Apple silicon `csrutil disable`
alone already implies Permissive Security; step 3 is still needed for third-party kexts.)

## Install and load (over ssh, from the Linux host or on the Mac)
```
cd ~/sas2308-dext/kext && make                      # rebuild
make check                                           # plist, signature, kmutil symbols, exported symbols
sudo rm -rf /Library/Extensions/SAS2308HBA.kext      # only if an older copy exists (check with ls first)
sudo cp -R build/SAS2308HBA.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/SAS2308HBA.kext
sudo kmutil load -p /Library/Extensions/SAS2308HBA.kext
```
(inferred) The first load stages the kext into the Auxiliary Kernel Collection and macOS
shows "System Extension Blocked". Then:
5. System Settings → Privacy & Security → "Allow" for the SAS2308HBA extension (keyboard/mouse or Screen Sharing needed).
6. Reboot when prompted. After reboot: `kmutil showloaded | grep SAS2308` (only loads once the card is present — it matches PCI 1000:0087).
Logs (os_log subsystem `org.sas2308.driver.SAS2308HBA` (the bundle ID), one category per FreeBSD debug class;
details in design.md "Logging"):
```
# zsh has a builtin `log`, so use the full path /usr/bin/log
/usr/bin/log stream --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'                  # default: level 2 (info)
/usr/bin/log stream --level debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'    # + xinfo/trace lines
/usr/bin/log stream --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA" AND category == "recovery"'
/usr/bin/log show --last 30m --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'         # after the fact
make -C ~/sas2308-dext/tools                                                # builds sas2308ctl
~/sas2308-dext/tools/sas2308ctl show                                        # level + mask
~/sas2308-dext/tools/sas2308ctl stats                                       # counters (or: ioreg -r -c SAS2308HBA -k SAS2308Statistics)
sudo ~/sas2308-dext/tools/sas2308ctl level debug                            # 0-4 / error warn info debug trace
sudo ~/sas2308-dext/tools/sas2308ctl mask +trace                            # FreeBSD class list or 0xMASK
```
Level at boot: `sudo nvram boot-args="<existing args> sas2308_log=3"` (0–4) or
`sas2308_debug=0x7ff` (raw mask), reboot. For a panic investigation add `sas2308_iolog=1`
(lines go to IOLog: `sudo dmesg | grep SAS2308HBA`, and into the panic report).

## Unload / roll back
```
sudo kmutil unload -b org.sas2308.driver.SAS2308HBA     # with no disks mounted
sudo rm -rf /Library/Extensions/SAS2308HBA.kext
sudo kmutil clear-staging                                 # then reboot
```
If it panics at boot: hold the power button → Recovery → Terminal:
`rm -rf "/Volumes/Macintosh HD/Library/Extensions/SAS2308HBA.kext"`, then
`kmutil trigger-panic-medic --volume-root "/Volumes/Macintosh HD"` (inferred), reboot.
To restore full security later: Recovery → `csrutil enable` and Startup Security Utility → Full Security.

## What to look at once it is loaded
```
ioreg -l -w0 | grep -E "Physical Interconnect|SAS Address"   # HBA + every target nub: SAS / External, 8-byte addresses
ioreg -r -c SAS2308HBA -k "Controller Characteristics"        # vendor LSI, product, firmware, 6 Gigabit
ioreg -r -c IOSCSIParallelInterfaceDevice | grep -E "SAS Target|Protocol Characteristics"
system_profiler SPSASDataType SPParallelSCSIDataType          # System Information (may list it under SAS or SCSI)
/usr/bin/log show --last 10m --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA" AND category == "init"' | grep -Ei "power|quiesc|wake|suspend"
```
Sleep / wake and restart are handled since M5b (IO held across sleep, IOC stopped before a
restart) but were never exercised on hardware: test them as in docs/design.md "Hardware test
plan" before relying on them with mounted disks.

## Notes
- Leaving SIP disabled lowers the Mac's protection system-wide (user approved this, 2026-11-18).
- Each macOS update may require rebuilding (`make`) and re-approving.
