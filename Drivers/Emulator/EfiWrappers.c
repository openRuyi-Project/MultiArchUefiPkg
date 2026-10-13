/** @file

    Copyright (c) 2023, Intel Corporation. All rights reserved.<BR>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

**/

#include "Emulator.h"
#include <Protocol/PciIo.h>

/*
 * EfiWrappers modify the UEFI environment presented to non-native
 * (emulated) code. Attempts to execute certain native functions
 * get redirected through custom implementations or denied.
 *
 * This is different from EfiHooks, in that EfiHooks affect
 * the UEFI environment presented to *all* code (including ourselves).
 */

#ifdef MAU_WRAPPED_ENTRY_POINTS
STATIC LIST_ENTRY  mEventList;

typedef struct {
  LIST_ENTRY          Link;
  EFI_EVENT           Event;
  VOID                *X64NotifyContext;
  EFI_EVENT_NOTIFY    X64NotifyFunction;
  UINT64              CallerProgramCounter;
  CpuContext          *Cpu;
} WRAPPED_EVENT_RECORD;

STATIC
VOID
EfiWrappersDumpEvents (
  )
{
  LIST_ENTRY            *Entry;
  WRAPPED_EVENT_RECORD  *Record;
  ImageRecord           *Image;

  DEBUG ((DEBUG_ERROR, "Wrapped EFI_EVENTs:\n"));
  for (Entry = GetFirstNode (&mEventList);
       !IsNull (&mEventList, Entry);
       Entry = GetNextNode (&mEventList, Entry))
  {
    Record = BASE_CR (Entry, WRAPPED_EVENT_RECORD, Link);
    Image  = ImageFindByAddress (Record->CallerProgramCounter);

    DEBUG ((
      DEBUG_ERROR,
      "\t%7a ImageBase 0x%lx Event %p Fn %p Context %p\n",
      Image->Cpu->Name,
      Image->ImageBase,
      Record->Event,
      Record->X64NotifyFunction,
      Record->X64NotifyContext
      ));
  }
}

STATIC
VOID
EFIAPI
EfiWrappersEventNotify (
  IN  EFI_EVENT  Event,
  IN  VOID       *Context
  )
{
  WRAPPED_EVENT_RECORD  *Record = Context;
  UINT64                Args[2] = { (UINT64)Event, (UINT64)Record->X64NotifyContext };

  CpuRunFunc (Record->Cpu, (UINT64)Record->X64NotifyFunction, Args);
}

STATIC
WRAPPED_EVENT_RECORD *
EfiWrappersFindEvent (
  IN  EFI_EVENT  Event
  )
{
  LIST_ENTRY            *Entry;
  WRAPPED_EVENT_RECORD  *Record;

  for (Entry = GetFirstNode (&mEventList);
       !IsNull (&mEventList, Entry);
       Entry = GetNextNode (&mEventList, Entry))
  {
    Record = BASE_CR (Entry, WRAPPED_EVENT_RECORD, Link);

    if (Record->Event == Event) {
      return Record;
    }
  }

  return NULL;
}

EFI_STATUS
EfiWrapperCloseEvent (
  IN  UINT64  OriginalProgramCounter,
  IN  UINT64  ReturnAddress,
  IN  UINT64  *Args
  )
{
  EFI_EVENT             Event = (EFI_EVENT)Args[0];
  WRAPPED_EVENT_RECORD  *Record;
  EFI_STATUS            Status;

  Record = EfiWrappersFindEvent (Event);
  Status = gBS->CloseEvent (Record->Event);

  if (Record != NULL) {
    CriticalBegin ();
    RemoveEntryList (&Record->Link);
    CriticalEnd ();
    FreePool (Record);
  }

  return Status;
}

EFI_STATUS
EfiWrapperCreateEventCommon (
  IN  UINT64  OriginalProgramCounter,
  IN  UINT64  ReturnAddress,
  IN  UINT64  *Args
  )
{
  UINT32                Type           = Args[0];
  EFI_TPL               NotifyTpl      = Args[1];
  EFI_EVENT_NOTIFY      NotifyFunction = (VOID *)Args[2];
  VOID                  *NotifyContext = (VOID *)Args[3];
  CONST EFI_GUID        *EventGroup    = NULL;
  EFI_EVENT             *Event;
  WRAPPED_EVENT_RECORD  *Record;
  EFI_STATUS            Status;

  if (OriginalProgramCounter == (UINT64)gBS->CreateEvent) {
    Event = (VOID *)Args[4];
  } else {
    ASSERT (OriginalProgramCounter == (UINT64)gBS->CreateEventEx);
    EventGroup = (VOID *)Args[4];
    Event      = (VOID *)Args[5];
  }

  Record = AllocatePool (sizeof *Record);
  if (Record == NULL) {
    DEBUG ((DEBUG_ERROR, "failed to allocate event wrapper\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  Record->Cpu                  = CpuGetTopContext ()->Cpu;
  Record->CallerProgramCounter = ReturnAddress;
  Record->X64NotifyContext     = NotifyContext;
  Record->X64NotifyFunction    = NotifyFunction;
  NotifyFunction               = EfiWrappersEventNotify;
  NotifyContext                = Record;

  /*
   * Before CreateEvent to avoid races! After CreateEvent succeeds,
   * the event could be signalled (and closed from notification fn).
   */
  CriticalBegin ();
  InsertTailList (&mEventList, &Record->Link);
  CriticalEnd ();

  if (OriginalProgramCounter == (UINT64)gBS->CreateEvent) {
    Status = gBS->CreateEvent (
                    Type,
                    NotifyTpl,
                    NotifyFunction,
                    NotifyContext,
                    &Record->Event
                    );
  } else {
    Status = gBS->CreateEventEx (
                    Type,
                    NotifyTpl,
                    NotifyFunction,
                    NotifyContext,
                    EventGroup,
                    &Record->Event
                    );
  }

  if (EFI_ERROR (Status)) {
    *Event = NULL;
    FreePool (Record);
    return Status;
  }

  *Event = Record->Event;
  return EFI_SUCCESS;
}

#endif /* MAU_WRAPPED_ENTRY_POINTS */

typedef struct {
  LIST_ENTRY                    Link;
  EFI_PCI_IO_PROTOCOL_IO_MEM    Functions[4];
} PCI_IO_ACCESS_RECORD;

STATIC LIST_ENTRY  mPciIoAccessList = INITIALIZE_LIST_HEAD_VARIABLE (mPciIoAccessList);

STATIC
EFI_STATUS
EfiWrapperPciIoAccess (
  IN UINT64  OriginalProgramCounter,
  IN UINT64  ReturnAddress,
  IN UINT64  *Args
  )
{
  EFI_PCI_IO_PROTOCOL_IO_MEM  Access;

  Access = (EFI_PCI_IO_PROTOCOL_IO_MEM)(UINTN)OriginalProgramCounter;
  return Access (
           (EFI_PCI_IO_PROTOCOL *)(UINTN)Args[0],
           (EFI_PCI_IO_PROTOCOL_WIDTH)(UINT32)Args[1],
           (UINT8)Args[2],
           Args[3],
           (UINTN)Args[4],
           (VOID *)(UINTN)Args[5]
           );
}

STATIC
EFI_STATUS
EfiWrappersRegisterPciIo (
  IN EFI_PCI_IO_PROTOCOL  *PciIo
  )
{
  PCI_IO_ACCESS_RECORD        *Record;
  LIST_ENTRY                  *Link;
  EFI_PCI_IO_PROTOCOL_IO_MEM  Functions[4] = {
    PciIo->Mem.Read, PciIo->Mem.Write, PciIo->Io.Read, PciIo->Io.Write
  };

  // Keep function addresses, not pointers into a removable controller object.
  for (Link = GetFirstNode (&mPciIoAccessList);
       !IsNull (&mPciIoAccessList, Link);
       Link = GetNextNode (&mPciIoAccessList, Link))
  {
    Record = BASE_CR (Link, PCI_IO_ACCESS_RECORD, Link);
    if (CompareMem (Record->Functions, Functions, sizeof (Functions)) == 0) {
      return EFI_SUCCESS;
    }
  }

  Record = AllocatePool (sizeof (*Record));
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record->Functions, Functions, sizeof (Functions));
  InsertTailList (&mPciIoAccessList, &Record->Link);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EfiWrapperHandleProtocol (
  IN UINT64  OriginalProgramCounter,
  IN UINT64  ReturnAddress,
  IN UINT64  *Args
  )
{
  EFI_HANDLE_PROTOCOL  HandleProtocol;
  EFI_GUID             *Protocol   = (EFI_GUID *)(UINTN)Args[1];
  VOID                 **Interface = (VOID **)(UINTN)Args[2];
  EFI_STATUS           Status;

  HandleProtocol = (EFI_HANDLE_PROTOCOL)(UINTN)OriginalProgramCounter;
  Status         = HandleProtocol ((EFI_HANDLE)(UINTN)Args[0], Protocol, Interface);
  if (!EFI_ERROR (Status) && CompareGuid (Protocol, &gEfiPciIoProtocolGuid)) {
    Status = EfiWrappersRegisterPciIo (*Interface);
  }

  return Status;
}

STATIC
EFI_STATUS
EfiWrapperLocateProtocol (
  IN UINT64  OriginalProgramCounter,
  IN UINT64  ReturnAddress,
  IN UINT64  *Args
  )
{
  EFI_LOCATE_PROTOCOL  LocateProtocol;
  EFI_GUID             *Protocol   = (EFI_GUID *)(UINTN)Args[0];
  VOID                 **Interface = (VOID **)(UINTN)Args[2];
  EFI_STATUS           Status;

  LocateProtocol = (EFI_LOCATE_PROTOCOL)(UINTN)OriginalProgramCounter;
  Status         = LocateProtocol (Protocol, (VOID *)(UINTN)Args[1], Interface);
  if (!EFI_ERROR (Status) && CompareGuid (Protocol, &gEfiPciIoProtocolGuid)) {
    Status = EfiWrappersRegisterPciIo (*Interface);
  }

  return Status;
}

/*
 * x64 callers may leave the upper half of a UINT32 argument unspecified.
 * RISC-V callees require narrow integer arguments extended to XLEN. Calling
 * through the declared prototype supplies that extension without truncating
 * the handles or pointers in the other argument slots.
 */
STATIC
EFI_STATUS
EfiWrapperOpenProtocol (
  IN UINT64  OriginalProgramCounter,
  IN UINT64  ReturnAddress,
  IN UINT64  *Args
  )
{
  EFI_OPEN_PROTOCOL    OpenProtocol;
  EFI_GUID             *Protocol = (EFI_GUID *)(UINTN)Args[1];
  EFI_PCI_IO_PROTOCOL  *PciIo;
  EFI_STATUS           Status;

  // Discover the native access functions before acquiring BY_DRIVER ownership.
  // A failed registration must not leave the controller claimed by the driver.
  if ((Protocol != NULL) && CompareGuid (Protocol, &gEfiPciIoProtocolGuid) &&
      ((UINT32)Args[5] != EFI_OPEN_PROTOCOL_TEST_PROTOCOL))
  {
    Status = gBS->HandleProtocol ((EFI_HANDLE)(UINTN)Args[0], Protocol, (VOID **)&PciIo);
    if (!EFI_ERROR (Status)) {
      Status = EfiWrappersRegisterPciIo (PciIo);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  OpenProtocol = (EFI_OPEN_PROTOCOL)(UINTN)OriginalProgramCounter;
  return OpenProtocol (
           (EFI_HANDLE)(UINTN)Args[0],
           Protocol,
           (VOID **)(UINTN)Args[2],
           (EFI_HANDLE)(UINTN)Args[3],
           (EFI_HANDLE)(UINTN)Args[4],
           (UINT32)Args[5]
           );
}

UINT64
EfiWrappersOverride (
  IN  UINT64  ProgramCounter
  )
{
  LIST_ENTRY            *Link;
  PCI_IO_ACCESS_RECORD  *Record;

  if (ProgramCounter == (UINT64)gBS->OpenProtocol) {
    return (UINT64)EfiWrapperOpenProtocol;
  }

  if (ProgramCounter == (UINT64)gBS->HandleProtocol) {
    return (UINT64)EfiWrapperHandleProtocol;
  }

  if (ProgramCounter == (UINT64)gBS->LocateProtocol) {
    return (UINT64)EfiWrapperLocateProtocol;
  }

  for (Link = GetFirstNode (&mPciIoAccessList);
       !IsNull (&mPciIoAccessList, Link);
       Link = GetNextNode (&mPciIoAccessList, Link))
  {
    Record = BASE_CR (Link, PCI_IO_ACCESS_RECORD, Link);
    for (UINTN Index = 0; Index < ARRAY_SIZE (Record->Functions); Index++) {
      if (ProgramCounter == (UINT64)(UINTN)Record->Functions[Index]) {
        return (UINT64)EfiWrapperPciIoAccess;
      }
    }
  }

 #ifdef MAU_WRAPPED_ENTRY_POINTS
  if (ProgramCounter == (UINT64)gBS->CreateEvent) {
    return (UINT64)EfiWrapperCreateEventCommon;
  } else if (ProgramCounter == (UINT64)gBS->CreateEventEx) {
    return (UINT64)EfiWrapperCreateEventCommon;
  } else if (ProgramCounter == (UINT64)gBS->CloseEvent) {
    return (UINT64)EfiWrapperCloseEvent;
  }

 #endif /* MAU_WRAPPED_ENTRY_POINTS */
  if (ProgramCounter == (UINTN)gBS->ExitBootServices) {
    DEBUG ((
      DEBUG_ERROR,
      "Unsupported emulated ExitBootServices\n"
      ));
    return (UINT64)&NativeUnsupported;
  } else if ((ProgramCounter == (UINTN)gCpu->RegisterInterruptHandler) ||
             (ProgramCounter == (UINTN)gCpu->Init))
  {
    /*
     * TODO: catch/filter gCpu->SetMemoryAttributes to ignore any
     * attempts to change attributes for the emulated image itself?
     */
    /*
     * TODO: this is an awful and high overhead way to trap a
     * protocol. Instead, trap HandleProtocol etc.
     */
    DEBUG ((
      DEBUG_ERROR,
      "Unsupported emulated RegisterInterruptHandler\n"
      ));
    return (UINT64)&NativeUnsupported;
  } else if (ProgramCounter == (UINTN)gBS->Exit) {
    return (UINT64)&CpuExitImage;
  }

  return ProgramCounter;
}

VOID
EfiWrappersInit (
  VOID
  )
{
 #ifdef MAU_WRAPPED_ENTRY_POINTS
  InitializeListHead (&mEventList);
 #endif /* MAU_WRAPPED_ENTRY_POINTS */
}

VOID
EfiWrappersDump (
  VOID
  )
{
 #ifdef MAU_WRAPPED_ENTRY_POINTS
  EfiWrappersDumpEvents ();
 #endif /* MAU_WRAPPED_ENTRY_POINTS */
}
