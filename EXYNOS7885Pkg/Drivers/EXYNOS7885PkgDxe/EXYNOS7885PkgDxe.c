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
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA0_CON_ADDR  0x11CB0000
#define EXYNOS_GPA0_DAT_ADDR  0x11CB0004

// Broches exactes SM-A205W (DTB gpio_keys)
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

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeypadTextIn = {
  KeypadReset,
  KeypadReadKeyStroke,
  NULL
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

  if ((mQueueHead != mQueueTail) && (mKeypadTextIn.WaitForKey != NULL)) {
    gBS->SignalEvent (mKeypadTextIn.WaitForKey);
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

  // Passer GPA0_5 (bits 23:20), GPA0_6 (bits 27:24), GPA0_7 (bits 31:28) en Input (0x0)
  Gpa0Con = MmioRead32 (EXYNOS_GPA0_CON_ADDR);
  Gpa0Con &= ~0xFFF00000;
  MmioWrite32 (EXYNOS_GPA0_CON_ADDR, Gpa0Con);

  mLastGpa0 = MmioRead32 (EXYNOS_GPA0_DAT_ADDR) & ALL_KEYS_MASK;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  KeypadWaitForKey,
                  NULL,
                  &mKeypadTextIn.WaitForKey
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &KeypadHandle,
                    &gEfiDevicePathProtocolGuid,
                    &mKeypadDevicePath,
                    &gEfiSimpleTextInProtocolGuid,
                    &mKeypadTextIn,
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
  }

  return EFI_SUCCESS;
}
