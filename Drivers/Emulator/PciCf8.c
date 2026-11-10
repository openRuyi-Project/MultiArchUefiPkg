/** @file
  Emulate x86 PCI configuration mechanism #1 through UEFI root bridges.

  Copyright (c) 2026, Han Gao <gaohan@iscas.ac.cn>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.
**/

#include "PciCf8.h"
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/PciRootBridgeIo.h>

#define PCI_CF8_ADDRESS  0xCF8
#define PCI_CF8_DATA     0xCFC
#define PCI_CF8_ENABLE   BIT31

typedef struct {
  LIST_ENTRY                       Link;
  EFI_HANDLE                       Handle;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL   *Io;
} PCI_CF8_ROOT;

STATIC LIST_ENTRY  mRoots = INITIALIZE_LIST_HEAD_VARIABLE (mRoots);
STATIC EFI_EVENT  mRootEvent;
STATIC VOID       *mRegistration;
STATIC UINT32     mAddress;
STATIC EFI_STATUS mRootStatus;

STATIC
VOID
EFIAPI
RootInstalled (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_HANDLE                       Handle;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL   *Io;
  EFI_STATUS                       Status;
  UINTN                            Size;
  LIST_ENTRY                       *Link;
  PCI_CF8_ROOT                     *Root;

  for (;;) {
    Size = sizeof (Handle);
    Status = gBS->LocateHandle (ByRegisterNotify, NULL, mRegistration, &Size, &Handle);
    if (EFI_ERROR (Status)) {
      return;
    }

    Status = gBS->HandleProtocol (Handle, &gEfiPciRootBridgeIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Root = NULL;
    for (Link = GetFirstNode (&mRoots); !IsNull (&mRoots, Link); Link = GetNextNode (&mRoots, Link)) {
      PCI_CF8_ROOT *Existing = BASE_CR (Link, PCI_CF8_ROOT, Link);
      if (Existing->Handle == Handle) {
        Root = Existing;
        break;
      }
    }

    if (Root == NULL) {
      Root = AllocateZeroPool (sizeof (*Root));
      if (Root == NULL) {
        mRootStatus = EFI_OUT_OF_RESOURCES;
        DEBUG ((DEBUG_ERROR, "PCI CF8: cannot record a root bridge\n"));
        return;
      }

      Root->Handle = Handle;
      InsertTailList (&mRoots, &Root->Link);
    }

    Root->Io = Io;
  }
}

VOID
PciCf8Cleanup (
  VOID
  )
{
  if (mRootEvent != NULL) {
    gBS->CloseEvent (mRootEvent);
    mRootEvent = NULL;
  }

  while (!IsListEmpty (&mRoots)) {
    PCI_CF8_ROOT *Root = BASE_CR (GetFirstNode (&mRoots), PCI_CF8_ROOT, Link);
    RemoveEntryList (&Root->Link);
    FreePool (Root);
  }

  mRegistration = NULL;
  mAddress = 0;
}

EFI_STATUS
PciCf8Init (
  IN BOOLEAN Enabled
  )
{
  EFI_STATUS Status;

  // Preserve the explicit read-as-zero/write-ignore port I/O policy.
  if (!Enabled) {
    return EFI_SUCCESS;
  }

  mRootStatus = EFI_SUCCESS;
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, RootInstalled, NULL, &mRootEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (&gEfiPciRootBridgeIoProtocolGuid, mRootEvent, &mRegistration);
  if (!EFI_ERROR (Status)) {
    gBS->SignalEvent (mRootEvent);
    Status = mRootStatus;
  }

  if (EFI_ERROR (Status)) {
    PciCf8Cleanup ();
  }

  return Status;
}

STATIC
BOOLEAN
RootContainsBus (
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL  *Io,
  IN UINT8                          Bus
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Descriptor;
  EFI_STATUS                        Status;

  Status = Io->Configuration (Io, (VOID **)&Descriptor);
  if (EFI_ERROR (Status) || (Descriptor == NULL)) {
    return FALSE;
  }

  // Configuration owns this buffer. Bus assignments can change during enumeration.
  while ((Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) &&
         (Descriptor->Len == sizeof (*Descriptor) - 3)) {
    if ((Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_BUS) &&
        (Descriptor->AddrLen != 0) &&
        (Bus >= Descriptor->AddrRangeMin) && (Bus <= Descriptor->AddrRangeMax)) {
      return TRUE;
    }

    Descriptor++;
  }

  return FALSE;
}

BOOLEAN
PciCf8Access (
  IN UINT32      Port,
  IN UINT32      Size,
  IN OUT UINT32  *Value,
  IN BOOLEAN     Write
  )
{
  LIST_ENTRY                             *Link;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL         *Io;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH    Width;
  EFI_STATUS                             Status;
  UINT8                                  Bus, Device, Function, Register;
  UINT64                                 Address;
  UINT32                                 ReadMask;

  if ((Port < PCI_CF8_ADDRESS) || (Port >= PCI_CF8_DATA + 4)) {
    return FALSE;
  }

  ReadMask = Size == 1 ? MAX_UINT8 : Size == 2 ? MAX_UINT16 : MAX_UINT32;
  if (!Write) {
    *Value = ReadMask;
  }

  // Mechanism #1 uses DWORD access to the address latch.
  if ((Port == PCI_CF8_ADDRESS) && (Size == 4)) {
    if (Write) {
      mAddress = *Value;
    } else {
      *Value = mAddress;
    }

    return TRUE;
  }

  if (((Size != 1) && (Size != 2) && (Size != 4)) ||
      (Port < PCI_CF8_DATA) || (Port + Size > PCI_CF8_DATA + 4) ||
      ((Port & (Size - 1)) != 0) || ((mAddress & PCI_CF8_ENABLE) == 0)) {
    return TRUE;
  }

  Bus      = (UINT8)(mAddress >> 16);
  Device   = (UINT8)((mAddress >> 11) & 0x1f);
  Function = (UINT8)((mAddress >> 8) & 7);
  Register = (UINT8)((mAddress & 0xfc) + Port - PCI_CF8_DATA);
  Address  = EFI_PCI_ADDRESS (Bus, Device, Function, Register);
  Width    = Size == 1 ? EfiPciWidthUint8 : Size == 2 ? EfiPciWidthUint16 : EfiPciWidthUint32;

  // Root bridges are discovered outside the emulation callbacks. No allocations
  // or protocol notifications are needed while the Unicorn engine is running.
  for (Link = GetFirstNode (&mRoots); !IsNull (&mRoots, Link); Link = GetNextNode (&mRoots, Link)) {
    Io = BASE_CR (Link, PCI_CF8_ROOT, Link)->Io;
    if ((Io->SegmentNumber != 0) || !RootContainsBus (Io, Bus)) {
      continue;
    }

    Status = Write ? Io->Pci.Write (Io, Width, Address, 1, Value) :
                     Io->Pci.Read (Io, Width, Address, 1, Value);
    if (EFI_ERROR (Status) && !Write) {
      *Value = ReadMask;
    }

    DEBUG ((DEBUG_VERBOSE, "PCI CF8 %a %02x:%02x.%x reg %02x size %u: %08x (%r)\n",
            Write ? "write" : "read", Bus, Device, Function, Register, Size, *Value, Status));
    break;
  }

  return TRUE;
}
