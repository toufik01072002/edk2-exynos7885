/*
 * ARM64 Linux Image wrapper for the SM-A205W Virtual Boot handoff.
 * ENABLE_HANDOFF is forced to 0 by build_bootshim.sh.
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

	/* ARM64 Linux Image header: exactly 64 bytes. */
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
	/* Candidate path only; normal packaged build forces ENABLE_HANDOFF=0. */
	mov	x9, x1
	mov	sp, x9
	ldr	x10, =UEFI_BASE
	ldr	x11, =UEFI_SIZE
1:
	ldp	x12, x13, [x9], #16
	stp	x12, x13, [x10], #16
	subs	x11, x11, #16
	b.ne	1b
	dsb	sy
	ic	iallu
	dsb	sy
	isb
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
	/* Prefix is fixed at 64 KiB; overflow is an assembly error. */
	.org	0x10000
_Payload:
	/* The 2 MiB EDK2 FD is concatenated here by build.sh. */
