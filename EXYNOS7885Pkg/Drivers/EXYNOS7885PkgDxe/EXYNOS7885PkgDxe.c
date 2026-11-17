#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/PlatformVirtualKeyboard.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA1_CON_ADDR  0x11CB0020
#define EXYNOS_GPA1_DAT_ADDR  0x11CB0024
#define EXYNOS_GPA1_PUD_ADDR  0x11CB0028

#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)
#define ALL_KEYS_MASK         (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT)

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC UINT32 mLastGpa1 = ALL_KEYS_MASK;
STATIC UINT32 mLastPrintedGpa1 = 0xFFFFFFFF;

STATIC
EFI_STATUS
EFIAPI
VirtualKbdRegister (
  VOID
  )
{
  UINT32 Gpa1Con = MmioRead32 (EXYNOS_GPA1_CON_ADDR);
  Gpa1Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA1_CON_ADDR, Gpa1Con);

  UINT32 Gpa1Pud = MmioRead32 (EXYNOS_GPA1_PUD_ADDR);
  Gpa1Pud |= (0x3 << 10) | (0x3 << 12) | (0x3 << 14);
  MmioWrite32 (EXYNOS_GPA1_PUD_ADDR, Gpa1Pud);

  mLastGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR) & ALL_KEYS_MASK;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
VirtualKbdReset (
  VOID
  )
{
  mLastGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR) & ALL_KEYS_MASK;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
VirtualKbdQuery (
  OUT VIRTUAL_KBD_KEY *VirtualKey
  )
{
  UINT32 RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  UINT32 CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
  UINT32 Fall0   = (mLastGpa1 & ~CurGpa1);

  if ((RawGpa1 != mLastPrintedGpa1) && (gST->ConOut != NULL)) {
    CHAR16 Buf[32];
    UnicodeSPrint (Buf, sizeof (Buf), L"[GPA1=0x%02x]", RawGpa1 & 0xFF);
    gST->ConOut->SetCursorPosition (gST->ConOut, 55, 0);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    mLastPrintedGpa1 = RawGpa1;
  }

  if (Fall0 != 0) {
    MicroSecondDelay (30000);
    RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
    CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
    Fall0   = (mLastGpa1 & ~CurGpa1);
  }

  mLastGpa1 = CurGpa1;

  if (Fall0 & KEY_VOL_UP_BIT) {
    VirtualKey->Key.ScanCode    = SCAN_UP;
    VirtualKey->Key.UnicodeChar = 0;
    return TRUE;
  }
  if (Fall0 & KEY_VOL_DOWN_BIT) {
    VirtualKey->Key.ScanCode    = SCAN_DOWN;
    VirtualKey->Key.UnicodeChar = 0;
    return TRUE;
  }
  if (Fall0 & KEY_POWER_BIT) {
    VirtualKey->Key.ScanCode    = SCAN_NULL;
    VirtualKey->Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
    return TRUE;
  }

  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
VirtualKbdClear (
  IN VIRTUAL_KBD_KEY *VirtualKey
  )
{
  return EFI_SUCCESS;
}

STATIC PLATFORM_VIRTUAL_KBD_PROTOCOL mPlatformVirtualKbd = {
  VirtualKbdRegister,
  VirtualKbdReset,
  VirtualKbdQuery,
  VirtualKbdClear
};

EFI_STATUS
EFIAPI
EXYNOS7885PkgEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gPlatformVirtualKeyboardProtocolGuid,
                  &mPlatformVirtualKbd,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
