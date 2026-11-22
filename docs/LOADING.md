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
# log stream never shows the kext's lines on macOS 27; log show misses them until logd learns of
# the kext (usually ~1 min after boot, sometimes never; design.md "Lines missing"). Sure way:
sudo launchctl kickstart -k system/com.apple.logd                           # once per boot
/usr/bin/log show --last 30m --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"'         # default: level 2 (info)
/usr/bin/log show --last 30m --debug --predicate 'subsystem == "org.sas2308.driver.SAS2308HBA"' # + xinfo/trace lines (if persisted)
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

## How to unplug a drive: Eject, wait for the stop, pull

The disks never spin down for idleness (`sas2308_idle_spindown` 0, the default); Eject is what
stops one (EJ, docs/design.md):
1. Eject it: Finder's eject button, or `diskutil eject diskN` (the whole disk; find N with
   `diskutil list external physical`). This unmounts its volumes and macOS sends the drive its
   stop (START STOP UNIT), which the kext passes on: `sas2308ctl stats` shows
   "Stops passed (ejected)" + 1.
2. Wait for the stop: about 20 s before moving the drive. The stop is sent with IMMED 1, so the
   drive answers before its platters have stopped; the spin-down time itself was not measured.
   That `diskutil list` still shows the drive after the eject is normal: for a fixed disk macOS
   keeps the disk objects until the drive is gone.
3. Pull it (or switch off its bay / the enclosure). Plugged back in, it comes back as a new disk
   (`diskutil mount diskNs2` if its volume does not mount by itself).

To use an ejected drive again without pulling it: `diskutil mount diskNs2` (or any read of
/dev/diskN): macOS sends START and the drive spins up (7.5-7.7 s measured), then mounts.
`sas2308ctl spindown diskN|all` does the same eject for drives with nothing mounted (it refuses a
mounted one). Do not stop a drive any other way (a START STOP UNIT or ATA STANDBY of your own):
the HBA's SAT layer then answers every read NOT READY until a START, which macOS sends only after
its own eject or idle stop.

## Disk spin-down (S1, S2, EJ) and Spotlight on the SAS disks
```
~/sas2308-dext/tools/sas2308ctl stats | grep -Ei "idle|spin|stops"   # idle stops completed here / sent / in grace, ejects, spin-downs
sudo ~/sas2308-dext/tools/sas2308ctl set sas2308_idle_spindown 0   # 0 never for idleness (default), 1 always (macOS), 2 only when not in use; Eject stops the disk in 0 and 2
sudo ~/sas2308-dext/tools/sas2308ctl set sas2308_spindown_grace 900  # mode 2: s after attach with no spin-down (boot); 0 = none
sudo ~/sas2308-dext/tools/sas2308ctl spindown disk5            # stop one disk now: an eject (refused if mounted)
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

All 8 drives were reformatted on 2026-11-22 (GPT + exFAT, one volume per drive named after its
serial, /Volumes/<serial>) and are scratch. The earlier `/etc/fstab` noauto line for the old exFAT
volume on drive8 is gone (no /etc/fstab on the Mac, checked 2026-11-22). A GUI login mounts all
8 volumes; without one, mount them with `diskutil mount diskNs2`.

## Notes
- Leaving SIP disabled lowers the Mac's protection system-wide (user approved this, 2026-11-18).
- Each macOS update may require rebuilding (`make`) and re-approving.
