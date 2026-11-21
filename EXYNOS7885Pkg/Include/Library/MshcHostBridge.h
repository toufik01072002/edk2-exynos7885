#ifndef _MSHC_HOST_BRIDGE_H_
#define _MSHC_HOST_BRIDGE_H_

#include <Uefi.h>

#define CLKSEL_TIMING(Div, FineDrive, Drive, Sample) \
  ((((Div) & 0x7) << 24) | (((FineDrive) & 0x3) << 22) | \
   (((Drive) & 0x7) << 16) | ((Sample) & 0x7))

typedef struct {
  UINTN ControllerId;
  UINTN BaseAddress;
  UINT32 ClkSelTiming;
  UINT32 ClkSelTimingSdr50;
  UINT32 ClkSelTimingSdr104;
  UINT8 ClkSelDivider;
  UINTN CiuClockHz;
  BOOLEAN Coherent;
  UINT8 BusWidth;
  UINTN FifoOffset;
  UINTN FifoItemBytes;
  UINTN ClkSelOffset;
} MSHC_HOST;

UINTN MshcBoardGetHostCount(VOID);
EFI_STATUS MshcBoardInit(IN UINTN Index, IN OUT MSHC_HOST *Host);
EFI_STATUS MshcBoardSetCiuClock(IN OUT MSHC_HOST *Host, IN UINTN Frequency);
VOID MshcBoardPostReset(IN OUT MSHC_HOST *Host);
EFI_STATUS MshcBoardSetVoltage(IN OUT MSHC_HOST *Host, IN UINT32 Millivolts);
EFI_STATUS MshcBoardPowerCycle(IN OUT MSHC_HOST *Host);
BOOLEAN MshcBoardIsCardPresent(IN MSHC_HOST *Host);
BOOLEAN MshcBoardIsReadOnly(IN MSHC_HOST *Host);

#endif
