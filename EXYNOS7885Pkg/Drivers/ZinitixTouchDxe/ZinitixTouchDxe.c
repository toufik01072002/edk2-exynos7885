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

#define I2C4_BASE               0x13870000ULL
#define I2CCON                  (I2C4_BASE + 0x00)
#define I2CSTAT                 (I2C4_BASE + 0x04)
#define I2CADD                  (I2C4_BASE + 0x08)
#define I2CDS                   (I2C4_BASE + 0x0C)
#define I2CLC                   (I2C4_BASE + 0x10)

#define I2CCON_ACKEN            (1U << 7)
#define I2CCON_IRQEN            (1U << 5)
#define I2CCON_IRQPEND          (1U << 4)

#define I2CSTAT_MASTER_RX       (2U << 6)
#define I2CSTAT_MASTER_TX       (3U << 6)
#define I2CSTAT_START_BUSY      (1U << 5)
#define I2CSTAT_TXRX_EN         (1U << 4)
#define I2CSTAT_LAST_BIT        (1U << 0)

#define ZINITIX_I2C_ADDR        0x20
#define BT532_SWRESET_CMD       0x0000
#define BT532_WAKEUP_CMD        0x0001
#define BT532_CLEAR_INT_CMD     0x0003
#define BT532_TOUCH_MODE        0x0010
#define BT532_POINT_STATUS_REG  0x0080
#define BT532_X_RESOLUTION      0x00C0
#define BT532_Y_RESOLUTION      0x00C1
#define BT532_INT_ENABLE_FLAG   0x00F0

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
STATIC EFI_EVENT                  mPollTimerEvent = NULL;

STATIC VOID GpioAndBusInit (VOID) {
  UINT32 Val;

  Val = MmioRead32 (GPG2_CON);
  Val = (Val & ~(0xFU << 4)) | (0x1U << 4);
  MmioWrite32 (GPG2_CON, Val);

  Val = MmioRead32 (GPG2_PUD);
  Val &= ~(0x3U << 2);
  MmioWrite32 (GPG2_PUD, Val);

  Val = MmioRead32 (GPG2_DAT);
  Val |= (1U << 1);
  MmioWrite32 (GPG2_DAT, Val);

  Val = MmioRead32 (GPA0_CON);
  Val &= ~0xFU;
  MmioWrite32 (GPA0_CON, Val);

  Val = MmioRead32 (GPA0_PUD);
  Val = (Val & ~0x3U) | 0x3U;
  MmioWrite32 (GPA0_PUD, Val);

  Val = MmioRead32 (GPP4_CON);
  Val = (Val & ~0xFFU) | 0x22U;
  MmioWrite32 (GPP4_CON, Val);

  Val = MmioRead32 (GPP4_PUD);
  Val = (Val & ~0xFU) | 0xFU;
  MmioWrite32 (GPP4_PUD, Val);

  MicroSecondDelay (50000);

  MmioWrite32 (I2CSTAT, 0x00);
  MmioWrite32 (I2CLC, 0x07);
  MmioWrite32 (I2CCON, I2CCON_ACKEN | I2CCON_IRQEN | 0x0F);
  MmioWrite32 (I2CSTAT, I2CSTAT_TXRX_EN);
}

STATIC EFI_STATUS I2cWaitIrq (VOID) {
  UINTN Timeout = 5000;
  while (Timeout--) {
    if (MmioRead32 (I2CCON) & I2CCON_IRQPEND) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (5);
  }
  return EFI_TIMEOUT;
}

STATIC VOID I2cClearIrq (BOOLEAN Ack) {
  UINT32 Con = MmioRead32 (I2CCON);
  if (Ack) {
    Con |= I2CCON_ACKEN;
  } else {
    Con &= ~I2CCON_ACKEN;
  }
  Con &= ~I2CCON_IRQPEND;
  MmioWrite32 (I2CCON, Con);
}

STATIC VOID I2cStop (UINT32 Mode) {
  UINTN Timeout = 1000;
  MmioWrite32 (I2CSTAT, Mode | I2CSTAT_TXRX_EN);
  I2cClearIrq (TRUE);
  while ((MmioRead32 (I2CSTAT) & I2CSTAT_START_BUSY) && Timeout--) {
    MicroSecondDelay (5);
  }
}

STATIC EFI_STATUS I2cWriteBytes (UINT8 SlaveAddr, CONST UINT8 *Data, UINTN Len) {
  UINTN i;
  MmioWrite32 (I2CCON, I2CCON_ACKEN | I2CCON_IRQEN | 0x0F);
  MmioWrite32 (I2CDS, (SlaveAddr << 1) | 0x0);
  MmioWrite32 (I2CSTAT, I2CSTAT_MASTER_TX | I2CSTAT_START_BUSY | I2CSTAT_TXRX_EN);

  if (EFI_ERROR (I2cWaitIrq ())) {
    I2cStop (I2CSTAT_MASTER_TX);
    return EFI_TIMEOUT;
  }
  if (MmioRead32 (I2CSTAT) & I2CSTAT_LAST_BIT) {
    I2cStop (I2CSTAT_MASTER_TX);
    return EFI_NO_RESPONSE;
  }

  for (i = 0; i < Len; i++) {
    MmioWrite32 (I2CDS, Data[i]);
    I2cClearIrq (TRUE);
    if (EFI_ERROR (I2cWaitIrq ())) {
      I2cStop (I2CSTAT_MASTER_TX);
      return EFI_TIMEOUT;
    }
    if (MmioRead32 (I2CSTAT) & I2CSTAT_LAST_BIT) {
      I2cStop (I2CSTAT_MASTER_TX);
      return EFI_DEVICE_ERROR;
    }
  }

  I2cStop (I2CSTAT_MASTER_TX);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS I2cReadReg (UINT16 Reg, UINT8 *Buf, UINTN Len) {
  EFI_STATUS Status;
  UINT8 RegBytes[2];
  UINTN i;

  RegBytes[0] = (UINT8)(Reg & 0xFF);
  RegBytes[1] = (UINT8)((Reg >> 8) & 0xFF);

  Status = I2cWriteBytes (ZINITIX_I2C_ADDR, RegBytes, 2);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (30);

  MmioWrite32 (I2CCON, I2CCON_ACKEN | I2CCON_IRQEN | 0x0F);
  MmioWrite32 (I2CDS, (ZINITIX_I2C_ADDR << 1) | 0x1);
  MmioWrite32 (I2CSTAT, I2CSTAT_MASTER_RX | I2CSTAT_START_BUSY | I2CSTAT_TXRX_EN);

  if (EFI_ERROR (I2cWaitIrq ())) {
    I2cStop (I2CSTAT_MASTER_RX);
    return EFI_TIMEOUT;
  }
  if (MmioRead32 (I2CSTAT) & I2CSTAT_LAST_BIT) {
    I2cStop (I2CSTAT_MASTER_RX);
    return EFI_NO_RESPONSE;
  }

  for (i = 0; i < Len; i++) {
    BOOLEAN SendAck = (i + 1 < Len);
    I2cClearIrq (SendAck);
    if (EFI_ERROR (I2cWaitIrq ())) {
      I2cStop (I2CSTAT_MASTER_RX);
      return EFI_TIMEOUT;
    }
    Buf[i] = (UINT8)(MmioRead32 (I2CDS) & 0xFF);
  }

  I2cStop (I2CSTAT_MASTER_RX);
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

STATIC EFI_STATUS ZinitixHwInit (VOID) {
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINTN Retry;

  for (Retry = 0; Retry < 5; Retry++) {
    Status = ZinitixWriteCmd (BT532_WAKEUP_CMD);
    if (!EFI_ERROR (Status)) {
      break;
    }
    MicroSecondDelay (10000);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (10000);
  ZinitixWriteCmd (BT532_SWRESET_CMD);
  MicroSecondDelay (20000);

  ZinitixWriteReg (BT532_TOUCH_MODE, 0x0000);
  ZinitixWriteReg (BT532_X_RESOLUTION, 720);
  ZinitixWriteReg (BT532_Y_RESOLUTION, 1560);
  ZinitixWriteReg (BT532_INT_ENABLE_FLAG, 0x000F);

  ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
  MicroSecondDelay (1000);
  ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI TouchPollCallback (IN EFI_EVENT Event, IN VOID *Context) {
  ZINITIX_POINT_INFO Info;
  EFI_ABSOLUTE_POINTER_PROTOCOL *This = (EFI_ABSOLUTE_POINTER_PROTOCOL *)Context;

  if ((MmioRead32 (GPA0_DAT) & 0x1U) != 0) {
    return;
  }

  ZeroMem (&Info, sizeof (Info));
  if (EFI_ERROR (I2cReadReg (BT532_POINT_STATUS_REG, (UINT8 *)&Info, sizeof (Info)))) {
    ZinitixWriteCmd (BT532_CLEAR_INT_CMD);
    return;
  }

  ZinitixWriteCmd (BT532_CLEAR_INT_CMD);

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
      gBS->SignalEvent (This->WaitForInput);
    }
  } else {
    if (mLastState.ActiveButtons != 0) {
      mLastState.ActiveButtons = 0;
      mLastState.CurrentZ      = 0;
      mStateChanged            = TRUE;
      gBS->SignalEvent (This->WaitForInput);
    }
  }
}

STATIC EFI_STATUS EFIAPI TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
  IN BOOLEAN                       ExtendedVerification
  )
{
  ZeroMem (&mLastState, sizeof (mLastState));
  mStateChanged = FALSE;
  if (ExtendedVerification) {
    ZinitixHwInit ();
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
  if (!mStateChanged) {
    return EFI_NOT_READY;
  }
  CopyMem (State, &mLastState, sizeof (EFI_ABSOLUTE_POINTER_STATE));
  mStateChanged = FALSE;
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI TouchWaitForInput (IN EFI_EVENT Event, IN VOID *Context) {
  TouchPollCallback (Event, Context);
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
Status = ZinitixHwInit ();
if (EFI_ERROR (Status)) {
DEBUG ((DEBUG_WARN, "ZinitixTouchDxe: init returned %r\n", Status));
}
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
if (EFI_ERROR (Status)) {
return Status;
}
Status = gBS->CreateEvent (
EVT_TIMER | EVT_NOTIFY_SIGNAL,
TPL_CALLBACK,
TouchPollCallback,
&mAbsolutePointer,
&mPollTimerEvent
);
if (!EFI_ERROR (Status)) {
gBS->SetTimer (mPollTimerEvent, TimerPeriodic, 150000);
}
return EFI_SUCCESS;
}
