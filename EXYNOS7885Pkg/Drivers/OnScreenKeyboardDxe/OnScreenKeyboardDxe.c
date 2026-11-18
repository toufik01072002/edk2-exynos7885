#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/DevicePath.h>

#define SCREEN_W        720
#define SCREEN_H        1560

#define TOGGLE_X        585
#define TOGGLE_Y        1485
#define TOGGLE_W        125
#define TOGGLE_H        62

#define KBD_TOP         1120
#define KBD_ROWS        5
#define KBD_COLS        10
#define KEY_W           70
#define KEY_H           68
#define KEY_GAP_X       2
#define KEY_GAP_Y       3
#define KEY_QUEUE_SIZE  64

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} OSK_DEVICE_PATH;
#pragma pack()

STATIC OSK_DEVICE_PATH mOskDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x4C9A1E72, 0x5D3B, 0x4A88, { 0xB1, 0x02, 0x9F, 0x6E, 0x2D, 0x4C, 0x8A, 0x11 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC BOOLEAN   mKbdVisible   = FALSE;
STATIC BOOLEAN   mShiftActive  = FALSE;
STATIC BOOLEAN   mPrevTouching = FALSE;
STATIC EFI_EVENT mOskTimer     = NULL;

STATIC EFI_INPUT_KEY mKeyQueue[KEY_QUEUE_SIZE];
STATIC UINTN mQueueHead = 0;
STATIC UINTN mQueueTail = 0;

STATIC CONST CHAR16 mLayoutNormal[KBD_ROWS][KBD_COLS] = {
  { L'1', L'2', L'3', L'4', L'5', L'6', L'7', L'8', L'9', L'0' },
  { L'q', L'w', L'e', L'r', L't', L'y', L'u', L'i', L'o', L'p' },
  { L'a', L's', L'd', L'f', L'g', L'h', L'j', L'k', L'l', L'-' },
  { L'^', L'z', L'x', L'c', L'v', L'b', L'n', L'm', L'.', L'\b' },
  { L'/', L':', L'\\', L' ', L' ', L' ', L' ', L'<', L'>', L'\r' }
};

STATIC CONST CHAR16 mLayoutShift[KBD_ROWS][KBD_COLS] = {
  { L'!', L'@', L'#', L'$', L'%', L'^', L'&', L'*', L'(', L')' },
  { L'Q', L'W', L'E', L'R', L'T', L'Y', L'U', L'I', L'O', L'P' },
  { L'A', L'S', L'D', L'F', L'G', L'H', L'J', L'K', L'L', L'_' },
  { L'^', L'Z', L'X', L'C', L'V', L'B', L'N', L'M', L';', L'\b' },
  { L'?', L'=', L'|', L' ', L' ', L' ', L' ', L'[', L']', L'\r' }
};

STATIC VOID FillRect (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  UINTN X, UINTN Y, UINTN W, UINTN H,
  UINT8 R, UINT8 G, UINT8 B
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;
  Color.Red      = R;
  Color.Green    = G;
  Color.Blue     = B;
  Color.Reserved = 0;
  Gop->Blt (Gop, &Color, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

STATIC VOID DrawKeycapPattern (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  UINTN X, UINTN Y, CHAR16 Ch
  )
{
  UINT8 R = 240, G = 240, B = 245;
  UINTN Code = (UINTN)Ch;
  UINTN Bit;

  if (Ch == L'\r') { R = 80; G = 230; B = 120; }
  else if (Ch == L'\b') { R = 245; G = 90; B = 90; }
  else if (Ch == L'^') { R = 100; G = 185; B = 255; }

  for (Bit = 0; Bit < 8; Bit++) {
    UINTN Bx = X + 15 + (Bit % 4) * 10;
    UINTN By = Y + 22 + (Bit / 4) * 12;
    if ((Code >> Bit) & 1U) {
      FillRect (Gop, Bx, By, 7, 7, R, G, B);
    }
  }
}

STATIC VOID RenderOsk (VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  UINTN Row, Col;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) || Gop == NULL) {
    return;
  }

  if (mKbdVisible) {
    FillRect (Gop, TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 200, 60, 60);
  } else {
    FillRect (Gop, TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 30, 120, 210);
  }
  FillRect (Gop, TOGGLE_X + 20, TOGGLE_Y + 26, 85, 10, 255, 255, 255);

  if (!mKbdVisible) {
    return;
  }

  FillRect (Gop, 0, KBD_TOP - 4, SCREEN_W, (KBD_ROWS * (KEY_H + KEY_GAP_Y)) + 8, 20, 22, 28);

  for (Row = 0; Row < KBD_ROWS; Row++) {
    for (Col = 0; Col < KBD_COLS; Col++) {
      UINTN Kx = 1 + Col * (KEY_W + KEY_GAP_X);
      UINTN Ky = KBD_TOP + Row * (KEY_H + KEY_GAP_Y);
      CHAR16 Ch = mShiftActive ? mLayoutShift[Row][Col] : mLayoutNormal[Row][Col];

      if (Row == 4 && Col >= 3 && Col <= 6) {
        FillRect (Gop, Kx, Ky, KEY_W + KEY_GAP_X, KEY_H, 65, 70, 82);
      } else if (Ch == L'\r') {
        FillRect (Gop, Kx, Ky, KEY_W, KEY_H, 35, 130, 70);
      } else if (Ch == L'\b') {
        FillRect (Gop, Kx, Ky, KEY_W, KEY_H, 150, 45, 45);
      } else if (Ch == L'^') {
        FillRect (Gop, Kx, Ky, KEY_W, KEY_H, mShiftActive ? 45 : 55, mShiftActive ? 130 : 60, mShiftActive ? 220 : 72);
      } else {
        FillRect (Gop, Kx, Ky, KEY_W, KEY_H, 50, 54, 64);
      }
      DrawKeycapPattern (Gop, Kx, Ky, Ch);
    }
  }
}

STATIC VOID PushKey (CHAR16 UnicodeChar, UINT16 ScanCode, EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This) {
  UINTN NextTail = (mQueueTail + 1) % KEY_QUEUE_SIZE;
  if (NextTail == mQueueHead) {
    return;
  }
  mKeyQueue[mQueueTail].ScanCode    = ScanCode;
  mKeyQueue[mQueueTail].UnicodeChar = UnicodeChar;
  mQueueTail = NextTail;
  gBS->SignalEvent (This->WaitForKey);
}

STATIC VOID HandleTouchAt (UINT64 X, UINT64 Y, EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This) {
  if (X >= TOGGLE_X && X <= (TOGGLE_X + TOGGLE_W) &&
      Y >= TOGGLE_Y && Y <= (TOGGLE_Y + TOGGLE_H)) {
    mKbdVisible = !mKbdVisible;
    if (!mKbdVisible) {
      EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
      if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) && Gop != NULL) {
        FillRect (Gop, 0, KBD_TOP - 4, SCREEN_W, (KBD_ROWS * (KEY_H + KEY_GAP_Y)) + 8, 0, 0, 0);
      }
    }
    RenderOsk ();
    return;
  }

  if (!mKbdVisible || Y < KBD_TOP) {
    return;
  }

  UINTN Row = (UINTN)((Y - KBD_TOP) / (KEY_H + KEY_GAP_Y));
  UINTN Col = (UINTN)(X / (KEY_W + KEY_GAP_X));
  if (Row >= KBD_ROWS || Col >= KBD_COLS) {
    return;
  }

  CHAR16 Ch = mShiftActive ? mLayoutShift[Row][Col] : mLayoutNormal[Row][Col];
  if (Ch == L'^') {
    mShiftActive = !mShiftActive;
    RenderOsk ();
    return;
  }

  if (Ch == L'<') {
    PushKey (CHAR_NULL, SCAN_UP, This);
  } else if (Ch == L'>') {
    PushKey (CHAR_NULL, SCAN_DOWN, This);
  } else {
    PushKey (Ch, SCAN_NULL, This);
  }

  if (mShiftActive) {
    mShiftActive = FALSE;
    RenderOsk ();
  }
}

STATIC VOID EFIAPI OskTimerCallback (IN EFI_EVENT Event, IN VOID *Context) {
  EFI_ABSOLUTE_POINTER_PROTOCOL *AbsPtr = NULL;
  EFI_ABSOLUTE_POINTER_STATE State;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)Context;
  STATIC UINTN RedrawCounter = 0;

  if ((RedrawCounter++ % 40) == 0) {
    RenderOsk ();
  }

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&AbsPtr)) || AbsPtr == NULL) {
    return;
  }

  if (!EFI_ERROR (AbsPtr->GetState (AbsPtr, &State))) {
    BOOLEAN Touching = ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0);
    if (Touching && !mPrevTouching) {
      HandleTouchAt (State.CurrentX, State.CurrentY, This);
    }
    mPrevTouching = Touching;
  }
}

STATIC EFI_STATUS EFIAPI OskReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  mQueueHead = 0;
  mQueueTail = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI OskReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (mQueueHead == mQueueTail) {
    return EFI_NOT_READY;
  }
  *Key = mKeyQueue[mQueueHead];
  mQueueHead = (mQueueHead + 1) % KEY_QUEUE_SIZE;
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI OskWaitForKey (IN EFI_EVENT Event, IN VOID *Context) {
  OskTimerCallback (Event, Context);
  if (mQueueHead != mQueueTail) {
    gBS->SignalEvent (Event);
  }
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mOskTextIn = {
  OskReset,
  OskReadKeyStroke,
  NULL
};

EFI_STATUS EFIAPI OskEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  OskWaitForKey,
                  &mOskTextIn,
                  &mOskTextIn.WaitForKey
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiSimpleTextInProtocolGuid, &mOskTextIn,
                  &gEfiDevicePathProtocolGuid, &mOskDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OskTimerCallback,
                  &mOskTextIn,
                  &mOskTimer
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mOskTimer, TimerPeriodic, 250000);
  }

  return EFI_SUCCESS;
}
