#include <AsmMacroLib.h>
#include <Library/ArmLib.h>

.text
.align 3

enableDecon:
        ret

ASM_FUNC(ArmPlatformPeiBootAction)
        ret

ASM_FUNC(ArmPlatformIsPrimaryCore)
  MOV32 (w1, FixedPcdGet32(PcdArmPrimaryCoreMask))
  and   x0, x0, x1
  MOV32 (w1, FixedPcdGet32(PcdArmPrimaryCore))
  cmp   w0, w1
  cset  x0, eq
  ret

ASM_FUNC(ArmPlatformGetPrimaryCoreMpId)
  MOV32 (w0, FixedPcdGet32(PcdArmPrimaryCore))
  ret

ASM_FUNC(ArmPlatformGetCorePosition)
  and   x1, x0, #ARM_CORE_MASK
  and   x0, x0, #ARM_CLUSTER_MASK
  add   x0, x1, x0, LSR #6
  ret
