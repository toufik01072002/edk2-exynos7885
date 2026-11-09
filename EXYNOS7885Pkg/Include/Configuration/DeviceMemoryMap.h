#ifndef _DEVICE_MEMORY_MAP_H_
#define _DEVICE_MEMORY_MAP_H_

#include <Library/ArmLib.h>

#define MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT 128

/* Below flag is used for system memory */
#define SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES                               \
  EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |        \
      EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |     \
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |                               \
      EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |                         \
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |                            \
      EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE

typedef enum { NoHob, AddMem, AddDev, HobOnlyNoCacheSetting, MaxMem } DeviceMemoryAddHob;

#define MEMORY_REGION_NAME_MAX_LENGTH 32

typedef struct {
  CHAR8                        Name[MEMORY_REGION_NAME_MAX_LENGTH];
  EFI_PHYSICAL_ADDRESS         Address;
  UINT64                       Length;
  DeviceMemoryAddHob           HobOption;
  EFI_RESOURCE_TYPE            ResourceType;
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttribute;
  EFI_MEMORY_TYPE              MemoryType;
  ARM_MEMORY_REGION_ATTRIBUTES ArmAttributes;
} ARM_MEMORY_REGION_DESCRIPTOR_EX, *PARM_MEMORY_REGION_DESCRIPTOR_EX;

#define MEM_RES EFI_RESOURCE_MEMORY_RESERVED
#define MMAP_IO EFI_RESOURCE_MEMORY_MAPPED_IO
#define SYS_MEM EFI_RESOURCE_SYSTEM_MEMORY

#define SYS_MEM_CAP SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES
#define INITIALIZED EFI_RESOURCE_ATTRIBUTE_INITIALIZED
#define WRITE_COMBINEABLE EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE
#define UNCACHEABLE EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE

#define Reserv EfiReservedMemoryType
#define Conv EfiConventionalMemory
#define BsData EfiBootServicesData
#define BsCode EfiBootServicesCode
#define RtData EfiRuntimeServicesData
#define RtCode EfiRuntimeServicesCode
#define MmIO EfiMemoryMappedIO

#define DEVICE ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
#define WRITE_THROUGH ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_THROUGH_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_BACK ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define WRITE_BACK_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define UNCACHED_UNBUFFERED ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED
#define UNCACHED_UNBUFFERED_XN ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
*/

//--------------------- Register ---------------------
    {"Periphs",           0x00000000, 0x15000000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},

//--------------------- DDR --------------------- */
// RAM Bank 0: 0x80000000 - 0xBBC00000 (956 MiB)
// Reserved regions within bank 0:
    {"TIMA Reserved",     0xB8000000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},

// Usable RAM in Bank 0 (excluding TIMA at 0xB8000000-0xBA000000)
    {"RAM Bank 0 Low",    0x80000000, 0x38000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 0x80000000-0xB8000000 (896 MiB)
    {"RAM Bank 0 High",   0xBA000000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 0xBA000000-0xBBC00000 (60 MiB)

// RAM Bank 1: 0xC0000000 - 0x100000000 (1024 MiB)
// Reserved regions within bank 1:
    {"seclog Reserved",   0xC0000000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},  // 512 KiB

// Usable RAM in Bank 1 (excluding seclog at start)
    {"RAM Bank 1",        0xC0080000, 0x3FF80000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // 0xC0080000-0x100000000 (1023.5 MiB)

// RAM Bank 2: 0x880000000 - 0x8C0000000 (1024 MiB) - ABOVE 4GB!
// CMA pools are dynamic, don't reserve statically - mark as usable
    {"RAM Bank 2",        0x880000000, 0x40000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},   // 1024 MiB (0x40000000)

// Fixed reservations (not usable system memory):
    {"WiFi/BT Reserved",  0xE9000000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},  // 4 MiB
    {"ABOX Reserved",     0xE9400000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},  // 4 MiB
    {"Framebuffer",       0xEC000000, 0x01400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},  // 20 MiB (0x1400000)
    {"Video/Crypto/Modem/CP", 0xF0C00000, 0x0E000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN}, // 224 MiB (0xFEC00000 - 0xF0C00000)
    {"GNSS Reserved",     0xFFC00000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},  // 4 MiB

    /* Terminator for MMU */
    { "Terminator", 0, 0, 0, 0, 0, 0, 0}};

#endif