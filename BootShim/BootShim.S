_Head:
	/* Linux arm64 Image entry: x0 carries the FDT pointer. */
	adr	x1, _Payload
	b	_Start

	/* Linux arm64 Image header (image_size is patched after concatenation). */
	.quad 0x00080000	/* text_offset: valid 2 MiB-window offset */
	.quad 0		/* image_size: finalized by finalize_linux_image.py */
	.quad 0		/* flags: little-endian, default page-size encoding */
	.quad 0
	.quad 0
	.quad 0
	.ascii "ARM\x64"
	.long 0

_Start:
	/*
	 * Phase 2 safety gate:
	 * Do not copy or branch into the FD until the EDK2 entry ABI,
	 * exception level, cache/MMU state, FDT handoff and CPU shutdown
	 * contract have been implemented and reviewed.
	 */
	b	_HandoffBlocked

_HandoffBlocked:
	wfe
	b	_HandoffBlocked

	.align 4
_Payload:
	/* The FD payload is concatenated here by build.sh. */
