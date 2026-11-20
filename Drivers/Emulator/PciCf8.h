/** @file
  x86 PCI configuration port emulation.

  Copyright (c) 2026, Han Gao <gaohan@iscas.ac.cn>
  SPDX-License-Identifier: LGPL-2.0-or-later
**/
#pragma once
#include <Uefi.h>

EFI_STATUS
PciCf8Init (IN BOOLEAN Enabled);

VOID
PciCf8Cleanup (VOID);

BOOLEAN
PciCf8Access (IN UINT32 Port, IN UINT32 Size, IN OUT UINT32 *Value, IN BOOLEAN Write);

BOOLEAN
PciRootMemoryAccess (IN UINT64 Address, IN UINT32 Size, IN OUT UINT64 *Value, IN BOOLEAN Write);
