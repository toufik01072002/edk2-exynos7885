#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA0_CON_ADDR  0x11CB0000
#define EXYNOS_GPA0_DAT_ADDR  0x11CB0004

#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)
#define ALL_KEYS_MASK         (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT)

#define KEY_QUEUE_SIZE        16

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC UINT32        mLastGpa0 = ALL_KEYS_MASK;
STATIC EFI_EVENT     mPollTimerEvent = NULL;
STATIC EFI_INPUT_KEY mKeyQueue[KEY_QUEUE_SIZE];
STATIC UINTN         mQueueHead = 0;
STATIC UINTN         mQueueTail = 0;

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
VOID
EnqueueKey (
  IN UINT16 ScanCode,
  IN CHAR16 UnicodeChar
  )
{
  UINTN NextTail = (mQueueTail + 1) % KEY_QUEUE_SIZE;
  if (NextTail != mQueueHead) {
    mKeyQueue[mQueueTail].ScanCode    = ScanCode;
    mKeyQueue[mQueueTail].UnicodeChar = UnicodeChar;
    mQueueTail = NextTail;
  }
}

STATIC
EFI_STATUS
EFIAPI
KeypadReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mQueueHead = 0;
  mQueueTail = 0;
  mLastGpa0  = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;
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
  if (mQueueHead == mQueueTail) {
    return EFI_NOT_READY;
  }

  *Key = mKeyQueue[mQueueHead];
  mQueueHead = (mQueueHead + 1) % KEY_QUEUE_SIZE;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
KeypadWaitForKey (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  if (mQueueHead != mQueueTail) {
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
KeypadPollTimerCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  UINT32 CurGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;
  UINT32 Fall0   = (mLastGpa0 & ~CurGpa0);

  mLastGpa0 = CurGpa0;

  if (Fall0 & KEY_VOL_UP_BIT) {
    EnqueueKey (SCAN_UP, 0);
  }
  if (Fall0 & KEY_VOL_DOWN_BIT) {
    EnqueueKey (SCAN_DOWN, 0);
  }
  if (Fall0 & KEY_POWER_BIT) {
    EnqueueKey (SCAN_NULL, CHAR_CARRIAGE_RETURN);
  }

  if (mQueueHead != mQueueTail) {
    if (mKeypadTextIn.WaitForKey != NULL) {
      gBS->SignalEvent (mKeypadTextIn.WaitForKey);
    }
    if (mKeypadTextInEx.WaitForKeyEx != NULL) {
      gBS->SignalEvent (mKeypadTextInEx.WaitForKeyEx);
    }
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
  EFI_HANDLE KeypadHandle = NULL;
  UINT32     Gpa0Con;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  Gpa0Con = MmioRead32 (EXYNOS_GPA0_CON_ADDR);
  Gpa0Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA0_CON_ADDR, Gpa0Con);

  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;

  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextIn.WaitForKey);
  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextInEx.WaitForKeyEx);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &KeypadHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mKeypadDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mKeypadTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mKeypadTextInEx,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    EfiBootManagerUpdateConsoleVariable (ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mKeypadDevicePath, NULL);
    gBS->ConnectController (KeypadHandle, NULL, NULL, TRUE);

    Status = gBS->CreateEvent (
                    EVT_TIMER | EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    KeypadPollTimerCallback,
                    NULL,
                    &mPollTimerEvent
                    );
    if (!EFI_ERROR (Status)) {
      gBS->SetTimer (mPollTimerEvent, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (50));
    }
  }

  return EFI_SUCCESS;
}
