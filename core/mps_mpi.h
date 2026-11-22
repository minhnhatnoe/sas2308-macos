/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Minh Nhat Nguyen
 *
 * mps_mpi.h - single include point for the MPI2 message headers.
 *
 * The headers in mpi/ are unmodified copies from FreeBSD sys/dev/mps/mpi
 * (BSD-2-Clause, LSI/Avago; core/VENDOR.txt has the upstream commit).
 *
 * mpi2_type.h picks "unsigned long" for U32 unless it recognises the
 * platform, which is 64-bit on arm64 (and x86_64) Darwin. We therefore define the
 * basic types ourselves and tell mpi2_type.h they already exist
 * (that is what the MPI_TYPE_H guard is for).
 */
#ifndef MPS_MPI_H
#define MPS_MPI_H

#include <stdint.h>

#ifndef MPI_TYPE_H
#define MPI_TYPE_H
typedef int8_t   S8;
typedef uint8_t  U8;
typedef int16_t  S16;
typedef uint16_t U16;
typedef int32_t  S32;
typedef uint32_t U32;
typedef struct _S64 { U32 Low; S32 High; } S64;
typedef struct _U64 { U32 Low; U32 High; } U64;
typedef S8  *PS8;
typedef U8  *PU8;
typedef S16 *PS16;
typedef U16 *PU16;
typedef S32 *PS32;
typedef U32 *PU32;
typedef S64 *PS64;
typedef U64 *PU64;
#endif

#include "mpi/mpi2_type.h"
#include "mpi/mpi2.h"
#include "mpi/mpi2_ioc.h"
#include "mpi/mpi2_sas.h"
#include "mpi/mpi2_cnfg.h"
#include "mpi/mpi2_init.h"
#include "mpi/mpi2_tool.h"

/* MPI2 is little-endian on the wire; arm64 macOS is little-endian. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "mps core assumes a little-endian host"
#endif
#define MPS_LE16(x) ((uint16_t)(x))
#define MPS_LE32(x) ((uint32_t)(x))

#endif /* MPS_MPI_H */
