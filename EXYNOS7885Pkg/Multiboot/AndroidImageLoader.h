#ifndef ANDROID_IMAGE_LOADER_H_
#define ANDROID_IMAGE_LOADER_H_

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

EFI_STATUS
EFIAPI
AndroidStageBootImage (
  IN EFI_HANDLE ImageHandle,
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *FileName
  );

#endif
