# Review records

Kept so the ids in commit messages, code comments and docs/design.md resolve.
Line numbers in them refer to the commit each one names, not to master.

| File | What | Ids |
|---|---|---|
| `2026-11-final.md` | code review of the port and the kext, findings by severity | C1-C2, H1-H5, M1-M8, L1... ("review M3") |
| `2026-11-freebsd-deviations.md` | every deviation from FreeBSD mps/CAM, the plan | |
| `2026-11-decisions.md` | question-and-answer round on each deviation, with outcomes | D1-D81 |
| `2026-11-vendor.md` | question-and-answer round on the vendor branch and the rewritten history | V1-V16 |

Each question-and-answer entry has an answer ("Answer:") and a check of that answer
("Check:"). Review ids are written "review M3" in commits and
code, to keep them apart from the development milestones M1-M5c
(docs/design.md, top).
