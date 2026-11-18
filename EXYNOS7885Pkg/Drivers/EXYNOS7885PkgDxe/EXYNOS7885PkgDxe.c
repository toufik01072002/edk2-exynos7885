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

#define EXYNOS_GPA1_DAT_ADDR   0x11CB0064

#define KEY_VOL_UP_BIT         (1 << 5)
#define KEY_VOL_DOWN_BIT       (1 << 6)
#define KEY_POWER_BIT          (1 << 7)

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC EFI_HANDLE mKeypadHandle   = NULL;
STATIC EFI_EVENT  mConInFixEvent  = NULL;

STATIC BOOLEAN    mVolUpLocked    = FALSE;
STATIC BOOLEAN    mVolDownLocked  = FALSE;
STATIC BOOLEAN    mPowerLocked    = FALSE;

STATIC
BOOLEAN
PollHardwareKey (
  OUT EFI_INPUT_KEY *Key OPTIONAL
  )
{
  UINT32 Dat = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);

  // Déverrouillage individuel dès que le bit remonte à 1 (relâché)
  if (Dat & KEY_VOL_UP_BIT) {
    mVolUpLocked = FALSE;
  }
  if (Dat & KEY_VOL_DOWN_BIT) {
    mVolDownLocked = FALSE;
  }
  if (Dat & KEY_POWER_BIT) {
    mPowerLocked = FALSE;
  }

  // Détection d'appui uniquement si la touche n'est pas verrouillée
  if (!(Dat & KEY_VOL_UP_BIT) && !mVolUpLocked) {
    mVolUpLocked = TRUE;
    if (Key != NULL) {
      Key->ScanCode    = SCAN_UP;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }

  if (!(Dat & KEY_VOL_DOWN_BIT) && !mVolDownLocked) {
    mVolDownLocked = TRUE;
    if (Key != NULL) {
      Key->ScanCode    = SCAN_DOWN;
      Key->UnicodeChar = 0;
    }
    return TRUE;
  }

  if (!(Dat & KEY_POWER_BIT) && !mPowerLocked) {
    mPowerLocked = TRUE;
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
  UINT32 Dat = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  mVolUpLocked   = !(Dat & KEY_VOL_UP_BIT);
  mVolDownLocked = !(Dat & KEY_VOL_DOWN_BIT);
  mPowerLocked   = !(Dat & KEY_POWER_BIT);
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

  // Ne pas écraser CON/PUD : conserver l'état matériel initialisé par S-Boot
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
