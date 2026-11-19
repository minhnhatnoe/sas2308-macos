/*
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
 * (IOSCSIParallelInterfaceController.cpp:522), so the highest ID (63) is
 * never destroyed there: Leftovers() returns what the family loop missed.
 */
#ifndef SAS2308HBA_TARGETS_H
#define SAS2308HBA_TARGETS_H

#include <stdint.h>

class SAS2308TargetTable
{
public:
    static constexpr uint32_t kMaxTargets = 64;

    enum Action { kNone, kCreate, kDestroy };
    struct Work {
        Action   action;
        uint32_t targetID;
    };

    /*
     * A rescan or lost-device event: for each ID in `ids', bit set in
     * `present' = the core has the device. Returns true if the hotplug
     * thread has work.
     */
    bool Update(uint64_t ids, uint64_t present)
    {
        bool changed = false;
        if (fStopped)
            return false;
        for (uint32_t i = 0; i < kMaxTargets; i++) {
            const uint64_t bit = 1ull << i;
            if (!(ids & bit))
                continue;
            /* busy: the hotplug thread is creating (not yet created) or destroying (still created) it */
            const bool busy = (fBusy & bit) != 0;
            const bool created = (fCreated & bit) != 0;
            const bool willExist = busy ? (created ? (fWantCreate & bit) != 0 : !(fWantDestroy & bit))
                                        : ((created && !(fWantDestroy & bit)) || (fWantCreate & bit));
            if ((present & bit) && !willExist) {
                fWantCreate |= bit;                 /* created and not destroying: Next() skips it */
                fWantDestroy &= ~bit;
                changed = true;
            } else if (!(present & bit) && willExist) {
                fWantCreate &= ~bit;
                if (created || busy)                /* a create in progress is destroyed after it */
                    fWantDestroy |= bit;
                changed = true;
            }
        }
        return changed;
    }

    /* The next piece of work at or after `cursor' (advanced past it). */
    Work Next(uint32_t &cursor)
    {
        for (; !fStopped && fStarted && cursor < kMaxTargets; cursor++) {
            const uint64_t bit = 1ull << cursor;
            if (fBusy & bit)
                continue;
            if (fWantDestroy & bit) {
                fWantDestroy &= ~bit;
                fWantCreate &= ~bit;
                if (fCreated & bit) {
                    fBusy |= bit;
                    return Work{kDestroy, cursor++};
                }
            } else if (fWantCreate & bit) {
                fWantCreate &= ~bit;
                if (!(fCreated & bit)) {
                    fBusy |= bit;
                    return Work{kCreate, cursor++};
                }
            }
        }
        return Work{kNone, kMaxTargets};
    }

    /* The hotplug thread finished `w' (ok: CreateTargetForID() result). */
    void Done(const Work &w, bool ok)
    {
        if (w.action == kNone || w.targetID >= kMaxTargets)
            return;
        const uint64_t bit = 1ull << w.targetID;
        fBusy &= ~bit;
        if (w.action == kCreate && ok)
            fCreated |= bit;
        else if (w.action == kDestroy)
            fCreated &= ~bit;
    }

    void Start() { fStarted = true; }
    /* No more work: termination or stop. Pending wishes are dropped. */
    void Stop()
    {
        fStopped = true;
        fWantCreate = fWantDestroy = 0;
    }
    /*
     * After the family's willTerminate destroyed IDs 0 .. familyLimit - 1:
     * the created targets it did not destroy (ID 63 with familyLimit 63).
     * Every target counts as destroyed afterwards.
     */
    uint64_t Leftovers(uint32_t familyLimit)
    {
        const uint64_t missed = familyLimit >= kMaxTargets ? 0 : fCreated & ~((1ull << familyLimit) - 1);
        fCreated = 0;
        return missed;
    }

    bool Stopped() const { return fStopped; }
    bool Busy() const { return fBusy != 0; }
    bool Created(uint32_t id) const { return id < kMaxTargets && (fCreated & (1ull << id)); }
    uint64_t CreatedMask() const { return fCreated; }
    bool Pending() const { return (fWantCreate | fWantDestroy) != 0; }

private:
    uint64_t fCreated = 0;      /* CreateTargetForID() succeeded, not destroyed */
    uint64_t fWantCreate = 0;
    uint64_t fWantDestroy = 0;
    uint64_t fBusy = 0;         /* handed to the hotplug thread, not Done() yet */
    bool     fStarted = false;  /* StartController ran */
    bool     fStopped = false;  /* willTerminate / stop: no new work */
};

#endif /* SAS2308HBA_TARGETS_H */
