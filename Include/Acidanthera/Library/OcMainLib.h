/** @file
  Copyright (C) 2026, vit9696. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef OC_MAIN_LIB
#define OC_MAIN_LIB

#include <Library/BaseOverflowLib.h>
#include <Library/OcAppleKernelLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConfigurationLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcStorageLib.h>

#include <Protocol/OcBootstrap.h>

/**
  OpenCore version reported to log and NVRAM.
  OPEN_CORE_VERSION must follow X.Y.Z format, where X.Y.Z are single digits.
**/
#define OPEN_CORE_VERSION  "1.0.8"

/**
  OpenCore build type reported to log and NVRAM.
**/
#if defined (OC_TARGET_RELEASE)
#define OPEN_CORE_TARGET  "REL"          ///< Release.
#elif defined (OC_TARGET_DEBUG)
#define OPEN_CORE_TARGET  "DBG"          ///< Debug with compiler optimisations.
#elif defined (OC_TARGET_NOOPT)
#define OPEN_CORE_TARGET  "NPT"          ///< Debug with no compiler optimisations.
#else
  #error "Unknown target definition"
#endif

#define OPEN_CORE_ROOT_PATH  L"EFI\\OC"

#define OPEN_CORE_APP_PATH  L"OpenCore.efi"

#define OPEN_CORE_CONFIG_PATH  L"config.plist"

#define OPEN_CORE_LOG_PREFIX_PATH  L"opencore"

#define OPEN_CORE_ACPI_PATH  L"ACPI\\"

#define OPEN_CORE_UEFI_DRIVER_PATH  L"Drivers\\"

#define OPEN_CORE_KEXT_PATH  L"Kexts\\"

#define OPEN_CORE_TOOL_PATH  L"Tools\\"

/**
  Obtain cryptographic key if it was installed.

  @param[in]  Bootstrap  bootstrap protocol.

  @return public key or NULL.
**/
OC_RSA_PUBLIC_KEY *
OcGetVaultKey (
  VOID
  );

/**
  Load ACPI compatibility support like custom tables.

  @param[in]  Storage   OpenCore storage.
  @param[in]  Config    OpenCore configuration.
**/
VOID
OcLoadAcpiSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  );

/**
  Load device properties compatibility support.

  @param[in]  Config    OpenCore configuration.
**/
VOID
OcLoadDevPropsSupport (
  IN OC_GLOBAL_CONFIG  *Config
  );

/**
  Load drivers.

  @param[in]  Storage             OpenCore storage.
  @param[in]  Config              OpenCore configuration.
  @param[in]  DriversToConnect    Drivers which require later connection.
  @param[in]  LoadEarly           If TRUE load any early phase drivers, otherwise load normal phase.
**/
VOID
OcLoadDrivers (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  OC_GLOBAL_CONFIG    *Config,
  OUT EFI_HANDLE          **DriversToConnect  OPTIONAL,
  IN  BOOLEAN             LoadEarly
  );

/**
  Load Kernel compatibility support like kexts.

  @param[in]  Storage   OpenCore storage.
  @param[in]  Config    OpenCore configuration.
  @param[in]  CpuInfo   CPU information.
**/
VOID
OcLoadKernelSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config,
  IN OC_CPU_INFO         *CpuInfo
  );

/**
  Apply kernel quirk.
**/
EFI_STATUS
OcKernelApplyQuirk (
  IN     KERNEL_QUIRK_NAME  Quirk,
  IN     KERNEL_CACHE_TYPE  CacheType,
  IN     UINT32             DarwinVersion,
  IN OUT VOID               *Context,
  IN OUT PATCHER_CONTEXT    *KernelPatcher
  );

/**
  Inject kexts.
**/
VOID
OcKernelInjectKexts (
  IN OC_GLOBAL_CONFIG   *Config,
  IN KERNEL_CACHE_TYPE  CacheType,
  IN VOID               *Context,
  IN UINT32             DarwinVersion,
  IN BOOLEAN            Is32Bit,
  IN UINT32             LinkedExpansion,
  IN UINT32             ReservedExeSize
  );

/**
  Apply kernel patch.
**/
VOID
OcKernelApplyPatches (
  IN     OC_GLOBAL_CONFIG   *Config,
  IN     OC_CPU_INFO        *CpuInfo,
  IN     UINT32             DarwinVersion,
  IN     BOOLEAN            Is32Bit,
  IN     KERNEL_CACHE_TYPE  CacheType,
  IN     VOID               *Context,
  IN OUT UINT8              *Kernel,
  IN     UINT32             Size
  );

/**
  Apply kernel block patch.
**/
VOID
OcKernelBlockKexts (
  IN     OC_GLOBAL_CONFIG   *Config,
  IN     UINT32             DarwinVersion,
  IN     BOOLEAN            Is32Bit,
  IN     KERNEL_CACHE_TYPE  CacheType,
  IN     VOID               *Context
  );

/**
  Process prelinked.
**/
EFI_STATUS
OcKernelProcessPrelinked (
  IN     OC_GLOBAL_CONFIG  *Config,
  IN     UINT32            DarwinVersion,
  IN     BOOLEAN           Is32Bit,
  IN OUT UINT8             *Kernel,
  IN     UINT32            *KernelSize,
  IN     UINT32            AllocatedSize,
  IN     UINT32            LinkedExpansion,
  IN     UINT32            ReservedExeSize
  );

/**
  Cleanup Kernel compatibility support on failure.
**/
VOID
OcUnloadKernelSupport (
  VOID
  );

/**
  Load NVRAM compatibility support.

  @param[in]  Storage   OpenCore storage.
  @param[in]  Config    OpenCore configuration.
**/
VOID
OcLoadNvramSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  );

/**
  Obtain Legacy Secure Boot ECID (system-id).

  @param[in]  Config   Configuration.
  @param[out] ApECID   Legacy Secure Boot ECID.
**/
VOID
OcGetLegacySecureBootECID (
  IN  OC_GLOBAL_CONFIG  *Config,
  OUT UINT64            *ApECID
  );

/**
  Obtain default Secure Boot hardware model.

  @param[in]  Config   Configuration.
  @param[in]  CpuInfo  CPU information.
  @return Apple Secure Boot model.
**/
CONST CHAR8 *
OcGetDefaultSecureBootModel (
  IN  OC_GLOBAL_CONFIG  *Config,
  IN  OC_CPU_INFO       *CpuInfo
  );

/**
  Load platform compatibility support like DataHub or SMBIOS.

  @param[in]  Config    OpenCore configuration.
  @param[in]  CpuInfo   CPU information.
**/
VOID
OcLoadPlatformSupport (
  IN OC_GLOBAL_CONFIG  *Config,
  IN OC_CPU_INFO       *CpuInfo
  );

/**
  Load UEFI compatibility support like drivers.

  @param[in]  Storage   OpenCore storage.
  @param[in]  Config    OpenCore configuration.
  @param[in]  CpuInfo   CPU information.
  @param[out] Signature OpenCore SHA-1 booter signature, all zero when unavailable.
**/
VOID
OcLoadUefiSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config,
  IN OC_CPU_INFO         *CpuInfo,
  IN UINT8               *Signature
  );

/**
  Load UEFI input compatibility support.

  @param[out] Config    OpenCore configuration.
**/
VOID
OcLoadUefiInputSupport (
  IN OC_GLOBAL_CONFIG  *Config
  );

/**
  Load UEFI output compatibility support.

  @param[in]  Storage   OpenCore storage.
  @param[out] Config    OpenCore configuration.
**/
VOID
OcLoadUefiOutputSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  );

/**
  Load UEFI audio compatibility support.

  @param[in]  Storage   OpenCore storage.
  @param[out] Config    OpenCore configuration.
**/
VOID
OcLoadUefiAudioSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  );

/**
  Schedule Exit Boot Services event in TPL_APPLICATION mode.

  @param[in]  Handler   Handler function to invoke.
  @param[in]  Context   Handler function context.
**/
VOID
OcScheduleExitBootServices (
  IN EFI_EVENT_NOTIFY  Handler,
  IN VOID              *Context
  );

/**
  Get human readable version string.

  @retval null-terminated 7-bit ASCII version string.
**/
CONST CHAR8 *
OcMiscGetVersionString (
  VOID
  );

/**
  Load early miscellaneous support like configuration.

  @param[in]  Storage   OpenCore storage.
  @param[out] Config    OpenCore configuration.
  @param[in]  VaultKey  Vault key.

  @retval EFI_SUCCESS when allowed to continue.
**/
EFI_STATUS
OcMiscEarlyInit (
  IN  OC_STORAGE_CONTEXT  *Storage,
  OUT OC_GLOBAL_CONFIG    *Config,
  IN  OC_RSA_PUBLIC_KEY   *VaultKey  OPTIONAL
  );

/**
  Load middle miscellaneous support like device path.

  @param[in]  Storage        OpenCore storage.
  @param[in]  Config         OpenCore configuration.
  @param[in]  RootPath       Root load path (e.g. path to OC directory).
  @param[in]  LoadPath       OpenCore loading device path (absolute).
  @param[in]  StorageHandle  OpenCore storage loading handle (e.g. FS handle).
  @param[out] Signature      OpenCore SHA-1 booter signature, optional.

  @retval EFI_SUCCESS on success, informational.
**/
VOID
OcMiscMiddleInit (
  IN  OC_STORAGE_CONTEXT        *Storage,
  IN  OC_GLOBAL_CONFIG          *Config,
  IN  CONST CHAR16              *RootPath,
  IN  EFI_DEVICE_PATH_PROTOCOL  *LoadPath,
  IN  EFI_HANDLE                StorageHandle,
  OUT UINT8                     *Signature  OPTIONAL
  );

/**
  Load late miscellaneous support like Apple hibernation or panic saving.

  @param[in]  Storage    OpenCore storage.
  @param[in]  Config     OpenCore configuration.

  @retval EFI_SUCCESS on success, informational.
**/
EFI_STATUS
OcMiscLateInit (
  IN  OC_STORAGE_CONTEXT  *Storage,
  IN  OC_GLOBAL_CONFIG    *Config
  );

/**
  Load system report.

  @param[in]  LoadHandle OpenCore loading handle.

  @retval EFI_SUCCESS on success, informational.
**/
VOID
OcMiscLoadSystemReport (
  IN  OC_GLOBAL_CONFIG  *Config,
  IN  EFI_HANDLE        LoadHandle OPTIONAL
  );

/**
  Load late miscellaneous support like boot screen config.

  @param[in]  Storage         OpenCore storage.
  @param[in]  Config          OpenCore configuration.
  @param[in]  Privilege       OpenCore privilege context.
  @param[in]  StartImage      Image starting routine used.
  @param[in]  LoadHandle      OpenCore loading handle.
  @param[in]  CustomBootGuid  Use custom (gOcVendorVariableGuid) for Boot#### variables.
**/
VOID
OcMiscBoot (
  IN  OC_STORAGE_CONTEXT    *Storage,
  IN  OC_GLOBAL_CONFIG      *Config,
  IN  OC_PRIVILEGE_CONTEXT  *Privilege OPTIONAL,
  IN  OC_IMAGE_START        StartImage,
  IN  BOOLEAN               CustomBootGuid,
  IN  EFI_HANDLE            LoadHandle
  );

/**
  Load miscellaneous support after UEFI quirks.

  @param[in]  Config     OpenCore configuration.
**/
VOID
OcMiscUefiQuirksLoaded (
  IN OC_GLOBAL_CONFIG  *Config
  );

/**
  Determine platform support for 64-bit kernel mode based
  on kernel version.

  @param[in]  KernelVersion   Kernel version.
**/
BOOLEAN
OcPlatformIs64BitSupported (
  IN UINT32  KernelVersion
  );

/**
  Unload loaded images by name.

  @param[in]  Config     OpenCore configuration.
**/
VOID
OcUnloadDrivers (
  IN  OC_GLOBAL_CONFIG  *Config
  );

/**
  Dump loaded image driver info to the specified directory.

  @param[in]  Root     Directory to write CPU data.

  @retval EFI_SUCCESS on success.
**/
EFI_STATUS
OcDriverInfoDump (
  IN EFI_FILE_PROTOCOL  *Root
  );

/**
  Let the user pick a configuration file and optionally adjust boot arguments
  for the current boot.

  Must be called before the configuration file is read.

  @param[in] Storage   Storage context of EFI/OC.

  @return Path of the configuration file to load, relative to EFI/OC.
**/
CONST CHAR16 *
OcConfigMenuSelect (
  IN OC_STORAGE_CONTEXT  *Storage
  );

/**
  Apply the boot arguments entered in the configuration menu.

  Must be called after OcLoadNvramSupport so that the value wins over the one
  from the plist. The variable is written as volatile, i.e. it is gone after a
  reboot.
**/
VOID
OcConfigMenuApplyBootArgs (
  VOID
  );

/**
  Initialise the GOP based renderer used by the configuration menu.

  @retval TRUE   The renderer is ready, graphical drawing may be used.
  @retval FALSE  No suitable framebuffer, the caller must use text mode.
**/
BOOLEAN
OcmGfxInit (
  VOID
  );

/**
  Whether the graphical renderer is available.

  @retval TRUE  Graphical drawing is possible.
**/
BOOLEAN
OcmGfxReady (
  VOID
  );

/**
  Fill the whole screen.

  @param[in] Rgb   Colour in 0x00RRGGBB form.
**/
VOID
OcmGfxClear (
  IN UINT32  Rgb
  );

/**
  Fill a rectangle in virtual layout units.

  @param[in] X      Left edge.
  @param[in] Y      Top edge.
  @param[in] W      Width.
  @param[in] H      Height.
  @param[in] Rgb    Colour in 0x00RRGGBB form.
**/
VOID
OcmGfxFill (
  IN INTN    X,
  IN INTN    Y,
  IN INTN    W,
  IN INTN    H,
  IN UINT32  Rgb
  );

/**
  Draw an ASCII string in virtual layout units.

  @param[in] X       Left edge.
  @param[in] Y       Top edge.
  @param[in] Text    Text to draw.
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
  );

/**
  Measure a string in virtual layout units.

  @param[in] Text    Text to measure.
  @param[in] Scale   Character scale.

  @return Width in layout units.
**/
UINTN
OcmGfxTextWidth (
  IN CONST CHAR16  *Text,
  IN UINTN         Scale
  );

/**
  Width of the virtual layout in units.

  @return Layout width.
**/
UINTN
OcmGfxLayoutWidth (
  VOID
  );

/**
  Height of the virtual layout in units.

  @return Layout height.
**/
UINTN
OcmGfxLayoutHeight (
  VOID
  );

#endif // OC_MAIN_LIB
