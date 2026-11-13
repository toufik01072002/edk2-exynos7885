/*
 * ARM64 Linux Image wrapper for the SM-A205W Virtual Boot handoff.
 *
 * ENABLE_HANDOFF is forced to 0 by build_bootshim.sh. The enabled branch is
 * compiled separately as a candidate only; it is never packaged or launched.
 */
#ifndef ENABLE_HANDOFF
#define ENABLE_HANDOFF 0
#endif

#ifndef UEFI_BASE
#error "UEFI_BASE must be supplied by the build"
#endif
#ifndef UEFI_SIZE
#error "UEFI_SIZE must be supplied by the build"
#endif
#ifndef SEC_ENTRY
#error "SEC_ENTRY must be supplied by the build"
#endif

#if UEFI_BASE != 0x90000000
#error "Unexpected fixed EDK2 FD base"
#endif
#if UEFI_SIZE != 0x00200000
#error "Unexpected EDK2 FD size"
#endif
#if SEC_ENTRY != 0x90008280
#error "Unexpected PrePi SEC entry"
#endif
#if (UEFI_SIZE % 16) != 0
#error "FD size must be a multiple of the copy width"
#endif

_Head:
	/* x1 is our position-independent FD source pointer. */
	adr	x1, _Payload
	b	_Start

	/* ARM64 Linux Image header. image_size is finalized after concatenation. */
	.quad	0x00080000	/* text_offset */
	.quad	0		/* image_size */
	.quad	0		/* flags */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"
	.long	0

_Start:
#if ENABLE_HANDOFF
	/*
	 * Candidate path only. The normal build forces ENABLE_HANDOFF=0.
	 * Keep a private stack in the upper half of the 64 KiB prefix.
	 * The stack grows down; the FD source starts exactly at its top.
	 */
	mov	x9, x1
	mov	sp, x9

	/* Copy the complete FD to its fixed, reserved physical address. */
	ldr	x10, =UEFI_BASE
	ldr	x11, =UEFI_SIZE
1:
	ldp	x12, x13, [x9], #16
	stp	x12, x13, [x10], #16
	subs	x11, x11, #16
	b.ne	1b

	/* Stores are complete; make the copied SEC instructions executable. */
	dsb	sy
	ic	iallu
	dsb	sy
	isb

	/* Enter PrePi SEC, not the FD base. */
	ldr	x16, =SEC_ENTRY
	br	x16
#else
	/* Mandatory default safety gate: never copy or branch to the FD. */
	b	_HandoffBlocked
#endif

_HandoffBlocked:
	wfe
	b	_HandoffBlocked

	.ltorg
	/* Code and literal pool must stay below the private stack area. */
	.if (. - _Head) > 0x8000
	.error "BootShim code/literals overlap the private stack"
	.endif

	/* 0x8000..0x10000 is the private 32 KiB stack; payload begins at 0x10000. */
	.org	0x10000
_Payload:
	/* The 2 MiB EDK2 FD is concatenated here by build.sh. */
