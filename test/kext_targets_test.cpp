/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Noe
 *
 * kext_targets_test.cpp - the kext's target table (kext/SAS2308HBA_Targets.h)
 * on the host: hotplug decisions, termination (the family's willTerminate
 * loop stops before the highest ID), 1024 IDs, and the hotplug thread racing
 * termination. The IOKit calls around it (CreateTargetForID,
 * DestroyTargetForID, the family loop) are modelled by the test; see
 * docs/design.md "Hardware test plan" for what only the Mac can show.
 */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

#include "../kext/SAS2308HBA_Targets.h"

static int gChecks = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef SAS2308TargetTable T;
static const uint64_t kAll = ~0ull;
static uint64_t Bit(uint32_t i) { return 1ull << i; }

/* a table with its own storage: `n' IDs (64 by default, so the 64-bit masks below cover them) */
struct TT : T {
    std::vector<T::Entry> mem;
    explicit TT(uint32_t n = 64) : mem(n) { Attach(mem.data(), n); }
};

/* an event for the IDs in `ids' (< 64): present bit, generation gens[id] */
static bool Upd(T &t, uint64_t ids, uint64_t present, const uint32_t *gens = nullptr)
{
    bool changed = false;
    for (uint32_t i = 0; i < 64; i++)
        if (ids & Bit(i))
            changed |= t.Update(i, (present & Bit(i)) != 0, gens ? &gens[i] : nullptr);
    return changed;
}

static uint64_t CreatedMask(const T &t)
{
    uint64_t m = 0;
    for (uint32_t i = 0; i < 64 && i < t.Count(); i++)
        if (t.Created(i))
            m |= Bit(i);
    return m;
}

/* The hotplug thread: run every piece of work, `ok' for creates. */
static std::vector<T::Work> Drain(T &t, bool ok = true)
{
    std::vector<T::Work> done;
    uint32_t cursor = 0;
    for (;;) {
        T::Work w = t.Next(cursor);
        if (w.action == T::kNone)
            break;
        t.Done(w, ok);
        done.push_back(w);
    }
    return done;
}

/*
 * SAS2308HBA::willTerminate on the table: Stop, (the hotplug thread
 * finishes what it holds), the family destroys 0 .. highest-1, then
 * TerminateTargets destroys the leftovers. Returns every destroyed ID.
 */
static uint64_t Terminate(T &t, uint32_t highest = 63, std::vector<uint32_t> *all = nullptr)
{
    t.Stop();
    uint64_t destroyed = 0;
    for (uint32_t i = 0; i < highest; i++) {        /* IOSCSIParallelInterfaceController.cpp:522 */
        if (t.Created(i)) {
            if (i < 64) destroyed |= Bit(i);
            if (all) all->push_back(i);
        }
    }
    uint64_t left = 0;
    for (uint32_t i = highest; i < t.Count(); i++) {    /* SAS2308Glue::TerminateTargets */
        if (t.TakeCreated(i)) {
            if (i < 64) left |= Bit(i);
            if (all) all->push_back(i);
        }
    }
    t.ForgetAll();
    CHECK((left & destroyed) == 0);
    return destroyed | left;
}

int main()
{
    printf("targets: nothing before StartController\n");
    {
        TT t;
        CHECK(Upd(t, Bit(3), Bit(3)));
        CHECK(Drain(t).empty() && !t.Created(3) && t.Pending());
        t.Start();
        std::vector<T::Work> w = Drain(t);
        CHECK(w.size() == 1 && w[0].action == T::kCreate && w[0].targetID == 3 && t.Created(3));
        CHECK(!t.Pending() && !t.Busy());
    }

    printf("targets: all 64 IDs created in order, 63 included\n");
    {
        TT t;
        t.Start();
        CHECK(Upd(t, kAll, kAll));
        std::vector<T::Work> w = Drain(t);
        CHECK(w.size() == 64);
        for (uint32_t i = 0; i < 64; i++)
            CHECK(w[i].action == T::kCreate && w[i].targetID == i);
        CHECK(CreatedMask(t) == kAll);
        CHECK(!Upd(t, kAll, kAll));               /* present again: keep them */
        CHECK(Drain(t).empty());

        printf("targets: termination destroys 0..63 (the family loop misses 63)\n");
        CHECK(Terminate(t) == kAll);
        CHECK(CreatedMask(t) == 0 && t.Stopped());
        CHECK(!Upd(t, Bit(9), Bit(9)) && Drain(t).empty());   /* no work after Stop */
    }

    printf("targets: termination with only 63 created\n");
    {
        TT t;
        t.Start();
        t.Update(63, true);
        Drain(t);
        CHECK(Terminate(t) == Bit(63));             /* SAS2308HBA::TerminateTargets destroys it */
    }

    printf("targets: removal, re-add, failed create\n");
    {
        TT t;
        t.Start();
        Upd(t, Bit(1) | Bit(2), Bit(1) | Bit(2));
        Drain(t);
        CHECK(Upd(t, Bit(1), 0));                 /* 1 lost */
        std::vector<T::Work> w = Drain(t);
        CHECK(w.size() == 1 && w[0].action == T::kDestroy && w[0].targetID == 1 && !t.Created(1));
        CHECK(!Upd(t, Bit(1), 0));                /* already gone */
        CHECK(Upd(t, Bit(1), Bit(1)));            /* back */
        CHECK(Drain(t, false).size() == 1 && !t.Created(1));   /* CreateTargetForID failed */
        CHECK(!Upd(t, Bit(1), 0) && Drain(t).empty());
        /* wildcard rescan after a reinit: 2 found again, 1 still absent */
        CHECK(!Upd(t, kAll, Bit(2)));
        CHECK(Drain(t).empty() && t.Created(2));
        /* a create wanted, then lost before the thread ran: nothing to do */
        CHECK(Upd(t, Bit(4), Bit(4)) && Upd(t, Bit(4), 0));
        CHECK(Drain(t).empty() && !t.Created(4));
    }

    printf("targets: device lost while its CreateTargetForID runs -> destroyed after\n");
    {
        TT t;
        t.Start();
        Upd(t, Bit(7), Bit(7));
        uint32_t c = 0;
        T::Work w = t.Next(c);
        CHECK(w.action == T::kCreate && w.targetID == 7 && t.Busy());
        CHECK(Upd(t, Bit(7), 0));                 /* async event while the create runs */
        uint32_t c2 = 0;
        CHECK(t.Next(c2).action == T::kNone);       /* 7 is busy: nothing handed out twice */
        t.Done(w, true);
        std::vector<T::Work> d = Drain(t);
        CHECK(d.size() == 1 && d[0].action == T::kDestroy && d[0].targetID == 7 && !t.Created(7));
    }

    printf("targets: device back while its DestroyTargetForID runs -> created again\n");
    {
        TT t;
        t.Start();
        Upd(t, Bit(8), Bit(8));
        Drain(t);
        Upd(t, Bit(8), 0);
        uint32_t c = 0;
        T::Work w = t.Next(c);
        CHECK(w.action == T::kDestroy && w.targetID == 8);
        CHECK(Upd(t, Bit(8), Bit(8)));
        t.Done(w, true);
        std::vector<T::Work> d = Drain(t);
        CHECK(d.size() == 1 && d[0].action == T::kCreate && t.Created(8));
    }

    printf("targets: lost and back again while the create runs -> kept\n");
    {
        TT t;
        t.Start();
        Upd(t, Bit(12), Bit(12));
        uint32_t c = 0;
        T::Work w = t.Next(c);
        CHECK(Upd(t, Bit(12), 0) && Upd(t, Bit(12), Bit(12)));
        t.Done(w, true);
        CHECK(Drain(t).empty() && t.Created(12));
    }

    printf("targets: a different disk behind an ID (new generation) -> destroy, then create (review C2)\n");
    {
        TT t;
        t.Start();
        uint32_t g[64] = {};
        g[4] = 1;
        Upd(t, Bit(4), Bit(4), g);
        Drain(t);
        CHECK(t.Created(4));
        CHECK(!Upd(t, Bit(4), Bit(4), g));              /* same disk: the nub stays */
        g[4] = 2;
        CHECK(Upd(t, Bit(4), Bit(4), g));
        std::vector<T::Work> d = Drain(t);
        CHECK(d.size() == 2 && d[0].action == T::kDestroy && d[1].action == T::kCreate && d[1].gen == 2);
        CHECK(t.Created(4) && !t.Pending());
        /* a new disk while the old one's create runs: created, then replaced */
        g[5] = 1;
        Upd(t, Bit(5), Bit(5), g);
        uint32_t c = 0;
        T::Work w = t.Next(c);
        while (w.action != T::kNone && w.targetID != 5) { t.Done(w, true); w = t.Next(c); }
        CHECK(w.action == T::kCreate && w.targetID == 5);
        g[5] = 2;
        CHECK(Upd(t, Bit(5), Bit(5), g));
        t.Done(w, true);
        d = Drain(t);
        CHECK(d.size() == 2 && d[0].action == T::kDestroy && d[1].action == T::kCreate && d[1].gen == 2);
    }

    printf("targets: hotplug thread racing termination (create of 5 and 63 in progress)\n");
    for (uint32_t id : {5u, 63u}) {
        TT t;
        t.Start();
        Upd(t, Bit(0) | Bit(id), Bit(0) | Bit(id));
        uint32_t c = 0;
        T::Work w0 = t.Next(c);
        t.Done(w0, true);
        T::Work w = t.Next(c);                      /* the thread is inside CreateTargetForID(id) */
        CHECK(w.action == T::kCreate && w.targetID == id);
        t.Stop();                                   /* willTerminate: no new work ... */
        CHECK(!Upd(t, Bit(9), Bit(9)));
        uint32_t c2 = 0;
        CHECK(t.Next(c2).action == T::kNone);
        t.Done(w, true);                            /* ... QuiesceHotplug waits for this */
        CHECK(t.Created(id) && !t.Busy());
        const uint64_t destroyed = Terminate(t);    /* family loop + leftovers */
        CHECK(destroyed == (Bit(0) | Bit(id)) && CreatedMask(t) == 0);
    }

    printf("targets: 1024 IDs (MaxTargets + MaxVolumes, D7): 70 devices, ID 1023 destroyed at termination\n");
    {
        TT t(1024);
        t.Start();
        for (uint32_t i = 0; i < 69; i++)
            CHECK(t.Update(i * 3, true));
        CHECK(t.Update(1023, true) && !t.Update(1024, true));
        std::vector<T::Work> w = Drain(t);
        CHECK(w.size() == 70 && w.back().targetID == 1023);
        uint32_t n = 0;
        for (uint32_t i = 0; i < t.Count(); i++)
            n += t.Created(i);
        CHECK(n == 70);
        CHECK(t.Update(204, false) && Drain(t).size() == 1 && !t.Created(204));
        std::vector<uint32_t> all;
        Terminate(t, 1023, &all);                   /* the family loop: 0 .. 1022 */
        CHECK(all.size() == 69 && all.back() == 1023);
        for (uint32_t i = 0; i < t.Count(); i++)
            CHECK(!t.Created(i));
    }

    printf("targets: stop with wishes pending drops them\n");
    {
        TT t;
        t.Start();
        Upd(t, Bit(10) | Bit(11), Bit(10) | Bit(11));
        t.Stop();
        CHECK(!t.Pending() && Drain(t).empty() && Terminate(t) == 0);
    }

    printf("PASS (%d checks)\n", gChecks);
    return 0;
}
