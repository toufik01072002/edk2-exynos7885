#include "GzipDecompress.h"
#include "puff.h"

#include <Library/UefiBootServicesTableLib.h>

#define GZIP_MAX_OUTPUT  0x04000000U

STATIC UINT32 mCrcTable[256];
STATIC BOOLEAN mCrcTableReady = FALSE;

STATIC
UINT32
ReadLe32 (
  IN CONST UINT8 *Data
  )
{
  return (UINT32)Data[0] |
         ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) |
         ((UINT32)Data[3] << 24);
}

STATIC
UINT32
GzipCrc32 (
  IN CONST UINT8 *Data,
  IN UINTN Size
  )
{
  UINT32 Crc;
  UINT32 Value;
  UINTN Index;
  UINTN Bit;

  if (!mCrcTableReady) {
    for (Index = 0; Index < 256; Index++) {
      Value = (UINT32)Index;
      for (Bit = 0; Bit < 8; Bit++) {
        Value = (Value >> 1) ^ ((Value & 1) ? 0xEDB88320U : 0);
      }
      mCrcTable[Index] = Value;
    }
    mCrcTableReady = TRUE;
  }

  Crc = 0xFFFFFFFFU;
  for (Index = 0; Index < Size; Index++) {
    Crc = mCrcTable[(Crc ^ Data[Index]) & 0xFF] ^ (Crc >> 8);
  }
  return Crc ^ 0xFFFFFFFFU;
}

EFI_STATUS
EFIAPI
GzipDecompress (
  IN CONST UINT8 *CompressedData,
  IN UINTN CompressedSize,
  OUT EFI_PHYSICAL_ADDRESS *OutputAddress,
  OUT UINTN *OutputSize,
  OUT UINTN *OutputPages
  )
{
  UINTN HeaderOffset;
  UINTN DeflateSize;
  UINT32 ExpectedSize;
  UINT32 ExpectedCrc;
  UINTN ActualSize;
  unsigned long DestLength;
  unsigned long SourceLength;
  int PuffStatus;
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS MaxAddress;
  UINTN Pages;
  UINT8 Flags;

  if (CompressedData == NULL || OutputAddress == NULL ||
      OutputSize == NULL || OutputPages == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *OutputAddress = 0;
  *OutputSize = 0;
  *OutputPages = 0;

  if (CompressedSize < 18 ||
      CompressedData[0] != 0x1F ||
      CompressedData[1] != 0x8B ||
      CompressedData[2] != 8) {
    return EFI_UNSUPPORTED;
  }

  Flags = CompressedData[3];
  if ((Flags & 0xE0) != 0) {
    return EFI_UNSUPPORTED;
  }

  HeaderOffset = 10;
  if ((Flags & 0x04) != 0) {
    UINTN ExtraSize;
    if (HeaderOffset + 2 > CompressedSize - 8) {
      return EFI_COMPROMISED_DATA;
    }
    ExtraSize = (UINTN)CompressedData[HeaderOffset] |
                ((UINTN)CompressedData[HeaderOffset + 1] << 8);
    HeaderOffset += 2;
    if (ExtraSize > CompressedSize - 8 - HeaderOffset) {
      return EFI_COMPROMISED_DATA;
    }
    HeaderOffset += ExtraSize;
  }

  if ((Flags & 0x08) != 0) {
    while (HeaderOffset < CompressedSize - 8 &&
           CompressedData[HeaderOffset] != 0) {
      HeaderOffset++;
    }
    if (HeaderOffset >= CompressedSize - 8) {
      return EFI_COMPROMISED_DATA;
    }
    HeaderOffset++;
  }

  if ((Flags & 0x10) != 0) {
    while (HeaderOffset < CompressedSize - 8 &&
           CompressedData[HeaderOffset] != 0) {
      HeaderOffset++;
    }
    if (HeaderOffset >= CompressedSize - 8) {
      return EFI_COMPROMISED_DATA;
    }
    HeaderOffset++;
  }

  if ((Flags & 0x02) != 0) {
    if (HeaderOffset + 2 > CompressedSize - 8) {
      return EFI_COMPROMISED_DATA;
    }
    HeaderOffset += 2;
  }

  if (HeaderOffset >= CompressedSize - 8) {
    return EFI_COMPROMISED_DATA;
  }

  ExpectedCrc = ReadLe32 (CompressedData + CompressedSize - 8);
  ExpectedSize = ReadLe32 (CompressedData + CompressedSize - 4);
  if (ExpectedSize == 0 || ExpectedSize > GZIP_MAX_OUTPUT) {
    return EFI_BAD_BUFFER_SIZE;
  }

  DeflateSize = CompressedSize - HeaderOffset - 8;
  Pages = EFI_SIZE_TO_PAGES (ExpectedSize);
  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  Pages,
                  &MaxAddress
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DestLength = ExpectedSize;
  SourceLength = DeflateSize;
  PuffStatus = puff (
                 (UINT8 *)(UINTN)MaxAddress,
                 &DestLength,
                 CompressedData + HeaderOffset,
                 &SourceLength
                 );

  if (PuffStatus != 0 || SourceLength != DeflateSize ||
      DestLength != ExpectedSize) {
    Status = EFI_COMPROMISED_DATA;
    goto Error;
  }

  ActualSize = (UINTN)DestLength;
  if (GzipCrc32 ((CONST UINT8 *)(UINTN)MaxAddress, ActualSize) != ExpectedCrc) {
    Status = EFI_CRC_ERROR;
    goto Error;
  }

  *OutputAddress = MaxAddress;
  *OutputSize = ActualSize;
  *OutputPages = Pages;
  return EFI_SUCCESS;

Error:
  gBS->FreePages (MaxAddress, Pages);
  return Status;
}
