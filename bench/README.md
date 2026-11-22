# Benchmarks: scripts and raw results

**Warning: most scripts here destroy data.** They pick every drive behind the HBA that matches
`scripts/sasdisks.sh` (the test setup's 7 Seagate ST18000NM000J, by model, size, SAS protocol and
IORegistry parent; edit it for other drives) and write to their raw disks (`/dev/rdiskN`): write,
mixed and verify passes overwrite whatever is on them, partition tables included. Run them only on
drives you have emptied. As a guard, every script that selects drives this way unmounts them and
then refuses to run unless the environment variable `SAS2308_YES_DESTROY` names exactly the
selected disks (the refusal prints the list), e.g.

    sudo SAS2308_YES_DESTROY=disk4,disk5,disk6,disk7,disk8,disk9,disk10 bash bench/scripts/phaseA.sh

Tools: `tools/sasbench` (`make -C tools`) and `tools/sas2308ctl`; the final run also builds
`final/scripts/fsbench` and `socsens` (`make -C bench/final/scripts`). smartctl (smartmontools)
is taken from `PATH`, `SMARTCTL=/path/to/smartctl` overrides it. New output goes to `bench/out/`
(ignored by git); the directories below are the committed results of the runs the docs describe.

| Path | What | Used by |
|---|---|---|
| `scripts/` | first benchmark run: `setup.sh`, `lib.sh`, `phase*.sh` (sequential, random, mixed, cache, verify), `monitor.sh`; the queue-depth run `q_bench.sh`, `q_bench2.sh`, `q_monitor.sh`; shared `sasdisks.sh` (drive selection) and `confirm.sh` (the guard above) | `docs/benchmarks-20261120.md` |
| `*.csv`, `*.txt`, `run.log.gz`, `seeds` (top level) | raw results of that run (2026-11-20, kext 67BF18CF): `results.csv` (one row per sasbench job), soak and warm-up CSVs, temperatures, SMART before/after, driver counters, a SYNCHRONIZE CACHE trace excerpt | `docs/benchmarks-20261120.md` |
| `q2048/` | queue-depth test with 2040 commands (RequestCredit 10240) | `docs/benchmarks-20261120.md` |
| `final/scripts/` | final benchmark and thermal soak: `run_all.sh`, `lib_final.sh`, `perf.sh`, `soak.sh`, `recheck.sh`, `smoke.sh`, `krecheck*.sh`, `monitor_final.sh`, `fsbench.c`, `socsens.c`, `assemble.py`, `compare.py`, `plot_temps.py`, `doc-template.md` | `docs/benchmarks-20261121-final.md` |
| `final/` | raw results of the final run (2026-11-20/21, kext 085DA9D6): `results.csv`, `run.log.gz`, soak, temperatures (`temps.png`), per-test driver counters in `stats/`, write-cache and SYNCHRONIZE CACHE traces | `docs/benchmarks-20261121-final.md` |
| `final/krecheck/` | the follow-up check of the drives' cache and power behaviour; `epc/`: the EPC timer probe (smartctl output) | `docs/benchmarks-20261121-final.md` |
| `profile/scripts/` | host-side cost profile: DTrace scripts (`*.d`), `profile.sh`, `base.sh`, `repro.sh`, the analysis (`analyze.py`, `roles.py`, `stk.py`, `gate.py`, `wl.py`) | `docs/perf-profile-20261121.md` |
| `profile/085DA9D6/` | raw profile data of kext 085DA9D6 | `docs/perf-profile-20261121.md` |

Drive serials are replaced by `drive1`..`drive8` in all committed files.
