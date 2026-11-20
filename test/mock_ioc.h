/*
 * mock_ioc.h - software model of the SAS2308 IOC (MPI 2.0 IT firmware) as the
 * host driver sees it: doorbell handshake, diag reset, request/reply queues,
 * SCSI IO with SGL walking (chains), task management, events, faults, fake
 * DMA (IOVA -> host memory) and a virtual clock.
 *
 * Shared by mock_ioc_test.cpp and mps_port_test.cpp (the FreeBSD-shaped
 * core, through the mps_os_* hooks in mps_host_os.cpp).  Include it before any core header; define CHECK(cond)
 * first, and MOCK_SEG to use your own {iova, len} segment type.
 */
#ifndef MOCK_IOC_H
#define MOCK_IOC_H

#ifndef CHECK
#error "define CHECK(cond) before including mock_ioc.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
#include <map>
#include <set>
#include <string>
#include "../core/mps_mpi.h"

#ifdef MOCK_SEG
typedef MOCK_SEG MockSegT;
#else
struct MockSeg { uint64_t iova; uint32_t len; };
typedef MockSeg MockSegT;
#endif

/* Byte `k` of a read transfer, as the model's "disk" returns it. */
static inline uint8_t Pat(uint64_t k, uint8_t seed)
{
    return (uint8_t)(((k + 1) * 2654435761ull) >> 13) ^ seed;
}

struct MockIocModel {
    /* ---- registers ---- */
    uint32_t state = MPI2_IOC_STATE_OPERATIONAL;   /* as left by a boot ROM: forces a MUR */
    uint32_t dbData = 0, his = 0, him = 0;
    bool used = false, dead = false;
    enum { Idle, RxReq, TxReply } hs = Idle;
    unsigned expect = 0;
    std::vector<uint32_t> rx;
    std::vector<uint16_t> tx;
    size_t txi = 0;
    int murCount = 0, ackCount = 0;

    /* ---- diag reset (HostDiagnostic + WriteSequence) ---- */
    int wseq = 0, diagResetCount = 0;
    bool diagWE = false, diagStuck = false;

    /* ---- IO / task management behaviour knobs ---- */
    struct Pending { uint16_t smid, dev; uint8_t lun[8]; std::vector<MockSegT> sgl; };
    std::vector<Pending> pending;              /* IOs the "firmware" holds */
    bool holdIo = false;                       /* keep IOs pending until a TM/reset */
    bool dropCompletion = false;               /* do the IO, lose its completion */
    bool faultOnTm = false;                    /* go to FAULT when a TM arrives */
    bool faultOnEventsOnce = false;            /* FAULT when EVENT_NOTIFICATION is posted (nested fault in reinit) */
    bool faultOnIocInitOnce = false;           /* FAULT instead of answering the IOCInit handshake */
    uint16_t holdDev = 0;                      /* hold only IOs for this handle (0: holdIo decides) */
    uint16_t failDev = 0;                      /* IOs to this handle fail: IO_DATA_ERROR + MEDIUM ERROR sense */
    std::vector<uint16_t> devices = {0x0009};  /* announced by PortEnable */
    uint32_t tmIgnoreMask = 0;                 /* 1 << TaskType: never answer */
    uint32_t tmDeferMask = 0;                  /* 1 << TaskType: answer only on ReleaseTm() */
    std::vector<uint16_t> deferredTm;          /* SMIDs of TMs waiting for ReleaseTm() */
    uint32_t tmNoTermMask = 0;                 /* 1 << TaskType: answer, terminate nothing */
    struct TmRec { uint8_t type, msgFlags, descFlags; uint16_t dev, mid; uint8_t lun[8]; };
    std::vector<TmRec> tms;
    int dropped = 0;                           /* requests posted to a non-operational IOC */
    uint8_t lastLun[8]{};
    uint16_t tmSmidLo = 3, tmSmidHi = 7;       /* TM SMIDs allowed: [lo, hi) */
    bool strictEventMasks = true;              /* EventMasks[1] all ones (M1-M4 core; mpssas registers IR events) */
    uint16_t maxTargets = 0;                   /* IOCFacts.MaxTargets */
    uint32_t iocCaps = 0;                      /* IOCFacts.IOCCapabilities */
    uint32_t lastEventMasks[4]{};             /* last EVENT_NOTIFICATION (1 = masked) */
    int eventRegistrations = 0, portEnables = 0, iocInits = 0, removeDevices = 0;

    /* ---- config pages: SAS Device Page 0 (handle 0x0001 = the HBA itself) ---- */
    static constexpr uint16_t kHostHandle = 0x0001;
    static constexpr uint64_t kHostSasAddr = 0x500605B000001000ull;
    static uint64_t SasAddrOf(uint16_t h) { return h == kHostHandle ? kHostSasAddr : 0x5000C50012340000ull + h; }
    int configHeaders = 0, configReads = 0, configChained = 0;
    uint16_t configFailHandle = 0;             /* page reads for this handle: CONFIG_INVALID_PAGE */
    uint16_t lastConfigHandle = 0;
    bool dropConfigOnce = false;               /* lose the next config reply (wait timeout)*/
    /*
     * Device identity (TF2): SATA devices answer SATA_PASSTHROUGH IDENTIFY
     * with this serial number; SAS devices report sasAddr[h] (default
     * SasAddrOf(h)) in page 0.  A test swaps the disk behind a handle by
     * changing either.
     */
    std::map<uint16_t, std::string> sataSerial;
    std::map<uint16_t, uint64_t> sasAddr;
    int identifies = 0;
    bool dropIdentify = false;                 /* lose IDENTIFY replies */
    std::set<uint16_t> identifyFail;           /* IDENTIFY of these handles fails */
    bool murIgnored = false;                   /* MUR never stops the IOC (wedged) */
    uint64_t deadUntilUs = 0;                  /* registers read all-ones until then */
    /*
     * Hot-plug of several drives (SAS2308 IT, direct attach: one topology
     * list per PHY): the next list is posted when a config page read
     * arrives, i.e. while the host's event task sleeps in it (page buffer lent).
     */
    std::vector<std::vector<uint16_t>> topologyOnConfig;
    std::vector<uint16_t> portEnableList;      /* non-empty: port enable announces only these */
    /* IO Unit Page 7: the IOC temperature */
    uint16_t iocTemp = 52;
    uint8_t  iocTempUnits = MPI2_IOUNITPAGE7_IOC_TEMP_CELSIUS;
    int      iounit7Headers = 0, iounit7Reads = 0;

    /* ---- IOC-side view of the queues (from IOCInit) ---- */
    uint64_t reqBase = 0, postBase = 0, freeBase = 0;
    uint32_t replyHigh = 0, senseHigh = 0, frameSz = 0, pq = 0, fq = 0;
    uint32_t iocPost = 0, iocFree = 0, hostFree = 0, hostPost = 0, postLow = 0;
    static constexpr uint32_t kReplyFrameDwords = 32;
    uint16_t reqFrameDwords = 32;              /* IOCFacts.IOCRequestFrameSize (SAS2308: 32) */
    uint8_t  maxChainDepth = 128;              /* IOCFacts.MaxChainDepth (SAS2308: 128) */

    /* ---- what the last SCSI IO's SGL looked like ---- */
    uint32_t lastChains = 0, maxChainsSeen = 0;
    std::vector<MockSegT> lastSgl;
    std::vector<uint8_t> lastWrite;            /* data the "disk" received */
    uint64_t sglChecked = 0;

    /* ---- fake DMA ---- */
    struct Region { uint64_t iova; uint8_t *p; size_t n; uint8_t *block; };
    std::vector<Region> regions;
    uint64_t nextIova = 0x80000000ull;
    int liveAllocs = 0;

    /* ---- time: virtual, advanced by sleeps and by the test ---- */
    uint64_t nowUs = 1000000;
    uint64_t sleptMs = 0;                      /* total SleepMs() time */
    int sleeps = 0;
    bool abandon = false;                      /* SleepMs() returns false (owner detaching) */
    std::function<void()> onSleep;             /* runs inside SleepMs: "another thread" while the lock is dropped */
    bool inSleepHook = false;


    uint8_t *Ptr(uint64_t iova, size_t n) {
        for (auto &r : regions)
            if (iova >= r.iova && iova + n <= r.iova + r.n) return r.p + (iova - r.iova);
        printf("FAIL: IOC touched unmapped IOVA 0x%llx+%zu (a DART fault on real hardware)\n",
               (unsigned long long)iova, n);
        exit(1);
    }

    /* ---- the IOC's side of the bus (the host's MpsHw / mps_os_* forward here) ---- */
    uint32_t RegRead(uint32_t off) {
        if (dead || nowUs < deadUntilUs) return 0xFFFFFFFF;
        switch (off) {
        case MPI2_DOORBELL_OFFSET: return state | (used ? MPI2_DOORBELL_USED : 0) | dbData;
        case MPI2_HOST_INTERRUPT_STATUS_OFFSET: return his;
        case MPI2_HOST_INTERRUPT_MASK_OFFSET: return him;
        case MPI2_HOST_DIAGNOSTIC_OFFSET:
            return (diagWE ? MPI2_DIAG_DIAG_WRITE_ENABLE : 0) | (diagStuck ? MPI2_DIAG_RESET_ADAPTER : 0);
        default: return 0;
        }
    }
    void RegWrite(uint32_t off, uint32_t v) {
        if (dead) return;
        switch (off) {
        case MPI2_DOORBELL_OFFSET: DoorbellWrite(v); break;
        case MPI2_HOST_INTERRUPT_STATUS_OFFSET: HisClear(); break;
        case MPI2_HOST_INTERRUPT_MASK_OFFSET: him = v; break;
        case MPI2_REPLY_FREE_HOST_INDEX_OFFSET: hostFree = v; break;
        case MPI2_REPLY_POST_HOST_INDEX_OFFSET: hostPost = v; break;
        case MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET: postLow = v; break;
        case MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET: RequestPosted(postLow, v); break;
        case MPI2_WRITE_SEQUENCE_OFFSET: {
            static const uint8_t key[] = { 0x0, 0xF, 0x4, 0xB, 0x2, 0x7, 0xD };
            wseq = (v & 0xF) == key[wseq] ? wseq + 1 : ((v & 0xF) == key[0] ? 1 : 0);
            if (wseq == 7) { diagWE = true; wseq = 0; }
            break;
        }
        case MPI2_HOST_DIAGNOSTIC_OFFSET:
            if ((v & MPI2_DIAG_RESET_ADAPTER) && diagWE) DiagReset();
            break;
        default: break;
        }
    }
    /* Host sleep: virtual time passes and "other threads" (onSleep) run. */
    bool Sleep(uint32_t ms) {
        nowUs += (uint64_t)ms * 1000; sleptMs += ms; sleeps++;
        if (onSleep && !inSleepHook) { inSleepHook = true; onSleep(); inSleepHook = false; }
        return !abandon;
    }
    /* Fake DMA memory: `pieces' > 0 maps it as several IOVA ranges of that size (gaps between). */
    void *DmaAlloc(size_t size, size_t align, uint64_t *iova, size_t pieces = 0,
                   std::vector<uint64_t> *pieceIovas = nullptr) {
        nextIova = (nextIova + align - 1) & ~(uint64_t)(align - 1);
        uint8_t *p = (uint8_t *)calloc(1, size ? size : 1);
        *iova = nextIova;
        if (pieces == 0) {
            regions.push_back({nextIova, p, size, p});
            nextIova += size + 0x1000;
        } else {
            for (size_t off = 0; off < size; off += pieces) {
                size_t n = size - off < pieces ? size - off : pieces;
                regions.push_back({nextIova, p + off, n, p});
                if (pieceIovas) pieceIovas->push_back(nextIova);
                nextIova += n + 0x1000;
                nextIova = (nextIova + align - 1) & ~(uint64_t)(align - 1);
            }
        }
        liveAllocs++;
        return p;
    }
    void DmaFree(void *cpu) {
        if (!cpu) return;
        for (size_t i = 0; i < regions.size();)
            if (regions[i].block == cpu) regions.erase(regions.begin() + i);
            else i++;
        free(cpu);
        liveAllocs--;
    }
    /* A reply descriptor is waiting and the reply interrupt is not masked. */
    bool InterruptPending() const { return pq && iocPost != hostPost && !(him & MPI2_HIM_RIM); }


    /* ---- fault / reset model ---- */
    void Fault(uint16_t code) { state = MPI2_IOC_STATE_FAULT | code; }
    void DiagReset() {
        diagResetCount++; deferredTm.clear();
        diagWE = false; wseq = 0;
        hs = Idle; used = false; dbData = 0; his = 0;
        him |= MPI2_HIM_RIM;
        pending.clear();
        iocPost = iocFree = 0;
        state = diagStuck ? MPI2_IOC_STATE_RESET : MPI2_IOC_STATE_READY;
    }
    void PostRaw(uint64_t v) {
        MPI2_REPLY_DESCRIPTORS_UNION d; memcpy(&d, &v, 8); PostDescriptor(d);
    }
    uint32_t FreeFrameAddr(uint32_t i) {
        uint32_t a; memcpy(&a, Ptr(freeBase + (uint64_t)i * 4, 4), 4); return a;
    }
    /*
     * Walk an SCSI IO SGL the way the firmware does, in FreeBSD's format
     * (mps_push_sge / mps_add_chain): simple 64-bit SGEs from SGLOffset0;
     * where the frame runs out, the last SGE carries LAST_ELEMENT and is
     * followed by a 64-bit chain element (Length = request frame size,
     * NextChainOffset 0, request ChainOffset 0) pointing at the next frame.
     * The final SGE has LAST_ELEMENT|END_OF_BUFFER|END_OF_LIST. Checks every
     * flag, length and address, the chain depth against IOCFacts
     * MaxChainDepth; with `transfer` it moves the data (reads get
     * Pat(k, seed), writes land in lastWrite).
     */
    std::vector<MockSegT> WalkSgl(uint8_t *f, bool transfer, uint32_t *chainsOut) {
        auto *q = (MPI2_SCSI_IO_REQUEST *)f;
        const bool read = q->Control & MPI2_SCSIIO_CONTROL_READ;
        const bool write = q->Control & MPI2_SCSIIO_CONTROL_WRITE;
        const uint8_t seed = q->CDB.CDB32[5];
        std::vector<MockSegT> segs;
        CHECK(q->SGLOffset0 * 4u == offsetof(MPI2_SCSI_IO_REQUEST, SGL));
        CHECK(q->ChainOffset == 0);                 /* FreeBSD: chains found by element type */
        uint8_t *seg = (uint8_t *)&q->SGL, *end = f + frameSz;
        uint32_t chains = 0, inFrame = 0;
        uint64_t k = 0, total = 0;
        bool prevLast = false;
        if (transfer && write) lastWrite.clear();
        for (;;) {
            CHECK(seg + 12 <= end);                 /* every element inside its frame */
            const uint8_t flags = seg[3];
            if ((flags & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_CHAIN_ELEMENT) {
                MPI2_SGE_CHAIN64 ce; memcpy(&ce, seg, sizeof(ce));
                CHECK(ce.Flags == (MPI2_SGE_FLAGS_CHAIN_ELEMENT | MPI2_SGE_FLAGS_64_BIT_ADDRESSING));
                CHECK(ce.Length == frameSz);        /* FreeBSD mps_add_chain: the whole frame */
                CHECK(ce.NextChainOffset == 0);
                CHECK(inFrame == 0 || prevLast);    /* SGE before a chain is LAST_ELEMENT */
                uint64_t a = ((uint64_t)ce.Address.High << 32) | ce.Address.Low;
                CHECK(a % 16 == 0);
                CHECK(++chains <= maxChainDepth);
                seg = Ptr(a, ce.Length);
                end = seg + ce.Length;
                inFrame = 0; prevLast = false;
                continue;
            }
            CHECK((flags & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_SIMPLE_ELEMENT);
            CHECK(flags & MPI2_SGE_FLAGS_64_BIT_ADDRESSING);
            MPI2_SGE_SIMPLE64 sge; memcpy(&sge, seg, sizeof(sge));
            const uint32_t len = sge.FlagsLength & 0xFFFFFF;
            const uint64_t a = ((uint64_t)sge.Address.High << 32) | sge.Address.Low;
            const bool last = flags & MPI2_SGE_FLAGS_LAST_ELEMENT, eol = flags & MPI2_SGE_FLAGS_END_OF_LIST;
            if (eol) {
                CHECK(last && (flags & MPI2_SGE_FLAGS_END_OF_BUFFER));
            } else {
                CHECK(!(flags & MPI2_SGE_FLAGS_END_OF_BUFFER));
                CHECK(len != 0);
                if (last) {                         /* LAST_ELEMENT only right before a chain */
                    CHECK(seg + 24 <= end);
                    CHECK((seg[12 + 3] & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_CHAIN_ELEMENT);
                }
            }
            if (len) {
                CHECK(read || write);
                CHECK(!!(flags & MPI2_SGE_FLAGS_HOST_TO_IOC) == write);
                uint8_t *d = Ptr(a, len);
                if (transfer) {
                    if (read) for (uint32_t i = 0; i < len; i++) d[i] = Pat(k + i, seed);
                    else lastWrite.insert(lastWrite.end(), d, d + len);
                }
                segs.push_back(MockSegT{a, len});
                k += len;
            } else {
                /* the no-data SGE (FreeBSD marks it HOST_TO_IOC) */
                CHECK(eol && segs.empty() && !read && !write);
            }
            total += len;
            inFrame++; prevLast = last;
            if (eol) break;
            seg += 12;
        }
        CHECK(total == q->DataLength);
        *chainsOut = chains;
        sglChecked++;
        return segs;
    }
    static bool SameSgl(const std::vector<MockSegT> &a, const std::vector<MockSegT> &b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].iova != b[i].iova || a[i].len != b[i].len) return false;
        return true;
    }
    /* A held IO's SGL (incl. chain frames) must be intact while the IOC owns it. */
    void Recheck(const Pending &p) {
        uint32_t ch;
        CHECK(SameSgl(WalkSgl(Ptr(reqBase + (uint64_t)p.smid * frameSz, frameSz), false, &ch), p.sgl));
    }
    /* Complete every held IO successfully (the firmware catching up). */
    void ReleasePending() {
        for (const Pending &p : pending) {
            Recheck(p);
            MPI2_REPLY_DESCRIPTORS_UNION d{};
            d.SCSIIOSuccess.ReplyFlags = MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS;
            d.SCSIIOSuccess.SMID = p.smid;
            PostDescriptor(d);
        }
        pending.clear();
    }
    void Terminate(const Pending &p) {
        Recheck(p);
        MPI2_SCSI_IO_REPLY sr{};
        sr.MsgLength = sizeof(sr) / 4; sr.Function = MPI2_FUNCTION_SCSI_IO_REQUEST; sr.DevHandle = p.dev;
        sr.IOCStatus = MPI2_IOCSTATUS_SCSI_TASK_TERMINATED;
        AddressReply(p.smid, &sr, sizeof(sr));
    }

    /* ---- doorbell state machine ---- */
    void DoorbellWrite(uint32_t v) {
        if (hs == Idle) {
            uint8_t fn = v >> MPI2_DOORBELL_FUNCTION_SHIFT;
            if (fn == MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET && murIgnored) {
                murCount++;                            /* wedged: keeps running, keeps its requests */
            } else if (fn == MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET) {
                state = MPI2_IOC_STATE_READY; murCount++;
                pending.clear(); deferredTm.clear();   /* the message unit forgets its requests */
            } else if (fn == MPI2_FUNCTION_HANDSHAKE &&
                       (state & MPI2_IOC_STATE_MASK) != MPI2_IOC_STATE_FAULT) {   /* a faulted IOC does not answer */
                expect = (v & MPI2_DOORBELL_ADD_DWORDS_MASK) >> MPI2_DOORBELL_ADD_DWORDS_SHIFT;
                rx.clear(); hs = RxReq; used = true; his |= MPI2_HIS_IOC2SYS_DB_STATUS;
            }
        } else if (hs == RxReq) {
            rx.push_back(v);
            if (rx.size() == expect) {
                HandshakeRequest();
                if (tx.empty()) { hs = Idle; used = false; return; }   /* faulted instead */
                hs = TxReply; txi = 0; dbData = tx[0]; his |= MPI2_HIS_IOC2SYS_DB_STATUS;
            }
        }
    }
    void HisClear() {
        his = 0;
        if (hs != TxReply) return;
        if (++txi < tx.size()) { dbData = tx[txi]; his |= MPI2_HIS_IOC2SYS_DB_STATUS; }
        else { hs = Idle; used = false; dbData = 0; his |= MPI2_HIS_IOC2SYS_DB_STATUS; }
    }
    void SetTx(const void *reply, size_t bytes) {
        tx.resize(bytes / 2);
        memcpy(tx.data(), reply, bytes);
    }
    void HandshakeRequest() {
        uint8_t fn = ((const uint8_t *)rx.data())[3];
        if (fn == MPI2_FUNCTION_IOC_FACTS) {
            MPI2_IOC_FACTS_REPLY f{};
            f.MsgLength = sizeof(f) / 4;
            f.Function = fn;
            f.MsgVersion = MPI2_VERSION;
            f.RequestCredit = 100;
            f.HighPriorityCredit = 8;
            f.IOCRequestFrameSize = reqFrameDwords;
            f.MaxChainDepth = maxChainDepth;
            f.ReplyFrameSize = kReplyFrameDwords;
            f.MaxReplyDescriptorPostQueueDepth = 128;
            f.FWVersion.Struct.Major = 20;
            f.MaxTargets = maxTargets;
            f.IOCCapabilities = iocCaps;
            SetTx(&f, sizeof(f));
        } else if (fn == MPI2_FUNCTION_IOC_INIT) {
            if (faultOnIocInitOnce) { faultOnIocInitOnce = false; Fault(0x1D00); tx.clear(); return; }
            MPI2_IOC_INIT_REQUEST in{};
            memcpy(&in, rx.data(), sizeof(in) < rx.size() * 4 ? sizeof(in) : rx.size() * 4);
            CHECK(in.WhoInit == MPI2_WHOINIT_HOST_DRIVER);
            frameSz = in.SystemRequestFrameSize * 4;
            pq = in.ReplyDescriptorPostQueueDepth; fq = in.ReplyFreeQueueDepth;
            reqBase = ((uint64_t)in.SystemRequestFrameBaseAddress.High << 32) | in.SystemRequestFrameBaseAddress.Low;
            postBase = ((uint64_t)in.ReplyDescriptorPostQueueAddress.High << 32) | in.ReplyDescriptorPostQueueAddress.Low;
            freeBase = ((uint64_t)in.ReplyFreeQueueAddress.High << 32) | in.ReplyFreeQueueAddress.Low;
            replyHigh = in.SystemReplyAddressHigh; senseHigh = in.SenseBufferAddressHigh;
            iocPost = iocFree = 0;
            iocInits++;
            state = MPI2_IOC_STATE_OPERATIONAL;
            MPI2_IOC_INIT_REPLY r{};
            r.MsgLength = sizeof(r) / 4; r.Function = fn;
            SetTx(&r, sizeof(r));
        } else {
            CHECK(!"unexpected handshake function");
        }
    }

    /* ---- queue model ---- */
    void PostDescriptor(const MPI2_REPLY_DESCRIPTORS_UNION &d) {
        uint8_t *slot = Ptr(postBase + (uint64_t)iocPost * 8, 8);
        uint64_t cur; memcpy(&cur, slot, 8);
        CHECK(cur == ~0ull);                        /* host must have recycled the slot */
        memcpy(slot, &d, 8);
        iocPost = (iocPost + 1) % pq;
    }
    void AddressReply(uint16_t smid, const void *reply, size_t n) {
        CHECK(iocFree != (hostFree + 1) % fq || true);
        CHECK(iocFree != hostFree);                 /* free queue must not be empty */
        uint32_t addr; memcpy(&addr, Ptr(freeBase + (uint64_t)iocFree * 4, 4), 4);
        iocFree = (iocFree + 1) % fq;
        uint8_t *frame = Ptr(((uint64_t)replyHigh << 32) | addr, kReplyFrameDwords * 4);
        memset(frame, 0, kReplyFrameDwords * 4);
        memcpy(frame, reply, n);
        MPI2_REPLY_DESCRIPTORS_UNION d{};
        d.AddressReply.ReplyFlags = MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY;
        d.AddressReply.SMID = smid;
        d.AddressReply.ReplyFrameAddress = addr;
        PostDescriptor(d);
    }
    /* Answer the deferred TMs now (late, possibly after their timeout). */
    void ReleaseTm() {
        std::vector<uint16_t> d; d.swap(deferredTm);
        for (uint16_t smid : d) AnswerTm(Ptr(reqBase + (uint64_t)smid * frameSz, frameSz), smid);
    }
    void AnswerTm(uint8_t *f, uint16_t smid) {
        auto *t = (MPI2_SCSI_TASK_MANAGE_REQUEST *)f;
        uint32_t n = 0;
        if (!(tmNoTermMask & (1u << t->TaskType))) {
            for (size_t i = 0; i < pending.size();) {
                const Pending &pd = pending[i];
                bool match = pd.dev == t->DevHandle &&
                    (t->TaskType == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK ? pd.smid == t->TaskMID :
                     t->TaskType == MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET ? true :
                     memcmp(pd.lun, t->LUN, 8) == 0);
                if (match) { Terminate(pd); pending.erase(pending.begin() + i); n++; }
                else i++;
            }
        }
        MPI2_SCSI_TASK_MANAGE_REPLY tr{};
        tr.MsgLength = sizeof(tr) / 4; tr.Function = MPI2_FUNCTION_SCSI_TASK_MGMT; tr.DevHandle = t->DevHandle;
        tr.TaskType = t->TaskType; tr.ResponseCode = MPI2_SCSITASKMGMT_RSP_TM_COMPLETE;
        tr.TerminationCount = n;
        AddressReply(smid, &tr, sizeof(tr));
    }
    void PostTopologyEvent(const std::vector<uint16_t> &handles,
                           uint8_t status = MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED) {
        uint8_t buf[kReplyFrameDwords * 4]{};
        auto *ev = (MPI2_EVENT_NOTIFICATION_REPLY *)buf;
        ev->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
        ev->Event = MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST;
        ev->AckRequired = MPI2_EVENT_NOTIFICATION_ACK_REQUIRED;
        ev->EventContext = 0x1234;
        ev->EventDataLength = (uint16_t)(3 + handles.size());   /* dwords: 12-byte header + PHY entries */
        auto *t = (MPI2_EVENT_DATA_SAS_TOPOLOGY_CHANGE_LIST *)ev->EventData;
        t->NumEntries = (uint8_t)handles.size();
        t->NumPhys = 8;
        for (size_t i = 0; i < handles.size(); i++) {
            t->PHY[i].AttachedDevHandle = handles[i];
            t->PHY[i].LinkRate = 0xA0;              /* 6.0 Gbit/s */
            t->PHY[i].PhyStatus = status;
        }
        AddressReply(0, buf, sizeof(buf));
    }
    /* IO Unit Page 7 (not extended): header, then READ_CURRENT; one simple SGE. */
    void AnswerIoUnit7(MPI2_CONFIG_REQUEST *q, uint16_t smid) {
        CHECK(q->Header.PageNumber == 7 && q->Header.PageVersion == MPI2_IOUNITPAGE7_PAGEVERSION);
        MPI2_CONFIG_REPLY cr{};
        cr.MsgLength = sizeof(cr) / 4; cr.Function = MPI2_FUNCTION_CONFIG; cr.Action = q->Action;
        cr.Header.PageType = MPI2_CONFIG_PAGETYPE_IO_UNIT; cr.Header.PageNumber = 7;
        cr.Header.PageVersion = MPI2_IOUNITPAGE7_PAGEVERSION;
        cr.Header.PageLength = sizeof(Mpi2IOUnitPage7_t) / 4;
        if (q->Action == MPI2_CONFIG_ACTION_PAGE_HEADER) {
            iounit7Headers++;
            AddressReply(smid, &cr, sizeof(cr));
            return;
        }
        CHECK(q->Action == MPI2_CONFIG_ACTION_PAGE_READ_CURRENT);
        CHECK(q->Header.PageLength == sizeof(Mpi2IOUnitPage7_t) / 4);
        iounit7Reads++;
        MPI2_SGE_SIMPLE64 sge; memcpy(&sge, &q->PageBufferSGE, sizeof(sge));
        if (((sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT) & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_CHAIN_ELEMENT) {
            MPI2_SGE_CHAIN64 ce; memcpy(&ce, &q->PageBufferSGE, sizeof(ce));
            memcpy(&sge, Ptr(((uint64_t)ce.Address.High << 32) | ce.Address.Low, sizeof(sge)), sizeof(sge));
        }
        const uint32_t fl = sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT;
        CHECK((fl & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_SIMPLE_ELEMENT && !(fl & MPI2_SGE_FLAGS_HOST_TO_IOC));
        CHECK((sge.FlagsLength & 0xFFFFFF) == sizeof(Mpi2IOUnitPage7_t));
        Mpi2IOUnitPage7_t pg{};
        pg.Header = cr.Header;
        pg.PCIeWidth = MPI2_IOUNITPAGE7_PCIE_WIDTH_X8; pg.PCIeSpeed = MPI2_IOUNITPAGE7_PCIE_SPEED_8_0_GBPS;
        pg.IOCTemperature = iocTemp; pg.IOCTemperatureUnits = iocTempUnits;
        pg.IOCSpeed = MPI2_IOUNITPAGE7_IOC_SPEED_FULL;
        memcpy(Ptr(((uint64_t)sge.Address.High << 32) | sge.Address.Low, sizeof(pg)), &pg, sizeof(pg));
        AddressReply(smid, &cr, sizeof(cr));
    }
    /* MPI2_FUNCTION_CONFIG: header, then READ_CURRENT of SAS Device Page 0 (FreeBSD mps_config.c). */
    void AnswerConfig(uint8_t *f, uint16_t smid, uint8_t descType) {
        auto *q = (MPI2_CONFIG_REQUEST *)f;
        if (!topologyOnConfig.empty() && q->Action == MPI2_CONFIG_ACTION_PAGE_READ_CURRENT) {
            std::vector<uint16_t> next = topologyOnConfig.front();
            topologyOnConfig.erase(topologyOnConfig.begin());
            PostTopologyEvent(next);
        }
        if (dropConfigOnce) { dropConfigOnce = false; return; }
        CHECK(descType == MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE);
        if (q->Header.PageType == MPI2_CONFIG_PAGETYPE_IO_UNIT) { AnswerIoUnit7(q, smid); return; }
        CHECK(q->Header.PageType == MPI2_CONFIG_PAGETYPE_EXTENDED);
        CHECK(q->ExtPageType == MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE && q->Header.PageNumber == 0);
        MPI2_CONFIG_REPLY cr{};
        cr.MsgLength = sizeof(cr) / 4; cr.Function = MPI2_FUNCTION_CONFIG; cr.Action = q->Action;
        cr.Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED; cr.Header.PageVersion = MPI2_SASDEVICE0_PAGEVERSION;
        cr.ExtPageType = MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE;
        cr.ExtPageLength = sizeof(Mpi2SasDevicePage0_t) / 4;
        if (q->Action == MPI2_CONFIG_ACTION_PAGE_HEADER) {
            configHeaders++;
            AddressReply(smid, &cr, sizeof(cr));
            return;
        }
        CHECK(q->Action == MPI2_CONFIG_ACTION_PAGE_READ_CURRENT);
        CHECK(q->ExtPageLength == sizeof(Mpi2SasDevicePage0_t) / 4);
        CHECK((q->PageAddress & MPI2_SAS_DEVICE_PGAD_FORM_MASK) == MPI2_SAS_DEVICE_PGAD_FORM_HANDLE);
        const uint16_t h = (uint16_t)(q->PageAddress & MPI2_SAS_DEVICE_PGAD_HANDLE_MASK);
        lastConfigHandle = h;
        configReads++;
        /*
         * One simple 64-bit SGE, IOC to host, the page length exactly.  FreeBSD's
         * mps_push_sge puts it in a chain frame: PageBufferSGE (12 bytes) holds a
         * chain element when "segment + chain" does not fit.
         */
        MPI2_SGE_SIMPLE64 sge; memcpy(&sge, &q->PageBufferSGE, sizeof(sge));
        if (((sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT) & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_CHAIN_ELEMENT) {
            MPI2_SGE_CHAIN64 ce; memcpy(&ce, &q->PageBufferSGE, sizeof(ce));
            CHECK(ce.Flags == (MPI2_SGE_FLAGS_CHAIN_ELEMENT | MPI2_SGE_FLAGS_64_BIT_ADDRESSING));
            CHECK(ce.Length == frameSz);
            memcpy(&sge, Ptr(((uint64_t)ce.Address.High << 32) | ce.Address.Low, sizeof(sge)), sizeof(sge));
            configChained++;
        }
        const uint32_t fl = sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT;
        CHECK((fl & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_SIMPLE_ELEMENT);
        CHECK(fl & MPI2_SGE_FLAGS_64_BIT_ADDRESSING);
        CHECK(!(fl & MPI2_SGE_FLAGS_HOST_TO_IOC));
        CHECK((fl & MPI2_SGE_FLAGS_END_OF_LIST) && (fl & MPI2_SGE_FLAGS_LAST_ELEMENT));
        CHECK((sge.FlagsLength & 0xFFFFFF) == sizeof(Mpi2SasDevicePage0_t));
        bool known = h == kHostHandle;
        for (uint16_t d : devices) known |= d == h;
        if (!known || h == configFailHandle) {
            cr.IOCStatus = MPI2_IOCSTATUS_CONFIG_INVALID_PAGE;
            AddressReply(smid, &cr, sizeof(cr));
            return;
        }
        Mpi2SasDevicePage0_t pg{};
        pg.Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED; pg.Header.PageVersion = MPI2_SASDEVICE0_PAGEVERSION;
        pg.Header.ExtPageType = MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE;
        pg.Header.ExtPageLength = sizeof(pg) / 4;
        const uint64_t a = sasAddr.count(h) ? sasAddr[h] : SasAddrOf(h);
        pg.SASAddress.High = (uint32_t)(a >> 32); pg.SASAddress.Low = (uint32_t)a;
        pg.DevHandle = h;
        pg.ParentDevHandle = h == kHostHandle ? 0 : kHostHandle;
        pg.PhyNum = (uint8_t)(h & 7);
        pg.DeviceInfo = h == kHostHandle ?
            (MPI2_SAS_DEVICE_INFO_SSP_INITIATOR | MPI2_SAS_DEVICE_INFO_STP_INITIATOR |
             MPI2_SAS_DEVICE_INFO_SMP_INITIATOR) :
            sataSerial.count(h) ? (MPI2_SAS_DEVICE_INFO_SATA_DEVICE | MPI2_SAS_DEVICE_INFO_STP_TARGET |
                                   MPI2_SAS_DEVICE_INFO_END_DEVICE) :
            (MPI2_SAS_DEVICE_INFO_SSP_TARGET | MPI2_SAS_DEVICE_INFO_END_DEVICE);
        pg.DeviceName.High = 0x5000C500; pg.DeviceName.Low = 0xABC00000u + h;
        memcpy(Ptr(((uint64_t)sge.Address.High << 32) | sge.Address.Low, sizeof(pg)), &pg, sizeof(pg));
        AddressReply(smid, &cr, sizeof(cr));
    }
    /* SATA_PASSTHROUGH IDENTIFY DEVICE (FreeBSD mpssas_get_sata_identify): 512 bytes PIO in. */
    void AnswerIdentify(uint8_t *f, uint16_t smid, uint8_t descType) {
        auto *q = (MPI2_SATA_PASSTHROUGH_REQUEST *)f;
        CHECK(descType == MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE);
        CHECK(q->CommandFIS[0] == 0x27 && q->CommandFIS[1] == 0x80 && q->CommandFIS[2] == 0xEC);
        CHECK(q->DataLength == 512);
        identifies++;
        if (dropIdentify) return;
        MPI2_SGE_SIMPLE64 sge; memcpy(&sge, &q->SGL, sizeof(sge));
        if (((sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT) & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_CHAIN_ELEMENT) {
            MPI2_SGE_CHAIN64 ce; memcpy(&ce, &q->SGL, sizeof(ce));
            memcpy(&sge, Ptr(((uint64_t)ce.Address.High << 32) | ce.Address.Low, sizeof(sge)), sizeof(sge));
        }
        const uint32_t fl = sge.FlagsLength >> MPI2_SGE_FLAGS_SHIFT;
        CHECK((fl & MPI2_SGE_FLAGS_ELEMENT_MASK) == MPI2_SGE_FLAGS_SIMPLE_ELEMENT && !(fl & MPI2_SGE_FLAGS_HOST_TO_IOC));
        CHECK((sge.FlagsLength & 0xFFFFFF) == 512);
        MPI2_SATA_PASSTHROUGH_REPLY r{};
        r.MsgLength = sizeof(r) / 4; r.Function = MPI2_FUNCTION_SATA_PASSTHROUGH; r.DevHandle = q->DevHandle;
        auto it = sataSerial.find(q->DevHandle);
        if (it == sataSerial.end() || identifyFail.count(q->DevHandle)) {
            r.IOCStatus = MPI2_IOCSTATUS_SCSI_DEVICE_NOT_THERE;
        } else {
            uint8_t id[512]{};
            auto put = [&](int word, int nchar, const std::string &str) {   /* ATA strings: byte-swapped words */
                for (int i = 0; i < nchar; i++) {
                    char c = i < (int)str.size() ? str[i] : ' ';
                    id[word * 2 + (i ^ 1)] = (uint8_t)c;
                }
            };
            put(10, 20, it->second);
            put(27, 40, "ST18000NM000J-2TV103");
            id[217 * 2] = 0x20; id[217 * 2 + 1] = 0x1C;          /* 7200 rpm */
            memcpy(Ptr(((uint64_t)sge.Address.High << 32) | sge.Address.Low, 512), id, 512);
        }
        AddressReply(smid, &r, sizeof(r));
    }
    void RequestPosted(uint32_t low, uint32_t) {
        if (state != MPI2_IOC_STATE_OPERATIONAL) { dropped++; return; }   /* faulted: ignored */
        uint16_t smid = low >> 16;
        uint8_t descType = low & MPI2_REQ_DESCRIPT_FLAGS_TYPE_MASK;
        CHECK(smid != 0);
        uint8_t *f = Ptr(reqBase + (uint64_t)smid * frameSz, frameSz);
        uint8_t fn = f[3];
        MPI2_DEFAULT_REPLY r{};
        r.MsgLength = sizeof(r) / 4; r.Function = fn;

        if (fn == MPI2_FUNCTION_EVENT_NOTIFICATION) {
            auto *q = (MPI2_EVENT_NOTIFICATION_REQUEST *)f;
            if (strictEventMasks) {
                CHECK(!(q->EventMasks[0] & (1u << MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST)));
                CHECK(q->EventMasks[1] == 0xFFFFFFFF);
            }
            memcpy(lastEventMasks, q->EventMasks, sizeof(lastEventMasks));
            eventRegistrations++;
            if (faultOnEventsOnce) { faultOnEventsOnce = false; Fault(0x1D00); return; }
            AddressReply(smid, &r, sizeof(r));
        } else if (fn == MPI2_FUNCTION_PORT_ENABLE) {
            portEnables++;
            PostTopologyEvent(portEnableList.empty() ? devices : portEnableList);   /* discovery happens before the reply */
            AddressReply(smid, &r, sizeof(r));
        } else if (fn == MPI2_FUNCTION_EVENT_ACK) {
            auto *a = (MPI2_EVENT_ACK_REQUEST *)f;
            CHECK(a->Event == MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST && a->EventContext == 0x1234);
            ackCount++;
            AddressReply(smid, &r, sizeof(r));
        } else if (fn == MPI2_FUNCTION_SCSI_IO_REQUEST) {
            auto *q = (MPI2_SCSI_IO_REQUEST *)f;
            CHECK((low & 0xFF) == MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO);
            CHECK(q->SGLOffset0 == 24);
            bool known = false;
            for (uint16_t h : devices) known |= h == q->DevHandle;
            CHECK(known);
            nowUs += 37;                            /* the IO takes a little time: nonzero latency */
            lastSgl = WalkSgl(f, true, &lastChains);
            if (lastChains > maxChainsSeen) maxChainsSeen = lastChains;
            memcpy(lastLun, q->LUN, 8);
            if (dropCompletion) return;             /* firmware done, descriptor lost */
            if (failDev && q->DevHandle == failDev) {
                uint8_t *s = Ptr(((uint64_t)senseHigh << 32) | q->SenseBufferLowAddress, 18);
                memset(s, 0, 18); s[0] = 0x70; s[2] = 0x03; s[7] = 10; s[12] = 0x11; s[13] = 0x00;
                MPI2_SCSI_IO_REPLY sr{};
                sr.MsgLength = sizeof(sr) / 4; sr.Function = fn; sr.DevHandle = q->DevHandle;
                sr.IOCStatus = MPI2_IOCSTATUS_SCSI_IO_DATA_ERROR | MPI2_IOCSTATUS_FLAG_LOG_INFO_AVAILABLE;
                sr.IOCLogInfo = 0x31120303;
                sr.SCSIStatus = 0x02; sr.SCSIState = MPI2_SCSI_STATE_AUTOSENSE_VALID; sr.SenseCount = 18;
                AddressReply(smid, &sr, sizeof(sr));
                return;
            }
            if (holdDev ? q->DevHandle == holdDev : holdIo) {
                Pending pd{smid, q->DevHandle, {}, lastSgl};
                memcpy(pd.lun, q->LUN, 8);
                pending.push_back(pd);
                return;
            }
            if (q->CDB.CDB32[0] == 0xA1 || q->CDB.CDB32[0] == 0x85) {
                /*
                 * ATA PASS-THROUGH with CK_COND (SMART): SAT returns the ATA
                 * registers as CHECK CONDITION, descriptor sense, RECOVERED
                 * ERROR, 0x00/0x1D, ATA Status Return descriptor.
                 */
                uint8_t *s = Ptr(((uint64_t)senseHigh << 32) | q->SenseBufferLowAddress, 22);
                memset(s, 0, 22); s[0] = 0x72; s[1] = 0x01; s[2] = 0x00; s[3] = 0x1D; s[7] = 14;
                s[8] = 0x09; s[9] = 0x0C; s[11] = 0x00; s[21] = 0x50;   /* ATA status: DRDY|DSC */
                MPI2_SCSI_IO_REPLY sr{};
                sr.MsgLength = sizeof(sr) / 4; sr.Function = fn; sr.DevHandle = q->DevHandle;
                sr.SCSIStatus = 0x02; sr.SCSIState = MPI2_SCSI_STATE_AUTOSENSE_VALID; sr.SenseCount = 22;
                AddressReply(smid, &sr, sizeof(sr));
            } else if (q->CDB.CDB32[0] == 0x00) {   /* TEST UNIT READY -> CHECK CONDITION */
                uint8_t *s = Ptr(((uint64_t)senseHigh << 32) | q->SenseBufferLowAddress, 18);
                memset(s, 0, 18); s[0] = 0x70; s[2] = 0x02; s[7] = 10; s[12] = 0x04;
                MPI2_SCSI_IO_REPLY sr{};
                sr.MsgLength = sizeof(sr) / 4; sr.Function = fn; sr.DevHandle = q->DevHandle;
                sr.SCSIStatus = 0x02; sr.SCSIState = MPI2_SCSI_STATE_AUTOSENSE_VALID; sr.SenseCount = 18;
                AddressReply(smid, &sr, sizeof(sr));
            } else {
                MPI2_REPLY_DESCRIPTORS_UNION d{};
                d.SCSIIOSuccess.ReplyFlags = MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS;
                d.SCSIIOSuccess.SMID = smid;
                PostDescriptor(d);
            }
        } else if (fn == MPI2_FUNCTION_SCSI_TASK_MGMT) {
            auto *t = (MPI2_SCSI_TASK_MANAGE_REQUEST *)f;
            CHECK(descType == MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY);
            CHECK(smid >= tmSmidLo && smid < tmSmidHi);
            TmRec rec{t->TaskType, t->MsgFlags, descType, t->DevHandle, t->TaskMID, {}};
            memcpy(rec.lun, t->LUN, 8);
            tms.push_back(rec);
            if (faultOnTm) { Fault(0x2622); return; }
            if (tmIgnoreMask & (1u << t->TaskType)) return;
            if (tmDeferMask & (1u << t->TaskType)) { deferredTm.push_back(smid); return; }
            AnswerTm(f, smid);
        } else if (fn == MPI2_FUNCTION_CONFIG) {
            AnswerConfig(f, smid, descType);
        } else if (fn == MPI2_FUNCTION_SATA_PASSTHROUGH) {
            AnswerIdentify(f, smid, descType);
        } else if (fn == MPI2_FUNCTION_SAS_IO_UNIT_CONTROL) {
            auto *q = (MPI2_SAS_IOUNIT_CONTROL_REQUEST *)f;
            CHECK(q->Operation == MPI2_SAS_OP_REMOVE_DEVICE);
            removeDevices++;
            for (size_t i = 0; i < devices.size(); i++)
                if (devices[i] == q->DevHandle) { devices.erase(devices.begin() + i); break; }
            MPI2_SAS_IOUNIT_CONTROL_REPLY cr{};
            cr.MsgLength = sizeof(cr) / 4; cr.Function = fn; cr.DevHandle = q->DevHandle;
            AddressReply(smid, &cr, sizeof(cr));
        } else {
            CHECK(!"unexpected request function");
        }
    }
};

#endif /* MOCK_IOC_H */
