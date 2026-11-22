#include "AndroidImageLoader.h"
#include "GzipDecompress.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Guid/FileInfo.h>

VOID
EFIAPI
Arm64LinuxHandoff (
  IN UINT64 KernelEntry,
  IN UINT64 KernelSize,
  IN UINT64 InitrdBase,
  IN UINT64 InitrdSize,
  IN UINT64 FdtBase,
  IN UINT64 FdtSize
  );

#define ANDROID_PAGE_SIZE       2048U
#define ANDROID_HEADER_V0       0U
#define ANDROID_MAX_KERNEL      0x08000000ULL
#define ANDROID_MAX_KERNEL_MEM  0x10000000ULL
#define ANDROID_MAX_RAMDISK     0x04000000ULL
#define ARM64_IMAGE_MAGIC       0x644D5241U

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
UINT64
ReadLe64 (
  IN CONST UINT8 *Data
  )
{
  return (UINT64)ReadLe32 (Data) |
         ((UINT64)ReadLe32 (Data + 4) << 32);
}

STATIC
EFI_STATUS
ReadExact (
  IN EFI_FILE_PROTOCOL *File,
  IN UINT64 Offset,
  OUT VOID *Buffer,
  IN UINTN Size
  )
{
  EFI_STATUS Status;
  UINTN Total;
  UINTN Chunk;

  Status = File->SetPosition (File, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Total = 0;
  while (Total < Size) {
    Chunk = Size - Total;
    Status = File->Read (File, &Chunk, (UINT8 *)Buffer + Total);
    if (EFI_ERROR (Status) || Chunk == 0) {
      return EFI_DEVICE_ERROR;
    }
    Total += Chunk;
  }

  return EFI_SUCCESS;
}


STATIC
UINT32
ReadBe32 (
  IN CONST UINT8 *Data
  )
{
  return ((UINT32)Data[0] << 24) |
         ((UINT32)Data[1] << 16) |
         ((UINT32)Data[2] << 8) |
         (UINT32)Data[3];
}

STATIC
UINT64
ReadBe64 (
  IN CONST UINT8 *Data
  )
{
  return ((UINT64)ReadBe32 (Data) << 32) | ReadBe32 (Data + 4);
}

STATIC
VOID
WriteBe32 (
  OUT UINT8 *Data,
  IN UINT32 Value
  )
{
  Data[0] = (UINT8)(Value >> 24);
  Data[1] = (UINT8)(Value >> 16);
  Data[2] = (UINT8)(Value >> 8);
  Data[3] = (UINT8)Value;
}

STATIC
VOID
WriteBe64 (
  OUT UINT8 *Data,
  IN UINT64 Value
  )
{
  WriteBe32 (Data, (UINT32)(Value >> 32));
  WriteBe32 (Data + 4, (UINT32)Value);
}

STATIC
BOOLEAN
FdtNameEquals (
  IN CONST CHAR8 *Name,
  IN UINTN NameLimit,
  IN CONST CHAR8 *Expected
  )
{
  UINTN Index;

  for (Index = 0; Expected[Index] != '\0'; Index++) {
    if (Index >= NameLimit || Name[Index] != Expected[Index]) {
      return FALSE;
    }
  }

  return (Index == NameLimit && Name[Index] == '\0');
}

STATIC
EFI_STATUS
ReadCpioHex32 (
  IN CONST UINT8 *Data,
  OUT UINT32 *Value
  )
{
  UINT32 Result;
  UINTN Index;
  UINT8 Digit;

  Result = 0;
  for (Index = 0; Index < 8; Index++) {
    if (Data[Index] >= '0' && Data[Index] <= '9') {
      Digit = (UINT8)(Data[Index] - '0');
    } else if (Data[Index] >= 'a' && Data[Index] <= 'f') {
      Digit = (UINT8)(Data[Index] - 'a' + 10);
    } else if (Data[Index] >= 'A' && Data[Index] <= 'F') {
      Digit = (UINT8)(Data[Index] - 'A' + 10);
    } else {
      return EFI_COMPROMISED_DATA;
    }
    Result = (Result << 4) | Digit;
  }

  *Value = Result;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidateNewcCpio (
  IN CONST UINT8 *Archive,
  IN UINTN ArchiveSize,
  OUT UINT32 *EntryCount
  )
{
  UINTN Offset;
  UINTN NameSize;
  UINTN FileSize;
  UINTN NameOffset;
  UINTN DataOffset;
  UINTN NextOffset;
  UINT32 ParsedNameSize;
  UINT32 ParsedFileSize;
  UINT32 ExpectedCheck;
  UINT32 ActualCheck;
  UINT32 Index;
  UINT32 Count;
  UINTN ByteIndex;
  BOOLEAN IsCrc;
  BOOLEAN FoundTrailer;
  EFI_STATUS Status;

  if (Archive == NULL || EntryCount == NULL || ArchiveSize < 110) {
    return EFI_INVALID_PARAMETER;
  }

  *EntryCount = 0;
  Offset = 0;
  Count = 0;
  FoundTrailer = FALSE;

  while (Offset <= ArchiveSize && ArchiveSize - Offset >= 110) {
    if (CompareMem (Archive + Offset, "070701", 6) == 0) {
      IsCrc = FALSE;
    } else if (CompareMem (Archive + Offset, "070702", 6) == 0) {
      IsCrc = TRUE;
    } else {
      return EFI_COMPROMISED_DATA;
    }

    Status = ReadCpioHex32 (Archive + Offset + 54, &ParsedFileSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = ReadCpioHex32 (Archive + Offset + 94, &ParsedNameSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = ReadCpioHex32 (Archive + Offset + 102, &ExpectedCheck);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (ParsedNameSize == 0) {
      return EFI_COMPROMISED_DATA;
    }

    NameSize = (UINTN)ParsedNameSize;
    FileSize = (UINTN)ParsedFileSize;
    NameOffset = Offset + 110;
    if (NameOffset < Offset || NameSize > ArchiveSize - NameOffset) {
      return EFI_COMPROMISED_DATA;
    }
    if (Archive[NameOffset + NameSize - 1] != 0) {
      return EFI_COMPROMISED_DATA;
    }

    DataOffset = (NameOffset + NameSize + 3) & ~(UINTN)3;
    if (DataOffset < NameOffset || DataOffset > ArchiveSize ||
        FileSize > ArchiveSize - DataOffset) {
      return EFI_COMPROMISED_DATA;
    }

    NextOffset = (DataOffset + FileSize + 3) & ~(UINTN)3;
    if (NextOffset < DataOffset || NextOffset > ArchiveSize) {
      return EFI_COMPROMISED_DATA;
    }

    if (IsCrc) {
      ActualCheck = 0;
      for (ByteIndex = 0; ByteIndex < FileSize; ByteIndex++) {
        ActualCheck += Archive[DataOffset + ByteIndex];
      }
      if (ActualCheck != ExpectedCheck) {
        return EFI_CRC_ERROR;
      }
    } else if (ExpectedCheck != 0) {
      return EFI_COMPROMISED_DATA;
    }

    if (NameSize == sizeof ("TRAILER!!!") &&
        CompareMem (Archive + NameOffset, "TRAILER!!!", sizeof ("TRAILER!!!")) == 0) {
      FoundTrailer = TRUE;
      Offset = NextOffset;
      break;
    }

    if (Count == MAX_UINT32) {
      return EFI_BAD_BUFFER_SIZE;
    }
    Count++;
    Offset = NextOffset;
  }

  if (!FoundTrailer) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = (UINT32)Offset; (UINTN)Index < ArchiveSize; Index++) {
    if (Archive[Index] != 0) {
      return EFI_COMPROMISED_DATA;
    }
  }

  *EntryCount = Count;
  return (Count == 0) ? EFI_COMPROMISED_DATA : EFI_SUCCESS;
}

STATIC
EFI_STATUS
PatchFdtInitrd (
  IN OUT UINT8 *Fdt,
  IN UINTN BufferSize,
  IN UINT64 InitrdStart,
  IN UINT64 InitrdEnd
  )
{
  UINT32 TotalSize;
  UINT32 StructOffset;
  UINT32 StringsOffset;
  UINT32 StringsSize;
  UINT32 StructSize;
  UINT32 Position;
  UINT32 StructEnd;
  UINT32 Token;
  UINT32 PropertySize;
  UINT32 NameOffset;
  UINT32 NameLength;
  UINT32 NodeLength;
  UINT32 Depth;
  UINT32 ChosenDepth;
  UINT32 Index;
  UINT8 *PropertyData;
  CONST CHAR8 *PropertyName;
  BOOLEAN FoundStart;
  BOOLEAN FoundEnd;
  UINT64 Value;

  if (Fdt == NULL || BufferSize < 40 ||
      ReadBe32 (Fdt) != 0xD00DFEEDU) {
    return EFI_UNSUPPORTED;
  }

  TotalSize = ReadBe32 (Fdt + 4);
  StructOffset = ReadBe32 (Fdt + 8);
  StringsOffset = ReadBe32 (Fdt + 12);
  if (ReadBe32 (Fdt + 20) < 17) {
    return EFI_UNSUPPORTED;
  }
  StringsSize = ReadBe32 (Fdt + 32);
  StructSize = ReadBe32 (Fdt + 36);

  if (TotalSize < 40 || TotalSize > BufferSize ||
      StructOffset > TotalSize || StructSize > TotalSize - StructOffset ||
      StringsOffset > TotalSize || StringsSize > TotalSize - StringsOffset ||
      (StructOffset & 3) != 0) {
    return EFI_COMPROMISED_DATA;
  }

  StructEnd = StructOffset + StructSize;
  Position = StructOffset;
  Depth = 0;
  ChosenDepth = 0;
  FoundStart = FALSE;
  FoundEnd = FALSE;

  while (Position <= StructEnd && StructEnd - Position >= 4) {
    Token = ReadBe32 (Fdt + Position);
    Position += 4;

    if (Token == 1) {
      Index = Position;
      while (Index < StructEnd && Fdt[Index] != 0) {
        Index++;
      }
      if (Index >= StructEnd) {
        return EFI_COMPROMISED_DATA;
      }

      NodeLength = Index - Position;
      if (Depth == 1 && NodeLength == 6 &&
          CompareMem (Fdt + Position, "chosen", 6) == 0) {
        ChosenDepth = Depth + 1;
      }

      Position = (Index + 1 + 3) & ~3U;
      if (Position > StructEnd || Depth == MAX_UINT32) {
        return EFI_COMPROMISED_DATA;
      }
      Depth++;
    } else if (Token == 2) {
      if (Depth == 0) {
        return EFI_COMPROMISED_DATA;
      }
      if (Depth == ChosenDepth) {
        ChosenDepth = 0;
      }
      Depth--;
    } else if (Token == 3) {
      if (StructEnd - Position < 8) {
        return EFI_COMPROMISED_DATA;
      }
      PropertySize = ReadBe32 (Fdt + Position);
      NameOffset = ReadBe32 (Fdt + Position + 4);
      Position += 8;

      if (PropertySize > StructEnd - Position ||
          NameOffset >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      PropertyName = (CONST CHAR8 *)(Fdt + StringsOffset + NameOffset);
      NameLength = 0;
      while (NameOffset + NameLength < StringsSize &&
             PropertyName[NameLength] != '\0') {
        NameLength++;
      }
      if (NameOffset + NameLength >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      if (Depth == ChosenDepth && ChosenDepth != 0) {
        PropertyData = Fdt + Position;
        if (FdtNameEquals (PropertyName, NameLength, "linux,initrd-start")) {
          if (PropertySize == 4) {
            if (InitrdStart > MAX_UINT32) {
              return EFI_BAD_BUFFER_SIZE;
            }
            WriteBe32 (PropertyData, (UINT32)InitrdStart);
          } else if (PropertySize == 8) {
            WriteBe64 (PropertyData, InitrdStart);
          } else {
            return EFI_UNSUPPORTED;
          }
          FoundStart = TRUE;
        } else if (FdtNameEquals (PropertyName, NameLength, "linux,initrd-end")) {
          if (PropertySize == 4) {
            if (InitrdEnd > MAX_UINT32) {
              return EFI_BAD_BUFFER_SIZE;
            }
            WriteBe32 (PropertyData, (UINT32)InitrdEnd);
          } else if (PropertySize == 8) {
            WriteBe64 (PropertyData, InitrdEnd);
          } else {
            return EFI_UNSUPPORTED;
          }
          FoundEnd = TRUE;
        }
      }

      Position = (Position + PropertySize + 3) & ~3U;
      if (Position > StructEnd) {
        return EFI_COMPROMISED_DATA;
      }
    } else if (Token == 4) {
      continue;
    } else if (Token == 9) {
      break;
    } else {
      return EFI_COMPROMISED_DATA;
    }
  }

  if (!FoundStart || !FoundEnd) {
    return EFI_NOT_FOUND;
  }

  Value = InitrdEnd - InitrdStart;
  if (Value == 0 || InitrdEnd < InitrdStart) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
UINT64
FdtReadCells (
  IN CONST UINT8 *Data,
  IN UINT32 CellCount
  )
{
  if (CellCount == 1) {
    return ReadBe32 (Data);
  }
  return ((UINT64)ReadBe32 (Data) << 32) | ReadBe32 (Data + 4);
}

STATIC
BOOLEAN
RangesOverlap (
  IN UINT64 BaseA,
  IN UINT64 SizeA,
  IN UINT64 BaseB,
  IN UINT64 SizeB
  )
{
  if (SizeA == 0 || SizeB == 0) {
    return FALSE;
  }
  if (BaseA > MAX_UINT64 - SizeA ||
      BaseB > MAX_UINT64 - SizeB) {
    return TRUE;
  }
  return (BaseA < BaseB + SizeB) && (BaseB < BaseA + SizeA);
}

STATIC
EFI_STATUS
ValidateFdtReservedMemory (
  IN CONST UINT8 *Fdt,
  IN UINTN BufferSize,
  IN EFI_PHYSICAL_ADDRESS KernelBase,
  IN UINT64 KernelSize,
  IN EFI_PHYSICAL_ADDRESS RamdiskBase,
  IN UINT64 RamdiskSize,
  IN EFI_PHYSICAL_ADDRESS InitrdBase,
  IN UINT64 InitrdSize,
  IN EFI_PHYSICAL_ADDRESS FdtBase,
  IN UINT64 FdtSize
  )
{
  UINT32 TotalSize;
  UINT32 ReserveMapOffset;
  UINT32 ReserveMapPosition;
  UINT64 ReserveAddress;
  UINT64 ReserveSize;
  BOOLEAN ReserveMapTerminated;
  UINT32 StructOffset;
  UINT32 StringsOffset;
  UINT32 StringsSize;
  UINT32 StructSize;
  UINT32 Position;
  UINT32 StructEnd;
  UINT32 Token;
  UINT32 PropertySize;
  UINT32 NameOffset;
  UINT32 NameLength;
  UINT32 NodeLength;
  UINT32 Depth;
  UINT32 ReservedDepth;
  UINT32 AddressCells;
  UINT32 SizeCells;
  UINT32 TupleSize;
  UINT32 Offset;
  UINT64 RegionBase;
  UINT64 RegionSize;
  CONST CHAR8 *PropertyName;
  BOOLEAN InReserved;

  if (Fdt == NULL || BufferSize < 40 || ReadBe32 (Fdt) != 0xD00DFEEDU) {
    return EFI_UNSUPPORTED;
  }

  TotalSize = ReadBe32 (Fdt + 4);
  StructOffset = ReadBe32 (Fdt + 8);
  StringsOffset = ReadBe32 (Fdt + 12);
  ReserveMapOffset = ReadBe32 (Fdt + 16);
  StringsSize = ReadBe32 (Fdt + 32);
  StructSize = ReadBe32 (Fdt + 36);
  if (TotalSize < 40 || TotalSize > BufferSize ||
      StructOffset > TotalSize || StructSize > TotalSize - StructOffset ||
      StringsOffset > TotalSize || StringsSize > TotalSize - StringsOffset ||
      ReserveMapOffset < 40 || (ReserveMapOffset & 7U) != 0 ||
      ReserveMapOffset > TotalSize || TotalSize - ReserveMapOffset < 16) {
    return EFI_COMPROMISED_DATA;
  }

  //
  // Check the FDT memory-reservation block in addition to /reserved-memory.
  // Entries are big-endian 64-bit address/size pairs, terminated by 0, 0.
  //
  ReserveMapPosition = ReserveMapOffset;
  ReserveMapTerminated = FALSE;
  while (TotalSize - ReserveMapPosition >= 16) {
    ReserveAddress = ReadBe64 (Fdt + ReserveMapPosition);
    ReserveSize = ReadBe64 (Fdt + ReserveMapPosition + 8);
    ReserveMapPosition += 16;

    if (ReserveAddress == 0 && ReserveSize == 0) {
      ReserveMapTerminated = TRUE;
      break;
    }
    if (ReserveSize == 0 ||
        ReserveAddress > MAX_UINT64 - ReserveSize) {
      return EFI_COMPROMISED_DATA;
    }

    if (RangesOverlap (KernelBase, KernelSize, ReserveAddress, ReserveSize) ||
        RangesOverlap (RamdiskBase, RamdiskSize, ReserveAddress, ReserveSize) ||
        RangesOverlap (InitrdBase, InitrdSize, ReserveAddress, ReserveSize) ||
        RangesOverlap (FdtBase, FdtSize, ReserveAddress, ReserveSize)) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  if (!ReserveMapTerminated) {
    return EFI_COMPROMISED_DATA;
  }

  Position = StructOffset;
  StructEnd = StructOffset + StructSize;
  Depth = 0;
  ReservedDepth = 0;
  AddressCells = 2;
  SizeCells = 1;
  InReserved = FALSE;

  while (Position <= StructEnd && StructEnd - Position >= 4) {
    Token = ReadBe32 (Fdt + Position);
    Position += 4;

    if (Token == 1) {
      Offset = Position;
      while (Offset < StructEnd && Fdt[Offset] != 0) {
        Offset++;
      }
      if (Offset >= StructEnd) {
        return EFI_COMPROMISED_DATA;
      }
      NodeLength = Offset - Position;

      if (Depth == 1 && NodeLength == 15 &&
          CompareMem (Fdt + Position, "reserved-memory", 15) == 0) {
        InReserved = TRUE;
        ReservedDepth = 2;
      }
      Position = (Offset + 1 + 3) & ~3U;
      if (Position > StructEnd || Depth == MAX_UINT32) {
        return EFI_COMPROMISED_DATA;
      }
      Depth++;
    } else if (Token == 2) {
      if (Depth == 0) {
        return EFI_COMPROMISED_DATA;
      }
      Depth--;
      if (InReserved && Depth < ReservedDepth) {
        InReserved = FALSE;
        ReservedDepth = 0;
      }
    } else if (Token == 3) {
      if (StructEnd - Position < 8) {
        return EFI_COMPROMISED_DATA;
      }
      PropertySize = ReadBe32 (Fdt + Position);
      NameOffset = ReadBe32 (Fdt + Position + 4);
      Position += 8;
      if (PropertySize > StructEnd - Position || NameOffset >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      PropertyName = (CONST CHAR8 *)(Fdt + StringsOffset + NameOffset);
      NameLength = 0;
      while (NameOffset + NameLength < StringsSize &&
             PropertyName[NameLength] != '\0') {
        NameLength++;
      }
      if (NameOffset + NameLength >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      if (Depth == 1) {
        if (FdtNameEquals (PropertyName, NameLength, "#address-cells") &&
            PropertySize == 4) {
          AddressCells = ReadBe32 (Fdt + Position);
        } else if (FdtNameEquals (PropertyName, NameLength, "#size-cells") &&
                   PropertySize == 4) {
          SizeCells = ReadBe32 (Fdt + Position);
        }
      } else if (InReserved && Depth == ReservedDepth) {
        if (FdtNameEquals (PropertyName, NameLength, "#address-cells") &&
            PropertySize == 4) {
          AddressCells = ReadBe32 (Fdt + Position);
        } else if (FdtNameEquals (PropertyName, NameLength, "#size-cells") &&
                   PropertySize == 4) {
          SizeCells = ReadBe32 (Fdt + Position);
        } else if (FdtNameEquals (PropertyName, NameLength, "ranges")) {
          if (PropertySize != 0) {
            return EFI_UNSUPPORTED;
          }
        }
      } else if (InReserved && Depth == ReservedDepth + 1 &&
                 FdtNameEquals (PropertyName, NameLength, "reg")) {
        if ((AddressCells != 1 && AddressCells != 2) ||
            (SizeCells != 1 && SizeCells != 2)) {
          return EFI_UNSUPPORTED;
        }
        TupleSize = (AddressCells + SizeCells) * 4;
        if (TupleSize == 0 || PropertySize == 0 ||
            (PropertySize % TupleSize) != 0) {
          return EFI_COMPROMISED_DATA;
        }

        for (Offset = 0; Offset < PropertySize; Offset += TupleSize) {
          RegionBase = FdtReadCells (Fdt + Position + Offset, AddressCells);
          RegionSize = FdtReadCells (
                         Fdt + Position + Offset + AddressCells * 4,
                         SizeCells
                         );
          if (RegionSize == 0 || RegionBase > MAX_UINT64 - RegionSize) {
            return EFI_COMPROMISED_DATA;
          }

          if (RangesOverlap (KernelBase, KernelSize, RegionBase, RegionSize) ||
              RangesOverlap (RamdiskBase, RamdiskSize, RegionBase, RegionSize) ||
              RangesOverlap (InitrdBase, InitrdSize, RegionBase, RegionSize) ||
              RangesOverlap (FdtBase, FdtSize, RegionBase, RegionSize)) {
            return EFI_SECURITY_VIOLATION;
          }
        }
      }

      Position = (Position + PropertySize + 3) & ~3U;
      if (Position > StructEnd) {
        return EFI_COMPROMISED_DATA;
      }
    } else if (Token == 4) {
      continue;
    } else if (Token == 9) {
      break;
    } else {
      return EFI_COMPROMISED_DATA;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LoadAndPatchFdt (
  IN EFI_FILE_PROTOCOL *Directory,
  IN UINT64 InitrdStart,
  IN UINT64 InitrdEnd,
  OUT UINT8 **FdtBuffer,
  OUT UINTN *FdtBufferSize
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 Header[40];
  UINT32 TotalSize;
  UINT8 *Buffer;

  if (Directory == NULL || FdtBuffer == NULL || FdtBufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *FdtBuffer = NULL;
  *FdtBufferSize = 0;
  File = NULL;
  Info = NULL;
  Buffer = NULL;

  Status = Directory->Open (
                        Directory,
                        &File,
                        L"running.dtb",
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  InfoSize,
                  (VOID **)&Info
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Info->FileSize < sizeof (Header) ||
      Info->FileSize > 0x00200000ULL ||
      Info->FileSize > MAX_UINTN) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, 0, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (ReadBe32 (Header) != 0xD00DFEEDU) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  TotalSize = ReadBe32 (Header + 4);
  if (TotalSize < sizeof (Header) || TotalSize > Info->FileSize) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  TotalSize,
                  (VOID **)&Buffer
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ReadExact (File, 0, Buffer, TotalSize);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = PatchFdtInitrd (Buffer, TotalSize, InitrdStart, InitrdEnd);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  *FdtBuffer = Buffer;
  *FdtBufferSize = TotalSize;
  Buffer = NULL;
  Status = EFI_SUCCESS;

Done:
  if (Buffer != NULL) {
    gBS->FreePool (Buffer);
  }
  if (Info != NULL) {
    gBS->FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }
  return Status;
}

STATIC
UINT64
AlignUp (
  IN UINT64 Value,
  IN UINT32 Alignment
  )
{
  return (Value + Alignment - 1) & ~((UINT64)Alignment - 1);
}

STATIC
BOOLEAN
RangeIsLoaderData (
  IN EFI_MEMORY_DESCRIPTOR *MemoryMap,
  IN UINTN MemoryMapSize,
  IN UINTN DescriptorSize,
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINT64 Size
  )
{
  UINTN Offset;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  UINT64 DescriptorStart;
  UINT64 DescriptorSizeBytes;
  UINT64 DescriptorEnd;
  UINT64 RangeEnd;

  if (Size == 0 || Address > MAX_UINT64 - Size) {
    return FALSE;
  }
  RangeEnd = Address + Size;

  for (Offset = 0; Offset + DescriptorSize <= MemoryMapSize;
       Offset += DescriptorSize) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + Offset);
    if (Descriptor->Type != EfiLoaderData ||
        Descriptor->NumberOfPages > MAX_UINT64 / EFI_PAGE_SIZE) {
      continue;
    }

    DescriptorStart = Descriptor->PhysicalStart;
    DescriptorSizeBytes = Descriptor->NumberOfPages * EFI_PAGE_SIZE;
    if (DescriptorStart > MAX_UINT64 - DescriptorSizeBytes) {
      continue;
    }
    DescriptorEnd = DescriptorStart + DescriptorSizeBytes;

    if (Address >= DescriptorStart && RangeEnd <= DescriptorEnd) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
ValidateBootMemoryMap (
  IN EFI_PHYSICAL_ADDRESS KernelBase,
  IN UINT64 KernelSize,
  IN EFI_PHYSICAL_ADDRESS RamdiskBase,
  IN UINT64 RamdiskSize,
  IN EFI_PHYSICAL_ADDRESS InitrdBase,
  IN UINT64 InitrdSize,
  IN EFI_PHYSICAL_ADDRESS FdtBase,
  IN UINT64 FdtSize
  )
{
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN MemoryMapSize;
  UINTN MapKey;
  UINTN DescriptorSize;
  UINT32 DescriptorVersion;

  MemoryMap = NULL;
  MemoryMapSize = 0;
  DescriptorSize = 0;

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL || DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) {
    return EFI_DEVICE_ERROR;
  }

  MemoryMapSize += DescriptorSize * 8;
  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  MemoryMapSize,
                  (VOID **)&MemoryMap
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    gBS->FreePool (MemoryMap);
    return Status;
  }

  if (!RangeIsLoaderData (MemoryMap, MemoryMapSize, DescriptorSize, KernelBase, KernelSize) ||
      !RangeIsLoaderData (MemoryMap, MemoryMapSize, DescriptorSize, RamdiskBase, RamdiskSize) ||
      !RangeIsLoaderData (MemoryMap, MemoryMapSize, DescriptorSize, InitrdBase, InitrdSize) ||
      !RangeIsLoaderData (MemoryMap, MemoryMapSize, DescriptorSize, FdtBase, FdtSize)) {
    Status = EFI_SECURITY_VIOLATION;
  } else {
    Status = EFI_SUCCESS;
  }

  gBS->FreePool (MemoryMap);
  return Status;
}

EFI_STATUS
EFIAPI
AndroidStageBootImage (
  IN EFI_HANDLE ImageHandle,
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *FileName
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 Header[ANDROID_PAGE_SIZE];
  UINT8 KernelHeader[64];
  UINT32 KernelSize;
  UINT32 RamdiskSize;
  UINT32 SecondSize;
  UINT32 PageSize;
  UINT32 HeaderVersion;
  UINT64 KernelOffset;
  UINT64 RamdiskOffset;
  UINT64 SecondOffset;
  UINT64 ImageEnd;
  UINT64 KernelTextOffset;
  UINT64 KernelImageSize;
  UINT64 KernelFlags;
  UINT64 KernelAllocationSize;
  UINT64 KernelAllocationEnd;
  UINT64 KernelAlignedBase;
  UINT64 KernelPages;
  UINT64 RamdiskPages;
  UINT8 *FdtBuffer;
  UINTN FdtBufferSize;
  UINTN FdtPages;
  EFI_PHYSICAL_ADDRESS FdtAddress;
  EFI_PHYSICAL_ADDRESS MaxAddress;
  EFI_PHYSICAL_ADDRESS KernelAddress;
  EFI_PHYSICAL_ADDRESS KernelAllocationAddress;
  UINT64 KernelImageEnd;
  UINT64 InitrdEnd;
  UINT64 KernelWindowBase;
  UINT64 InitrdWindowBase;
  EFI_PHYSICAL_ADDRESS RamdiskAddress;
  EFI_PHYSICAL_ADDRESS DecompressedRamdiskAddress;
  UINTN DecompressedRamdiskSize;
  UINTN DecompressedRamdiskPages;
  BOOLEAN KernelAllocated;
  BOOLEAN RamdiskAllocated;
  BOOLEAN DecompressedRamdiskAllocated;
  BOOLEAN FdtAllocated;
  UINT32 CpioEntryCount;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN MemoryMapSize;
  UINTN MemoryMapCapacity;
  UINTN MapKey;
  UINTN DescriptorSize;
  UINTN Attempt;
  UINT32 DescriptorVersion;
  UINTN CurrentEl;

  File = NULL;
  Info = NULL;
  KernelAddress = 0;
  KernelAllocationAddress = 0;
  KernelImageEnd = 0;
  InitrdEnd = 0;
  KernelWindowBase = 0;
  InitrdWindowBase = 0;
  RamdiskAddress = 0;
  DecompressedRamdiskAddress = 0;
  DecompressedRamdiskSize = 0;
  DecompressedRamdiskPages = 0;
  FdtBuffer = NULL;
  FdtBufferSize = 0;
  FdtPages = 0;
  FdtAddress = 0;
  KernelAllocated = FALSE;
  RamdiskAllocated = FALSE;
  DecompressedRamdiskAllocated = FALSE;
  FdtAllocated = FALSE;
  CpioEntryCount = 0;
  MemoryMap = NULL;
  MemoryMapSize = 0;
  MemoryMapCapacity = 0;
  MapKey = 0;
  DescriptorSize = 0;
  DescriptorVersion = 0;
  CurrentEl = 0;
  KernelPages = 0;
  RamdiskPages = 0;

  if (Directory == NULL || FileName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Directory->Open (
                        Directory,
                        &File,
                        (CHAR16 *)FileName,
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  InfoSize,
                  (VOID **)&Info
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Info->FileSize < ANDROID_PAGE_SIZE ||
      Info->FileSize > 0x10000000ULL) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, 0, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (CompareMem (Header, "ANDROID!", 8) != 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelSize = ReadLe32 (Header + 8);
  RamdiskSize = ReadLe32 (Header + 16);
  SecondSize = ReadLe32 (Header + 24);
  PageSize = ReadLe32 (Header + 36);
  HeaderVersion = ReadLe32 (Header + 40);

  if (HeaderVersion != ANDROID_HEADER_V0 ||
      PageSize < ANDROID_PAGE_SIZE ||
      PageSize > 65536 ||
      (PageSize & (PageSize - 1)) != 0 ||
      KernelSize == 0 || KernelSize > ANDROID_MAX_KERNEL ||
      RamdiskSize == 0 || RamdiskSize > ANDROID_MAX_RAMDISK) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelOffset = PageSize;
  RamdiskOffset = KernelOffset + AlignUp (KernelSize, PageSize);
  SecondOffset = RamdiskOffset + AlignUp (RamdiskSize, PageSize);
  ImageEnd = SecondOffset + AlignUp (SecondSize, PageSize);

  if (RamdiskOffset < KernelOffset ||
      SecondOffset < RamdiskOffset ||
      ImageEnd < SecondOffset ||
      ImageEnd > Info->FileSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, KernelOffset, KernelHeader, sizeof (KernelHeader));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (ReadLe32 (KernelHeader + 56) != ARM64_IMAGE_MAGIC) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelTextOffset = ReadLe64 (KernelHeader + 8);
  KernelImageSize = ReadLe64 (KernelHeader + 16);
  KernelFlags = ReadLe64 (KernelHeader + 24);
  if (KernelImageSize < KernelSize ||
      KernelImageSize > ANDROID_MAX_KERNEL_MEM ||
      KernelTextOffset > 0x200000 ||
      (KernelFlags & 0xFFFFFFFFFFFFFFF1ULL) != 0 ||
      ((KernelFlags >> 1) & 3) != 1) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, RamdiskOffset, Header, 2);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  if (Header[0] != 0x1F || Header[1] != 0x8B) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelAllocationSize = KernelTextOffset + KernelImageSize + 0x1FFFFFULL;
  if (KernelAllocationSize < KernelImageSize || KernelAllocationSize > MAX_UINTN) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  KernelPages = EFI_SIZE_TO_PAGES ((UINTN)KernelAllocationSize);
  RamdiskPages = EFI_SIZE_TO_PAGES ((UINTN)RamdiskSize);

  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  (UINTN)KernelPages,
                  &MaxAddress
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  KernelAllocationAddress = MaxAddress;
  KernelAllocated = TRUE;

  KernelAlignedBase = AlignUp (KernelAllocationAddress, 0x200000);
  KernelAddress = KernelAlignedBase + KernelTextOffset;
  KernelAllocationEnd = KernelAllocationAddress + EFI_PAGES_TO_SIZE (KernelPages);
  if (KernelAlignedBase < KernelAllocationAddress ||
      KernelAddress < KernelAlignedBase ||
      KernelAddress > MAX_UINT64 - KernelImageSize ||
      KernelAddress + KernelImageSize > KernelAllocationEnd ||
      KernelAllocationEnd > 0x100000000ULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  // Determine the Linux-required 1 GiB window before allocating the initrd.
  KernelWindowBase = KernelAddress & ~0x3FFFFFFFULL;

  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  (UINTN)RamdiskPages,
                  &MaxAddress
                  );
  RamdiskAddress = MaxAddress;
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  RamdiskAllocated = TRUE;

  SetMem ((VOID *)(UINTN)KernelAddress, (UINTN)KernelImageSize, 0);
  Status = ReadExact (
             File,
             KernelOffset,
             (VOID *)(UINTN)KernelAddress,
             KernelSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ReadExact (
             File,
             RamdiskOffset,
             (VOID *)(UINTN)RamdiskAddress,
             RamdiskSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = GzipDecompress (
             (CONST UINT8 *)(UINTN)RamdiskAddress,
             RamdiskSize,
             KernelWindowBase,
             KernelWindowBase + 0x40000000ULL,
             &DecompressedRamdiskAddress,
             &DecompressedRamdiskSize,
             &DecompressedRamdiskPages
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  DecompressedRamdiskAllocated = TRUE;

  // Linux arm64 requires the complete Image and initrd to share a 1 GiB window.
  if (DecompressedRamdiskSize == 0 ||
      DecompressedRamdiskAddress > MAX_UINT64 - DecompressedRamdiskSize ||
      KernelAddress > MAX_UINT64 - KernelImageSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  KernelImageEnd = KernelAddress + KernelImageSize;
  InitrdEnd = DecompressedRamdiskAddress + DecompressedRamdiskSize;
  KernelWindowBase = KernelAddress & ~0x3FFFFFFFULL;
  InitrdWindowBase = DecompressedRamdiskAddress & ~0x3FFFFFFFULL;
  if (KernelWindowBase != InitrdWindowBase ||
      ((KernelImageEnd - 1) & ~0x3FFFFFFFULL) != KernelWindowBase ||
      ((InitrdEnd - 1) & ~0x3FFFFFFFULL) != KernelWindowBase) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = ValidateNewcCpio (
             (CONST UINT8 *)(UINTN)DecompressedRamdiskAddress,
             DecompressedRamdiskSize,
             &CpioEntryCount
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = LoadAndPatchFdt (
             Directory,
             DecompressedRamdiskAddress,
             InitrdEnd,
             &FdtBuffer,
             &FdtBufferSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  FdtPages = EFI_SIZE_TO_PAGES (FdtBufferSize);
  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  FdtPages,
                  &MaxAddress
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  FdtAddress = MaxAddress;
  FdtAllocated = TRUE;
  if ((FdtAddress & 7ULL) != 0 ||
      FdtBufferSize < 40 || FdtBufferSize > 0x00200000U ||
      FdtAddress > 0x100000000ULL - FdtBufferSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  CopyMem (
    (VOID *)(UINTN)FdtAddress,
    FdtBuffer,
    FdtBufferSize
    );

  Status = ValidateFdtReservedMemory (
             FdtBuffer,
             FdtBufferSize,
             KernelAllocationAddress,
             EFI_PAGES_TO_SIZE ((UINTN)KernelPages),
             RamdiskAddress,
             EFI_PAGES_TO_SIZE ((UINTN)RamdiskPages),
             DecompressedRamdiskAddress,
             EFI_PAGES_TO_SIZE (DecompressedRamdiskPages),
             FdtAddress,
             EFI_PAGES_TO_SIZE (FdtPages)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ValidateBootMemoryMap (
             KernelAllocationAddress,
             EFI_PAGES_TO_SIZE ((UINTN)KernelPages),
             RamdiskAddress,
             EFI_PAGES_TO_SIZE ((UINTN)RamdiskPages),
             DecompressedRamdiskAddress,
             EFI_PAGES_TO_SIZE (DecompressedRamdiskPages),
             FdtAddress,
             EFI_PAGES_TO_SIZE (FdtPages)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Print (L"\nPreparation du transfert direct vers le noyau Android...\n");
  Print (L"Entree ARM64 : 0x%Lx | DTB : 0x%Lx\n",
         KernelAddress, FdtAddress);

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  if (ImageHandle == NULL || (CurrentEl != 0x4 && CurrentEl != 0x8)) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  // Release temporary buffers and file handles before the final memory map.
  if (File != NULL) {
    File->Close (File);
    File = NULL;
  }
  if (Info != NULL) {
    gBS->FreePool (Info);
    Info = NULL;
  }
  if (FdtBuffer != NULL) {
    gBS->FreePool (FdtBuffer);
    FdtBuffer = NULL;
  }
  if (RamdiskAllocated) {
    gBS->FreePages (RamdiskAddress, (UINTN)RamdiskPages);
    RamdiskAllocated = FALSE;
  }

  // Allocate a final memory-map buffer with room for descriptor growth.
  MemoryMapSize = 0;
  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL ||
      DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR) ||
      DescriptorSize > MAX_UINTN / 8 ||
      MemoryMapSize > MAX_UINTN - DescriptorSize * 8) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  MemoryMapCapacity = MemoryMapSize + DescriptorSize * 8;
  Status = gBS->AllocatePool (
                  EfiLoaderData,
                  MemoryMapCapacity,
                  (VOID **)&MemoryMap
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  // ExitBootServices may require one retry with a refreshed map key.
  for (Attempt = 0; Attempt < 2; Attempt++) {
    MemoryMapSize = MemoryMapCapacity;
    Status = gBS->GetMemoryMap (
                    &MemoryMapSize,
                    MemoryMap,
                    &MapKey,
                    &DescriptorSize,
                    &DescriptorVersion
                    );
    if (EFI_ERROR (Status)) {
      break;
    }

    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  if (EFI_ERROR (Status)) {
    goto Done;
  }

  // Boot Services are gone. The assembly trampoline must not return.
  Arm64LinuxHandoff (
    KernelAddress,
    KernelImageSize,
    DecompressedRamdiskAddress,
    DecompressedRamdiskSize,
    FdtAddress,
    FdtBufferSize
    );
  CpuDeadLoop ();
  Status = EFI_ABORTED;

Done:
  if (MemoryMap != NULL) {
    gBS->FreePool (MemoryMap);
  }
  if (FdtAllocated) {
    gBS->FreePages (FdtAddress, FdtPages);
  }
  if (FdtBuffer != NULL) {
    gBS->FreePool (FdtBuffer);
  }
  if (DecompressedRamdiskAllocated) {
    gBS->FreePages (DecompressedRamdiskAddress, DecompressedRamdiskPages);
  }
  if (RamdiskAllocated) {
    gBS->FreePages (RamdiskAddress, (UINTN)RamdiskPages);
  }
  if (KernelAllocated) {
    gBS->FreePages (KernelAllocationAddress, (UINTN)KernelPages);
  }
  if (Info != NULL) {
    gBS->FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }

  if (EFI_ERROR (Status)) {
    Print (L"\nEchec du chargement Android : %r\n", Status);
  }
  return Status;
}
