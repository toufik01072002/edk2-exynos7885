#ifndef GZIP_DECOMPRESS_H_
#define GZIP_DECOMPRESS_H_

#include <Uefi.h>

EFI_STATUS
EFIAPI
GzipDecompress (
  IN CONST UINT8 *CompressedData,
  IN UINTN CompressedSize,
  IN EFI_PHYSICAL_ADDRESS WindowBase,
  IN EFI_PHYSICAL_ADDRESS WindowEnd,
  OUT EFI_PHYSICAL_ADDRESS *OutputAddress,
  OUT UINTN *OutputSize,
  OUT UINTN *OutputPages
  );

#endif
