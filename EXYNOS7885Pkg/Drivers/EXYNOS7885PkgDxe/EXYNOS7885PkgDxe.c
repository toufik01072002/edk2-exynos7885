#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA0_DAT_ADDR  0x11CB0004
#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)

EFI_CPU_ARCH_PROTOCOL *gCpu;
STATIC UINT32 mLastGpa0State = (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT);

typedef struct {
  VENDOR_DEVICE_PATH       VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL End;
} KEYPAD_DEVICE_PATH;

STATIC KEYPAD_DEVICE_PATH mKeypadDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x8a91b012, 0x45c1, 0x49d2, { 0x91, 0x10, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC
EFI_STATUS
EFIAPI
KeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mLastGpa0State = (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeypadReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  UINT32 Current = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT);
  UINT32 Pressed = (mLastGpa0State & ~Current);
  mLastGpa0State = Current;

  if (Pressed & KEY_VOL_UP_BIT) {
    Key->ScanCode    = SCAN_UP;
    Key->UnicodeChar = 0;
    return EFI_SUCCESS;
  }
  if (Pressed & KEY_VOL_DOWN_BIT) {
    Key->ScanCode    = SCAN_DOWN;
    Key->UnicodeChar = 0;
    return EFI_SUCCESS;
  }
  if (Pressed & KEY_POWER_BIT) {
    Key->ScanCode    = SCAN_NULL;
    Key->UnicodeChar = CHAR_CARRIAGE_RETURN;
    return EFI_SUCCESS;
  }

  return EFI_NOT_READY;
}

STATIC
VOID
EFIAPI
KeypadWaitForKey (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  UINT32 Current = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT);
  if ((mLastGpa0State & ~Current) != 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeypadTextIn = {
  KeypadReset,
  KeypadReadKeyStroke,
  NULL
};

EFI_STATUS
EFIAPI
EXYNOS7885PkgEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_HANDLE KeypadHandle = NULL;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  KeypadWaitForKey,
                  NULL,
                  &mKeypadTextIn.WaitForKey
                  );
  if (!EFI_ERROR (Status)) {
    gBS->InstallMultipleProtocolInterfaces (
           &KeypadHandle,
           &gEfiDevicePathProtocolGuid,
           &mKeypadDevicePath,
           &gEfiSimpleTextInProtocolGuid,
           &mKeypadTextIn,
           NULL
           );
  }

  return EFI_SUCCESS;
}
