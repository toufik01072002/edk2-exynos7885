#ifndef GZIP_DECOMPRESS_H_
#define GZIP_DECOMPRESS_H_

#include <Uefi.h>

EFI_STATUS
EFIAPI
GzipDecompress (
  IN CONST UINT8 *CompressedData,
  IN UINTN CompressedSize,
  OUT EFI_PHYSICAL_ADDRESS *OutputAddress,
  OUT UINTN *OutputSize,
  OUT UINTN *OutputPages
  );

#endif
