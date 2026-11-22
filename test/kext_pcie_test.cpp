/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * kext_pcie_test.cpp - the "PCIe Link" decoding (kext/SAS2308HBA_PCIe.h):
 * register values as the SAS2308 behind the JHL7440 shows them, and the
 * edge cases of the size and speed fields.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../kext/SAS2308HBA_PCIe.h"

static int gChecks = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

using namespace SAS2308Pcie;

int main()
{
    printf("pcie: Gen3 x4 of an x8 card, MPS 128 of 4096 supported, MRRS 512\n");
    {
        /* DevCap MPSS 5; DevCtl 0x2810: MRRS 2, no-snoop, relaxed ordering, MPS 0; LinkCap 8 GT/s x8; LinkSta 0x1043 */
        const Link l = Decode(0x00008FC5u, 0x2810, 0x0003F083u, 0x1043);
        CHECK(l.speed == 3 && l.width == 4 && l.maxSpeed == 3 && l.maxWidth == 8);
        CHECK(l.mps == 128 && l.mrrs == 512 && l.mpsSupported == 4096);
        CHECK(strcmp(SpeedName(l.speed), "8.0 GT/s (Gen3)") == 0);
    }
    printf("pcie: every size code, every speed code\n");
    {
        for (uint32_t c = 0; c < 8; c++) {
            const Link l = Decode(c, (uint16_t)((c << 5) | (c << 12)), 0, 0);
            const uint32_t want = c <= 5 ? 128u << c : 0;
            CHECK(l.mps == want && l.mrrs == want && l.mpsSupported == want);
        }
        CHECK(SizeBytes(5) == 4096 && SizeBytes(6) == 0);
        CHECK(!SpeedName(0) && !SpeedName(7) && strcmp(SpeedName(1), "2.5 GT/s (Gen1)") == 0 &&
              strcmp(SpeedName(4), "16.0 GT/s (Gen4)") == 0);
        const Link w = Decode(0, 0, 0x3F5u << 0, (uint16_t)((32 << 4) | 1));   /* x32 at 2.5 GT/s */
        CHECK(w.width == 32 && w.speed == 1 && w.maxSpeed == 5 && w.maxWidth == 0x3F);
    }
    printf("PASS (%d checks)\n", gChecks);
    return 0;
}
