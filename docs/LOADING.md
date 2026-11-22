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

## Install, roll back, uninstall with the scripts
On the Mac, as the admin user (the scripts call sudo; you type the password). Start with
a dry run: it prints every action and changes nothing.
```
cd ~/sas2308-dext
mac/install.sh -n               # dry run: security state, bundle, backup path, volumes to unmount
mac/install.sh                  # make + make check, back up the installed bundle to
                                # ~/sas2308-rollback-<UUID8>, unmount the volumes on the SAS2308HBA
                                # disks, copy to /Library/Extensions, chown, kmutil load -p
mac/install.sh --verify --expect 7   # after approval + restart: loaded UUID, drives, counters
mac/install.sh --rollback -n    # newest backup that differs from the installed one (or give a path)
mac/uninstall.sh -n             # backup, unmount, kmutil unload, remove, clear-staging if staged
```
`mac/install.sh path/to/SAS2308HBA.kext` installs a given bundle; `--force` reinstalls the
installed UUID. When kmutil returns 27 the script prints the approval steps (below, 5-6).
The manual steps below do the same and are the fallback.

## Install and load by hand (over ssh, from the Linux host or on the Mac)
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

## Unload / roll back (by hand; `mac/uninstall.sh` does this)
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

## Disk spin-down (S2) and Spotlight on the SAS disks
```
~/sas2308-dext/tools/sas2308ctl stats | grep -Ei "idle|spin"   # idle stops completed here / sent / in grace, spin-downs
sudo ~/sas2308-dext/tools/sas2308ctl set sas2308_idle_spindown 2   # 0 never spin down, 1 always (macOS), 2 only when not in use (default)
sudo ~/sas2308-dext/tools/sas2308ctl set sas2308_spindown_grace 900  # s after attach with no spin-down (boot); 0 = none
sudo ~/sas2308-dext/tools/sas2308ctl spindown disk5            # stop one disk now (refused if mounted / open)
sudo ~/sas2308-dext/tools/sas2308ctl spindown all              # every disk behind the HBA that is not mounted
pmset -g | grep disksleep                                     # macOS's idle timer (minutes) that sends the idle stops
```
Spotlight (set 2026-11-22, macOS 27): system-wide, external volumes are not indexed:
`sudo defaults write /Library/Preferences/com.apple.SpotlightServer.plist ExternalVolumesIgnore -bool true`
then `sudo launchctl kickstart -k system/com.apple.metadata.mds`. Verified: a fresh JHFS+ volume
showed "Indexing and searching disabled" and got no .Spotlight-V100 (control without the key:
"Indexing enabled" and .Spotlight-V100 created); the internal disk stays "Indexing enabled".
Per volume: `sudo mdutil -i off /Volumes/F145TEST` (stored on the volume). Undo:
`sudo defaults delete /Library/Preferences/com.apple.SpotlightServer.plist ExternalVolumesIgnore`,
kickstart mds again, `sudo mdutil -i on /Volumes/F145TEST`.

The exFAT volume drive8 (on the drive of the same serial, contents unknown, not ours) is kept
from auto-mounting by `/etc/fstab`: `UUID=<volume-UUID> none exfat rw,noauto`
(a GUI login would otherwise mount it read-write and macOS would write .fseventsd to it). Undo:
delete that line (`sudo vifs`).

## Notes
- Leaving SIP disabled lowers the Mac's protection system-wide (user approved this, 2026-11-18).
- Each macOS update may require rebuilding (`make`) and re-approving.
