#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>

#define PINCTRL_ALIVE_BASE      0x11CB0000ULL
#define GPA0_CON                (PINCTRL_ALIVE_BASE + 0x040)
#define GPA0_DAT                (PINCTRL_ALIVE_BASE + 0x044)
#define GPA0_PUD                (PINCTRL_ALIVE_BASE + 0x048)

#define PINCTRL_TOP_BASE        0x139B0000ULL
#define GPP4_CON                (PINCTRL_TOP_BASE + 0x0A0)
#define GPP4_DAT                (PINCTRL_TOP_BASE + 0x0A4)
#define GPP4_PUD                (PINCTRL_TOP_BASE + 0x0A8)

#define GPG2_CON                (PINCTRL_TOP_BASE + 0x160)
#define GPG2_DAT                (PINCTRL_TOP_BASE + 0x164)
#define GPG2_PUD                (PINCTRL_TOP_BASE + 0x168)

#define ZINITIX_I2C_ADDR        0x20

/* Commandes et registres Zinitix ZT7548 (exynos7884B-a20) */
#define BT532_SWRESET_CMD       0x0000
#define BT532_WAKEUP_CMD        0x0001
#define BT532_CLEAR_INT_CMD     0x0003
#define BT532_TOUCH_MODE        0x0010
#define BT532_POINT_STATUS_REG  0x0080
#define BT532_X_RESOLUTION      0x00C0
#define BT532_Y_RESOLUTION      0x00C1
#define BT532_INT_ENABLE_FLAG   0x00F0

#define ZINITIX_VENDOR_CMD_EN   0xC000
#define ZINITIX_PROG_START      0xC001
#define ZINITIX_NVM_INIT        0xC002
#define ZINITIX_INTN_CLEAR      0xC004
#define ZINITIX_CHIP_CODE_REG   0xCC00

#define BIT_PT_EXIST            (1U << 11)
#define BIT_MUST_ZERO           (1U << 13)
#define MAX_FINGERS             5

#pragma pack(1)
typedef struct {
  UINT16 X;
  UINT16 Y;
  UINT8  Width;
  UINT8  SubStatus;
} ZINITIX_COORD;

typedef struct {
  UINT16        Status;
  UINT8         FingerCnt;
  UINT8         TimeStamp;
  ZINITIX_COORD Coord[MAX_FINGERS];
} ZINITIX_POINT_INFO;

typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} TOUCH_DEVICE_PATH;
#pragma pack()

STATIC TOUCH_DEVICE_PATH mTouchDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x8B4E3A21, 0x7C9D, 0x4F12, { 0x9E, 0x5A, 0x3B, 0x8D, 0x1C, 0x6F, 0x4A, 0x02 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_ABSOLUTE_POINTER_MODE  mTouchMode;
STATIC EFI_ABSOLUTE_POINTER_STATE mLastState;
STATIC BOOLEAN                    mStateChanged   = FALSE;
STATIC BOOLEAN                    mHwReady        = FALSE;
STATIC UINT16                     mChipCode       = 0;

/* --- Bit-Bang I2C sur GPP4[0] (SDA) et GPP4[1] (SCL) --- */

STATIC VOID BbSdaLow (VOID) {
  UINT32 Dat = MmioRead32 (GPP4_DAT) & ~(1U << 0);
  UINT32 Con = (MmioRead32 (GPP4_CON) & ~0x0FU) | 0x01U;
  MmioWrite32 (GPP4_DAT, Dat);
  MmioWrite32 (GPP4_CON, Con);
}

STATIC VOID BbSdaHigh (VOID) {
  UINT32 Con = MmioRead32 (GPP4_CON) & ~0x0FU;
  MmioWrite32 (GPP4_CON, Con);
}

STATIC UINT8 BbSdaRead (VOID) {
  return (UINT8)(MmioRead32 (GPP4_DAT) & 0x1U);
}

STATIC VOID BbSclLow (VOID) {
  UINT32 Dat = MmioRead32 (GPP4_DAT) & ~(1U << 1);
  UINT32 Con = (MmioRead32 (GPP4_CON) & ~0xF0U) | 0x10U;
  MmioWrite32 (GPP4_DAT, Dat);
  MmioWrite32 (GPP4_CON, Con);
}

STATIC VOID BbSclHigh (VOID) {
  UINTN Timeout = 500;
  UINT32 Con = MmioRead32 (GPP4_CON) & ~0xF0U;
  MmioWrite32 (GPP4_CON, Con);
  while (((MmioRead32 (GPP4_DAT) & (1U << 1)) == 0) && Timeout--) {
    MicroSecondDelay (2);
  }
}

STATIC VOID BbStart (VOID) {
  BbSdaHigh ();
  BbSclHigh ();
  MicroSecondDelay (5);
  BbSdaLow ();
  MicroSecondDelay (5);
  BbSclLow ();
  MicroSecondDelay (5);
}

STATIC VOID BbStop (VOID) {
  BbSdaLow ();
  MicroSecondDelay (5);
  BbSclHigh ();
  MicroSecondDelay (5);
  BbSdaHigh ();
  MicroSecondDelay (5);
}

STATIC BOOLEAN BbWriteByte (UINT8 Byte) {
  UINTN Bit;
  BOOLEAN Ack;

  for (Bit = 0; Bit < 8; Bit++) {
    if ((Byte & 0x80U) != 0) {
      BbSdaHigh ();
    } else {
      BbSdaLow ();
    }
    MicroSecondDelay (3);
    BbSclHigh ();
    MicroSecondDelay (5);
    BbSclLow ();
    MicroSecondDelay (3);
    Byte <<= 1;
  }

  BbSdaHigh ();
  MicroSecondDelay (3);
  BbSclHigh ();
  MicroSecondDelay (5);
  Ack = (BbSdaRead () == 0);
  BbSclLow ();
  MicroSecondDelay (3);
  return Ack;
}

STATIC UINT8 BbReadByte (BOOLEAN SendAck) {
  UINTN Bit;
  UINT8 Byte = 0;

  BbSdaHigh ();
  for (Bit = 0; Bit < 8; Bit++) {
    Byte <<= 1;
    MicroSecondDelay (3);
    BbSclHigh ();
    MicroSecondDelay (5);
    if (BbSdaRead () != 0) {
      Byte |= 1U;
    }
    BbSclLow ();
    MicroSecondDelay (3);
  }

  if (SendAck) {
    BbSdaLow ();
  } else {
    BbSdaHigh ();
  }
  MicroSecondDelay (3);
  BbSclHigh ();
  MicroSecondDelay (5);
  BbSclLow ();
  BbSdaHigh ();
  MicroSecondDelay (3);
  return Byte;
}

STATIC EFI_STATUS I2cWriteBytes (UINT8 SlaveAddr, CONST UINT8 *Data, UINTN Len) {
  UINTN i;
  BbStart ();
  if (!BbWriteByte ((UINT8)((SlaveAddr << 1) | 0x0U))) {
    BbStop ();
    return EFI_NO_RESPONSE;
  }
  for (i = 0; i < Len; i++) {
    if (!BbWriteByte (Data[i])) {
      BbStop ();
      return EFI_DEVICE_ERROR;
    }
  }
  BbStop ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS I2cReadReg (UINT16 Reg, UINT8 *Buf, UINTN Len) {
  UINT8 RegBytes[2];
  UINTN i;

  RegBytes[0] = (UINT8)(Reg & 0xFF);
  RegBytes[1] = (UINT8)((Reg >> 8) & 0xFF);

  BbStart ();
  if (!BbWriteByte ((UINT8)((ZINITIX_I2C_ADDR << 1) | 0x0U))) {
    BbStop ();
    return EFI_NO_RESPONSE;
  }
  if (!BbWriteByte (RegBytes[0]) || !BbWriteByte (RegBytes[1])) {
    BbStop ();
    return EFI_DEVICE_ERROR;
  }

  MicroSecondDelay (30);
  BbStart ();
  if (!BbWriteByte ((UINT8)((ZINITIX_I2C_ADDR << 1) | 0x1U))) {
    BbStop ();
    return EFI_NO_RESPONSE;
  }
  for (i = 0; i < Len; i++) {
    Buf[i] = BbReadByte (i + 1 < Len);
  }
  BbStop ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ZinitixWriteCmd (UINT16 Cmd) {
  UINT8 Buf[2];
  Buf[0] = (UINT8)(Cmd & 0xFF);
  Buf[1] = (UINT8)((Cmd >> 8) & 0xFF);
  return I2cWriteBytes (ZINITIX_I2C_ADDR, Buf, 2);
}

STATIC EFI_STATUS ZinitixWriteReg (UINT16 Reg, UINT16 Val) {
  UINT8 Buf[4];
  Buf[0] = (UINT8)(Reg & 0xFF);
  Buf[1] = (UINT8)((Reg >> 8) & 0xFF);
  Buf[2] = (UINT8)(Val & 0xFF);
  Buf[3] = (UINT8)((Val >> 8) & 0xFF);
  return I2cWriteBytes (ZINITIX_I2C_ADDR, Buf, 4);
}

STATIC VOID GpioAndBusInit (VOID) {
  UINT32 Val;

  Val = (MmioRead32 (GPG2_CON) & ~(0xFU << 4)) | (0x1U << 4);
  MmioWrite32 (GPG2_CON, Val);
  Val = MmioRead32 (GPG2_PUD) & ~(0x3U << 2);
  MmioWrite32 (GPG2_PUD, Val);

  MmioWrite32 (GPG2_DAT, MmioRead32 (GPG2_DAT) & ~(1U << 1));
  MicroSecondDelay (20000);
  MmioWrite32 (GPG2_DAT, MmioRead32 (GPG2_DAT) | (1U << 1));

  Val = MmioRead32 (GPA0_CON) & ~0xFU;
  MmioWrite32 (GPA0_CON, Val);
  Val = (MmioRead32 (GPA0_PUD) & ~0x3U) | 0x3U;
  MmioWrite32 (GPA0_PUD, Val);

  Val = MmioRead32 (GPP4_CON) & ~0xFFU;
  MmioWrite32 (GPP4_CON, Val);
  Val = (MmioRead32 (GPP4_PUD) & ~0xFU) | 0xFU;
  MmioWrite32 (GPP4_PUD, Val);

  MicroSecondDelay (100000);
}

STATIC EFI_STATUS ZinitixPowerSequence (VOID) {
  UINTN Retry;
  EFI_STATUS Status;

  for (Retry = 0; Retry < 5; Retry++) {
    Status = ZinitixWriteReg (ZINITIX_VENDOR_CMD_EN, 0x0001);
    if (EFI_ERROR (Status)) {
      MicroSecondDelay (20000);
      continue;
    }
    MicroSecondDelay (20);

    Status = I2cReadReg (ZINITIX_CHIP_CODE_REG, (UINT8 *)&mChipCode, sizeof (mChipCode));
    if (EFI_ERROR (Status)) {
      MicroSecondDelay (20000);
      continue;
    }
    MicroSecondDelay (20);

    if (EFI_ERROR (ZinitixWriteCmd (ZINITIX_INTN_CLEAR))) {
      MicroSecondDelay (20000);
      continue;
    }
    MicroSecondDelay (20);

    if (EFI_ERROR (ZinitixWriteReg (ZINITIX_NVM_INIT, 0x0001))) {
      MicroSecondDelay (20000);
      continue;
    }
    MicroSecondDelay (3000);

    if (EFI_ERROR (ZinitixWriteReg (ZINITIX_PROG_START, 0x0001))) {
      MicroSecondDelay (20000);
      continue;
    }
    MicroSecondDelay (150000);
    return EFI_SUCCESS;
  }

  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS ZinitixHwInit (VOID) {
  EFI_STATUS Status;
  UINTN Retry;

  Status = ZinitixPowerSequence ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = 0; Retry < 8; Retry++) {
    Status = ZinitixWriteCmd (BT532_WAKEUP_CMD);
    if (!EFI_ERROR (Status)) {
      break;
    }
    MicroSecondDelay (15000);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (10000);
  ZinitixWriteCmd (BT532_SWRESET_CMD);
  MicroSecondDelay (100000);

  ZinitixWriteReg (BT532_TOUCH_MODE, 0x0000);
  ZinitixWriteReg (BT532_X_RESOLUTION, 720);
  ZinitixWriteReg (BT532_Y_RESOLUTION, 1560);
  ZinitixWriteReg (BT532_INT_ENABLE_FLAG, 0x000F);

  ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
  MicroSecondDelay (1000);
  ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
  return EFI_SUCCESS;
}

STATIC VOID PollHardwareTouch (IN EFI_ABSOLUTE_POINTER_PROTOCOL *This) {
  ZINITIX_POINT_INFO Info;

  if (!mHwReady) {
    if (!EFI_ERROR (ZinitixHwInit ())) {
      mHwReady = TRUE;
    } else {
      return;
    }
  }

  ZeroMem (&Info, sizeof (Info));
  if (EFI_ERROR (I2cReadReg (BT532_POINT_STATUS_REG, (UINT8 *)&Info, sizeof (Info)))) {
    return;
  }

  if ((MmioRead32 (GPA0_DAT) & 0x1U) == 0 || Info.Status != 0) {
    ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
  }

  if ((Info.Status & BIT_MUST_ZERO) != 0 || Info.Status == 0xFFFF || Info.Status == 0x0001) {
    return;
  }

  if ((Info.Status & BIT_PT_EXIST) != 0 && Info.FingerCnt > 0) {
    UINT16 X = Info.Coord[0].X;
    UINT16 Y = Info.Coord[0].Y;
    if (X < 720 && Y < 1560) {
      mLastState.CurrentX      = X;
      mLastState.CurrentY      = Y;
      mLastState.CurrentZ      = 1;
mLastState.ActiveButtons = EFI_ABSP_TouchActive;
mStateChanged            = TRUE;
if (This != NULL && This->WaitForInput != NULL) {
gBS->SignalEvent (This->WaitForInput);
}
}
} else {
if (mLastState.ActiveButtons != 0) {
mLastState.ActiveButtons = 0;
mLastState.CurrentZ      = 0;
mStateChanged            = TRUE;
if (This != NULL && This->WaitForInput != NULL) {
gBS->SignalEvent (This->WaitForInput);
}
}
}
}
STATIC EFI_STATUS EFIAPI TouchReset (
IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
IN BOOLEAN                        ExtendedVerification
)
{
ZeroMem (&mLastState, sizeof (mLastState));
mStateChanged = FALSE;
if (ExtendedVerification) {
mHwReady = !EFI_ERROR (ZinitixHwInit ());
}
return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI TouchGetState (
IN  EFI_ABSOLUTE_POINTER_PROTOCOL *This,
OUT EFI_ABSOLUTE_POINTER_STATE    *State
)
{
if (State == NULL) {
return EFI_INVALID_PARAMETER;
}
PollHardwareTouch (This);
CopyMem (State, &mLastState, sizeof (EFI_ABSOLUTE_POINTER_STATE));
if (!mStateChanged) {
return EFI_NOT_READY;
}
mStateChanged = FALSE;
return EFI_SUCCESS;
}
STATIC VOID EFIAPI TouchWaitForInput (IN EFI_EVENT Event, IN VOID *Context) {
PollHardwareTouch ((EFI_ABSOLUTE_POINTER_PROTOCOL *)Context);
if (mStateChanged) {
gBS->SignalEvent (Event);
}
}
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL mAbsolutePointer = {
TouchReset,
TouchGetState,
NULL,
&mTouchMode
};
EFI_STATUS EFIAPI ZinitixTouchEntryPoint (
IN EFI_HANDLE       ImageHandle,
IN EFI_SYSTEM_TABLE *SystemTable
)
{
EFI_STATUS Status;
EFI_HANDLE TouchHandle = NULL;
ZeroMem (&mTouchMode, sizeof (mTouchMode));
mTouchMode.AbsoluteMinX = 0;
mTouchMode.AbsoluteMinY = 0;
mTouchMode.AbsoluteMinZ = 0;
mTouchMode.AbsoluteMaxX = 720;
mTouchMode.AbsoluteMaxY = 1560;
mTouchMode.AbsoluteMaxZ = 1;
mTouchMode.Attributes   = 0;
GpioAndBusInit ();
mHwReady = !EFI_ERROR (ZinitixHwInit ());
Status = gBS->CreateEvent (
EVT_NOTIFY_WAIT,
TPL_NOTIFY,
TouchWaitForInput,
&mAbsolutePointer,
&mAbsolutePointer.WaitForInput
);
if (EFI_ERROR (Status)) {
return Status;
}
Status = gBS->InstallMultipleProtocolInterfaces (
&TouchHandle,
&gEfiAbsolutePointerProtocolGuid, &mAbsolutePointer,
&gEfiDevicePathProtocolGuid, &mTouchDevicePath,
NULL
);
return Status;
}
