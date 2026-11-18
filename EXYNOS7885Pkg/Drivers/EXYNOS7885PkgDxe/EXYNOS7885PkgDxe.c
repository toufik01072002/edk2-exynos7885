#include <Configuration/BootDevices.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA1_CON_ADDR   0x11CB0060
#define EXYNOS_GPA1_DAT_ADDR   0x11CB0064
#define EXYNOS_GPA1_PUD_ADDR   0x11CB0068

#define KEY_VOL_UP_BIT         (1 << 5)
#define KEY_VOL_DOWN_BIT       (1 << 6)
#define KEY_POWER_BIT          (1 << 7)

// Nombre de lectures consecutives a 1 (relache) exigees pour rearmer une touche.
// Filtre totalement les broches flottantes et les rebonds mecaniques.
#define HIGH_STABLE_THRESHOLD  15000

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC EFI_HANDLE mKeypadHandle   = NULL;
STATIC EFI_EVENT  mConInFixEvent  = NULL;

STATIC UINT32     mVolUpHighCnt   = HIGH_STABLE_THRESHOLD;
STATIC UINT32     mVolDownHighCnt = HIGH_STABLE_THRESHOLD;
STATIC UINT32     mPowerHighCnt   = HIGH_STABLE_THRESHOLD;

STATIC
BOOLEAN
PollHardwareKey (
  OUT EFI_INPUT_KEY *Key OPTIONAL
  )
{
  UINT32 Dat = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);

  // Mise a jour des compteurs d'etat haut (1 = relache)
  if (Dat & KEY_VOL_UP_BIT) {
    if (mVolUpHighCnt < HIGH_STABLE_THRESHOLD) {
      mVolUpHighCnt++;
    }
  } else {
    // Le bouton est appuye (0) : declenche UNIQUEMENT s'il a ete stablement relache avant
    if (mVolUpHighCnt >= HIGH_STABLE_THRESHOLD) {
      mVolUpHighCnt = 0;
      if (Key != NULL) {
        Key->ScanCode    = SCAN_UP;
        Key->UnicodeChar = 0;
      }
      return TRUE;
    }
    mVolUpHighCnt = 0;
  }

  if (Dat & KEY_VOL_DOWN_BIT) {
    if (mVolDownHighCnt < HIGH_STABLE_THRESHOLD) {
      mVolDownHighCnt++;
    }
  } else {
    if (mVolDownHighCnt >= HIGH_STABLE_THRESHOLD) {
      mVolDownHighCnt = 0;
      if (Key != NULL) {
        Key->ScanCode    = SCAN_DOWN;
        Key->UnicodeChar = 0;
      }
      return TRUE;
    }
    mVolDownHighCnt = 0;
  }

  if (Dat & KEY_POWER_BIT) {
    if (mPowerHighCnt < HIGH_STABLE_THRESHOLD) {
      mPowerHighCnt++;
    }
  } else {
    if (mPowerHighCnt >= HIGH_STABLE_THRESHOLD) {
      mPowerHighCnt = 0;
      if (Key != NULL) {
        Key->ScanCode    = SCAN_NULL;
        Key->UnicodeChar = CHAR_CARRIAGE_RETURN;
      }
      return TRUE;
    }
    mPowerHighCnt = 0;
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
  UINT32 Dat = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  if (!(Dat & KEY_VOL_UP_BIT)) {
    mVolUpHighCnt = 0;
  }
  if (!(Dat & KEY_VOL_DOWN_BIT)) {
    mVolDownHighCnt = 0;
  }
  if (!(Dat & KEY_POWER_BIT)) {
    mPowerHighCnt = 0;
  }
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
  gBS->SignalEvent (Event);
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
ForceBindConInCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  if (mKeypadHandle != NULL) {
    gST->ConsoleInHandle = mKeypadHandle;
    gST->ConIn           = &mKeypadTextIn;
    gBS->Hdr.CRC32       = 0;
    gBS->CalculateCrc32 (gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);
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

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  ASSERT_EFI_ERROR (Status);

  KeypadReset (NULL, FALSE);

  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextIn.WaitForKey);
  gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, NULL, &mKeypadTextInEx.WaitForKeyEx);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mKeypadHandle,
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

  gBS->CreateEventEx (
         EVT_NOTIFY_SIGNAL,
         TPL_CALLBACK,
         ForceBindConInCallback,
         NULL,
         &gEfiEndOfDxeEventGroupGuid,
         &mConInFixEvent
         );

  return EFI_SUCCESS;
}
