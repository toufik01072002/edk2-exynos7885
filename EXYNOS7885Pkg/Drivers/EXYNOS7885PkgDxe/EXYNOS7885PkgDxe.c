#include <Configuration/BootDevices.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
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
BOOLEAN
PollHardwareKey (
  OUT EFI_INPUT_KEY *Key OPTIONAL
  )
{
  UINT32 RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  UINT32 CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
  UINT32 Fall1   = (mLastGpa1 & ~CurGpa1);

  if ((RawGpa1 != mLastPrintedGpa1) && (gST->ConOut != NULL)) {
    CHAR16 Buf;
    UnicodeSPrint (Buf, sizeof (Buf), L"[GPA1=0x%02x]", RawGpa1 & 0xFF);
    gST->ConOut->SetCursorPosition (gST->ConOut, 55, 0);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    mLastPrintedGpa1 = RawGpa1;
  }

  if (Fall1 != 0) {
    MicroSecondDelay (30000);
    RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
    CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
    Fall1   = (mLastGpa1 & ~CurGpa1);
  }

  mLastGpa1 = CurGpa1;

  if (Fall1 & KEY_VOL_UP_BIT) {
    if (Key != NULL) {
      Key->ScanCode    = SCAN_UP;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }
  if (Fall1 & KEY_VOL_DOWN_BIT) {
    if (Key != NULL) {
      Key->ScanCode    = SCAN_DOWN;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }
  if (Fall1 & KEY_POWER_BIT) {
    if (Key != NULL) {
      Key->ScanCode    = SCAN_NULL;
      Key->UnicodeChar = CHAR_CARRIAGE_RETURN;
    }
    return TRUE;
  }

  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
KeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mLastGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR) & ALL_KEYS_MASK;
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
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (PollHardwareKey (Key)) {
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
  UINT32 RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  UINT32 CurGpa1 = RawGpa1 & ALL_KEYS_MASK;

  if ((RawGpa1 != mLastPrintedGpa1) && (gST->ConOut != NULL)) {
    CHAR16 Buf;
    UnicodeSPrint (Buf, sizeof (Buf), L"[GPA1=0x%02x]", RawGpa1 & 0xFF);
    gST->ConOut->SetCursorPosition (gST->ConOut, 55, 0);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    mLastPrintedGpa1 = RawGpa1;
  }

  if ((mLastGpa1 & ~CurGpa1) != 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC
EFI_STATUS
EFIAPI
KeypadResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return KeypadReset (NULL, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
KeypadReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  OUT EFI_KEY_DATA                      *KeyData
  )
{
  EFI_STATUS Status;
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (KeyData, sizeof (EFI_KEY_DATA));
  Status = KeypadReadKeyStroke (NULL, &KeyData->Key);
  if (!EFI_ERROR (Status)) {
    KeyData->KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID;
    KeyData->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
KeypadSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN EFI_KEY_TOGGLE_STATE              *KeyToggleState
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeypadRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN  EFI_KEY_DATA                      *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                             **NotifyHandle
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeypadUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  IN VOID                              *NotificationHandle
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeypadTextIn = {
  KeypadReset,
  KeypadReadKeyStroke,
  NULL
};

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mKeypadTextInEx = {
  KeypadResetEx,
  KeypadReadKeyStrokeEx,
  NULL,
  KeypadSetState,
  KeypadRegisterKeyNotify,
  KeypadUnregisterKeyNotify
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
  UINT32     Gpa1Con;
  UINT32     Gpa1Pud;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  Gpa1Con = MmioRead32 (EXYNOS_GPA1_CON_ADDR);
  Gpa1Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA1_CON_ADDR, Gpa1Con);

  Gpa1Pud = MmioRead32 (EXYNOS_GPA1_PUD_ADDR);
  Gpa1Pud |= (0x3 << 10) | (0x3 << 12) | (0x3 << 14);
  MmioWrite32 (EXYNOS_GPA1_PUD_ADDR, Gpa1Pud);

  mLastGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR) & ALL_KEYS_MASK;

  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextIn.WaitForKey);
  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextInEx.WaitForKeyEx);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &KeypadHandle,
                  &gEfiDevicePathProtocolGuid,
                  &KeyPadDxeDevicePath,
                  &gEFIDroidKeypadDeviceProtocolGuid,
                  &mKeypadTextIn,
                  &gEfiSimpleTextInProtocolGuid,
                  &mKeypadTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mKeypadTextInEx,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
