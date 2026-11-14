/** @file
  OpenCore multi-configuration selector and temporary boot arguments editor.

  Unofficial modification. Runs before the configuration file is read, so the
  whole initialisation chain (drivers, NVRAM, ACPI, kernel) is built from the
  configuration the user picked. Boot arguments entered here are applied after
  OcLoadNvramSupport as a volatile NVRAM write, thus affecting the current
  boot only and never touching config.plist.

  Copyright (C) 2026, OpenCore Multi-Config contributors.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>

#include <Guid/AppleVariable.h>
#include <Guid/FileInfo.h>
#include <Guid/OcVariable.h>

#include <Protocol/SimpleTextIn.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcMainLib.h>
#include <Library/OcStorageLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#define OCMC_CONFIG_DIR     L"Configs"
#define OCMC_QUIET_MARKER   L"Configs\\.quiet"
#define OCMC_TRACE_MARKER   L"Configs\\.trace"
#define OCMC_TRACE_FILE     L"ocmc-trace.log"
#define OCMC_MAX_ENTRIES    16
#define OCMC_NAME_LEN       64
#define OCMC_PATH_LEN       128
#define OCMC_ARGS_LEN       512
#define OCMC_MENU_SECONDS   3
#define OCMC_QUIET_POLL_MS  1500
#define OCMC_POLL_STEP_MS   20
#define OCMC_KEY_POLL_MS    250
#define OCMC_ROOT_NAME      L"config"
#define OCMC_DEFAULT_VAR    L"OC-ConfigDefault"
#define OCMC_BUILD_TAG      L"mc4"

#define OCMC_APPLE_BOOT_GUID_STR  "7C436110-AB2A-4BBB-A880-FE41995C9F82"

///
/// Palette of the graphical menu, 0x00RRGGBB.
///
#define OCMC_COLOR_BG       0x080B10
#define OCMC_COLOR_PANEL    0x101822
#define OCMC_COLOR_LINE     0x1E2C3A
#define OCMC_COLOR_ACCENT   0x2FE3C8
#define OCMC_COLOR_ACCENT2  0x1B7F72
#define OCMC_COLOR_FG       0xE6F2F0
#define OCMC_COLOR_DIM      0x5C7684
#define OCMC_COLOR_DARK     0x04120F
#define OCMC_COLOR_WARN     0xFFC24B

///
/// Path of the configuration file to load, relative to EFI/OC.
///
STATIC CHAR16  mConfigPath[OCMC_PATH_LEN];

///
/// Per configuration boot arguments and their edit state.
///
STATIC CHAR16  mArgs[OCMC_MAX_ENTRIES][OCMC_ARGS_LEN];
STATIC BOOLEAN mArgsLoaded[OCMC_MAX_ENTRIES];
STATIC BOOLEAN mArgsEdited[OCMC_MAX_ENTRIES];

///
/// Index of the configuration that was finally chosen.
///
STATIC UINTN   mChosenIndex;
STATIC BOOLEAN mChosenValid;

///
/// Reusable timer event for key waiting.
///
STATIC EFI_EVENT  mKeyTimer;

STATIC BOOLEAN  mTraceEnabled;

///
/// TRUE when the GOP renderer took over the menu.
///
STATIC BOOLEAN  mGfx;

///
/// Storage context kept for tracing after the menu is gone.
///
STATIC OC_STORAGE_CONTEXT  *mStorage;

/**
  Append a line to the trace file when EFI/OC/Configs/.trace exists.

  @param[in] Storage   Storage context of EFI/OC.
  @param[in] Text      ASCII text to append, a newline is added.
**/
STATIC
VOID
OcmTrace (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN CONST CHAR8         *Text
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              Size;
  CHAR8              Buffer[192];

  if (!mTraceEnabled || (Storage == NULL) || (Storage->Storage == NULL)) {
    return;
  }

  File   = NULL;
  Status = Storage->Storage->Open (
                             Storage->Storage,
                             &File,
                             OCMC_TRACE_FILE,
                             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                             0
                             );
  if (EFI_ERROR (Status) || (File == NULL)) {
    return;
  }

  AsciiSPrint (Buffer, sizeof (Buffer), "%a\r\n", Text);
  Size = AsciiStrLen (Buffer);
  File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
  File->Write (File, &Size, Buffer);
  File->Close (File);
}

/**
  Query the console width, falling back to 80 columns.

  @return Number of columns.
**/
STATIC
UINTN
OcmGetColumns (
  VOID
  )
{
  UINTN  Columns;
  UINTN  Rows;

  Columns = 80;
  Rows    = 25;

  if ((gST->ConOut == NULL) || (gST->ConOut->Mode == NULL) || (gST->ConOut->QueryMode == NULL)) {
    return Columns;
  }

  if (EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows))) {
    return 80;
  }

  if ((Columns < 40) || (Columns > 300)) {
    return 80;
  }

  return Columns;
}

/**
  Print a string at the given console row, padding it with spaces up to the
  console width so that leftovers of a longer previous line disappear.

  @param[in] Row      Zero based row.
  @param[in] Text     Text to print.
**/
STATIC
VOID
OcmPrintAt (
  IN UINTN         Row,
  IN CONST CHAR16  *Text
  )
{
  UINTN  Columns;
  UINTN  Length;
  UINTN  Index;

  if ((gST->ConOut == NULL) || (gST->ConOut->SetCursorPosition == NULL)) {
    return;
  }

  Columns = OcmGetColumns ();

  gST->ConOut->SetCursorPosition (gST->ConOut, 0, Row);
  Print (L"%s", Text);

  Length = StrLen (Text);
  for (Index = Length; Index < Columns; ++Index) {
    Print (L" ");
  }
}

/**
  Case insensitive suffix check.

  @param[in] Name     File name.
  @param[in] Suffix   Suffix to match, including the dot.

  @retval TRUE  Name ends with Suffix.
**/
STATIC
BOOLEAN
OcmHasSuffix (
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Suffix
  )
{
  UINTN  NameLen;
  UINTN  SuffixLen;
  UINTN  Index;

  NameLen   = StrLen (Name);
  SuffixLen = StrLen (Suffix);

  if ((NameLen <= SuffixLen) || (SuffixLen == 0)) {
    return FALSE;
  }

  for (Index = 0; Index < SuffixLen; ++Index) {
    if (CharToUpper (Name[NameLen - SuffixLen + Index]) != CharToUpper (Suffix[Index])) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Check whether the given file name must be listed.

  @param[in] Name     File name.

  @retval TRUE  The file must be listed.
**/
STATIC
BOOLEAN
OcmIsConfigName (
  IN CONST CHAR16  *Name
  )
{
  if ((Name == NULL) || (Name[0] == L'\0') || (Name[0] == L'.')) {
    return FALSE;
  }

  return OcmHasSuffix (Name, L".plist");
}

/**
  Insert a name into a sorted list, ignoring duplicates and overflow.

  @param[in,out] Names    List of names.
  @param[in,out] Count    Number of names in the list.
  @param[in]     Name     Name to insert.
**/
STATIC
VOID
OcmInsertName (
  IN OUT CHAR16  (*Names)[OCMC_NAME_LEN],
  IN OUT UINTN   *Count,
  IN     CHAR16  *Name
  )
{
  UINTN  Index;
  UINTN  Position;
  UINTN  Move;

  for (Index = 0; Index < *Count; ++Index) {
    if (StrCmp (Names[Index], Name) == 0) {
      return;
    }
  }

  if (*Count >= OCMC_MAX_ENTRIES) {
    return;
  }

  Position = *Count;
  for (Index = 0; Index < *Count; ++Index) {
    if (StrCmp (Names[Index], Name) > 0) {
      Position = Index;
      break;
    }
  }

  for (Move = *Count; Move > Position; --Move) {
    StrCpyS (Names[Move], OCMC_NAME_LEN, Names[Move - 1]);
  }

  StrCpyS (Names[Position], OCMC_NAME_LEN, Name);
  ++(*Count);
}

/**
  Enumerate configuration files.

  Entry zero is always the stock configuration file in EFI/OC, the remaining
  entries come from EFI/OC/Configs sorted alphabetically.

  @param[in]  Storage   Storage context of EFI/OC.
  @param[out] Names     Names without the .plist extension.
  @param[out] Count     Number of names, at least one on success.

  @retval EFI_SUCCESS   Enumeration finished.
  @retval other         Storage is not usable.
**/
STATIC
EFI_STATUS
OcmListConfigs (
  IN  OC_STORAGE_CONTEXT  *Storage,
  OUT CHAR16              (*Names)[OCMC_NAME_LEN],
  OUT UINTN               *Count
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *Directory;
  EFI_FILE_INFO     *Info;
  UINTN             InfoSize;
  UINTN             BufferSize;
  UINTN             NameLen;

  *Count = 0;

  StrCpyS (Names[0], OCMC_NAME_LEN, OCMC_ROOT_NAME);
  *Count = 1;

  if ((Storage == NULL) || (Storage->Storage == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Directory = NULL;
  Status    = Storage->Storage->Open (
                                Storage->Storage,
                                &Directory,
                                OCMC_CONFIG_DIR,
                                EFI_FILE_MODE_READ,
                                0
                                );
  if (EFI_ERROR (Status) || (Directory == NULL)) {
    return EFI_NOT_FOUND;
  }

  BufferSize = 0x400;
  Info       = AllocatePool (BufferSize);
  if (Info == NULL) {
    Directory->Close (Directory);
    return EFI_OUT_OF_RESOURCES;
  }

  for (;;) {
    InfoSize = BufferSize;
    Status   = Directory->Read (Directory, &InfoSize, Info);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FreePool (Info);
      BufferSize = InfoSize;
      Info       = AllocatePool (BufferSize);
      if (Info == NULL) {
        Directory->Close (Directory);
        return EFI_OUT_OF_RESOURCES;
      }

      continue;
    }

    if (EFI_ERROR (Status) || (InfoSize == 0)) {
      break;
    }

    if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
      continue;
    }

    if (!OcmIsConfigName (Info->FileName)) {
      continue;
    }

    NameLen = StrLen (Info->FileName);
    NameLen -= 6;
    if (NameLen == 0) {
      continue;
    }

    if (NameLen >= OCMC_NAME_LEN) {
      NameLen = OCMC_NAME_LEN - 1;
    }

    Info->FileName[NameLen] = L'\0';
    OcmInsertName (Names, Count, Info->FileName);
  }

  FreePool (Info);
  Directory->Close (Directory);

  return EFI_SUCCESS;
}

/**
  Build a configuration path from a short name.

  @param[in]  Name     Short name, OCMC_ROOT_NAME for the stock file.
  @param[out] Path     Resulting path relative to EFI/OC.
**/
STATIC
VOID
OcmBuildPath (
  IN  CONST CHAR16  *Name,
  OUT CHAR16        *Path
  )
{
  if (StrCmp (Name, OCMC_ROOT_NAME) == 0) {
    StrCpyS (Path, OCMC_PATH_LEN, OPEN_CORE_CONFIG_PATH);
  } else {
    UnicodeSPrint (Path, OCMC_PATH_LEN * sizeof (CHAR16), L"%s\\%s.plist", OCMC_CONFIG_DIR, Name);
  }
}

/**
  Read the remembered default name from NVRAM.

  @param[out] Name     Short name of the default configuration.
**/
STATIC
VOID
OcmLoadDefaultName (
  OUT CHAR16  *Name
  )
{
  UINTN       Size;
  EFI_STATUS  Status;

  StrCpyS (Name, OCMC_NAME_LEN, OCMC_ROOT_NAME);

  Size   = OCMC_NAME_LEN * sizeof (CHAR16);
  Status = gRT->GetVariable (
                  OCMC_DEFAULT_VAR,
                  &gOcVendorVariableGuid,
                  NULL,
                  &Size,
                  Name
                  );
  if (EFI_ERROR (Status) || (Name[0] == L'\0')) {
    StrCpyS (Name, OCMC_NAME_LEN, OCMC_ROOT_NAME);
  }

  Name[OCMC_NAME_LEN - 1] = L'\0';
}

/**
  Store the default name to NVRAM.

  @param[in] Name     Short name of the default configuration.

  @retval EFI_SUCCESS  The name was stored.
**/
STATIC
EFI_STATUS
OcmSaveDefaultName (
  IN CONST CHAR16  *Name
  )
{
  return gRT->SetVariable (
                OCMC_DEFAULT_VAR,
                &gOcVendorVariableGuid,
                EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                StrSize (Name),
                (VOID *)Name
                );
}

/**
  Verify that the configuration file exists and can be parsed.

  @param[in] Storage   Storage context of EFI/OC.
  @param[in] Path      Path relative to EFI/OC.

  @retval TRUE  The file is loadable by OpenCore.
**/
STATIC
BOOLEAN
OcmValidateConfig (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN CONST CHAR16        *Path
  )
{
  CHAR8             *Data;
  UINT32            Size;
  OC_GLOBAL_CONFIG  *Config;
  EFI_STATUS        Status;

  Data = OcStorageReadFileUnicode (Storage, Path, &Size);
  if ((Data == NULL) || (Size == 0)) {
    if (Data != NULL) {
      FreePool (Data);
    }

    return FALSE;
  }

  Config = AllocateZeroPool (sizeof (*Config));
  if (Config == NULL) {
    FreePool (Data);
    return TRUE;
  }

  Status = OcConfigurationInit (Config, Data, Size, NULL);
  OcConfigurationFree (Config);
  FreePool (Config);
  FreePool (Data);

  return (BOOLEAN)(Status == EFI_SUCCESS);
}

/**
  Extract boot-args from a configuration file.

  @param[in]  Storage   Storage context of EFI/OC.
  @param[in]  Path      Path relative to EFI/OC.
  @param[out] Args      Buffer of OCMC_ARGS_LEN characters.

  @retval TRUE  The file parses and contains boot-args.
**/
STATIC
BOOLEAN
OcmReadConfigBootArgs (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  CONST CHAR16        *Path,
  OUT CHAR16              *Args
  )
{
  CHAR8             *Data;
  UINT32            Size;
  OC_GLOBAL_CONFIG  *Config;
  OC_ASSOC          *Map;
  UINTN             GuidIndex;
  UINTN             VarIndex;
  EFI_STATUS        Status;
  BOOLEAN           Found;

  Args[0] = L'\0';
  Found   = FALSE;
  Data    = OcStorageReadFileUnicode (Storage, Path, &Size);
  if ((Data == NULL) || (Size == 0)) {
    if (Data != NULL) {
      FreePool (Data);
    }

    return FALSE;
  }

  Config = AllocateZeroPool (sizeof (*Config));
  if (Config == NULL) {
    FreePool (Data);
    return FALSE;
  }

  Status = OcConfigurationInit (Config, Data, Size, NULL);
  if (!EFI_ERROR (Status)) {
    for (GuidIndex = 0; (GuidIndex < Config->Nvram.Add.Count) && !Found; ++GuidIndex) {
      if (AsciiStriCmp (OC_BLOB_GET (Config->Nvram.Add.Keys[GuidIndex]), OCMC_APPLE_BOOT_GUID_STR) != 0) {
        continue;
      }

      Map = Config->Nvram.Add.Values[GuidIndex];
      for (VarIndex = 0; VarIndex < Map->Count; ++VarIndex) {
        if (AsciiStriCmp (OC_BLOB_GET (Map->Keys[VarIndex]), "boot-args") != 0) {
          continue;
        }

        AsciiStrToUnicodeStrS ((CHAR8 *)OC_BLOB_GET (Map->Values[VarIndex]), Args, OCMC_ARGS_LEN);
        Found = TRUE;
        break;
      }
    }
  }

  OcConfigurationFree (Config);
  FreePool (Config);
  FreePool (Data);

  return Found;
}

/**
  Read the boot arguments currently present in NVRAM.

  @param[out] Args     Buffer of OCMC_ARGS_LEN characters.
**/
STATIC
VOID
OcmLoadNvramBootArgs (
  OUT CHAR16  *Args
  )
{
  CHAR8       Buffer[OCMC_ARGS_LEN];
  UINTN       Size;
  EFI_STATUS  Status;

  Args[0] = L'\0';
  Size    = sizeof (Buffer);

  Status = gRT->GetVariable (
                  L"boot-args",
                  &gAppleBootVariableGuid,
                  NULL,
                  &Size,
                  Buffer
                  );
  if (EFI_ERROR (Status) || (Size == 0) || (Size > sizeof (Buffer))) {
    return;
  }

  Buffer[Size - 1] = '\0';
  AsciiStrToUnicodeStrS (Buffer, Args, OCMC_ARGS_LEN);
}

/**
  Load the boot arguments of a configuration on first use.

  The value is taken from the configuration file itself, so that switching
  entries shows what that entry would use, and falls back to the current
  NVRAM value when the file does not define one.

  @param[in] Storage   Storage context of EFI/OC.
  @param[in] Names     Configuration names.
  @param[in] Index     Configuration index.
**/
STATIC
VOID
OcmEnsureArgs (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN CHAR16              (*Names)[OCMC_NAME_LEN],
  IN UINTN               Index
  )
{
  CHAR16  Path[OCMC_PATH_LEN];

  if ((Index >= OCMC_MAX_ENTRIES) || mArgsLoaded[Index]) {
    return;
  }

  OcmBuildPath (Names[Index], Path);
  if (!OcmReadConfigBootArgs (Storage, Path, mArgs[Index])) {
    OcmLoadNvramBootArgs (mArgs[Index]);
  }

  mArgsLoaded[Index] = TRUE;
}

/**
  Discard pending key strokes, so that editor input starts clean and key
  repeats from menu navigation do not leak into the text field.

  @param[in] Limit   Maximum number of keys to discard.
**/
STATIC
VOID
OcmFlushKeys (
  IN UINTN  Limit
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;

  if ((gST->ConIn == NULL) || (gST->ConIn->ReadKeyStroke == NULL)) {
    return;
  }

  for (Index = 0; Index < Limit; ++Index) {
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      break;
    }
  }
}

/**
  Wait for a key press.

  @param[out] Key         Pressed key.
  @param[in]  TimeoutMs   Timeout in milliseconds.

  @retval EFI_SUCCESS  Key was pressed.
  @retval EFI_TIMEOUT  No key arrived in time.
**/
STATIC
EFI_STATUS
OcmReadKey (
  OUT EFI_INPUT_KEY  *Key,
  IN  UINTN          TimeoutMs
  )
{
  UINTN       Elapsed;
  EFI_STATUS  Status;
  EFI_EVENT   Events[2];
  UINTN       Index;

  if ((gST->ConIn == NULL) || (gST->ConIn->ReadKeyStroke == NULL)) {
    gBS->Stall (TimeoutMs * 1000);
    return EFI_TIMEOUT;
  }

  //
  // Prefer waiting on the event: polling alone loses keys on firmware that
  // only fills the queue while somebody waits for it.
  //
  if ((gST->ConIn->WaitForKey != NULL) && (TimeoutMs > 0)) {
    if ((mKeyTimer == NULL) && EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_APPLICATION, NULL, NULL, &mKeyTimer))) {
      mKeyTimer = NULL;
    }

    if (mKeyTimer != NULL) {
      Events[0] = gST->ConIn->WaitForKey;
      Events[1] = mKeyTimer;
      gBS->SetTimer (mKeyTimer, TimerRelative, (UINT64)TimeoutMs * 10000);
      Status = gBS->WaitForEvent (2, Events, &Index);
      gBS->SetTimer (mKeyTimer, TimerCancel, 0);
      if (EFI_ERROR (Status) || (Index != 0)) {
        return EFI_TIMEOUT;
      }

      return gST->ConIn->ReadKeyStroke (gST->ConIn, Key);
    }
  }

  Elapsed = 0;
  for (;;) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    if (Elapsed >= TimeoutMs) {
      return EFI_TIMEOUT;
    }

    gBS->Stall (OCMC_POLL_STEP_MS * 1000);
    Elapsed += OCMC_POLL_STEP_MS;
  }
}

/**
  Draw text right aligned at the given layout coordinate.

  @param[in] XRight   Right edge in layout units.
  @param[in] Y        Top edge in layout units.
  @param[in] Text     Text to draw.
  @param[in] Rgb      Colour in 0x00RRGGBB form.
  @param[in] Scale    Character scale.
**/
STATIC
VOID
OcmGfxTextRight (
  IN INTN          XRight,
  IN INTN          Y,
  IN CONST CHAR16  *Text,
  IN UINT32        Rgb,
  IN UINTN         Scale
  )
{
  OcmGfxText (XRight - (INTN)OcmGfxTextWidth (Text, Scale), Y, Text, Rgb, Scale);
}

/**
  Draw the configuration menu with the graphical renderer.

  @param[in] Names       Configuration names.
  @param[in] Count       Number of names.
  @param[in] Selected    Selected index.
  @param[in] DefaultIdx  Index of the remembered default.
  @param[in] Seconds     Remaining countdown seconds, 0 when the user interacted.
  @param[in] Message     Optional status message.
**/
STATIC
VOID
OcmDrawMenuGfx (
  IN CHAR16         (*Names)[OCMC_NAME_LEN],
  IN UINTN          Count,
  IN UINTN          Selected,
  IN UINTN          DefaultIdx,
  IN UINTN          Seconds,
  IN CONST CHAR16   *Message
  )
{
  CHAR16  Line[256];
  CHAR16  Path[OCMC_PATH_LEN];
  CHAR16  DefPath[OCMC_PATH_LEN];
  UINTN   Index;
  INTN    Row;

  OcmGfxClear (OCMC_COLOR_BG);

  OcmGfxFill (0, 0, 960, 68, OCMC_COLOR_PANEL);
  OcmGfxFill (0, 66, 960, 2, OCMC_COLOR_ACCENT);
  OcmGfxText (32, 18, L"OCMC  //  MULTI-CONFIG", OCMC_COLOR_FG, 2);

  UnicodeSPrint (Line, sizeof (Line), L"OPENCORE 1.0.8   BUILD %s", OCMC_BUILD_TAG);
  OcmGfxTextRight (928, 26, Line, OCMC_COLOR_DIM, 1);

  for (Index = 0; Index < Count; ++Index) {
    Row = 74 + (INTN)Index * 50;
    OcmBuildPath (Names[Index], Path);
    UnicodeSPrint (Line, sizeof (Line), L"%02u", (UINT32)(Index + 1));

    if (Index == Selected) {
      OcmGfxFill (32, Row, 896, 44, OCMC_COLOR_ACCENT);
      OcmGfxFill (32, Row, 6, 44, OCMC_COLOR_DARK);
      OcmGfxText (56, Row + 14, Line, OCMC_COLOR_DARK, 1);
      OcmGfxText (108, Row + 14, Path, OCMC_COLOR_DARK, 1);
      if (Index == DefaultIdx) {
        OcmGfxTextRight (906, Row + 14, L"DEFAULT", OCMC_COLOR_DARK, 1);
      }
    } else {
      OcmGfxText (56, Row + 14, Line, OCMC_COLOR_DIM, 1);
      OcmGfxText (108, Row + 14, Path, OCMC_COLOR_FG, 1);
      if (Index == DefaultIdx) {
        OcmGfxTextRight (906, Row + 14, L"DEFAULT", OCMC_COLOR_DIM, 1);
      }
    }
  }

  OcmGfxFill (32, 250, 896, 1, OCMC_COLOR_LINE);
  OcmGfxText (32, 268, L"BOOT-ARGS", OCMC_COLOR_DIM, 1);
  OcmGfxText (
    168,
    268,
    mArgsEdited[Selected] ? L"EDITED FOR THIS BOOT" : L"FROM CONFIG",
    OCMC_COLOR_ACCENT,
    1
    );
  OcmGfxTextRight (928, 268, L"[SPACE] EDIT", OCMC_COLOR_ACCENT2, 1);

  OcmGfxFill (32, 316, 896, 1, OCMC_COLOR_LINE);
  OcmGfxText (32, 336, L"SELECTED", OCMC_COLOR_DIM, 1);
  OcmGfxText (168, 336, Path, OCMC_COLOR_FG, 1);
  OcmBuildPath (Names[DefaultIdx], DefPath);
  OcmGfxText (32, 360, L"DEFAULT", OCMC_COLOR_DIM, 1);
  OcmGfxText (168, 360, DefPath, OCMC_COLOR_FG, 1);
  OcmGfxText (32, 384, L"MODE", OCMC_COLOR_DIM, 1);
  OcmGfxText (168, 384, L"TEMPORARY OVERRIDE, VOLATILE NVRAM", OCMC_COLOR_FG, 1);

  if (Message != NULL) {
    OcmGfxText (32, 416, Message, OCMC_COLOR_WARN, 1);
  }

  OcmGfxFill (0, 470, 960, 1, OCMC_COLOR_LINE);
  OcmGfxText (32, 482, L"[UP/DOWN] SELECT", OCMC_COLOR_DIM, 1);
  OcmGfxText (216, 482, L"[ENTER/ESC] BOOT", OCMC_COLOR_DIM, 1);
  OcmGfxText (392, 482, L"[D] SET DEFAULT", OCMC_COLOR_DIM, 1);

  if (Seconds > 0) {
    UnicodeSPrint (Line, sizeof (Line), L"AUTO BOOT %u s", (UINT32)Seconds);
    OcmGfxTextRight (928, 482, Line, OCMC_COLOR_ACCENT, 1);
  }
}

/**
  Draw the configuration menu.

  @param[in] Names       Configuration names.
  @param[in] Count       Number of names.
  @param[in] Selected    Selected index.
  @param[in] DefaultIdx  Index of the remembered default.
  @param[in] Seconds     Remaining countdown seconds, 0 when the user interacted.
  @param[in] Message     Optional status message.
**/
STATIC
VOID
OcmDrawMenu (
  IN CHAR16         (*Names)[OCMC_NAME_LEN],
  IN UINTN          Count,
  IN UINTN          Selected,
  IN UINTN          DefaultIdx,
  IN UINTN          Seconds,
  IN CONST CHAR16   *Message
  )
{
  CHAR16  Line[320];
  CHAR16  Path[OCMC_PATH_LEN];
  UINTN   Index;

  if (mGfx) {
    OcmDrawMenuGfx (Names, Count, Selected, DefaultIdx, Seconds, Message);
    return;
  }

  if ((gST->ConOut == NULL) || (gST->ConOut->ClearScreen == NULL)) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  gST->ConOut->ClearScreen (gST->ConOut);

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
  UnicodeSPrint (Line, sizeof (Line), L"  OpenCore Multi-Config  (%s)", OCMC_BUILD_TAG);
  OcmPrintAt (1, Line);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
  OcmPrintAt (2, L"  Up/Down select   Enter/ESC boot   SPACE/E edit boot-args   D default");
  OcmPrintAt (3, L"");

  for (Index = 0; Index < Count; ++Index) {
    OcmBuildPath (Names[Index], Path);
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"  %c %s%s",
      (Index == Selected) ? L'>' : L' ',
      Path,
      (Index == DefaultIdx) ? L"   [default]" : L""
      );

    if (Index == Selected) {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY));
    } else {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    }

    OcmPrintAt (5 + Index, Line);
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  OcmPrintAt (5 + Count, L"");

  //
  // Only a short state is shown here: the arguments themselves are long and
  // would wrap over the whole screen, hiding the countdown and the messages.
  //
  UnicodeSPrint (
    Line,
    sizeof (Line),
    L"  boot-args: %s   [SPACE to edit]",
    mArgsEdited[Selected] ? L"edited for this boot" : L"from config"
    );
  OcmPrintAt (6 + Count, Line);

  if (Message != NULL) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
    OcmPrintAt (7 + Count, Message);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  } else {
    OcmPrintAt (7 + Count, L"");
  }

  if (Seconds > 0) {
    UnicodeSPrint (Line, sizeof (Line), L"  Booting default in %u s", (UINT32)Seconds);
    OcmPrintAt (8 + Count, Line);
  } else {
    OcmPrintAt (8 + Count, L"");
  }

  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 9 + Count);
}

/**
  Edit boot arguments with a minimal line editor.

  @param[in,out] Args   Buffer of OCMC_ARGS_LEN characters.

  @retval TRUE  User accepted the value.
  @retval FALSE User cancelled.
**/
STATIC
BOOLEAN
OcmEditBootArgs (
  IN OUT CHAR16  *Args
  )
{
  CHAR16         Line[OCMC_ARGS_LEN + 32];
  UINTN          Length;
  UINTN          Columns;
  UINTN          MaxLen;
  UINTN          Start;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Accepted;

  Length   = StrLen (Args);
  Accepted = FALSE;

  OcmFlushKeys (32);

  if (mGfx) {
    OcmGfxClear (OCMC_COLOR_BG);
    OcmGfxFill (0, 0, 960, 68, OCMC_COLOR_PANEL);
    OcmGfxFill (0, 66, 960, 2, OCMC_COLOR_ACCENT);
    OcmGfxText (32, 18, L"BOOT ARGUMENTS", OCMC_COLOR_FG, 2);
    OcmGfxTextRight (928, 26, L"THIS BOOT ONLY", OCMC_COLOR_DIM, 1);
    OcmGfxText (32, 100, L"ENTER / ESC = SAVE      CTRL+Z = DISCARD", OCMC_COLOR_DIM, 1);
    OcmGfxFill (32, 150, 4, 56, OCMC_COLOR_ACCENT);
    OcmGfxFill (0, 470, 960, 1, OCMC_COLOR_LINE);
    OcmGfxText (32, 482, L"APPLIES TO THIS BOOT ONLY", OCMC_COLOR_DIM, 1);
    OcmGfxTextRight (928, 482, L"VOLATILE NVRAM", OCMC_COLOR_ACCENT, 1);
    MaxLen = 100;
  } else {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    gST->ConOut->ClearScreen (gST->ConOut);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
    OcmPrintAt (1, L"  Boot arguments for this boot");
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
    OcmPrintAt (2, L"  Enter / Esc = save    Ctrl+Z = discard    Backspace = delete");
    MaxLen = 0;
  }

  for (;;) {
    //
    // Keep the field on a single line: show the tail that fits and mark the
    // hidden part with '<', so long argument lists never wrap the screen.
    //
    if (mGfx) {
      Start = 0;
      if (Length + 1 > MaxLen) {
        Start = Length + 1 - MaxLen;
      }

      OcmGfxFill (36, 150, 888, 56, 0x0D151D);
      UnicodeSPrint (Line, sizeof (Line), L"%c %s_", (Start > 0) ? L'<' : L'>', Args + Start);
      OcmGfxText (52, 170, Line, OCMC_COLOR_FG, 1);

      OcmGfxFill (32, 214, 280, 16, OCMC_COLOR_BG);
      UnicodeSPrint (Line, sizeof (Line), L"LEN %u", (UINT32)Length);
      OcmGfxText (48, 214, Line, OCMC_COLOR_DIM, 1);
    } else {
      Columns = OcmGetColumns ();
      MaxLen  = (Columns > 10) ? (Columns - 6) : 40;
      Start   = 0;
      if (Length + 1 > MaxLen) {
        Start = Length + 1 - MaxLen;
      }

      UnicodeSPrint (Line, sizeof (Line), L"  %c %s_", (Start > 0) ? L'<' : L'>', Args + Start);
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY));
      OcmPrintAt (4, Line);
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    }

    if (EFI_ERROR (OcmReadKey (&Key, 30000))) {
      continue;
    }

    if (Key.UnicodeChar == 0x1A) {
      //
      // Ctrl+Z: discard changes and go back.
      //
      Accepted = FALSE;
      break;
    }

    if ((Key.ScanCode == SCAN_ESC) || (Key.UnicodeChar == CHAR_CARRIAGE_RETURN)) {
      //
      // Esc and Enter both save: users expect leaving the field to keep it.
      //
      Accepted = TRUE;
      break;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Length > 0) {
        --Length;
        Args[Length] = L'\0';
      }

      continue;
    }

    if ((Key.UnicodeChar < 0x20) || (Key.UnicodeChar > 0x7E)) {
      continue;
    }

    if (Length + 1 >= OCMC_ARGS_LEN) {
      continue;
    }

    Args[Length] = Key.UnicodeChar;
    ++Length;
    Args[Length] = L'\0';
  }

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));

  return Accepted;
}

/**
  Let the user pick a configuration file and optionally change boot arguments.

  @param[in] Storage   Storage context of EFI/OC.

  @return Path of the configuration file to load, relative to EFI/OC.
**/
CONST CHAR16 *
OcConfigMenuSelect (
  IN OC_STORAGE_CONTEXT  *Storage
  )
{
  CHAR16         Names[OCMC_MAX_ENTRIES][OCMC_NAME_LEN];
  CHAR16         DefaultName[OCMC_NAME_LEN];
  CHAR16         Message[160];
  CHAR8          Trace[160];
  UINTN          Count;
  UINTN          DefaultIdx;
  UINTN          Selected;
  UINTN          Index;
  UINTN          Seconds;
  UINTN          ElapsedMs;
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;
  BOOLEAN        Chosen;

  StrCpyS (mConfigPath, OCMC_PATH_LEN, OPEN_CORE_CONFIG_PATH);
  ZeroMem (mArgsEdited, sizeof (mArgsEdited));
  ZeroMem (mArgsLoaded, sizeof (mArgsLoaded));
  mChosenValid  = FALSE;
  mChosenIndex  = 0;
  mTraceEnabled = FALSE;

  if ((Storage == NULL) || (gST->ConOut == NULL)) {
    return mConfigPath;
  }

  Status = OcmListConfigs (Storage, Names, &Count);
  if (EFI_ERROR (Status) || (Count <= 1)) {
    //
    // No alternative configurations: keep the normal boot path untouched.
    //
    return mConfigPath;
  }

  mTraceEnabled = OcStorageExistsFileUnicode (Storage, OCMC_TRACE_MARKER);
  mStorage      = Storage;
  AsciiSPrint (Trace, sizeof (Trace), "menu start, entries=%u", (UINT32)Count);
  OcmTrace (Storage, Trace);

  OcmLoadDefaultName (DefaultName);
  DefaultIdx = 0;
  for (Index = 0; Index < Count; ++Index) {
    if (StrCmp (Names[Index], DefaultName) == 0) {
      DefaultIdx = Index;
      break;
    }
  }

  if (OcStorageExistsFileUnicode (Storage, OCMC_QUIET_MARKER)) {
    //
    // Quiet mode: only react when Esc is held right after handoff.
    //
    if (EFI_ERROR (OcmReadKey (&Key, OCMC_QUIET_POLL_MS)) || (Key.ScanCode != SCAN_ESC)) {
      OcmBuildPath (Names[DefaultIdx], mConfigPath);
      mChosenIndex = DefaultIdx;
      mChosenValid = TRUE;
      return mConfigPath;
    }
  }

  Selected   = DefaultIdx;
  Seconds    = OCMC_MENU_SECONDS;
  ElapsedMs  = 0;
  Message[0] = L'\0';
  Chosen     = FALSE;

  OcmEnsureArgs (Storage, Names, Selected);
  mGfx = OcmGfxInit ();
  OcmTrace (Storage, mGfx ? "render: gop" : "render: text");
  OcmDrawMenu (Names, Count, Selected, DefaultIdx, Seconds, NULL);

  for (;;) {
    if (EFI_ERROR (OcmReadKey (&Key, OCMC_KEY_POLL_MS))) {
      if ((Seconds > 0) && !Chosen) {
        ElapsedMs += OCMC_KEY_POLL_MS;
        if (ElapsedMs >= 1000) {
          ElapsedMs = 0;
          --Seconds;
          OcmDrawMenu (Names, Count, Selected, DefaultIdx, Seconds, NULL);
          if (Seconds == 0) {
            OcmBuildPath (Names[Selected], mConfigPath);
            break;
          }
        }
      }

      continue;
    }

    //
    // Any key press stops the countdown, the user is in control now.
    //
    Seconds = 0;

    AsciiSPrint (
      Trace,
      sizeof (Trace),
      "key scan=%02x char=%04x sel=%u",
      (UINT32)Key.ScanCode,
      (UINT32)Key.UnicodeChar,
      (UINT32)Selected
      );
    OcmTrace (Storage, Trace);

    if (Key.ScanCode == SCAN_UP) {
      Selected = (Selected == 0) ? (Count - 1) : (Selected - 1);
      OcmEnsureArgs (Storage, Names, Selected);
      OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, NULL);
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % Count;
      OcmEnsureArgs (Storage, Names, Selected);
      OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, NULL);
      continue;
    }

    if ((Key.UnicodeChar >= L'1') && (Key.UnicodeChar <= L'9')) {
      Index = (UINTN)(Key.UnicodeChar - L'1');
      if (Index < Count) {
        Selected = Index;
        OcmEnsureArgs (Storage, Names, Selected);
        OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, NULL);
      }

      continue;
    }

    if ((Key.UnicodeChar == L' ') || (Key.UnicodeChar == L'e') || (Key.UnicodeChar == L'E')) {
      OcmEnsureArgs (Storage, Names, Selected);
      OcmTrace (Storage, "edit enter");
      if (OcmEditBootArgs (mArgs[Selected])) {
        mArgsEdited[Selected] = TRUE;
        UnicodeSPrint (Message, sizeof (Message), L"  Boot args for %s apply to this boot only.", Names[Selected]);
        OcmTrace (Storage, "edit accepted");
      } else {
        Message[0] = L'\0';
        OcmTrace (Storage, "edit cancelled");
      }

      OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, Message);
      continue;
    }

    if ((Key.UnicodeChar == L'd') || (Key.UnicodeChar == L'D')) {
      Status = OcmSaveDefaultName (Names[Selected]);
      if (EFI_ERROR (Status)) {
        UnicodeSPrint (Message, sizeof (Message), L"  Cannot save default: %r", Status);
      } else {
        DefaultIdx = Selected;
        UnicodeSPrint (Message, sizeof (Message), L"  Default saved: %s", Names[Selected]);
      }

      OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, Message);
      continue;
    }

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.ScanCode == SCAN_ESC)) {
      //
      // Enter and Esc both boot the selected entry, so that leaving the menu
      // never silently switches to another configuration.
      //
      OcmBuildPath (Names[Selected], mConfigPath);
      if (OcmValidateConfig (Storage, mConfigPath)) {
        Chosen = TRUE;
        break;
      }

      UnicodeSPrint (Message, sizeof (Message), L"  Cannot parse %s, pick another one.", mConfigPath);
      OcmDrawMenu (Names, Count, Selected, DefaultIdx, 0, Message);
      continue;
    }
  }

  mChosenIndex = Selected;
  mChosenValid = TRUE;

  if (gST->ConOut != NULL) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    gST->ConOut->ClearScreen (gST->ConOut);
  }

  AsciiSPrint (
    Trace,
    sizeof (Trace),
    "chosen=%u edited=%u",
    (UINT32)mChosenIndex,
    (UINT32)(mArgsEdited[mChosenIndex] ? 1 : 0)
    );
  OcmTrace (Storage, Trace);

  DEBUG ((
    DEBUG_INFO,
    "OCMC: configuration %s, boot args %a\n",
    mConfigPath,
    mArgsEdited[mChosenIndex] ? "overridden" : "untouched"
    ));

  return mConfigPath;
}

/**
  Apply the boot arguments entered during configuration selection.

  Must be called after OcLoadNvramSupport, so that the override wins over the
  values configured in the plist. The variable is written as volatile, i.e. it
  is gone after a reboot.
**/
VOID
OcConfigMenuApplyBootArgs (
  VOID
  )
{
  CHAR8       Buffer[OCMC_ARGS_LEN];
  EFI_STATUS  Status;
  UINT32      Attributes;

  if (!mChosenValid || !mArgsEdited[mChosenIndex]) {
    return;
  }

  UnicodeStrToAsciiStrS (mArgs[mChosenIndex], Buffer, sizeof (Buffer));
  Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

  Status = gRT->SetVariable (
                  L"boot-args",
                  &gAppleBootVariableGuid,
                  Attributes,
                  AsciiStrSize (Buffer),
                  Buffer
                  );

  if (EFI_ERROR (Status)) {
    //
    // Some firmware refuses to change the attributes of an existing variable.
    //
    gRT->SetVariable (L"boot-args", &gAppleBootVariableGuid, 0, 0, NULL);
    Status = gRT->SetVariable (
                    L"boot-args",
                    &gAppleBootVariableGuid,
                    Attributes,
                    AsciiStrSize (Buffer),
                    Buffer
                    );
  }

  DEBUG ((
    DEBUG_INFO,
    "OCMC: boot-args override \"%a\" - %r\n",
    Buffer,
    Status
    ));

  if (mTraceEnabled) {
    CHAR8  Line[256];

    AsciiSPrint (Line, sizeof (Line), "apply: status=%r args=%a", Status, Buffer);
    OcmTrace (mStorage, Line);
  }
}
