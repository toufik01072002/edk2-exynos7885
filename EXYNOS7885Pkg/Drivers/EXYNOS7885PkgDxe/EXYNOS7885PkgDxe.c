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

#define EXYNOS_GPA0_CON_ADDR  0x11CB0000
#define EXYNOS_GPA0_DAT_ADDR  0x11CB0004
#define EXYNOS_GPA0_PUD_ADDR  0x11CB0008

#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)
#define ALL_KEYS_MASK         (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT)

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC UINT32 mLastGpa0 = ALL_KEYS_MASK;
STATIC UINT32 mLastPrintedGpa0 = 0xFFFFFFFF;

STATIC
EFI_STATUS
EFIAPI
VirtualKbdRegister (
  VOID
  )
{
  UINT32 Gpa0Con = MmioRead32 (EXYNOS_GPA0_CON_ADDR);
  Gpa0Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA0_CON_ADDR, Gpa0Con);

  UINT32 Gpa0Pud = MmioRead32 (EXYNOS_GPA0_PUD_ADDR);
  Gpa0Pud |= (0x3 << 10) | (0x3 << 12) | (0x3 << 14);
  MmioWrite32 (EXYNOS_GPA0_PUD_ADDR, Gpa0Pud);

  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
VirtualKbdReset (
  VOID
  )
{
  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
VirtualKbdQuery (
  OUT VIRTUAL_KBD_KEY *VirtualKey
  )
{
  UINT32 RawGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR);
  UINT32 CurGpa0 = RawGpa0 & ALL_KEYS_MASK;
  UINT32 Fall0   = (mLastGpa0 & ~CurGpa0);

  if ((RawGpa0 != mLastPrintedGpa0) && (gST->ConOut != NULL)) {
    CHAR16 Buf[32];
    UnicodeSPrint (Buf, sizeof (Buf), L"[GPA0=0x%02x]", RawGpa0 & 0xFF);
    gST->ConOut->SetCursorPosition (gST->ConOut, 55, 0);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    mLastPrintedGpa0 = RawGpa0;
  }

  if (Fall0 != 0) {
    MicroSecondDelay (30000);
    RawGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR);
    CurGpa0 = RawGpa0 & ALL_KEYS_MASK;
    Fall0   = (mLastGpa0 & ~CurGpa0);
  }

  mLastGpa0 = CurGpa0;

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
