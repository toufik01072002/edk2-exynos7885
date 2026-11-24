#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../Multiboot/AndroidImageLoader.h"

#define ALPINE_DIR L"media\\0\\UEFI\\Alpine\\virtual-boot"
#define INITRD_NAME L"initramfs-alpine.cpio.gz"
#define DTB_NAME L"exynos7885-a205w-virtual.dtb"

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S; EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;
  EFI_FILE_PROTOCOL *Root=NULL, *Alpine=NULL;
  EFI_INPUT_KEY Key;

  Print(L"\n=== A205W LINUX TRACE LAUNCHER ===\n");
  Print(L"Mode: lanceur autonome; handoff ARM64 existant instrumente.\n");
  S=gBS->HandleProtocol(ImageHandle,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(EFI_ERROR(S) || Loaded==NULL) return S;
  S=gBS->HandleProtocol(Loaded->DeviceHandle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(S) || Fs==NULL) return S;
  S=Fs->OpenVolume(Fs,&Root);
  if(EFI_ERROR(S) || Root==NULL) return S;
  S=Root->Open(Root,&Alpine,ALPINE_DIR,EFI_FILE_MODE_READ,0);
  if(EFI_ERROR(S) || Alpine==NULL) {
    Print(L"[FAIL] Alpine virtual-boot directory: %r\n",S);
    Root->Close(Root);
    return S;
  }

  Print(L"Kernel  : " ALPINE_DIR L"\\Image\n");
  Print(L"Initrd  : " ALPINE_DIR L"\\" INITRD_NAME L"\n");
  Print(L"DTB     : " ALPINE_DIR L"\\" DTB_NAME L"\n");
  Print(L"\nStarting existing LinuxStageComponents()...\n");
  S=LinuxStageComponents(ImageHandle,Alpine,INITRD_NAME,DTB_NAME);
  Print(L"\n[RETURN] LinuxStageComponents returned: %r\n",S);
  Print(L"Le transfert a retourne; le journal mailbox doit etre releve apres reboot si gel.\n");
  if(gST->ConIn!=NULL) gST->ConIn->ReadKeyStroke(gST->ConIn,&Key);
  Alpine->Close(Alpine);
  Root->Close(Root);
  return S;
}
