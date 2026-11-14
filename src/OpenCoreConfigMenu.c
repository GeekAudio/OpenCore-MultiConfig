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
#define OCMC_BUILD_TAG      L"mc10"

///
/// Esc as delivered by firmware that reports no scan code, only the
/// control character. Plenty of USB legacy implementations do this.
///
#define OCMC_CHAR_ESC       0x001B

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
///
/// Card geometry, in virtual layout units (the layout is 1920x1080).
///
#define OCM_CARD_W      920
#define OCM_CARD_X      ((1920 - OCM_CARD_W) / 2)
#define OCM_CARD_PAD    44
#define OCM_CONTENT_X   (OCM_CARD_X + OCM_CARD_PAD)
#define OCM_CONTENT_W   (OCM_CARD_W - OCM_CARD_PAD * 2)
#define OCM_ROW_FIRST   128
#define OCM_ROW_STEP    52
#define OCM_ROW_H       44
#define OCM_ROW_SEL     0x0E2A26

///
/// State of what is currently on screen, so that a key press only repaints
/// the parts that actually changed instead of clearing the whole screen.
///
STATIC BOOLEAN  mCardDrawn;
STATIC UINTN    mDrawnCount;
STATIC UINTN    mDrawnSelected;
STATIC UINTN    mDrawnDefault;
STATIC BOOLEAN  mDrawnEdited;
STATIC UINTN    mDrawnSeconds;
STATIC CHAR16   mDrawnMessage[160];

/**
  Total card height for the given number of entries.

  @param[in] Count   Number of entries.

  @return Height in layout units.
**/
STATIC
UINTN
OcmCardHeight (
  IN UINTN  Count
  )
{
  if (Count == 0) {
    Count = 1;
  }

  return OCM_ROW_FIRST + (Count - 1) * OCM_ROW_STEP + OCM_ROW_H + 24 + 22 + 22 + 92;
}

/**
  Draw a single list row.

  @param[in] Names       Configuration names.
  @param[in] Index       Row index.
  @param[in] Selected    Selected index.
  @param[in] DefaultIdx  Index of the remembered default.
  @param[in] CardY       Top edge of the card in layout units.
**/
STATIC
VOID
OcmDrawRowGfx (
  IN CHAR16         (*Names)[OCMC_NAME_LEN],
  IN UINTN          Index,
  IN UINTN          Selected,
  IN UINTN          DefaultIdx,
  IN INTN           CardY
  )
{
  CHAR16  Line[16];
  CHAR16  Path[OCMC_PATH_LEN];
  INTN    Y;

  Y = CardY + OCM_ROW_FIRST + (INTN)Index * OCM_ROW_STEP;
  OcmBuildPath (Names[Index], Path);
  UnicodeSPrint (Line, sizeof (Line), L"%02u", (UINT32)(Index + 1));

  OcmGfxFill (OCM_CARD_X, Y, OCM_CARD_W, OCM_ROW_H, OCMC_COLOR_PANEL);

  if (Index == Selected) {
    OcmGfxFill (OCM_CARD_X, Y, OCM_CARD_W, OCM_ROW_H, OCM_ROW_SEL);
    OcmGfxFill (OCM_CARD_X, Y, 3, OCM_ROW_H, OCMC_COLOR_ACCENT);
    OcmGfxText (OCM_CONTENT_X + 20, Y + 14, Line, OCMC_COLOR_ACCENT, 1);
    OcmGfxText (OCM_CONTENT_X + 92, Y + 14, Path, OCMC_COLOR_FG, 1);
    if (Index == DefaultIdx) {
      OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W - 20, Y + 16, L"LAST USED", OCMC_COLOR_ACCENT2, 1);
    }
  } else {
    OcmGfxText (OCM_CONTENT_X + 20, Y + 14, Line, OCMC_COLOR_DIM, 1);
    OcmGfxText (OCM_CONTENT_X + 92, Y + 14, Path, OCMC_COLOR_FG, 1);
    if (Index == DefaultIdx) {
      OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W - 20, Y + 16, L"LAST USED", OCMC_COLOR_DIM, 1);
    }
  }
}

/**
  Draw the boot-args status line.

  @param[in] Selected    Selected index.
  @param[in] CardY       Top edge of the card.
  @param[in] Count       Number of entries.
**/
STATIC
VOID
OcmDrawStatusGfx (
  IN UINTN  Selected,
  IN INTN   CardY,
  IN UINTN  Count
  )
{
  INTN  Y;

  Y = CardY + OCM_ROW_FIRST + (INTN)(Count - 1) * OCM_ROW_STEP + OCM_ROW_H + 24 + 22;

  OcmGfxFill (OCM_CONTENT_X + 132, Y - 2, 360, 20, OCMC_COLOR_PANEL);
  OcmGfxText (
    OCM_CONTENT_X + 132,
    Y,
    mArgsEdited[Selected] ? L"EDITED FOR THIS BOOT" : L"FROM CONFIG",
    OCMC_COLOR_ACCENT,
    1
    );
}

/**
  Draw the whole card from scratch.

  @param[in] Names       Configuration names.
  @param[in] Count       Number of entries.
  @param[in] Selected    Selected index.
  @param[in] DefaultIdx  Index of the remembered default.
  @param[in] Seconds     Remaining countdown seconds.
  @param[in] Message     Optional status message.
**/
STATIC
VOID
OcmDrawCardGfx (
  IN CHAR16         (*Names)[OCMC_NAME_LEN],
  IN UINTN          Count,
  IN UINTN          Selected,
  IN UINTN          DefaultIdx,
  IN UINTN          Seconds,
  IN CONST CHAR16   *Message
  )
{
  CHAR16  Line[160];
  UINTN   Height;
  INTN    CardY;
  INTN    StatusY;
  INTN    FooterY;
  UINTN   Index;

  Height = OcmCardHeight (Count);
  CardY  = (INTN)((1080 - Height) / 2);

  OcmGfxClear (OCMC_COLOR_BG);

  OcmGfxFill (OCM_CARD_X, (INTN)CardY, OCM_CARD_W, (INTN)Height, OCMC_COLOR_PANEL);
  OcmGfxFill (OCM_CARD_X, (INTN)CardY, OCM_CARD_W, 3, OCMC_COLOR_ACCENT);
  OcmGfxFill (OCM_CARD_X, (INTN)CardY + 3, 1, (INTN)Height - 3, OCMC_COLOR_LINE);
  OcmGfxFill (OCM_CARD_X + OCM_CARD_W - 1, (INTN)CardY + 3, 1, (INTN)Height - 3, OCMC_COLOR_LINE);
  OcmGfxFill (OCM_CARD_X, (INTN)CardY + (INTN)Height - 1, OCM_CARD_W, 1, OCMC_COLOR_LINE);

  OcmGfxText (OCM_CONTENT_X, CardY + 46, L"OCMC  //  MULTI-CONFIG", OCMC_COLOR_FG, 2);
  UnicodeSPrint (Line, sizeof (Line), L"BUILD %s   OPENCORE 1.0.8", OCMC_BUILD_TAG);
  OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W, CardY + 58, Line, OCMC_COLOR_DIM, 1);

  OcmGfxFill (OCM_CONTENT_X, CardY + 100, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);

  for (Index = 0; Index < Count; ++Index) {
    OcmDrawRowGfx (Names, Index, Selected, DefaultIdx, CardY);
  }

  StatusY = CardY + OCM_ROW_FIRST + (INTN)(Count - 1) * OCM_ROW_STEP + OCM_ROW_H + 24;
  OcmGfxFill (OCM_CONTENT_X, StatusY, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);
  OcmGfxText (OCM_CONTENT_X, StatusY + 22, L"BOOT-ARGS", OCMC_COLOR_DIM, 1);
  OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W, StatusY + 23, L"[SPACE] EDIT", OCMC_COLOR_ACCENT2, 1);
  OcmDrawStatusGfx (Selected, CardY, Count);

  if (Message != NULL) {
    OcmGfxText (OCM_CONTENT_X, StatusY + 52, Message, OCMC_COLOR_WARN, 1);
  }

  FooterY = CardY + (INTN)Height - 64;
  OcmGfxFill (OCM_CONTENT_X, FooterY, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);
  OcmGfxText (OCM_CONTENT_X, FooterY + 22, L"[UP/DOWN] SELECT", OCMC_COLOR_DIM, 1);
  OcmGfxText (OCM_CONTENT_X + 260, FooterY + 22, L"[ENTER/ESC] BOOT", OCMC_COLOR_DIM, 1);
  OcmGfxText (OCM_CONTENT_X + 520, FooterY + 22, L"[D] REMEMBER", OCMC_COLOR_DIM, 1);

  mCardDrawn    = TRUE;
  mDrawnCount   = Count;
  mDrawnSelected = Selected;
  mDrawnDefault = DefaultIdx;
  mDrawnEdited  = mArgsEdited[Selected];
  mDrawnSeconds = Seconds;
  if (Message != NULL) {
    StrCpyS (mDrawnMessage, ARRAY_SIZE (mDrawnMessage), Message);
  } else {
    mDrawnMessage[0] = L'\0';
  }
}

/**
  Draw the configuration menu with the graphical renderer, repainting only the
  parts that changed since the previous call.

  @param[in] Names       Configuration names.
  @param[in] Count       Number of entries.
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
  CHAR16  Line[160];
  UINTN   Height;
  INTN    CardY;
  INTN    StatusY;
  INTN    FooterY;

  if (!mCardDrawn || (mDrawnCount != Count)) {
    OcmDrawCardGfx (Names, Count, Selected, DefaultIdx, Seconds, Message);
    return;
  }

  Height  = OcmCardHeight (Count);
  CardY   = (INTN)((1080 - Height) / 2);
  StatusY = CardY + OCM_ROW_FIRST + (INTN)(Count - 1) * OCM_ROW_STEP + OCM_ROW_H + 24;
  FooterY = CardY + (INTN)Height - 64;

  //
  // Selection moved: repaint the two affected rows only.
  //
  if (Selected != mDrawnSelected) {
    OcmDrawRowGfx (Names, mDrawnSelected, Selected, DefaultIdx, CardY);
    OcmDrawRowGfx (Names, Selected, Selected, DefaultIdx, CardY);
    mDrawnSelected = Selected;
  } else if (DefaultIdx != mDrawnDefault) {
    //
    // The default marker moved, repaint the rows that carry it.
    //
    OcmDrawRowGfx (Names, mDrawnDefault, Selected, DefaultIdx, CardY);
    OcmDrawRowGfx (Names, DefaultIdx, Selected, DefaultIdx, CardY);
    mDrawnDefault = DefaultIdx;
  }

  if (mArgsEdited[Selected] != mDrawnEdited) {
    OcmDrawStatusGfx (Selected, CardY, Count);
    mDrawnEdited = mArgsEdited[Selected];
  }

  if (Message != NULL) {
    if (StrCmp (Message, mDrawnMessage) != 0) {
      OcmGfxFill (OCM_CONTENT_X, StatusY + 50, OCM_CONTENT_W, 20, OCMC_COLOR_PANEL);
      OcmGfxText (OCM_CONTENT_X, StatusY + 52, Message, OCMC_COLOR_WARN, 1);
      StrCpyS (mDrawnMessage, ARRAY_SIZE (mDrawnMessage), Message);
    }
  } else if (mDrawnMessage[0] != L'\0') {
    OcmGfxFill (OCM_CONTENT_X, StatusY + 50, OCM_CONTENT_W, 20, OCMC_COLOR_PANEL);
    mDrawnMessage[0] = L'\0';
  }

  if (Seconds != mDrawnSeconds) {
    OcmGfxFill (OCM_CONTENT_X + OCM_CONTENT_W - 176, FooterY + 20, 176, 20, OCMC_COLOR_PANEL);
    if (Seconds > 0) {
      UnicodeSPrint (Line, sizeof (Line), L"AUTO BOOT %u s", (UINT32)Seconds);
      OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W, FooterY + 22, Line, OCMC_COLOR_ACCENT, 1);
    }

    mDrawnSeconds = Seconds;
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
  OcmPrintAt (2, L"  Up/Down select   Enter/ESC boot   SPACE/E edit boot-args   D remember");
  OcmPrintAt (3, L"");

  for (Index = 0; Index < Count; ++Index) {
    OcmBuildPath (Names[Index], Path);
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"  %c %s%s",
      (Index == Selected) ? L'>' : L' ',
      Path,
      (Index == DefaultIdx) ? L"   [last]" : L""
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
    UnicodeSPrint (Line, sizeof (Line), L"  Booting in %u s", (UINT32)Seconds);
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
  UINTN          Cursor;
  UINTN          Start;
  CHAR16         Dbg[48];
  INTN           CardY;
  EFI_INPUT_KEY  Key;
  BOOLEAN        Accepted;

  Length   = StrLen (Args);
  Cursor   = Length;
  Accepted = FALSE;
  Dbg[0]   = L'\0';

  OcmFlushKeys (32);

  if (mGfx) {
    UINTN  Height;
    INTN   FooterY;

    Height = 324;
    CardY  = (INTN)((1080 - Height) / 2);

    OcmGfxClear (OCMC_COLOR_BG);
    OcmGfxFill (OCM_CARD_X, CardY, OCM_CARD_W, (INTN)Height, OCMC_COLOR_PANEL);
    OcmGfxFill (OCM_CARD_X, CardY, OCM_CARD_W, 3, OCMC_COLOR_ACCENT);
    OcmGfxFill (OCM_CARD_X, CardY + 3, 1, (INTN)Height - 3, OCMC_COLOR_LINE);
    OcmGfxFill (OCM_CARD_X + OCM_CARD_W - 1, CardY + 3, 1, (INTN)Height - 3, OCMC_COLOR_LINE);
    OcmGfxFill (OCM_CARD_X, CardY + (INTN)Height - 1, OCM_CARD_W, 1, OCMC_COLOR_LINE);

    OcmGfxText (OCM_CONTENT_X, CardY + 46, L"BOOT ARGUMENTS", OCMC_COLOR_FG, 2);
    OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W, CardY + 58, L"THIS BOOT ONLY", OCMC_COLOR_DIM, 1);
    OcmGfxText (OCM_CONTENT_X, CardY + 96, L"ENTER / ESC = SAVE      CTRL+Z = DISCARD", OCMC_COLOR_DIM, 1);

    OcmGfxFill (OCM_CONTENT_X, CardY + 128, OCM_CONTENT_W, 60, 0x080E14);
    OcmGfxFill (OCM_CONTENT_X, CardY + 128, 3, 60, OCMC_COLOR_ACCENT);
    OcmGfxFill (OCM_CONTENT_X, CardY + 128, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);
    OcmGfxFill (OCM_CONTENT_X, CardY + 187, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);

    FooterY = CardY + (INTN)Height - 64;
    OcmGfxFill (OCM_CONTENT_X, FooterY, OCM_CONTENT_W, 1, OCMC_COLOR_LINE);
    OcmGfxText (OCM_CONTENT_X, FooterY + 22, L"APPLIES TO THIS BOOT ONLY", OCMC_COLOR_DIM, 1);
    OcmGfxTextRight (OCM_CONTENT_X + OCM_CONTENT_W, FooterY + 22, L"VOLATILE NVRAM", OCMC_COLOR_ACCENT, 1);

    MaxLen = (OCM_CONTENT_W - 40) / 8;
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
      //
      // Keep the caret anchored near the middle of the field. It moves freely
      // within the left/right margins; the text scrolls only once the caret
      // reaches the middle band, and scrolling stops at the text ends so the
      // caret can then travel out to the actual edge. Mirrors a normal editor.
      //
      Start = 0;
      if (Length > MaxLen) {
        INTN Mid;
        INTN MaxSt;
        Mid   = (INTN)(MaxLen - 1) / 2;
        MaxSt = (INTN)Length - (INTN)MaxLen + 1;
        if ((INTN)Cursor <= Mid) {
          //
          // Close to the head of the string: the caret moves on its own and the
          // text does not scroll yet.
          //
          Start = 0;
        } else if ((INTN)Cursor >= MaxSt + Mid) {
          //
          // Close to the tail: the caret moves on its own at the right edge.
          //
          Start = MaxSt;
          if (Start < 0) {
            Start = 0;
          }
        } else {
          //
          // Middle: pin the caret at the centre and scroll the text under it.
          //
          Start = (INTN)Cursor - Mid;
        }
      }

      //
      // Visible line. The caret is a solid block drawn over the character at the
      // cursor, with that character redrawn in the field colour so it stays
      // legible -- a thin underscore read as "covering" the letter and was hard
      // to spot when it moved.
      //
      {
        UINTN Di;
        UINTN Si;
        UINTN Idx;
        Di = 0;
        Line[Di++] = (Start > 0) ? L'<' : L' ';
        for (Si = 0; Si < MaxLen - 1; ++Si) {
          Idx = Start + Si;
          if (Idx >= Length) {
            break;
          }
          Line[Di++] = Args[Idx];
        }
        Line[Di] = L'\0';
      }

      OcmGfxFill (OCM_CONTENT_X + 6, CardY + 130, OCM_CONTENT_W - 10, 56, 0x080E14);
      OcmGfxText (OCM_CONTENT_X + 20, CardY + 146, Line, OCMC_COLOR_FG, 1);

      {
        INTN   CellW;
        INTN   Prefix;
        INTN   Cell;
        INTN   Cx;
        INTN   Cy;
        CHAR16 CurCh[2];

        CellW  = (INTN)OcmGfxTextWidth (L"A", 1);
        Prefix = (Start > 0) ? 1 : 0;
        Cell   = (INTN)(Cursor - Start) + Prefix;
        if (Cell < 0) {
          Cell = 0;
        }
        if (Cell > (INTN)(MaxLen - 1)) {
          Cell = (INTN)(MaxLen - 1);
        }

        Cx = OCM_CONTENT_X + 20 + Cell * CellW;
        Cy = CardY + 146;

        OcmGfxFill (Cx, Cy - 2, CellW, 20, OCMC_COLOR_ACCENT);
        CurCh[0] = (Cursor < Length) ? Args[Cursor] : L' ';
        CurCh[1] = L'\0';
        OcmGfxText (Cx, Cy, CurCh, 0x080E14, 1);
      }

      OcmGfxFill (OCM_CONTENT_X, CardY + 208, 260, 20, OCMC_COLOR_PANEL);
      UnicodeSPrint (Line, sizeof (Line), L"LEN %u", (UINT32)Length);
      OcmGfxText (OCM_CONTENT_X, CardY + 212, Line, OCMC_COLOR_DIM, 1);

      if (mTraceEnabled) {
        OcmGfxFill (OCM_CONTENT_X, CardY + 236, OCM_CONTENT_W, 20, OCMC_COLOR_PANEL);
        OcmGfxText (OCM_CONTENT_X, CardY + 240, Dbg, OCMC_COLOR_ACCENT, 1);
      }
    } else {
      Columns = OcmGetColumns ();
      MaxLen  = (Columns > 10) ? (Columns - 6) : 40;
      Start   = 0;
      if (Length > MaxLen) {
        {
          INTN Mid;
          INTN MaxSt;
          Mid   = (INTN)(MaxLen - 1) / 2;
          MaxSt = (INTN)Length - (INTN)MaxLen + 1;
          if ((INTN)Cursor <= Mid) {
            Start = 0;
          } else if ((INTN)Cursor >= MaxSt + Mid) {
            Start = MaxSt;
            if (Start < 0) {
              Start = 0;
            }
          } else {
            Start = (INTN)Cursor - Mid;
          }
        }
      }

      {
        UINTN Di;
        UINTN Si;
        UINTN Idx;
        Di = 0;
        Line[Di++] = (Start > 0) ? L'<' : L' ';
        for (Si = 0; Si < MaxLen - 1; ++Si) {
          Idx = Start + Si;
          if (Idx >= Length) {
            break;
          }
          if (Idx == Cursor) {
            Line[Di++] = L'_';
          }
          Line[Di++] = Args[Idx];
        }
        if (Cursor >= Length) {
          Line[Di++] = L'_';
        }
        Line[Di] = L'\0';
      }

      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY));
      OcmPrintAt (4, Line);
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));

      if (mTraceEnabled) {
        gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_RED, EFI_BLACK));
        OcmPrintAt (6, Dbg);
        gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
      }
    }

    if (EFI_ERROR (OcmReadKey (&Key, 30000))) {
      continue;
    }

    if (mTraceEnabled) {
      UnicodeSPrint (
        Dbg,
        sizeof (Dbg),
        L"scan=%02X char=%04X cur=%u len=%u",
        (UINT32)Key.ScanCode,
        (UINT32)Key.UnicodeChar,
        (UINT32)Cursor,
        (UINT32)Length
        );
    }

    if (Key.UnicodeChar == 0x1A) {
      //
      // Ctrl+Z: discard changes and go back.
      //
      Accepted = FALSE;
      break;
    }

    if ((Key.ScanCode == SCAN_ESC) || (Key.UnicodeChar == OCMC_CHAR_ESC) ||
        (Key.UnicodeChar == CHAR_CARRIAGE_RETURN)) {
      //
      // Esc and Enter both save: users expect leaving the field to keep it.
      // Esc is matched on the character as well, because firmware may report
      // it with an empty scan code.
      //
      Accepted = TRUE;
      break;
    }

    if (Key.ScanCode == SCAN_LEFT) {
      if (Cursor > 0) {
        --Cursor;
      }
      continue;
    }

    if (Key.ScanCode == SCAN_RIGHT) {
      if (Cursor < Length) {
        ++Cursor;
      }
      continue;
    }

    if (Key.ScanCode == SCAN_HOME) {
      Cursor = 0;
      continue;
    }

    if (Key.ScanCode == SCAN_END) {
      Cursor = Length;
      continue;
    }

    if ((Key.UnicodeChar == CHAR_BACKSPACE) || (Key.UnicodeChar == 0x7F)) {
      if (Cursor > 0) {
        UINTN Mi;
        for (Mi = Cursor - 1; Mi < Length; ++Mi) {
          Args[Mi] = Args[Mi + 1];
        }
        --Length;
        --Cursor;
      }
      continue;
    }

    if (Key.ScanCode == SCAN_DELETE) {
      if (Cursor < Length) {
        UINTN Mi;
        for (Mi = Cursor; Mi < Length; ++Mi) {
          Args[Mi] = Args[Mi + 1];
        }
        --Length;
      }
      continue;
    }

    if ((Key.UnicodeChar < 0x20) || (Key.UnicodeChar > 0x7E)) {
      continue;
    }

    if (Length + 1 >= OCMC_ARGS_LEN) {
      continue;
    }

    {
      UINTN Mi;
      for (Mi = Length + 1; Mi > Cursor; --Mi) {
        Args[Mi] = Args[Mi - 1];
      }
      Args[Cursor] = Key.UnicodeChar;
      ++Length;
      ++Cursor;
      Args[Length] = L'\0';
    }
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
  CHAR8          NameAscii[OCMC_NAME_LEN];
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
    if (EFI_ERROR (OcmReadKey (&Key, OCMC_QUIET_POLL_MS)) ||
        ((Key.ScanCode != SCAN_ESC) && (Key.UnicodeChar != OCMC_CHAR_ESC))) {
      OcmBuildPath (Names[DefaultIdx], mConfigPath);
      if (!OcmValidateConfig (Storage, mConfigPath)) {
        OcmTrace (Storage, "quiet: remembered config unreadable, using entry 1");
        DefaultIdx = 0;
        OcmBuildPath (Names[DefaultIdx], mConfigPath);
      }

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
            //
            // Boot the remembered entry, but never let a stale memory point at
            // a configuration that can no longer be read or parsed.
            //
            OcmBuildPath (Names[Selected], mConfigPath);
            if (!OcmValidateConfig (Storage, mConfigPath)) {
              OcmTrace (Storage, "auto-boot: remembered config unreadable, using entry 1");
              Selected = 0;
              OcmBuildPath (Names[Selected], mConfigPath);
            }

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

      //
      // The editor painted its own card over the menu. Force a full repaint
      // instead of an incremental one, or the stale editor frame stays on
      // screen and it looks like Esc did nothing.
      //
      mCardDrawn = FALSE;
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

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.ScanCode == SCAN_ESC) ||
        (Key.UnicodeChar == OCMC_CHAR_ESC)) {
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

  //
  // Remember the selection so that the next boot starts from the same entry.
  // Skipped when it already matches the stored one, to spare the NVRAM cell.
  //
  if (StrCmp (Names[Selected], DefaultName) != 0) {
    Status = OcmSaveDefaultName (Names[Selected]);
    UnicodeStrToAsciiStrS (Names[Selected], NameAscii, sizeof (NameAscii));
    AsciiSPrint (Trace, sizeof (Trace), "remember: %a - %r", NameAscii, Status);
    OcmTrace (Storage, Trace);
  }

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
