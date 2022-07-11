	.text

.section .text.fmemcpy, "ax", %progbits
.balign	4
.globl	fmemcpy
.type fmemcpy, %function
fmemcpy:
	// check for zero length copy or the same pointer
	cmp		r2, #0
	cmpne	r1, r0
	bxeq	lr
	// save a few registers for use and the return code (input dst)
	stmfd	sp!, {r0, r4, r5, lr}
	// check for forwards overlap (src > dst, distance < len)
	subs	r3, r0, r1
	cmpgt	r2, r3
	bgt		.Lmc_forwardoverlap
	// check for a short copy len.
	// 20 bytes is enough so that if a 16 byte alignment needs to happen there is at least a 
	//   wordwise copy worth of work to be done.
	cmp		r2, #(16+4)
	blt		.Lmc_bytewise
	// see if they are similarly aligned on 4 byte boundaries
	eor		r3, r0, r1
	tst		r3, #3
	bne		.Lmc_bytewise		// dissimilarly aligned, nothing we can do (for now)
	// check for 16 byte alignment on dst.
	// this will also catch src being not 4 byte aligned, since it is similarly 4 byte 
	//   aligned with dst at this point.
	tst		r0, #15
	bne		.Lmc_not16bytealigned
	// check to see if we have at least 32 bytes of data to copy.
	// if not, just revert to wordwise copy
	cmp		r2, #32
	blt		.Lmc_wordwise
.Lmc_bigcopy:
	// copy 32 bytes at a time. src & dst need to be at least 4 byte aligned, 
	// and we need at least 32 bytes remaining to copy
	// save r6-r7 for use in the big copy
	stmfd	sp!, {r6-r7}
	sub		r2, r2, #32		// subtract an extra 32 to the len so we can avoid an extra compare
.Lmc_bigcopy_loop:
	ldmia	r1!, {r4, r5, r6, r7}
	stmia	r0!, {r4, r5, r6, r7}
	ldmia	r1!, {r4, r5, r6, r7}
	subs	r2, r2, #32
	stmia	r0!, {r4, r5, r6, r7}
	bge		.Lmc_bigcopy_loop
	// restore r6-r7
	ldmfd	sp!, {r6-r7}
	// see if we are done
	adds	r2, r2, #32
	beq		.Lmc_done
	// less then 4 bytes left?
	cmp		r2, #4
	blt		.Lmc_bytewise
.Lmc_wordwise:
	// copy 4 bytes at a time.
	// src & dst are guaranteed to be word aligned, and at least 4 bytes are left to copy.
	subs	r2, r2, #4
.Lmc_wordwise_loop:
	ldr		r3, [r1], #4
	subs	r2, r2, #4
	str		r3, [r0], #4
	bge		.Lmc_wordwise_loop
	// correct the remaining len and test for completion
	adds	r2, r2, #4	
	beq		.Lmc_done
.Lmc_bytewise:
	// simple bytewise copy
	ldrb	r3, [r1], #1
	subs	r2, r2, #1
	strb	r3, [r0], #1
	bgt		.Lmc_bytewise
.Lmc_done:
	// load dst for return and restore r4,r5
	ldmfd	sp!, {r0, r4, r5, pc}
.Lmc_not16bytealigned:
	// dst is not 16 byte aligned, so we will copy up to 15 bytes to get it aligned.
	// src is guaranteed to be similarly word aligned with dst.
	// set the condition flags based on the alignment.
	lsl		r12, r0, #28
	rsb		r12, r12, #0
	msr		CPSR_f, r12				// move into NZCV fields in CPSR
	// move as many bytes as necessary to get the dst aligned
	ldrvsb	r3, [r1], #1			// V set
	ldrcsh	r4, [r1], #2			// C set
	ldreq	r5, [r1], #4			// Z set
	strvsb	r3, [r0], #1
	strcsh	r4, [r0], #2
	streq	r5, [r0], #4
	ldmmiia	r1!, {r3-r4}			// N set
	stmmiia	r0!, {r3-r4}
	// fix the remaining len
	sub		r2, r2, r12, lsr #28
	// test to see what we should do now
	cmp		r2, #32
	bge		.Lmc_bigcopy
	b		.Lmc_wordwise
	
	// src and dest overlap 'forwards' or dst > src
.Lmc_forwardoverlap:
	// do a bytewise reverse copy for now
	add		r1, r1, r2
	add		r0, r0, r2
.Lmc_bytewisereverse:
	// simple bytewise reverse copy
	ldrb	r3, [r1], #-1
	subs	r2, r2, #1
	strb	r3, [r0], #-1
	bgt		.Lmc_bytewisereverse
	b		.Lmc_done
	
.section .text.fmemset, "ax", %progbits
.balign	4
.globl	fmemset
.type fmemset, %function
fmemset:
	// check for zero length
	cmp		r2, #0
	bxeq	lr
	// save the original pointer
	mov		r12, r0
	// short memsets aren't worth optimizing
	cmp		r2, #(32 + 16)
	blt		.Lms_bytewise
	// fill a 32 bit register with the 8 bit value
	and		r1, r1, #0xff
	orr		r1, r1, r1, lsl #8
	orr		r1, r1, r1, lsl #16
	// check for 16 byte alignment
	tst		r0, #15
	bne		.Lms_not16bytealigned
.Lms_bigset:
	// dump some registers to make space for our values
	stmfd	sp!, { r4-r5 }
	
	// fill a bunch of registers with the set value
	mov		r3, r1
	mov		r4, r1
	mov		r5, r1
	// prepare the count register so we can avoid an extra compare
	sub 	r2, r2, #32
	// 32 bytes at a time
.Lms_bigset_loop:
	stmia	r0!, { r1, r3, r4, r5 }
	subs	r2, r2, #32
	stmia	r0!, { r1, r3, r4, r5 }
	bge		.Lms_bigset_loop
	// restore our dumped registers
	ldmfd	sp!, { r4-r5 }
	// see if we're done
	adds	r2, r2, #32
	beq		.Lms_done
.Lms_bytewise:
	// bytewise memset
	subs	r2, r2, #1
	strb	r1, [r0], #1
	bgt		.Lms_bytewise
.Lms_done:
	// restore the base pointer as return value
	mov		r0, r12
	bx		lr
.Lms_not16bytealigned:
	// dst is not 16 byte aligned, so we will set up to 15 bytes to get it aligned.
	// set the condition flags based on the alignment.
	lsl     r3, r0, #28
	rsb     r3, r3, #0
	msr     CPSR_f, r3             // move into NZCV fields in CPSR
	// move as many bytes as necessary to get the dst aligned
	strvsb  r1, [r0], #1			// V set
	strcsh  r1, [r0], #2			// C set
	streq   r1, [r0], #4			// Z set
	strmi   r1, [r0], #4			// N set
	strmi   r1, [r0], #4			// N set
	// fix the remaining len
	sub     r2, r2, r3, lsr #28
	// do the large memset
	b       .Lms_bigset
