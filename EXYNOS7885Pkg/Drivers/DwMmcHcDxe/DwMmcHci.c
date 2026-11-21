#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

#define IDMAC_MAX_DESCS       256
#define IDMAC_CHUNK_SIZE      4096
#define IDMAC_BOUNCE_SIZE     (IDMAC_MAX_DESCS * IDMAC_CHUNK_SIZE)
#define IDMAC_DMA_MAX_ADDR    0x87FFFFFFULL

typedef struct {
  BOOLEAN               Initialized;
  DW_MMC_IDMAC_DESC64   *DescRing;
  EFI_PHYSICAL_ADDRESS  DescPhys;
  UINT8                 *BounceBuf;
  EFI_PHYSICAL_ADDRESS  BouncePhys;
} DWMMC_DMA_CTX;

STATIC DWMMC_DMA_CTX mDmaCtx[EXYNOS_DWMMC_MAX_SLOTS];
STATIC UINT32        mSavedClkSel[EXYNOS_DWMMC_MAX_SLOTS] = { 0x03020001, 0x03020001 };
STATIC BOOLEAN       mRxTuned[EXYNOS_DWMMC_MAX_SLOTS]     = { FALSE, FALSE };
STATIC UINT32        mLastLoggedErr = 0;
STATIC UINT32        mCmdSeq        = 0;

STATIC
UINTN
DwMmcSlotIndex (
  IN UINTN BaseAddr
  )
{
  return (BaseAddr == EXYNOS_DWMMC0_BASE) ? 0 : 1;
}

STATIC
EFI_STATUS
DwMmcWaitForClear (
  IN UINTN  BaseAddr,
  IN UINT32 Offset,
  IN UINT32 Mask,
  IN UINT32 Timeout
  )
{
  while (Timeout > 0) {
    if ((MmioRead32 (BaseAddr + Offset) & Mask) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
    Timeout--;
  }

  return ((MmioRead32 (BaseAddr + Offset) & Mask) == 0)
         ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC
EFI_STATUS
DwMmcEnsureDmaContext (
  IN UINTN BaseAddr
  )
{
  UINTN                 Slot;
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  UINTN                 DescPages;
  UINTN                 BouncePages;

  Slot = DwMmcSlotIndex (BaseAddr);
  if (mDmaCtx[Slot].Initialized) {
    return EFI_SUCCESS;
  }

  DescPages   = EFI_SIZE_TO_PAGES (IDMAC_MAX_DESCS * sizeof (DW_MMC_IDMAC_DESC64));
  BouncePages = EFI_SIZE_TO_PAGES (IDMAC_BOUNCE_SIZE);

  Addr = IDMAC_DMA_MAX_ADDR;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  DescPages,
                  &Addr
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] Failed to alloc IDMAC desc ring: %r\n", (UINT32)BaseAddr, Status);
    return Status;
  }

  mDmaCtx[Slot].DescPhys = Addr;
  mDmaCtx[Slot].DescRing = (DW_MMC_IDMAC_DESC64 *)(UINTN)Addr;
  ZeroMem (mDmaCtx[Slot].DescRing, DescPages * EFI_PAGE_SIZE);

  Addr = IDMAC_DMA_MAX_ADDR;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  BouncePages,
                  &Addr
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] Failed to alloc IDMAC bounce buffer: %r\n", (UINT32)BaseAddr, Status);
    return Status;
  }

  mDmaCtx[Slot].BouncePhys = Addr;
  mDmaCtx[Slot].BounceBuf  = (UINT8 *)(UINTN)Addr;
  ZeroMem (mDmaCtx[Slot].BounceBuf, BouncePages * EFI_PAGE_SIZE);

  mDmaCtx[Slot].Initialized = TRUE;
  return EFI_SUCCESS;
}

STATIC
VOID
DwMmcEnableIdmacHw (
  IN UINTN BaseAddr
  )
{
  UINTN  Slot;
  UINT32 Ctrl;

  Slot = DwMmcSlotIndex (BaseAddr);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 100000);

  if (mDmaCtx[Slot].Initialized) {
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(mDmaCtx[Slot].DescPhys & 0xFFFFFFFFULL));
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(mDmaCtx[Slot].DescPhys >> 32));
  }

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, DW_MMC_IDMAC_INT_NI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_FBE);
  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl |= DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);
}

STATIC
EFI_STATUS
DwMmcRecoverController (
  IN UINTN  BaseAddr,
  IN UINT32 CmdIdx
  )
{
  EFI_STATUS Status;
  UINT32     Timeout;
  UINT32     RintSts;
  UINT32     Ctrl;
  UINTN      Slot;

  Slot = DwMmcSlotIndex (BaseAddr);

  Print (L"[DWMMC 0x%x] CMD%u recovery: CMD=0x%x RINT=0x%x STATUS=0x%x CTRL=0x%x CLKSEL=0x%x (saved=0x%x)\n",
         (UINT32)BaseAddr,
         CmdIdx,
         MmioRead32 (BaseAddr + DW_MMC_CMD),
         MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
         MmioRead32 (BaseAddr + DW_MMC_STATUS),
         MmioRead32 (BaseAddr + DW_MMC_CTRL),
         MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
         mSavedClkSel[Slot]);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl |= DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CTRL,
             DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET,
             100000
             );

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, mSavedClkSel[Slot]);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DwMmcEnableIdmacHw (BaseAddr);

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);

  MmioWrite32 (
    BaseAddr + DW_MMC_CMD,
    DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK | BIT29
    );

  Timeout = 100000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);

    if (RintSts & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      return EFI_DEVICE_ERROR;
    }

    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Timeout--;
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
DwMmcUpdateClock (
  IN UINTN BaseAddr
  )
{
  EFI_STATUS Status;
  UINT32     Timeout;
  UINT32     CmdVal;
  UINT32     RintSts;

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CMD,
             DW_MMC_CMD_START,
             100000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);

  CmdVal = DW_MMC_CMD_START |
           DW_MMC_CMD_UPD_CLK |
           DW_MMC_CMD_WAIT_PRVDATA |
           BIT29;

  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdVal);

  Timeout = 100000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);

    if (RintSts & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      return EFI_DEVICE_ERROR;
    }

    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Timeout--;
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
DwMmcInitController (
  IN UINTN BaseAddr
  )
{
  UINT32     ClkSelInit;
  UINT32     Ctrl;
  EFI_STATUS Status;
  UINTN      Slot;

  Slot = DwMmcSlotIndex (BaseAddr);

  Status = DwMmcEnsureDmaContext (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ClkSelInit = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (ClkSelInit != 0) {
    mSavedClkSel[Slot] = ClkSelInit;
  }

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, 0);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, 0);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_USE_IDMAC |
            DW_MMC_CTRL_DMA_ENABLE |
            DW_MMC_CTRL_INT_ENABLE);
  Ctrl |= DW_MMC_CTRL_RESET |
          DW_MMC_CTRL_FIFO_RESET |
          DW_MMC_CTRL_DMA_RESET;

  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CTRL,
             DW_MMC_CTRL_RESET |
             DW_MMC_CTRL_FIFO_RESET |
             DW_MMC_CTRL_DMA_RESET,
             100000
             );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] controller reset timeout\n", (UINT32)BaseAddr);
    return Status;
  }

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, mSavedClkSel[Slot]);
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2 << 28) | (15 << 16) | 16);

  DwMmcEnableIdmacHw (BaseAddr);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0] VERID=0x%x HCON=0x%x CTRL=0x%x CLKSEL=0x%x Desc=0x%lx\n",
           MmioRead32 (BaseAddr + DW_MMC_VERID),
           MmioRead32 (BaseAddr + DW_MMC_HCON),
           MmioRead32 (BaseAddr + DW_MMC_CTRL),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
           (UINT64)mDmaCtx[Slot].DescPhys);
  }

  return DwMmcSetClock (BaseAddr, 400);
}

EFI_STATUS
DwMmcSetClock (
  IN UINTN  BaseAddr,
  IN UINT32 ClockFreqKhz
  )
{
  UINT32     Div;
  EFI_STATUS Status;

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, 0);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ClockFreqKhz == 0) {
    return EFI_SUCCESS;
  }

  if (ClockFreqKhz <= 400) {
    Div = 64;
  } else if (ClockFreqKhz <= 25000) {
    Div = 16;
  } else {
    Div = 8;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CLKSRC, 0);
  MmioWrite32 (BaseAddr + DW_MMC_CLKDIV, Div);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, DW_MMC_CLKENA_CCLK_ENABLE);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    MmioWrite32 (BaseAddr + DW_MMC_CLKENA, 0);
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcSetBusWidth (
  IN UINTN BaseAddr,
  IN UINT8 BusWidth
  )
{
  UINT32 Ctype = DW_MMC_CTYPE_1BIT;

  if (BaseAddr != EXYNOS_DWMMC0_BASE) {
    if (BusWidth == 4) {
      Ctype = DW_MMC_CTYPE_4BIT;
    } else if (BusWidth == 8) {
      Ctype = DW_MMC_CTYPE_8BIT;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, Ctype);
  return DwMmcUpdateClock (BaseAddr);
}

STATIC
EFI_STATUS
DwMmcSetupIdmacDescriptors (
  IN UINTN   BaseAddr,
  IN BOOLEAN IsRead,
  IN VOID    *UserBuffer,
  IN UINT32  TotalBytes
  )
{
  UINTN                Slot;
  UINT32               NumDescs;
  UINT32               Remaining;
  UINT32               Offset;
  UINT32               i;
  DW_MMC_IDMAC_DESC64  *Ring;
  EFI_PHYSICAL_ADDRESS BufPhysBase;
  EFI_PHYSICAL_ADDRESS DescPhysBase;
  UINT32               Ctrl;

  if (TotalBytes > IDMAC_BOUNCE_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Slot         = DwMmcSlotIndex (BaseAddr);
  Ring         = mDmaCtx[Slot].DescRing;
  BufPhysBase  = mDmaCtx[Slot].BouncePhys;
  DescPhysBase = mDmaCtx[Slot].DescPhys;

  if (!IsRead) {
    CopyMem (mDmaCtx[Slot].BounceBuf, UserBuffer, TotalBytes);
  } else {
    ZeroMem (mDmaCtx[Slot].BounceBuf, TotalBytes);
  }

  WriteBackInvalidateDataCacheRange (mDmaCtx[Slot].BounceBuf, TotalBytes);

  NumDescs  = (TotalBytes + IDMAC_CHUNK_SIZE - 1) / IDMAC_CHUNK_SIZE;
  Remaining = TotalBytes;
  Offset    = 0;

  for (i = 0; i < NumDescs; i++) {
    UINT32 Chunk   = (Remaining > IDMAC_CHUNK_SIZE) ? IDMAC_CHUNK_SIZE : Remaining;
    UINT32 Flags   = IDMAC_DES0_OWN | IDMAC_DES0_DIC;
    UINT64 BufAddr = BufPhysBase + Offset;
    UINT64 NextAddr;

    if (i == 0) {
      Flags |= IDMAC_DES0_FS;
    }
    if (i == NumDescs - 1) {
      Flags |= IDMAC_DES0_LD | IDMAC_DES0_ER;
      Flags &= ~IDMAC_DES0_DIC;
      NextAddr = 0;
    } else {
      Flags |= IDMAC_DES0_CH;
      NextAddr = DescPhysBase + ((i + 1) * sizeof (DW_MMC_IDMAC_DESC64));
    }

    Ring[i].Des0           = Flags;
    Ring[i].Des1           = 0;
    Ring[i].Des2           = IDMAC_SET_BUFFER1_SIZE (Chunk);
    Ring[i].Des3           = 0;
    Ring[i].Buffer1AddrLo  = (UINT32)(BufAddr & 0xFFFFFFFFULL);
    Ring[i].Buffer1AddrHi  = (UINT32)(BufAddr >> 32);
    Ring[i].NextDescAddrLo = (UINT32)(NextAddr & 0xFFFFFFFFULL);
    Ring[i].NextDescAddrHi = (UINT32)(NextAddr >> 32);

    Offset    += Chunk;
    Remaining -= Chunk;
  }

  WriteBackInvalidateDataCacheRange (Ring, NumDescs * sizeof (DW_MMC_IDMAC_DESC64));

  MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
  DwMmcWaitForClear (BaseAddr, DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET, 100000);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 100000);

  MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(DescPhysBase & 0xFFFFFFFFULL));
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(DescPhysBase >> 32));
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, DW_MMC_IDMAC_INT_NI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_FBE);
  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl |= DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcExecCommandInternal (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  IN     BOOLEAN                             AllowRxRetune
  )
{
  UINT32     CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32     RintSts;
  UINT32     IdSts;
  UINT32     Timeout;
  EFI_STATUS Status;
  BOOLEAN    IsRead     = FALSE;
  UINT32     TotalBytes = 0;
  VOID       *UserData  = NULL;
  UINT16     CmdIdx;
  UINT32     CmdArg;
  BOOLEAN    IsDeselect;
  UINTN      Slot;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Slot       = DwMmcSlotIndex (BaseAddr);
  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);

  Status = DwMmcEnsureDmaContext (BaseAddr);
  if (EFI_ERROR (Status)) {
    Packet->TransactionStatus = Status;
    return Status;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE && AllowRxRetune) {
    mCmdSeq++;
    if (mCmdSeq >= 18 && mCmdSeq <= 32) {
      Print (L"[DW0 #%u] CMD%d(0x%x) RespType=%d InLen=%u\n",
             mCmdSeq, CmdIdx, CmdArg, Packet->SdMmcCmdBlk->ResponseType, Packet->InTransferLength);
    }
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    CmdReg |= DW_MMC_CMD_WAIT_PRVDATA;
  }

  if (CmdIdx == 6 && BaseAddr == EXYNOS_DWMMC0_BASE) {
    UINT8 Index = (CmdArg >> 16) & 0xFF;
    UINT8 Value = (CmdArg >> 8) & 0xFF;
    if ((Index == 183 || Index == 185) && Value != 0) {
      if (Packet->SdMmcStatusBlk != NULL) {
        Packet->SdMmcStatusBlk->Resp0 = 0x00000900;
      }
      Packet->TransactionStatus = EFI_SUCCESS;
      return EFI_SUCCESS;
    }
  }

  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 500000);
  if (EFI_ERROR (Status)) {
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (BaseAddr, DW_MMC_STATUS, DW_MMC_STATUS_DATA_BUSY, 500000);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }
  }

  if (CmdIdx == 7 || CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18) {
    if (MmioRead32 (BaseAddr + DW_MMC_CLKDIV) == 64) {
      Status = DwMmcSetClock (BaseAddr, 25000);
      if (EFI_ERROR (Status)) {
        Packet->TransactionStatus = Status;
        return Status;
      }
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);

  CmdReg |= (CmdIdx & 0x3F);

  if (CmdIdx == 0) {
    CmdReg |= DW_MMC_CMD_SEND_INIT;
  }

  if (!IsDeselect) {
    switch (Packet->SdMmcCmdBlk->ResponseType) {
      case SdMmcResponseTypeR1:
      case SdMmcResponseTypeR1b:
      case SdMmcResponseTypeR6:
      case SdMmcResponseTypeR7:
        CmdReg |= (DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_CHECK_RESP_CRC);
        break;
      case SdMmcResponseTypeR2:
        CmdReg |= (DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_RESP_LENGTH | DW_MMC_CMD_CHECK_RESP_CRC);
        break;
      case SdMmcResponseTypeR3:
      case SdMmcResponseTypeR4:
        CmdReg |= DW_MMC_CMD_RESP_EXP;
        break;
      default:
        break;
    }
  }

  if (Packet->InDataBuffer != NULL && Packet->InTransferLength > 0) {
    IsRead     = TRUE;
    TotalBytes = Packet->InTransferLength;
    UserData   = Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    UserData   = Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0) {
    Status = DwMmcSetupIdmacDescriptors (BaseAddr, IsRead, UserData, TotalBytes);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, CmdArg);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  if (TotalBytes > 0) {
    MmioWrite32 (BaseAddr + DW_MMC_PLDMND, 1);
  }

  Timeout = 500000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & (DW_MMC_INT_RE | DW_MMC_INT_RCRC | DW_MMC_INT_RTO | DW_MMC_INT_HLE)) {
      if (mLastLoggedErr < 8) {
        Print (L"[DWMMC 0x%x] CMD%d(0x%x) cmd err RINTSTS=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, CmdArg, RintSts);
        mLastLoggedErr++;
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      Packet->TransactionStatus = EFI_DEVICE_ERROR;
      return EFI_DEVICE_ERROR;
    }
    if (RintSts & DW_MMC_INT_CMD_DONE) {
      break;
    }
    MicroSecondDelay (1);
    Timeout--;
  }

  if ((MmioRead32 (BaseAddr + DW_MMC_RINTSTS) & DW_MMC_INT_CMD_DONE) == 0) {
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT cmd_done\n", (UINT32)BaseAddr, CmdIdx);
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (Packet->SdMmcStatusBlk != NULL && !IsDeselect) {
    if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2) {
      Packet->SdMmcStatusBlk->Resp0 = MmioRead32 (BaseAddr + DW_MMC_RESP0);
      Packet->SdMmcStatusBlk->Resp1 = MmioRead32 (BaseAddr + DW_MMC_RESP1);
      Packet->SdMmcStatusBlk->Resp2 = MmioRead32 (BaseAddr + DW_MMC_RESP2);
      Packet->SdMmcStatusBlk->Resp3 = MmioRead32 (BaseAddr + DW_MMC_RESP3);
    } else if (Packet->SdMmcCmdBlk->ResponseType >= SdMmcResponseTypeR1 &&
               Packet->SdMmcCmdBlk->ResponseType <= SdMmcResponseTypeR7) {
      Packet->SdMmcStatusBlk->Resp0 = MmioRead32 (BaseAddr + DW_MMC_RESP0);
    }
  }

  if (TotalBytes > 0) {
    UINT32 DoneMask = IsRead ? DW_MMC_IDMAC_INT_RI : DW_MMC_IDMAC_INT_TI;
    Timeout = 2000000;

    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if ((IdSts & DW_MMC_IDMAC_INT_FBE) || (RintSts & DW_MMC_INT_ERROR)) {
        if (IsRead &&
            AllowRxRetune &&
            !mRxTuned[Slot] &&
            (IdSts & DW_MMC_IDMAC_INT_FBE) == 0 &&
            (RintSts & DW_MMC_INT_ERROR) == DW_MMC_INT_DCRC) {
          UINT32 Phase;
          UINT32 BaseClkSel = mSavedClkSel[Slot] & ~0x7U;

          Print (L"[DWMMC 0x%x] CMD%d DCRC -> auto-tuning RX phase (0..7)...\n",
                 (UINT32)BaseAddr, CmdIdx);

          MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
          MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);

          for (Phase = 0; Phase < 8; Phase++) {
            UINT32 CandClkSel = BaseClkSel | Phase;
            mSavedClkSel[Slot] = CandClkSel;
            MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, CandClkSel);
            DwMmcUpdateClock (BaseAddr);

            Status = DwMmcExecCommandInternal (BaseAddr, Packet, FALSE);
            if (!EFI_ERROR (Status)) {
              mRxTuned[Slot] = TRUE;
              Print (L"[DWMMC 0x%x] RX phase tuned OK: CLKSEL=0x%x\n",
                     (UINT32)BaseAddr, CandClkSel);
              return EFI_SUCCESS;
            }
          }
        }

        if (mLastLoggedErr < 16) {
          Print (L"[DWMMC 0x%x] CMD%d IDMAC err IDSTS=0x%x RINT=0x%x DSCADDR=0x%x BUFADDR=0x%x\n",
                 (UINT32)BaseAddr, CmdIdx, IdSts, RintSts,
                 MmioRead32 (BaseAddr + DW_MMC_DSCADDRL),
                 MmioRead32 (BaseAddr + DW_MMC_BUFADDRL));
          mLastLoggedErr++;
        }
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) && ((IdSts & DoneMask) != 0 || (IdSts & DW_MMC_IDMAC_INT_FBE) == 0)) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (Timeout == 0) {
      Print (L"[DWMMC 0x%x] CMD%d IDMAC TIMEOUT IDSTS=0x%x RINT=0x%x\n",
             (UINT32)BaseAddr, CmdIdx,
             MmioRead32 (BaseAddr + DW_MMC_IDSTS),
             MmioRead32 (BaseAddr + DW_MMC_RINTSTS));
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (mDmaCtx[Slot].BounceBuf, TotalBytes);
      CopyMem (UserData, mDmaCtx[Slot].BounceBuf, TotalBytes);

      if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
        UINT8 *UserBuf = (UINT8 *)UserData;
        UINT32 SecCnt = (UINT32)UserBuf[212] |
                        ((UINT32)UserBuf[213] << 8) |
                        ((UINT32)UserBuf[214] << 16) |
                        ((UINT32)UserBuf[215] << 24);
        Print (L"[DW0 CMD8 IDMAC OK] CLKSEL=0x%x Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
               MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
               UserBuf[192], SecCnt,
               UserBuf[0], UserBuf[1], UserBuf[2], UserBuf[3],
               UserBuf[4], UserBuf[5], UserBuf[6], UserBuf[7]);
      }
    }

    MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  return DwMmcExecCommandInternal (BaseAddr, Packet, TRUE);
}
