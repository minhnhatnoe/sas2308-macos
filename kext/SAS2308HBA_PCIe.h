/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * SAS2308HBA_PCIe.h - decode the card's PCI Express capability registers for
 * the "PCIe Link" property: the negotiated link (Link Status), the maximum
 * payload and read request sizes in use (Device Control) and the payload size
 * the card supports (Device Capabilities). Plain C++, no IOKit: the class
 * reads the registers (SAS2308HBA_Props.cpp), test/kext_pcie_test.cpp checks
 * the decoding. Offsets are relative to the PCI Express capability (PCIe
 * Base Specification 7.5.3).
 */
#ifndef SAS2308HBA_PCIE_H
#define SAS2308HBA_PCIE_H

#include <stdint.h>

namespace SAS2308Pcie {

enum : uint8_t {
    kDevCap   = 0x04,               /* Device Capabilities (32 bit) */
    kDevCtl   = 0x08,               /* Device Control (16 bit) */
    kLinkCap  = 0x0C,               /* Link Capabilities (32 bit) */
    kLinkSta  = 0x12                /* Link Status (16 bit) */
};

struct Link {
    uint8_t  speed;                 /* Current Link Speed code: 1 = 2.5 GT/s (Gen1) ... 5 = 32 GT/s */
    uint8_t  width;                 /* Negotiated Link Width: lanes */
    uint8_t  maxSpeed;              /* Link Capabilities: Max Link Speed code */
    uint8_t  maxWidth;              /* Link Capabilities: Maximum Link Width */
    uint32_t mps;                   /* Device Control: Max_Payload_Size in use, bytes */
    uint32_t mrrs;                  /* Device Control: Max_Read_Request_Size, bytes */
    uint32_t mpsSupported;          /* Device Capabilities: Max_Payload_Size Supported, bytes */
};

/* 128 << code for the 3-bit size fields (codes 6 and 7 are reserved: 0) */
static inline uint32_t SizeBytes(uint32_t code) { return code <= 5 ? 128u << code : 0; }

static inline Link Decode(uint32_t devCap, uint16_t devCtl, uint32_t linkCap, uint16_t linkSta)
{
    Link l;
    l.speed = (uint8_t)(linkSta & 0xF);
    l.width = (uint8_t)((linkSta >> 4) & 0x3F);
    l.maxSpeed = (uint8_t)(linkCap & 0xF);
    l.maxWidth = (uint8_t)((linkCap >> 4) & 0x3F);
    l.mps = SizeBytes((devCtl >> 5) & 0x7);
    l.mrrs = SizeBytes((devCtl >> 12) & 0x7);
    l.mpsSupported = SizeBytes(devCap & 0x7);
    return l;
}

/* "8.0 GT/s (Gen3)"; NULL for a code the specification does not define */
static inline const char *SpeedName(uint8_t code)
{
    switch (code) {
    case 1: return "2.5 GT/s (Gen1)";
    case 2: return "5.0 GT/s (Gen2)";
    case 3: return "8.0 GT/s (Gen3)";
    case 4: return "16.0 GT/s (Gen4)";
    case 5: return "32.0 GT/s (Gen5)";
    case 6: return "64.0 GT/s (Gen6)";
    default: return nullptr;
    }
}

} /* namespace SAS2308Pcie */

#endif /* SAS2308HBA_PCIE_H */
