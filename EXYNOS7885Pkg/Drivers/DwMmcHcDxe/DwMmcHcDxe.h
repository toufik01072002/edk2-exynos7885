#ifndef _DW_MMC_HC_DXE_H_
#define _DW_MMC_HC_DXE_H_

#include <Uefi.h>
#include <Protocol/SdMmcPassThru.h>
#include <Protocol/DevicePath.h>
#include "DwMmcHci.h"

#define DW_MMC_HC_PRIVATE_SIGNATURE SIGNATURE_32 ('D', 'W', 'M', 'C')

typedef struct {
  UINT32                        Signature;
  EFI_HANDLE                    Handle;
  EFI_SD_MMC_PASS_THRU_PROTOCOL PassThru;
  EFI_DEVICE_PATH_PROTOCOL      *DevicePath;
  UINTN                         SlotBase[EXYNOS_DWMMC_MAX_SLOTS];
} DW_MMC_HC_PRIVATE_DATA;

#define DW_MMC_HC_PRIVATE_FROM_THIS(a) \
  CR (a, DW_MMC_HC_PRIVATE_DATA, PassThru, DW_MMC_HC_PRIVATE_SIGNATURE)

#endif
