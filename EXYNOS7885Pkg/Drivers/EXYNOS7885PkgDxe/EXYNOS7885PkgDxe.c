#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA0_CON_ADDR  0x11CB0000
#define EXYNOS_GPA0_DAT_ADDR  0x11CB0004
#define EXYNOS_GPA0_PUD_ADDR  0x11CB0008

#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)
#define ALL_KEYS_MASK         (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT)

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC EFI_HANDLE    mKeypadHandle = NULL;
STATIC UINT32        mLastGpa0 = ALL_KEYS_MASK;
STATIC UINT32        mLastPrintedGpa0 = 0xFFFFFFFF;
STATIC EFI_EVENT     mEndOfDxeEvent = NULL;

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
BOOLEAN
PollHardwareKey (
  OUT EFI_INPUT_KEY *Key OPTIONAL
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
    // Anti-rebond materiel de 30 ms sans interruption
    MicroSecondDelay (30000);
    RawGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR);
    CurGpa0 = RawGpa0 & ALL_KEYS_MASK;
    Fall0   = (mLastGpa0 & ~CurGpa0);
  }

  mLastGpa0 = CurGpa0;

  if (Fall0 & KEY_VOL_UP_BIT) {
    if (Key != NULL) {
      Key->ScanCode    = SCAN_UP;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }
  if (Fall0 & KEY_VOL_DOWN_BIT) {
    if (Key != NULL) {
      Key->ScanCode    = SCAN_DOWN;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }
  if (Fall0 & KEY_POWER_BIT) {
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
  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;
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
  // Verification directe sans consommer le front descendant
  UINT32 RawGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR);
  UINT32 CurGpa0 = RawGpa0 & ALL_KEYS_MASK;

  if ((RawGpa0 != mLastPrintedGpa0) && (gST->ConOut != NULL)) {
    CHAR16 Buf[32];
    UnicodeSPrint (Buf, sizeof (Buf), L"[GPA0=0x%02x]", RawGpa0 & 0xFF);
    gST->ConOut->SetCursorPosition (gST->ConOut, 55, 0);
    gST->ConOut->OutputString (gST->ConOut, Buf);
    mLastPrintedGpa0 = RawGpa0;
  }

  if ((mLastGpa0 & ~CurGpa0) != 0) {
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

STATIC
VOID
EFIAPI
OnEndOfDxeConnectKeypad (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  if (mKeypadHandle != NULL) {
    EfiBootManagerUpdateConsoleVariable (ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mKeypadDevicePath, NULL);
    gBS->ConnectController (mKeypadHandle, NULL, NULL, TRUE);
  }
}

EFI_STATUS
EFIAPI
EXYNOS7885PkgEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  UINT32     Gpa0Con;
  UINT32     Gpa0Pud;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  Gpa0Con = MmioRead32 (EXYNOS_GPA0_CON_ADDR);
  Gpa0Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA0_CON_ADDR, Gpa0Con);

  Gpa0Pud = MmioRead32 (EXYNOS_GPA0_PUD_ADDR);
  Gpa0Pud |= (0x3 << 10) | (0x3 << 12) | (0x3 << 14);
  MmioWrite32 (EXYNOS_GPA0_PUD_ADDR, Gpa0Pud);

  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;

  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextIn.WaitForKey);
  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextInEx.WaitForKeyEx);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mKeypadHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mKeypadDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mKeypadTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mKeypadTextInEx,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    gBS->CreateEventEx (
           EVT_NOTIFY_SIGNAL,
           TPL_CALLBACK,
           OnEndOfDxeConnectKeypad,
           NULL,
           &gEfiEndOfDxeEventGroupGuid,
           &mEndOfDxeEvent
           );
  }

  return EFI_SUCCESS;
}
