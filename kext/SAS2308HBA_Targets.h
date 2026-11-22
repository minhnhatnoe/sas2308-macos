/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * SAS2308HBA_Targets.h - which SCSI target nubs the controller has created,
 * wants created or wants destroyed.
 *
 * Plain C++ with no IOKit dependency, so the host tests can drive it
 * (test/kext_targets_test.cpp). The IOKit class owns one instance under
 * its fLock; the hotplug thread asks Next() for work, calls
 * CreateTargetForID()/DestroyTargetForID() with no lock held and reports
 * the result with Done().
 *
 * Termination (SAS2308HBA::willTerminate): Stop() first, so no new work is
 * handed out, then wait for the hotplug thread to finish the work it holds
 * (a CreateTargetForID() in progress completes and is recorded as created),
 * then let IOSCSIParallelInterfaceController::willTerminate destroy the
 * targets. The family's loop runs `index < fHighestSupportedDeviceID'
 * (IOSCSIParallelInterfaceController.cpp:522), so the highest ID is never
 * destroyed there: TakeCreated() hands out what the family loop missed.
 *
 * One entry per target ID, as many as the controller has (IOCFacts
 * MaxTargets + MaxVolumes, D7); the owner provides the zeroed storage.
 */
#ifndef SAS2308HBA_TARGETS_H
#define SAS2308HBA_TARGETS_H

#include <stdint.h>

class SAS2308TargetTable
{
public:
    enum Action { kNone, kCreate, kDestroy };

    struct Work {
        Action   action;
        uint32_t targetID;
        uint32_t gen;               /* kCreate: the generation of the device */
    };
    struct Entry {                  /* one target ID; all-zero = nothing */
        uint32_t gen;               /* generation of the nub created / being created */
        uint32_t wantGen;           /* generation of a wanted create */
        uint8_t  flags;             /* kCreated, kWantCreate, kWantDestroy, kBusy */
        uint64_t createdUs;         /* when its nub's creation began (uptime, S2's grace) */
    };
    enum : uint8_t {
        kCreated     = 1,           /* CreateTargetForID() succeeded, not destroyed */
        kWantCreate  = 2,
        kWantDestroy = 4,
        kBusy        = 8            /* handed to the hotplug thread, not Done() yet */
    };

    SAS2308TargetTable() = default;
    SAS2308TargetTable(const SAS2308TargetTable &) = delete;
    SAS2308TargetTable &operator=(const SAS2308TargetTable &) = delete;

    /* `count' zeroed entries, IDs 0 .. count-1; returns the old storage (Release) */
    void Attach(Entry *entries, uint32_t count) { fE = entries; fCount = count; }
    Entry *Release(uint32_t *count)
    {
        Entry *e = fE;
        if (count)
            *count = fCount;
        fE = nullptr;
        fCount = 0;
        return e;
    }
    uint32_t Count() const { return fCount; }

    /*
     * A rescan or lost-device event for one ID: `present' = the core has
     * the device; *gen (NULL: unknown, no change) = its generation
     * (mps_xnu_target_gen), which changes when a different disk is behind
     * the ID (review C2). A nub is never kept across a generation change:
     * destroy, then create. Returns true if the hotplug thread has work.
     */
    bool Update(uint32_t id, bool present, const uint32_t *genp = nullptr)
    {
        if (fStopped || id >= fCount)
            return false;
        Entry &e = fE[id];
        /* busy: the hotplug thread is creating (not yet created) or destroying (still created) it */
        const bool busy = (e.flags & kBusy) != 0;
        const bool created = (e.flags & kCreated) != 0;
        const bool wantC = (e.flags & kWantCreate) != 0, wantD = (e.flags & kWantDestroy) != 0;
        const bool willExist = busy ? (created ? wantC : !wantD) : ((created && !wantD) || wantC);
        /* the generation the nub will have once the pending work is done */
        const uint32_t willGen = wantC ? e.wantGen : e.gen;
        const uint32_t gen = genp ? *genp : willGen;
        if (present && !willExist) {
            e.flags |= kWantCreate;                 /* created and not destroying: Next() skips it */
            e.wantGen = gen;
            /* back while its nub is created or destroyed: kept, unless another disk */
            if (!((created || busy) && genp && gen != e.gen))
                e.flags &= (uint8_t)~kWantDestroy;
            return true;
        }
        if (present && gen != willGen) {
            /* a different disk behind the ID: the old nub goes first */
            if (created || busy)
                e.flags |= kWantDestroy;
            e.flags |= kWantCreate;
            e.wantGen = gen;
            return true;
        }
        if (!present && willExist) {
            e.flags &= (uint8_t)~kWantCreate;
            if (created || busy)                    /* a create in progress is destroyed after it */
                e.flags |= kWantDestroy;
            return true;
        }
        return false;
    }
    /* The next piece of work at or after `cursor' (advanced past it). */
    Work Next(uint32_t &cursor)
    {
        for (; !fStopped && fStarted && cursor < fCount; cursor++) {
            Entry &e = fE[cursor];
            if (e.flags & kBusy)
                continue;
            if (e.flags & kWantDestroy) {
                e.flags &= (uint8_t)~kWantDestroy;
                if (e.flags & kCreated) {
                    /* a wanted create (new generation) follows: the cursor stays on this ID */
                    e.flags |= kBusy;
                    fBusy++;
                    return Work{kDestroy, cursor, 0};
                }
            }
            if (e.flags & kWantCreate) {
                e.flags &= (uint8_t)~kWantCreate;
                if (!(e.flags & kCreated)) {
                    e.flags |= kBusy;
                    fBusy++;
                    e.gen = e.wantGen;
                    return Work{kCreate, cursor, e.gen};
                }
            }
        }
        return Work{kNone, fCount, 0};
    }
    /* The hotplug thread finished `w' (ok: CreateTargetForID() result). */
    void Done(const Work &w, bool ok)
    {
        if (w.action == kNone || w.targetID >= fCount)
            return;
        Entry &e = fE[w.targetID];
        if (e.flags & kBusy) {
            e.flags &= (uint8_t)~kBusy;
            fBusy--;
        }
        if (w.action == kCreate && ok)
            e.flags |= kCreated;
        else if (w.action == kDestroy)
            e.flags &= (uint8_t)~kCreated;
    }
    void Start() { fStarted = true; }
    /* No more work: termination or stop. Pending wishes are dropped. */
    void Stop()
    {
        fStopped = true;
        for (uint32_t i = 0; i < fCount; i++)
            fE[i].flags &= (uint8_t)~(kWantCreate | kWantDestroy);
    }
    /*
     * After the family's willTerminate destroyed IDs 0 .. familyLimit - 1:
     * true if `id' (>= familyLimit) still has a nub, which the caller
     * destroys; it counts as destroyed from now on. ForgetAll(): every
     * target counts as destroyed.
     */
    bool TakeCreated(uint32_t id)
    {
        if (id >= fCount || !(fE[id].flags & kCreated))
            return false;
        fE[id].flags &= (uint8_t)~kCreated;
        return true;
    }
    void ForgetAll()
    {
        for (uint32_t i = 0; i < fCount; i++)
            fE[i].flags &= (uint8_t)~kCreated;
    }
    bool Stopped() const { return fStopped; }
    bool Busy() const { return fBusy != 0; }
    bool Created(uint32_t id) const { return id < fCount && (fE[id].flags & kCreated); }
    /* the generation of the nub created or being created for `id' */
    uint32_t Gen(uint32_t id) const { return id < fCount ? fE[id].gen : 0; }
    /* when the hotplug thread began creating the nub for `id' (S2) */
    void Stamp(uint32_t id, uint64_t us) { if (id < fCount) fE[id].createdUs = us; }
    uint64_t Stamped(uint32_t id) const { return id < fCount ? fE[id].createdUs : 0; }
    bool Pending() const
    {
        for (uint32_t i = 0; i < fCount; i++)
            if (fE[i].flags & (kWantCreate | kWantDestroy))
                return true;
        return false;
    }

private:
    Entry   *fE = nullptr;
    uint32_t fCount = 0;
    uint32_t fBusy = 0;         /* entries with kBusy */
    bool     fStarted = false;  /* StartController ran */
    bool     fStopped = false;  /* willTerminate / stop: no new work */
};

#endif /* SAS2308HBA_TARGETS_H */
