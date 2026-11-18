/** @file
  Minimal GOP renderer for the OCMC configuration menu.

  Draws 32-bit colour rectangles and an embedded 8x16 bold bitmap font
  directly into the linear framebuffer. Everything is laid out in a virtual
  960x540 space and scaled by an integer factor, so pixels stay sharp at any
  common resolution.

  Copyright (C) 2026, OpenCore Multi-Config contributors.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>

#include <Protocol/GraphicsOutput.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcMainLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "OpenCoreConfigFont.h"

///
/// Virtual layout, all coordinates below are in these units.
///
#define OCM_LAYOUT_W  1920
#define OCM_LAYOUT_H  1080

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC UINT8                         *mFrameBuffer;
STATIC UINTN                         mPixelsPerLine;
STATIC UINTN                         mWidth;
STATIC UINTN                         mHeight;
STATIC UINTN                         mScale;
STATIC UINTN                         mOffsetX;
STATIC UINTN                         mOffsetY;
STATIC BOOLEAN                       mBlueFirst;
STATIC BOOLEAN                       mReady;

/**
  Convert a 0x00RRGGBB colour into the framebuffer pixel layout.

  @param[in] Rgb   Colour in 0x00RRGGBB form.

  @return Pixel value ready to be stored as 32 bits.
**/
STATIC
UINT32
OcmGfxPixel (
  IN UINT32  Rgb
  )
{
  UINT8  R;
  UINT8  G;
  UINT8  B;

  R = (UINT8)((Rgb >> 16) & 0xFF);
  G = (UINT8)((Rgb >> 8) & 0xFF);
  B = (UINT8)(Rgb & 0xFF);

  if (mBlueFirst) {
    return ((UINT32)0xFF << 24) | ((UINT32)R << 16) | ((UINT32)G << 8) | B;
  }

  return ((UINT32)0xFF << 24) | ((UINT32)B << 16) | ((UINT32)G << 8) | R;
}

/**
  Fill a rectangle given in framebuffer pixels.

  @param[in] X      Left edge.
  @param[in] Y      Top edge.
  @param[in] W      Width, may be negative for a mirrored fill.
  @param[in] H      Height.
  @param[in] Pixel  Pixel value from OcmGfxPixel.
**/
STATIC
VOID
OcmGfxFillPixels (
  IN INTN    X,
  IN INTN    Y,
  IN INTN    W,
  IN INTN    H,
  IN UINT32  Pixel
  )
{
  INTN    Row;
  INTN    Column;
  UINT32  *Line;

  if ((W <= 0) || (H <= 0)) {
    return;
  }

  if (X < 0) {
    W += X;
    X  = 0;
  }

  if (Y < 0) {
    H += Y;
    Y  = 0;
  }

  if ((X >= (INTN)mWidth) || (Y >= (INTN)mHeight)) {
    return;
  }

  if ((X + W) > (INTN)mWidth) {
    W = (INTN)mWidth - X;
  }

  if ((Y + H) > (INTN)mHeight) {
    H = (INTN)mHeight - Y;
  }

  for (Row = 0; Row < H; ++Row) {
    Line = (UINT32 *)(mFrameBuffer + ((UINTN)(Y + Row) * mPixelsPerLine * sizeof (UINT32)) + (UINTN)X * sizeof (UINT32));

    for (Column = 0; Column < W; ++Column) {
      Line[Column] = Pixel;
    }
  }
}

/**
  Pick the largest usable 32-bit GOP mode and prepare the renderer.

  @retval TRUE   The renderer is ready.
  @retval FALSE  No suitable GOP mode, the caller must fall back to text.
**/
BOOLEAN
OcmGfxInit (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
  UINTN                        ModeNumber;
  UINTN                        BestMode;
  UINTN                        BestArea;
  UINTN                        Area;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *BestInfo;
  UINTN                        SizeOfInfo;

  mReady = FALSE;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status) || (mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
    return FALSE;
  }

  BestMode = mGop->Mode->Mode;
  BestInfo = mGop->Mode->Info;
  BestArea = 0;

  for (ModeNumber = 0; ModeNumber < mGop->Mode->MaxMode; ++ModeNumber) {
    Info = NULL;
    if (EFI_ERROR (mGop->QueryMode (mGop, ModeNumber, &SizeOfInfo, &Info)) || (Info == NULL)) {
      continue;
    }

    if ((Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
        && (Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor))
    {
      continue;
    }

    Area = (UINTN)Info->HorizontalResolution * Info->VerticalResolution;
    if (Area > BestArea) {
      BestArea = Area;
      BestMode = ModeNumber;
      BestInfo = Info;
    }
  }

  if (BestArea == 0) {
    return FALSE;
  }

  if (mGop->Mode->Mode != BestMode) {
    Status = mGop->SetMode (mGop, (UINT32)BestMode);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "OCMC: GOP SetMode %u failed - %r\n", (UINT32)BestMode, Status));
      return FALSE;
    }

    BestInfo = mGop->Mode->Info;
  }

  mWidth          = BestInfo->HorizontalResolution;
  mHeight         = BestInfo->VerticalResolution;
  mPixelsPerLine  = BestInfo->PixelsPerScanLine;
  mFrameBuffer    = (UINT8 *)(UINTN)mGop->Mode->FrameBufferBase;
  mBlueFirst      = (BOOLEAN)(BestInfo->PixelFormat == PixelBlueGreenRedReserved8BitPerColor);

  if ((mWidth < OCM_LAYOUT_W) || (mHeight < OCM_LAYOUT_H) || (mFrameBuffer == NULL)) {
    DEBUG ((DEBUG_WARN, "OCMC: GOP %ux%u too small or no framebuffer\n", (UINT32)mWidth, (UINT32)mHeight));
    return FALSE;
  }

  mScale = mWidth / OCM_LAYOUT_W;
  if (mScale > (mHeight / OCM_LAYOUT_H)) {
    mScale = mHeight / OCM_LAYOUT_H;
  }

  if (mScale < 1) {
    mScale = 1;
  }

  if (mScale > 6) {
    mScale = 6;
  }

  mOffsetX = (mWidth - OCM_LAYOUT_W * mScale) / 2;
  mOffsetY = (mHeight - OCM_LAYOUT_H * mScale) / 2;
  mReady   = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "OCMC: GOP %ux%u scale %u offset %u,%u\n",
    (UINT32)mWidth,
    (UINT32)mHeight,
    (UINT32)mScale,
    (UINT32)mOffsetX,
    (UINT32)mOffsetY
    ));

  return TRUE;
}

/**
  Whether the graphical renderer is available.

  @retval TRUE  Graphics can be used.
**/
BOOLEAN
OcmGfxReady (
  VOID
  )
{
  return mReady;
}

/**
  Clear the whole screen to a colour.

  @param[in] Rgb   Background colour in 0x00RRGGBB form.
**/
VOID
OcmGfxClear (
  IN UINT32  Rgb
  )
{
  if (!mReady) {
    return;
  }

  OcmGfxFillPixels (0, 0, (INTN)mWidth, (INTN)mHeight, OcmGfxPixel (Rgb));
}

/**
  Fill a rectangle given in layout units.

  @param[in] X      Left edge in layout units.
  @param[in] Y      Top edge in layout units.
  @param[in] W      Width in layout units.
  @param[in] H      Height in layout units.
  @param[in] Rgb    Colour in 0x00RRGGBB form.
**/
VOID
OcmGfxFill (
  IN INTN    X,
  IN INTN    Y,
  IN INTN    W,
  IN INTN    H,
  IN UINT32  Rgb
  )
{
  if (!mReady) {
    return;
  }

  OcmGfxFillPixels (
    (INTN)mOffsetX + X * (INTN)mScale,
    (INTN)mOffsetY + Y * (INTN)mScale,
    W * (INTN)mScale,
    H * (INTN)mScale,
    OcmGfxPixel (Rgb)
    );
}

/**
  Draw one character at an exact pixel position, scaled.

  @param[in] Px       Left edge in framebuffer pixels.
  @param[in] Py       Top edge in framebuffer pixels.
  @param[in] Char     ASCII character.
  @param[in] Pixel    Pixel value.
  @param[in] Scale    Integer scale factor.
**/
STATIC
VOID
OcmGfxGlyph (
  IN INTN    Px,
  IN INTN    Py,
  IN CHAR16  Char,
  IN UINT32  Pixel,
  IN UINTN   Scale
  )
{
  UINTN  Index;
  UINTN  Row;
  UINTN  Column;
  UINT8  Bits;

  if ((Char < OCM_FONT_FIRST) || (Char >= (OCM_FONT_FIRST + OCM_FONT_COUNT))) {
    Char = L'?';
  }

  Index = (UINTN)(Char - OCM_FONT_FIRST);

  for (Row = 0; Row < OCM_FONT_H; ++Row) {
    Bits = mOcmFont[Index][Row];
    if (Bits == 0) {
      continue;
    }

    for (Column = 0; Column < OCM_FONT_W; ++Column) {
      if ((Bits & (0x80 >> Column)) == 0) {
        continue;
      }

      OcmGfxFillPixels (
        Px + (INTN)(Column * Scale),
        Py + (INTN)(Row * Scale),
        (INTN)Scale,
        (INTN)Scale,
        Pixel
        );
    }
  }
}

/**
  Draw an ASCII string in layout units.

  @param[in] X       Left edge in layout units.
  @param[in] Y       Top edge in layout units.
  @param[in] Text    Text to draw, non-ASCII characters become '?'.
  @param[in] Rgb     Colour in 0x00RRGGBB form.
  @param[in] Scale   Character scale, 1 means an 8x16 cell.
**/
VOID
OcmGfxText (
  IN INTN          X,
  IN INTN          Y,
  IN CONST CHAR16  *Text,
  IN UINT32        Rgb,
  IN UINTN         Scale
  )
{
  UINTN   Index;
  UINT32  Pixel;
  INTN    Px;
  INTN    Py;
  UINTN   Step;

  if (!mReady || (Text == NULL) || (Scale == 0)) {
    return;
  }

  Pixel = OcmGfxPixel (Rgb);
  Px    = (INTN)mOffsetX + X * (INTN)mScale;
  Py    = (INTN)mOffsetY + Y * (INTN)mScale;
  Step  = OCM_FONT_W * Scale * mScale;

  for (Index = 0; Text[Index] != L'\0'; ++Index) {
    OcmGfxGlyph (Px, Py, Text[Index], Pixel, Scale * mScale);
    Px += (INTN)Step;
  }
}

/**
  Width of a string in layout units.

  @param[in] Text    Text to measure.
  @param[in] Scale   Character scale.

  @return Width in layout units.
**/
UINTN
OcmGfxTextWidth (
  IN CONST CHAR16  *Text,
  IN UINTN         Scale
  )
{
  if (Text == NULL) {
    return 0;
  }

  return StrLen (Text) * OCM_FONT_W * Scale;
}

/**
  Width of the virtual layout in units.

  @return Layout width.
**/
UINTN
OcmGfxLayoutWidth (
  VOID
  )
{
  return OCM_LAYOUT_W;
}

/**
  Height of the virtual layout in units.

  @return Layout height.
**/
UINTN
OcmGfxLayoutHeight (
  VOID
  )
{
  return OCM_LAYOUT_H;
}
