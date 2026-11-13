/** @file
  Offline XhciPortLimit regression test. Only modifies a private memory buffer.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKernelLib.h>
#include <UserFile.h>

// KernelReader file helpers are unused by this in-memory test.
EFI_STATUS
OcGetFileData (
  IN  EFI_FILE_PROTOCOL  *File,
  IN  UINT32            Position,
  IN  UINT32            Size,
  OUT UINT8             *Buffer
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
OcGetFileSize (
  IN  EFI_FILE_PROTOCOL  *File,
  OUT UINT32            *Size
  )
{
  return EFI_UNSUPPORTED;
}

int
ENTRY_POINT (
  int   Argc,
  char  *Argv[]
  )
{
  UINT8              *Buffer;
  UINT8              *Original;
  UINT32             Size;
  UINT32             Version;
  UINT32             Index;
  UINT32             Changes;
  UINT32             Major;
  PRELINKED_CONTEXT  Context;
  EFI_STATUS         Status;
  int                Result;

  if (Argc != 2) {
    fprintf (stderr, "Usage: %s BootKernelExtensions.kc\n", Argv[0]);
    return 2;
  }

  Buffer = UserReadFile (Argv[1], &Size);
  if (Buffer == NULL) {
    return 2;
  }

  Original = AllocateCopyPool (Size, Buffer);
  if (Original == NULL) {
    FreePool (Buffer);
    return 2;
  }

  Result  = 1;
  Version = OcKernelReadDarwinVersion (Buffer, Size);
  printf ("Darwin version: %u\n", Version);
  if (!OcMatchDarwinVersion (Version, KERNEL_VERSION (27, 0, 0), KERNEL_VERSION (27, 99, 99))) {
    fprintf (stderr, "This regression test requires the examined Darwin 27 collection\n");
    goto Done;
  }

  // On this fixture only the modern symbol exists. Outside Darwin 27 the
  // existing code must still attempt the legacy second patch and fail.
  for (Major = 25; Major <= 28; ++Major) {
    if (Major == 27) {
      continue;
    }

    Status = PrelinkedContextInit (&Context, Buffer, Size, Size, FALSE);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    Status = PrelinkedContextApplyQuirk (&Context, KernelQuirkXhciPortLimit1, KERNEL_VERSION (Major, 0, 0));
    PrelinkedContextFree (&Context);
    CopyMem (Buffer, Original, Size);
    if (Status != EFI_NOT_FOUND) {
      fprintf (stderr, "Legacy dispatch changed for Darwin %u\n", Major);
      goto Done;
    }
  }

  Status = PrelinkedContextInit (&Context, Buffer, Size, Size, FALSE);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "Cannot initialize kernel collection: %llx\n", (unsigned long long)Status);
    goto Done;
  }

  Status = PrelinkedContextApplyQuirk (&Context, KernelQuirkXhciPortLimit1, Version);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "IOUSBHostFamily quirk failed: %llx\n", (unsigned long long)Status);
    goto FreeContext;
  }

  Status = PrelinkedContextApplyQuirk (&Context, KernelQuirkXhciPortLimit2, Version);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "AppleUSBXHCI quirk failed: %llx\n", (unsigned long long)Status);
    goto FreeContext;
  }

  Changes = 0;
  for (Index = 0; Index < Size; ++Index) {
    if (Buffer[Index] != Original[Index]) {
      printf ("File offset 0x%x: %02x -> %02x\n", Index, Original[Index], Buffer[Index]);
      if ((Original[Index] != 0x0F) || (Buffer[Index] != 0x40)) {
        fprintf (stderr, "Unexpected modification\n");
        goto FreeContext;
      }

      ++Changes;
    }
  }

  if (Changes != 2) {
    fprintf (stderr, "Expected exactly two changed limit bytes, got %u\n", Changes);
    goto FreeContext;
  }

  // The patched find sequence is now absent. Do not hide that failure.
  Status = PrelinkedContextApplyQuirk (&Context, KernelQuirkXhciPortLimit1, Version);
  if (Status != EFI_NOT_FOUND) {
    fprintf (stderr, "A missing host limit must return EFI_NOT_FOUND\n");
    goto FreeContext;
  }

  puts ("PASS: both USB limits patched; missing host pattern reported; no files written");
  Result = 0;

FreeContext:
  PrelinkedContextFree (&Context);
Done:
  FreePool (Original);
  FreePool (Buffer);
  return Result;
}
