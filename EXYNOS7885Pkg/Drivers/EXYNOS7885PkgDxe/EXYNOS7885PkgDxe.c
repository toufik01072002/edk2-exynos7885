#include <Configuration/BootDevices.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/Cpu.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include "EXYNOS7885PkgDxe.h"

#define EXYNOS_GPA1_CON_ADDR  0x11CB0060
#define EXYNOS_GPA1_DAT_ADDR  0x11CB0064
#define EXYNOS_GPA1_PUD_ADDR  0x11CB0068

#define KEY_VOL_UP_BIT        (1 << 5)
#define KEY_VOL_DOWN_BIT      (1 << 6)
#define KEY_POWER_BIT         (1 << 7)
#define ALL_KEYS_MASK         (KEY_VOL_UP_BIT | KEY_VOL_DOWN_BIT | KEY_POWER_BIT)

#define FB_BASE_ADDR          0xEC000000ULL
#define SCREEN_W              720
#define SCREEN_H              1560

#define TOGGLE_X              585
#define TOGGLE_Y              1485
#define TOGGLE_W              125
#define TOGGLE_H              62

#define KBD_TOP               1120
#define KBD_ROWS              5
#define KBD_COLS              10
#define KEY_W                 70
#define KEY_H                 68
#define KEY_GAP_X             2
#define KEY_GAP_Y             3
#define KEY_QUEUE_SIZE        64

EFI_CPU_ARCH_PROTOCOL *gCpu;

STATIC EFI_HANDLE mKeypadHandle  = NULL;
STATIC EFI_EVENT  mConInFixEvent = NULL;
STATIC UINT32     mLastGpa1      = ALL_KEYS_MASK;
STATIC BOOLEAN    mSendNullClear = FALSE;

STATIC BOOLEAN    mKbdVisible    = FALSE;
STATIC BOOLEAN    mShiftActive   = FALSE;
STATIC BOOLEAN    mPrevTouching  = FALSE;
STATIC UINTN      mPollTick      = 0;

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

STATIC VOID PushVirtualKey (CHAR16 UnicodeChar, UINT16 ScanCode) {
  UINTN NextTail = (mQueueTail + 1) % KEY_QUEUE_SIZE;
  if (NextTail == mQueueHead) {
    return;
  }
  mKeyQueue[mQueueTail].ScanCode    = ScanCode;
  mKeyQueue[mQueueTail].UnicodeChar = UnicodeChar;
  mQueueTail = NextTail;
}

STATIC VOID FillRect (UINTN X, UINTN Y, UINTN W, UINTN H, UINT8 R, UINT8 G, UINT8 B) {
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)FB_BASE_ADDR;
  UINT32 Pixel = 0xFF000000U | ((UINT32)R << 16) | ((UINT32)G << 8) | (UINT32)B;
  UINTN Row, Col;

  if (X >= SCREEN_W || Y >= SCREEN_H) {
    return;
  }
  if (X + W > SCREEN_W) {
    W = SCREEN_W - X;
  }
  if (Y + H > SCREEN_H) {
    H = SCREEN_H - Y;
  }

  for (Row = Y; Row < Y + H; Row++) {
    volatile UINT32 *Line = Fb + (Row * SCREEN_W) + X;
    for (Col = 0; Col < W; Col++) {
      Line[Col] = Pixel;
    }
    WriteBackInvalidateDataCacheRange ((VOID *)Line, W * sizeof (UINT32));
  }
}

STATIC CONST UINT8 mFont8x8[95][8] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
  {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00},
  {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
  {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
  {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
  {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
  {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
  {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
  {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
  {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
  {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
  {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
  {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
  {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00},
  {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
  {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
  {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
  {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
  {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
  {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
  {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
  {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
  {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
  {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
  {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
  {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
  {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
  {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
  {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
  {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
  {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
  {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
  {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
  {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
  {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
  {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
  {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
  {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
  {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
  {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
  {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
  {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
  {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
  {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
  {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
  {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
  {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
  {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00},
  {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
  {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
  {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
  {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00},
  {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
  {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
  {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
  {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
  {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
  {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
  {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
  {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
  {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
  {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
  {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
  {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00},
  {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
  {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
  {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
  {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C},
  {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
  {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
  {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00},
  {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
  {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
  {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
  {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
  {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
  {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00},
  {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00},
  {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
  {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
  {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00},
  {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
  {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC},
  {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00},
  {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
  {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
  {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
  {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}
};

STATIC VOID DrawKeycapPattern (UINTN X, UINTN Y, CHAR16 Ch) {
  UINT8 R = 245, G = 248, B = 252;
  CONST CHAR16 *Label = NULL;
  CHAR16 Single[2];
  UINTN i = 0;

  if (Ch == L'\r') { R = 255; G = 255; B = 255; Label = L"ENT"; }
  else if (Ch == L'\b') { R = 255; G = 255; B = 255; Label = L"DEL"; }
  else if (Ch == L'^') { R = 255; G = 255; B = 255; Label = L"SHF"; }
  else {
    Single[0] = Ch;
    Single[1] = L'\0';
    Label = Single;
  }

  UINTN Len = (Label[1] == L'\0') ? 1 : 3;
  UINTN StartX = X + (KEY_W - (Len * 18 - 2)) / 2;
  UINTN StartY = Y + (KEY_H - 16) / 2;

  while (Label[i] != L'\0') {
    CHAR16 C = Label[i];
    if (C >= 32 && C <= 126) {
      UINTN Idx = (UINTN)(C - 32);
      UINTN Row, Col;
      for (Row = 0; Row < 8; Row++) {
        UINT8 Bits = mFont8x8[Idx][Row];
        for (Col = 0; Col < 8; Col++) {
          if ((Bits & (0x80U >> Col)) != 0) {
            FillRect (StartX + i * 18 + Col * 2, StartY + Row * 2, 2, 2, R, G, B);
          }
        }
      }
    }
    i++;
  }
}

STATIC VOID RenderOsk (VOID) {
  UINTN Row, Col;

  if (mKbdVisible) {
    FillRect (TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 200, 60, 60);
  } else {
    FillRect (TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, 30, 120, 210);
  }
  FillRect (TOGGLE_X + 20, TOGGLE_Y + 26, 85, 10, 255, 255, 255);

  if (!mKbdVisible) {
    return;
  }

  FillRect (0, KBD_TOP - 4, SCREEN_W, (KBD_ROWS * (KEY_H + KEY_GAP_Y)) + 8, 20, 22, 28);

  for (Row = 0; Row < KBD_ROWS; Row++) {
    for (Col = 0; Col < KBD_COLS; Col++) {
      UINTN Kx = 1 + Col * (KEY_W + KEY_GAP_X);
      UINTN Ky = KBD_TOP + Row * (KEY_H + KEY_GAP_Y);
      CHAR16 Ch = mShiftActive ? mLayoutShift[Row][Col] : mLayoutNormal[Row][Col];

      if (Row == 4 && Col >= 3 && Col <= 6) {
        FillRect (Kx, Ky, KEY_W + KEY_GAP_X, KEY_H, 65, 70, 82);
      } else if (Ch == L'\r') {
        FillRect (Kx, Ky, KEY_W, KEY_H, 35, 130, 70);
      } else if (Ch == L'\b') {
        FillRect (Kx, Ky, KEY_W, KEY_H, 150, 45, 45);
      } else if (Ch == L'^') {
        FillRect (Kx, Ky, KEY_W, KEY_H, mShiftActive ? 45 : 55, mShiftActive ? 130 : 60, mShiftActive ? 220 : 72);
      } else {
        FillRect (Kx, Ky, KEY_W, KEY_H, 50, 54, 64);
      }
      DrawKeycapPattern (Kx, Ky, Ch);
    }
  }
}

STATIC VOID HandleTouchAt (UINT64 X, UINT64 Y) {
  if (X >= TOGGLE_X && X <= (TOGGLE_X + TOGGLE_W) &&
      Y >= TOGGLE_Y && Y <= (TOGGLE_Y + TOGGLE_H)) {
    mKbdVisible = !mKbdVisible;
    if (!mKbdVisible) {
      FillRect (0, KBD_TOP - 4, SCREEN_W, (KBD_ROWS * (KEY_H + KEY_GAP_Y)) + 8, 0, 0, 0);
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
    PushVirtualKey (CHAR_NULL, SCAN_UP);
  } else if (Ch == L'>') {
    PushVirtualKey (CHAR_NULL, SCAN_DOWN);
  } else {
    PushVirtualKey (Ch, SCAN_NULL);
  }

  if (mShiftActive) {
    mShiftActive = FALSE;
    RenderOsk ();
  }
}

STATIC VOID PollTouchAndRender (VOID) {
  EFI_ABSOLUTE_POINTER_PROTOCOL *AbsPtr = NULL;
  EFI_ABSOLUTE_POINTER_STATE State;

  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)FB_BASE_ADDR;
  UINT32 BtnPx = Fb[(TOGGLE_Y + 2) * SCREEN_W + (TOGGLE_X + 2)];
  UINT32 KbdPx = Fb[(KBD_TOP + 2) * SCREEN_W + 2];
  if (BtnPx == 0xFF000000U || BtnPx == 0x00000000U ||
      (mKbdVisible && (KbdPx == 0xFF000000U || KbdPx == 0x00000000U))) {
    RenderOsk ();
  }

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&AbsPtr)) || AbsPtr == NULL) {
    return;
  }

  if (!EFI_ERROR (AbsPtr->GetState (AbsPtr, &State))) {
    BOOLEAN Touching = ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0);
    if (Touching && !mPrevTouching) {
      HandleTouchAt (State.CurrentX, State.CurrentY);
    }
    mPrevTouching = Touching;
  }
}

STATIC BOOLEAN PollHardwareKey (OUT EFI_INPUT_KEY *Key OPTIONAL) {
  UINT32 RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
  UINT32 CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
  UINT32 Fall1   = (mLastGpa1 & ~CurGpa1);

  PollTouchAndRender ();

  if (mQueueHead != mQueueTail) {
    if (Key != NULL) {
      *Key = mKeyQueue[mQueueHead];
    }
    mQueueHead = (mQueueHead + 1) % KEY_QUEUE_SIZE;
    return TRUE;
  }

  if (Fall1 != 0) {
    MicroSecondDelay (30000);
    RawGpa1 = MmioRead32 (EXYNOS_GPA1_DAT_ADDR);
    CurGpa1 = RawGpa1 & ALL_KEYS_MASK;
    Fall1   = (mLastGpa1 & ~CurGpa1);
  }

  mLastGpa1 = CurGpa1;

  if (Fall1 & KEY_VOL_UP_BIT) {
    mSendNullClear = TRUE;
    if (Key != NULL) { Key->ScanCode = SCAN_UP; Key->UnicodeChar = 0; }
    return TRUE;
  }
  if (Fall1 & KEY_VOL_DOWN_BIT) {
    mSendNullClear = TRUE;
    if (Key != NULL) { Key->ScanCode = SCAN_DOWN; Key->UnicodeChar = 0; }
    return TRUE;
  }
  if (Fall1 & KEY_POWER_BIT) {
    mSendNullClear = TRUE;
    if (Key != NULL) { Key->ScanCode = SCAN_NULL; Key->UnicodeChar = CHAR_CARRIAGE_RETURN; }
    return TRUE;
  }

  if (mSendNullClear) {
    mSendNullClear = FALSE;
    if (Key != NULL) { Key->ScanCode = SCAN_NULL; Key->UnicodeChar = CHAR_NULL; }
    return TRUE;
  }

  return FALSE;
}

STATIC EFI_STATUS EFIAPI KeypadReset (IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, IN BOOLEAN ExtendedVerification) {
  mLastGpa1      = MmioRead32 (EXYNOS_GPA1_DAT_ADDR) & ALL_KEYS_MASK;
  mSendNullClear = FALSE;
  mQueueHead     = 0;
  mQueueTail     = 0;
  RenderOsk ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI KeypadReadKeyStroke (IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, OUT EFI_INPUT_KEY *Key) {
  if (Key == NULL) { return EFI_INVALID_PARAMETER; }
  if (PollHardwareKey (Key)) { return EFI_SUCCESS; }
  return EFI_NOT_READY;
}

STATIC VOID EFIAPI KeypadWaitForKey (IN EFI_EVENT Event, IN VOID *Context) {
  PollTouchAndRender ();
  gBS->SignalEvent (Event);
}

STATIC EFI_STATUS EFIAPI KeypadResetEx (IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, IN BOOLEAN ExtendedVerification) {
  return KeypadReset (NULL, ExtendedVerification);
}

STATIC EFI_STATUS EFIAPI KeypadReadKeyStrokeEx (IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, OUT EFI_KEY_DATA *KeyData) {
  EFI_STATUS Status;
  if (KeyData == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (KeyData, sizeof (EFI_KEY_DATA));
  Status = KeypadReadKeyStroke (NULL, &KeyData->Key);
  if (!EFI_ERROR (Status)) {
    KeyData->KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID;
    KeyData->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI KeypadSetState (IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, IN EFI_KEY_TOGGLE_STATE *KeyToggleState) {
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI KeypadRegisterKeyNotify (IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, IN EFI_KEY_DATA *KeyData, IN EFI_KEY_NOTIFY_FUNCTION KeyNotificationFunction, OUT VOID **NotifyHandle) {
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI KeypadUnregisterKeyNotify (IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, IN VOID *NotificationHandle) {
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeypadTextIn = { KeypadReset, KeypadReadKeyStroke, NULL };
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mKeypadTextInEx = {
  KeypadResetEx, KeypadReadKeyStrokeEx, NULL, KeypadSetState, KeypadRegisterKeyNotify, KeypadUnregisterKeyNotify
};

STATIC VOID EFIAPI ForceBindConInCallback (IN EFI_EVENT Event, IN VOID *Context) {
  if (mKeypadHandle != NULL) {
    gST->ConsoleInHandle = mKeypadHandle;
    gST->ConIn           = &mKeypadTextIn;
    gBS->Hdr.CRC32       = 0;
    gBS->CalculateCrc32 (gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);
    RenderOsk ();
  }
}

EFI_STATUS EFIAPI EXYNOS7885PkgEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
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
                  &mKeypadHandle,
                  &gEfiDevicePathProtocolGuid, &KeyPadDxeDevicePath,
                  &gEFIDroidKeypadDeviceProtocolGuid, &mKeypadTextIn,
                  &gEfiSimpleTextInProtocolGuid, &mKeypadTextIn,
                  &gEfiSimpleTextInputExProtocolGuid, &mKeypadTextInEx,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  gBS->CreateEventEx (
         EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ForceBindConInCallback, NULL,
         &gEfiEndOfDxeEventGroupGuid, &mConInFixEvent
         );

  return EFI_SUCCESS;
}
