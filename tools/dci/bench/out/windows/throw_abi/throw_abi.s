	.def	@feat.00;
	.scl	3;
	.type	0;
	.endef
	.globl	@feat.00
@feat.00 = 0
	.file	"throw_abi.cpp"
	.def	"?checked_divide@abi@@YAHHH@Z";
	.scl	2;
	.type	32;
	.endef
	.text
	.globl	"?checked_divide@abi@@YAHHH@Z"  # -- Begin function ?checked_divide@abi@@YAHHH@Z
	.p2align	4
"?checked_divide@abi@@YAHHH@Z":         # @"?checked_divide@abi@@YAHHH@Z"
.seh_proc "?checked_divide@abi@@YAHHH@Z"
# %bb.0:
	subq	$72, %rsp
	.seh_stackalloc 72
	.seh_endprologue
	movl	%edx, 68(%rsp)
	movl	%ecx, 64(%rsp)
	cmpl	$0, 68(%rsp)
	jne	.LBB0_2
# %bb.1:
	leaq	40(%rsp), %rcx
	leaq	"??_C@_04JMHLEDPH@div0?$AA@"(%rip), %rdx
	callq	"??0runtime_error@std@@QEAA@PEBD@Z"
	leaq	40(%rsp), %rcx
	leaq	"_TI2?AVruntime_error@std@@"(%rip), %rdx
	callq	_CxxThrowException
.LBB0_2:
	movl	64(%rsp), %eax
	cltd
	idivl	68(%rsp)
	.seh_startepilogue
	addq	$72, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"??0runtime_error@std@@QEAA@PEBD@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??0runtime_error@std@@QEAA@PEBD@Z"
	.globl	"??0runtime_error@std@@QEAA@PEBD@Z" # -- Begin function ??0runtime_error@std@@QEAA@PEBD@Z
	.p2align	4
"??0runtime_error@std@@QEAA@PEBD@Z":    # @"??0runtime_error@std@@QEAA@PEBD@Z"
.seh_proc "??0runtime_error@std@@QEAA@PEBD@Z"
# %bb.0:
	subq	$56, %rsp
	.seh_stackalloc 56
	.seh_endprologue
	movq	%rdx, 48(%rsp)
	movq	%rcx, 40(%rsp)
	movq	40(%rsp), %rcx
	movq	%rcx, 32(%rsp)                  # 8-byte Spill
	movq	48(%rsp), %rdx
	callq	"??0exception@std@@QEAA@QEBD@Z"
                                        # kill: def $rcx killed $rax
	movq	32(%rsp), %rax                  # 8-byte Reload
	leaq	"??_7runtime_error@std@@6B@"(%rip), %rcx
	movq	%rcx, (%rax)
	.seh_startepilogue
	addq	$56, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"??0runtime_error@std@@QEAA@AEBV01@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??0runtime_error@std@@QEAA@AEBV01@@Z"
	.globl	"??0runtime_error@std@@QEAA@AEBV01@@Z" # -- Begin function ??0runtime_error@std@@QEAA@AEBV01@@Z
	.p2align	4
"??0runtime_error@std@@QEAA@AEBV01@@Z": # @"??0runtime_error@std@@QEAA@AEBV01@@Z"
.seh_proc "??0runtime_error@std@@QEAA@AEBV01@@Z"
# %bb.0:
	subq	$56, %rsp
	.seh_stackalloc 56
	.seh_endprologue
	movq	%rdx, 48(%rsp)
	movq	%rcx, 40(%rsp)
	movq	40(%rsp), %rcx
	movq	%rcx, 32(%rsp)                  # 8-byte Spill
	movq	48(%rsp), %rdx
	callq	"??0exception@std@@QEAA@AEBV01@@Z"
                                        # kill: def $rcx killed $rax
	movq	32(%rsp), %rax                  # 8-byte Reload
	leaq	"??_7runtime_error@std@@6B@"(%rip), %rcx
	movq	%rcx, (%rax)
	.seh_startepilogue
	addq	$56, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"??0exception@std@@QEAA@AEBV01@@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??0exception@std@@QEAA@AEBV01@@Z"
	.globl	"??0exception@std@@QEAA@AEBV01@@Z" # -- Begin function ??0exception@std@@QEAA@AEBV01@@Z
	.p2align	4
"??0exception@std@@QEAA@AEBV01@@Z":     # @"??0exception@std@@QEAA@AEBV01@@Z"
.Lfunc_begin0:
.seh_proc "??0exception@std@@QEAA@AEBV01@@Z"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$64, %rsp
	.seh_stackalloc 64
	leaq	64(%rsp), %rbp
	.seh_setframe %rbp, 64
	.seh_endprologue
	movq	$-2, -8(%rbp)
	movq	%rdx, -16(%rbp)
	movq	%rcx, -24(%rbp)
	movq	-24(%rbp), %rax
	movq	%rax, -32(%rbp)                 # 8-byte Spill
	leaq	"??_7exception@std@@6B@"(%rip), %rcx
	movq	%rcx, (%rax)
	movq	%rax, %rdx
	addq	$8, %rdx
	xorps	%xmm0, %xmm0
	movups	%xmm0, 8(%rax)
	movq	-16(%rbp), %rcx
	addq	$8, %rcx
.Ltmp0:                                 # EH_LABEL
	callq	__std_exception_copy
	nop
.Ltmp1:                                 # EH_LABEL
	jmp	.LBB3_1
.LBB3_1:
	movq	-32(%rbp), %rax                 # 8-byte Reload
	.seh_startepilogue
	addq	$64, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$??0exception@std@@QEAA@AEBV01@@Z"@IMGREL
	.section	.text,"xr",discard,"??0exception@std@@QEAA@AEBV01@@Z"
	.seh_endproc
	.def	"?dtor$2@?0???0exception@std@@QEAA@AEBV01@@Z@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?dtor$2@?0???0exception@std@@QEAA@AEBV01@@Z@4HA":
.seh_proc "?dtor$2@?0???0exception@std@@QEAA@AEBV01@@Z@4HA"
.LBB3_2:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	64(%rdx), %rbp
	.seh_endprologue
	callq	__std_terminate
	int3
.Lfunc_end0:
	.seh_handlerdata
	.section	.text,"xr",discard,"??0exception@std@@QEAA@AEBV01@@Z"
	.seh_endproc
	.section	.xdata,"dr",associative,"??0exception@std@@QEAA@AEBV01@@Z",unique,0
	.p2align	2, 0x0
"$cppxdata$??0exception@std@@QEAA@AEBV01@@Z":
	.long	429065506                       # MagicNumber
	.long	1                               # MaxState
	.long	"$stateUnwindMap$??0exception@std@@QEAA@AEBV01@@Z"@IMGREL # UnwindMap
	.long	0                               # NumTryBlocks
	.long	0                               # TryBlockMap
	.long	3                               # IPMapEntries
	.long	"$ip2state$??0exception@std@@QEAA@AEBV01@@Z"@IMGREL # IPToStateXData
	.long	56                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$??0exception@std@@QEAA@AEBV01@@Z":
	.long	-1                              # ToState
	.long	"?dtor$2@?0???0exception@std@@QEAA@AEBV01@@Z@4HA"@IMGREL # Action
"$ip2state$??0exception@std@@QEAA@AEBV01@@Z":
	.long	.Lfunc_begin0@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp0@IMGREL                   # IP
	.long	0                               # ToState
	.long	.Ltmp1@IMGREL                   # IP
	.long	-1                              # ToState
	.section	.text,"xr",discard,"??0exception@std@@QEAA@AEBV01@@Z"
                                        # -- End function
	.def	"??1runtime_error@std@@UEAA@XZ";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??1runtime_error@std@@UEAA@XZ"
	.globl	"??1runtime_error@std@@UEAA@XZ" # -- Begin function ??1runtime_error@std@@UEAA@XZ
	.p2align	4
"??1runtime_error@std@@UEAA@XZ":        # @"??1runtime_error@std@@UEAA@XZ"
.seh_proc "??1runtime_error@std@@UEAA@XZ"
# %bb.0:
	subq	$40, %rsp
	.seh_stackalloc 40
	.seh_endprologue
	movq	%rcx, 32(%rsp)
	movq	32(%rsp), %rcx
	callq	"??1exception@std@@UEAA@XZ"
	nop
	.seh_startepilogue
	addq	$40, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"?catch_runtime@abi@@YAHHH@Z";
	.scl	2;
	.type	32;
	.endef
	.text
	.globl	"?catch_runtime@abi@@YAHHH@Z"   # -- Begin function ?catch_runtime@abi@@YAHHH@Z
	.p2align	4
"?catch_runtime@abi@@YAHHH@Z":          # @"?catch_runtime@abi@@YAHHH@Z"
.Lfunc_begin1:
.seh_proc "?catch_runtime@abi@@YAHHH@Z"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$64, %rsp
	.seh_stackalloc 64
	leaq	64(%rsp), %rbp
	.seh_setframe %rbp, 64
	.seh_endprologue
	movq	$-2, -16(%rbp)
	movl	%edx, -24(%rbp)
	movl	%ecx, -28(%rbp)
	movl	-24(%rbp), %edx
	movl	-28(%rbp), %ecx
.Ltmp2:                                 # EH_LABEL
	callq	"?checked_divide@abi@@YAHHH@Z"
	nop
.Ltmp3:                                 # EH_LABEL
	movl	%eax, -32(%rbp)                 # 4-byte Spill
	jmp	.LBB5_2
.LBB5_2:
	movl	-32(%rbp), %eax                 # 4-byte Reload
	movl	%eax, -20(%rbp)
	jmp	.LBB5_4
.LBB5_3:                                # Block address taken
$ehgcr_5_3:
	jmp	.LBB5_4
.LBB5_4:
	movl	-20(%rbp), %eax
	.seh_startepilogue
	addq	$64, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$?catch_runtime@abi@@YAHHH@Z"@IMGREL
	.text
	.seh_endproc
	.def	"?catch$1@?0??catch_runtime@abi@@YAHHH@Z@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?catch$1@?0??catch_runtime@abi@@YAHHH@Z@4HA":
.seh_proc "?catch$1@?0??catch_runtime@abi@@YAHHH@Z@4HA"
	.seh_handler __CxxFrameHandler3, @unwind, @except
.LBB5_1:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	64(%rdx), %rbp
	.seh_endprologue
	movl	$-1, -20(%rbp)
	leaq	.LBB5_3(%rip), %rax
	.seh_startepilogue
	addq	$32, %rsp
	popq	%rbp
	.seh_endepilogue
	retq                                    # CATCHRET
.Lfunc_end1:
	.seh_handlerdata
	.long	"$cppxdata$?catch_runtime@abi@@YAHHH@Z"@IMGREL
	.text
	.seh_endproc
	.section	.xdata,"dr"
	.p2align	2, 0x0
"$cppxdata$?catch_runtime@abi@@YAHHH@Z":
	.long	429065506                       # MagicNumber
	.long	2                               # MaxState
	.long	"$stateUnwindMap$?catch_runtime@abi@@YAHHH@Z"@IMGREL # UnwindMap
	.long	1                               # NumTryBlocks
	.long	"$tryMap$?catch_runtime@abi@@YAHHH@Z"@IMGREL # TryBlockMap
	.long	4                               # IPMapEntries
	.long	"$ip2state$?catch_runtime@abi@@YAHHH@Z"@IMGREL # IPToStateXData
	.long	48                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$?catch_runtime@abi@@YAHHH@Z":
	.long	-1                              # ToState
	.long	0                               # Action
	.long	-1                              # ToState
	.long	0                               # Action
"$tryMap$?catch_runtime@abi@@YAHHH@Z":
	.long	0                               # TryLow
	.long	0                               # TryHigh
	.long	1                               # CatchHigh
	.long	1                               # NumCatches
	.long	"$handlerMap$0$?catch_runtime@abi@@YAHHH@Z"@IMGREL # HandlerArray
"$handlerMap$0$?catch_runtime@abi@@YAHHH@Z":
	.long	8                               # Adjectives
	.long	"??_R0?AVruntime_error@std@@@8"@IMGREL # Type
	.long	56                              # CatchObjOffset
	.long	"?catch$1@?0??catch_runtime@abi@@YAHHH@Z@4HA"@IMGREL # Handler
	.long	56                              # ParentFrameOffset
"$ip2state$?catch_runtime@abi@@YAHHH@Z":
	.long	.Lfunc_begin1@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp2@IMGREL                   # IP
	.long	0                               # ToState
	.long	.Ltmp3@IMGREL                   # IP
	.long	-1                              # ToState
	.long	"?catch$1@?0??catch_runtime@abi@@YAHHH@Z@4HA"@IMGREL # IP
	.long	1                               # ToState
	.text
                                        # -- End function
	.def	"?catch_all@abi@@YAHHH@Z";
	.scl	2;
	.type	32;
	.endef
	.globl	"?catch_all@abi@@YAHHH@Z"       # -- Begin function ?catch_all@abi@@YAHHH@Z
	.p2align	4
"?catch_all@abi@@YAHHH@Z":              # @"?catch_all@abi@@YAHHH@Z"
.Lfunc_begin2:
.seh_proc "?catch_all@abi@@YAHHH@Z"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$64, %rsp
	.seh_stackalloc 64
	leaq	64(%rsp), %rbp
	.seh_setframe %rbp, 64
	.seh_endprologue
	movq	$-2, -8(%rbp)
	movl	%edx, -16(%rbp)
	movl	%ecx, -20(%rbp)
	movl	-16(%rbp), %edx
	movl	-20(%rbp), %ecx
.Ltmp4:                                 # EH_LABEL
	callq	"?checked_divide@abi@@YAHHH@Z"
	nop
.Ltmp5:                                 # EH_LABEL
	movl	%eax, -24(%rbp)                 # 4-byte Spill
	jmp	.LBB6_2
.LBB6_2:
	movl	-24(%rbp), %eax                 # 4-byte Reload
	movl	%eax, -12(%rbp)
	jmp	.LBB6_4
.LBB6_3:                                # Block address taken
$ehgcr_6_3:
	jmp	.LBB6_4
.LBB6_4:
	movl	-12(%rbp), %eax
	.seh_startepilogue
	addq	$64, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$?catch_all@abi@@YAHHH@Z"@IMGREL
	.text
	.seh_endproc
	.def	"?catch$1@?0??catch_all@abi@@YAHHH@Z@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?catch$1@?0??catch_all@abi@@YAHHH@Z@4HA":
.seh_proc "?catch$1@?0??catch_all@abi@@YAHHH@Z@4HA"
	.seh_handler __CxxFrameHandler3, @unwind, @except
.LBB6_1:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	64(%rdx), %rbp
	.seh_endprologue
	movl	$-2, -12(%rbp)
	leaq	.LBB6_3(%rip), %rax
	.seh_startepilogue
	addq	$32, %rsp
	popq	%rbp
	.seh_endepilogue
	retq                                    # CATCHRET
.Lfunc_end2:
	.seh_handlerdata
	.long	"$cppxdata$?catch_all@abi@@YAHHH@Z"@IMGREL
	.text
	.seh_endproc
	.section	.xdata,"dr"
	.p2align	2, 0x0
"$cppxdata$?catch_all@abi@@YAHHH@Z":
	.long	429065506                       # MagicNumber
	.long	2                               # MaxState
	.long	"$stateUnwindMap$?catch_all@abi@@YAHHH@Z"@IMGREL # UnwindMap
	.long	1                               # NumTryBlocks
	.long	"$tryMap$?catch_all@abi@@YAHHH@Z"@IMGREL # TryBlockMap
	.long	4                               # IPMapEntries
	.long	"$ip2state$?catch_all@abi@@YAHHH@Z"@IMGREL # IPToStateXData
	.long	56                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$?catch_all@abi@@YAHHH@Z":
	.long	-1                              # ToState
	.long	0                               # Action
	.long	-1                              # ToState
	.long	0                               # Action
"$tryMap$?catch_all@abi@@YAHHH@Z":
	.long	0                               # TryLow
	.long	0                               # TryHigh
	.long	1                               # CatchHigh
	.long	1                               # NumCatches
	.long	"$handlerMap$0$?catch_all@abi@@YAHHH@Z"@IMGREL # HandlerArray
"$handlerMap$0$?catch_all@abi@@YAHHH@Z":
	.long	64                              # Adjectives
	.long	0                               # Type
	.long	0                               # CatchObjOffset
	.long	"?catch$1@?0??catch_all@abi@@YAHHH@Z@4HA"@IMGREL # Handler
	.long	56                              # ParentFrameOffset
"$ip2state$?catch_all@abi@@YAHHH@Z":
	.long	.Lfunc_begin2@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp4@IMGREL                   # IP
	.long	0                               # ToState
	.long	.Ltmp5@IMGREL                   # IP
	.long	-1                              # ToState
	.long	"?catch$1@?0??catch_all@abi@@YAHHH@Z@4HA"@IMGREL # IP
	.long	1                               # ToState
	.text
                                        # -- End function
	.def	"?throw_int@abi@@YAHXZ";
	.scl	2;
	.type	32;
	.endef
	.globl	"?throw_int@abi@@YAHXZ"         # -- Begin function ?throw_int@abi@@YAHXZ
	.p2align	4
"?throw_int@abi@@YAHXZ":                # @"?throw_int@abi@@YAHXZ"
.seh_proc "?throw_int@abi@@YAHXZ"
# %bb.0:
	subq	$40, %rsp
	.seh_stackalloc 40
	.seh_endprologue
	movl	$42, 36(%rsp)
	leaq	36(%rsp), %rcx
	leaq	_TI1H(%rip), %rdx
	callq	_CxxThrowException
	int3
	.seh_endproc
                                        # -- End function
	.def	"?catch_int@abi@@YAHXZ";
	.scl	2;
	.type	32;
	.endef
	.globl	"?catch_int@abi@@YAHXZ"         # -- Begin function ?catch_int@abi@@YAHXZ
	.p2align	4
"?catch_int@abi@@YAHXZ":                # @"?catch_int@abi@@YAHXZ"
.Lfunc_begin3:
.seh_proc "?catch_int@abi@@YAHXZ"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$64, %rsp
	.seh_stackalloc 64
	leaq	64(%rsp), %rbp
	.seh_setframe %rbp, 64
	.seh_endprologue
	movq	$-2, -16(%rbp)
.Ltmp6:                                 # EH_LABEL
	callq	"?throw_int@abi@@YAHXZ"
	nop
.Ltmp7:                                 # EH_LABEL
	movl	%eax, -24(%rbp)                 # 4-byte Spill
	jmp	.LBB8_2
.LBB8_2:
	movl	-24(%rbp), %eax                 # 4-byte Reload
	movl	%eax, -20(%rbp)
	jmp	.LBB8_4
.LBB8_3:                                # Block address taken
$ehgcr_8_3:
	jmp	.LBB8_4
.LBB8_4:
	movl	-20(%rbp), %eax
	.seh_startepilogue
	addq	$64, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$?catch_int@abi@@YAHXZ"@IMGREL
	.text
	.seh_endproc
	.def	"?catch$1@?0??catch_int@abi@@YAHXZ@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?catch$1@?0??catch_int@abi@@YAHXZ@4HA":
.seh_proc "?catch$1@?0??catch_int@abi@@YAHXZ@4HA"
	.seh_handler __CxxFrameHandler3, @unwind, @except
.LBB8_1:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	64(%rdx), %rbp
	.seh_endprologue
	movl	-4(%rbp), %eax
	movl	%eax, -20(%rbp)
	leaq	.LBB8_3(%rip), %rax
	.seh_startepilogue
	addq	$32, %rsp
	popq	%rbp
	.seh_endepilogue
	retq                                    # CATCHRET
.Lfunc_end3:
	.seh_handlerdata
	.long	"$cppxdata$?catch_int@abi@@YAHXZ"@IMGREL
	.text
	.seh_endproc
	.section	.xdata,"dr"
	.p2align	2, 0x0
"$cppxdata$?catch_int@abi@@YAHXZ":
	.long	429065506                       # MagicNumber
	.long	2                               # MaxState
	.long	"$stateUnwindMap$?catch_int@abi@@YAHXZ"@IMGREL # UnwindMap
	.long	1                               # NumTryBlocks
	.long	"$tryMap$?catch_int@abi@@YAHXZ"@IMGREL # TryBlockMap
	.long	4                               # IPMapEntries
	.long	"$ip2state$?catch_int@abi@@YAHXZ"@IMGREL # IPToStateXData
	.long	48                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$?catch_int@abi@@YAHXZ":
	.long	-1                              # ToState
	.long	0                               # Action
	.long	-1                              # ToState
	.long	0                               # Action
"$tryMap$?catch_int@abi@@YAHXZ":
	.long	0                               # TryLow
	.long	0                               # TryHigh
	.long	1                               # CatchHigh
	.long	1                               # NumCatches
	.long	"$handlerMap$0$?catch_int@abi@@YAHXZ"@IMGREL # HandlerArray
"$handlerMap$0$?catch_int@abi@@YAHXZ":
	.long	0                               # Adjectives
	.long	"??_R0H@8"@IMGREL               # Type
	.long	60                              # CatchObjOffset
	.long	"?catch$1@?0??catch_int@abi@@YAHXZ@4HA"@IMGREL # Handler
	.long	56                              # ParentFrameOffset
"$ip2state$?catch_int@abi@@YAHXZ":
	.long	.Lfunc_begin3@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp6@IMGREL                   # IP
	.long	0                               # ToState
	.long	.Ltmp7@IMGREL                   # IP
	.long	-1                              # ToState
	.long	"?catch$1@?0??catch_int@abi@@YAHXZ@4HA"@IMGREL # IP
	.long	1                               # ToState
	.text
                                        # -- End function
	.def	"??0exception@std@@QEAA@QEBD@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??0exception@std@@QEAA@QEBD@Z"
	.globl	"??0exception@std@@QEAA@QEBD@Z" # -- Begin function ??0exception@std@@QEAA@QEBD@Z
	.p2align	4
"??0exception@std@@QEAA@QEBD@Z":        # @"??0exception@std@@QEAA@QEBD@Z"
.Lfunc_begin4:
.seh_proc "??0exception@std@@QEAA@QEBD@Z"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$80, %rsp
	.seh_stackalloc 80
	leaq	80(%rsp), %rbp
	.seh_setframe %rbp, 80
	.seh_endprologue
	movq	$-2, -8(%rbp)
	movq	%rdx, -16(%rbp)
	movq	%rcx, -24(%rbp)
	movq	-24(%rbp), %rax
	movq	%rax, -48(%rbp)                 # 8-byte Spill
	leaq	"??_7exception@std@@6B@"(%rip), %rcx
	movq	%rcx, (%rax)
	movq	%rax, %rdx
	addq	$8, %rdx
	xorps	%xmm0, %xmm0
	movups	%xmm0, 8(%rax)
	movq	-16(%rbp), %rax
	movq	%rax, -40(%rbp)
	movb	$1, -32(%rbp)
.Ltmp8:                                 # EH_LABEL
	leaq	-40(%rbp), %rcx
	callq	__std_exception_copy
	nop
.Ltmp9:                                 # EH_LABEL
	jmp	.LBB9_1
.LBB9_1:
	movq	-48(%rbp), %rax                 # 8-byte Reload
	.seh_startepilogue
	addq	$80, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$??0exception@std@@QEAA@QEBD@Z"@IMGREL
	.section	.text,"xr",discard,"??0exception@std@@QEAA@QEBD@Z"
	.seh_endproc
	.def	"?dtor$2@?0???0exception@std@@QEAA@QEBD@Z@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?dtor$2@?0???0exception@std@@QEAA@QEBD@Z@4HA":
.seh_proc "?dtor$2@?0???0exception@std@@QEAA@QEBD@Z@4HA"
.LBB9_2:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	80(%rdx), %rbp
	.seh_endprologue
	callq	__std_terminate
	int3
.Lfunc_end4:
	.seh_handlerdata
	.section	.text,"xr",discard,"??0exception@std@@QEAA@QEBD@Z"
	.seh_endproc
	.section	.xdata,"dr",associative,"??0exception@std@@QEAA@QEBD@Z",unique,1
	.p2align	2, 0x0
"$cppxdata$??0exception@std@@QEAA@QEBD@Z":
	.long	429065506                       # MagicNumber
	.long	1                               # MaxState
	.long	"$stateUnwindMap$??0exception@std@@QEAA@QEBD@Z"@IMGREL # UnwindMap
	.long	0                               # NumTryBlocks
	.long	0                               # TryBlockMap
	.long	3                               # IPMapEntries
	.long	"$ip2state$??0exception@std@@QEAA@QEBD@Z"@IMGREL # IPToStateXData
	.long	72                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$??0exception@std@@QEAA@QEBD@Z":
	.long	-1                              # ToState
	.long	"?dtor$2@?0???0exception@std@@QEAA@QEBD@Z@4HA"@IMGREL # Action
"$ip2state$??0exception@std@@QEAA@QEBD@Z":
	.long	.Lfunc_begin4@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp8@IMGREL                   # IP
	.long	0                               # ToState
	.long	.Ltmp9@IMGREL                   # IP
	.long	-1                              # ToState
	.section	.text,"xr",discard,"??0exception@std@@QEAA@QEBD@Z"
                                        # -- End function
	.def	"?what@exception@std@@UEBAPEBDXZ";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"?what@exception@std@@UEBAPEBDXZ"
	.globl	"?what@exception@std@@UEBAPEBDXZ" # -- Begin function ?what@exception@std@@UEBAPEBDXZ
	.p2align	4
"?what@exception@std@@UEBAPEBDXZ":      # @"?what@exception@std@@UEBAPEBDXZ"
.seh_proc "?what@exception@std@@UEBAPEBDXZ"
# %bb.0:
	subq	$24, %rsp
	.seh_stackalloc 24
	.seh_endprologue
	movq	%rcx, 16(%rsp)
	movq	16(%rsp), %rax
	movq	%rax, 8(%rsp)                   # 8-byte Spill
	cmpq	$0, 8(%rax)
	je	.LBB10_2
# %bb.1:
	movq	8(%rsp), %rax                   # 8-byte Reload
	movq	8(%rax), %rax
	movq	%rax, (%rsp)                    # 8-byte Spill
	jmp	.LBB10_3
.LBB10_2:
	leaq	"??_C@_0BC@EOODALEL@Unknown?5exception?$AA@"(%rip), %rax
	movq	%rax, (%rsp)                    # 8-byte Spill
	jmp	.LBB10_3
.LBB10_3:
	movq	(%rsp), %rax                    # 8-byte Reload
	.seh_startepilogue
	addq	$24, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"??_Gexception@std@@UEAAPEAXI@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??_Gexception@std@@UEAAPEAXI@Z"
	.globl	"??_Gexception@std@@UEAAPEAXI@Z" # -- Begin function ??_Gexception@std@@UEAAPEAXI@Z
	.p2align	4
"??_Gexception@std@@UEAAPEAXI@Z":       # @"??_Gexception@std@@UEAAPEAXI@Z"
.seh_proc "??_Gexception@std@@UEAAPEAXI@Z"
# %bb.0:
	subq	$72, %rsp
	.seh_stackalloc 72
	.seh_endprologue
	movl	%edx, 60(%rsp)
	movq	%rcx, 48(%rsp)
	movq	48(%rsp), %rcx
	movq	%rcx, 32(%rsp)                  # 8-byte Spill
	movq	%rcx, 64(%rsp)
	movl	60(%rsp), %eax
	movl	%eax, 44(%rsp)                  # 4-byte Spill
	callq	"??1exception@std@@UEAA@XZ"
	movl	44(%rsp), %eax                  # 4-byte Reload
	andl	$1, %eax
	cmpl	$0, %eax
	je	.LBB11_2
# %bb.1:
	movq	32(%rsp), %rcx                  # 8-byte Reload
	movl	$24, %edx
	callq	"??3@YAXPEAX_K@Z"
.LBB11_2:
	movq	64(%rsp), %rax
	.seh_startepilogue
	addq	$72, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.def	"??1exception@std@@UEAA@XZ";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??1exception@std@@UEAA@XZ"
	.globl	"??1exception@std@@UEAA@XZ"     # -- Begin function ??1exception@std@@UEAA@XZ
	.p2align	4
"??1exception@std@@UEAA@XZ":            # @"??1exception@std@@UEAA@XZ"
.Lfunc_begin5:
.seh_proc "??1exception@std@@UEAA@XZ"
	.seh_handler __CxxFrameHandler3, @unwind, @except
# %bb.0:
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movq	$-2, -8(%rbp)
	movq	%rcx, -16(%rbp)
	movq	-16(%rbp), %rcx
	leaq	"??_7exception@std@@6B@"(%rip), %rax
	movq	%rax, (%rcx)
	addq	$8, %rcx
.Ltmp10:                                # EH_LABEL
	callq	__std_exception_destroy
	nop
.Ltmp11:                                # EH_LABEL
	jmp	.LBB12_1
.LBB12_1:
	.seh_startepilogue
	addq	$48, %rsp
	popq	%rbp
	.seh_endepilogue
	retq
	.seh_handlerdata
	.long	"$cppxdata$??1exception@std@@UEAA@XZ"@IMGREL
	.section	.text,"xr",discard,"??1exception@std@@UEAA@XZ"
	.seh_endproc
	.def	"?dtor$2@?0???1exception@std@@UEAA@XZ@4HA";
	.scl	3;
	.type	32;
	.endef
	.p2align	4
"?dtor$2@?0???1exception@std@@UEAA@XZ@4HA":
.seh_proc "?dtor$2@?0???1exception@std@@UEAA@XZ@4HA"
.LBB12_2:
	movq	%rdx, 16(%rsp)
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	.seh_stackalloc 32
	leaq	48(%rdx), %rbp
	.seh_endprologue
	callq	__std_terminate
	int3
.Lfunc_end5:
	.seh_handlerdata
	.section	.text,"xr",discard,"??1exception@std@@UEAA@XZ"
	.seh_endproc
	.section	.xdata,"dr",associative,"??1exception@std@@UEAA@XZ",unique,2
	.p2align	2, 0x0
"$cppxdata$??1exception@std@@UEAA@XZ":
	.long	429065506                       # MagicNumber
	.long	1                               # MaxState
	.long	"$stateUnwindMap$??1exception@std@@UEAA@XZ"@IMGREL # UnwindMap
	.long	0                               # NumTryBlocks
	.long	0                               # TryBlockMap
	.long	3                               # IPMapEntries
	.long	"$ip2state$??1exception@std@@UEAA@XZ"@IMGREL # IPToStateXData
	.long	40                              # UnwindHelp
	.long	0                               # ESTypeList
	.long	1                               # EHFlags
"$stateUnwindMap$??1exception@std@@UEAA@XZ":
	.long	-1                              # ToState
	.long	"?dtor$2@?0???1exception@std@@UEAA@XZ@4HA"@IMGREL # Action
"$ip2state$??1exception@std@@UEAA@XZ":
	.long	.Lfunc_begin5@IMGREL            # IP
	.long	-1                              # ToState
	.long	.Ltmp10@IMGREL                  # IP
	.long	0                               # ToState
	.long	.Ltmp11@IMGREL                  # IP
	.long	-1                              # ToState
	.section	.text,"xr",discard,"??1exception@std@@UEAA@XZ"
                                        # -- End function
	.def	"??_Gruntime_error@std@@UEAAPEAXI@Z";
	.scl	2;
	.type	32;
	.endef
	.section	.text,"xr",discard,"??_Gruntime_error@std@@UEAAPEAXI@Z"
	.globl	"??_Gruntime_error@std@@UEAAPEAXI@Z" # -- Begin function ??_Gruntime_error@std@@UEAAPEAXI@Z
	.p2align	4
"??_Gruntime_error@std@@UEAAPEAXI@Z":   # @"??_Gruntime_error@std@@UEAAPEAXI@Z"
.seh_proc "??_Gruntime_error@std@@UEAAPEAXI@Z"
# %bb.0:
	subq	$72, %rsp
	.seh_stackalloc 72
	.seh_endprologue
	movl	%edx, 60(%rsp)
	movq	%rcx, 48(%rsp)
	movq	48(%rsp), %rcx
	movq	%rcx, 32(%rsp)                  # 8-byte Spill
	movq	%rcx, 64(%rsp)
	movl	60(%rsp), %eax
	movl	%eax, 44(%rsp)                  # 4-byte Spill
	callq	"??1runtime_error@std@@UEAA@XZ"
	movl	44(%rsp), %eax                  # 4-byte Reload
	andl	$1, %eax
	cmpl	$0, %eax
	je	.LBB13_2
# %bb.1:
	movq	32(%rsp), %rcx                  # 8-byte Reload
	movl	$24, %edx
	callq	"??3@YAXPEAX_K@Z"
.LBB13_2:
	movq	64(%rsp), %rax
	.seh_startepilogue
	addq	$72, %rsp
	.seh_endepilogue
	retq
	.seh_endproc
                                        # -- End function
	.section	.bss,"bw",discard,_Avx2WmemEnabledWeakValue
	.globl	_Avx2WmemEnabledWeakValue       # @_Avx2WmemEnabledWeakValue
	.p2align	2, 0x0
_Avx2WmemEnabledWeakValue:
	.long	0                               # 0x0

	.section	.rdata,"dr",discard,"??_C@_04JMHLEDPH@div0?$AA@"
	.globl	"??_C@_04JMHLEDPH@div0?$AA@"    # @"??_C@_04JMHLEDPH@div0?$AA@"
"??_C@_04JMHLEDPH@div0?$AA@":
	.asciz	"div0"

	.section	.data,"dw",discard,"??_R0?AVruntime_error@std@@@8"
	.globl	"??_R0?AVruntime_error@std@@@8" # @"??_R0?AVruntime_error@std@@@8"
	.p2align	4, 0x0
"??_R0?AVruntime_error@std@@@8":
	.quad	"??_7type_info@@6B@"
	.quad	0
	.asciz	".?AVruntime_error@std@@"

	.section	.xdata,"dr",discard,"_CT??_R0?AVruntime_error@std@@@8??0runtime_error@std@@QEAA@AEBV01@@Z24"
	.globl	"_CT??_R0?AVruntime_error@std@@@8??0runtime_error@std@@QEAA@AEBV01@@Z24" # @"_CT??_R0?AVruntime_error@std@@@8??0runtime_error@std@@QEAA@AEBV01@@Z24"
	.p2align	4, 0x0
"_CT??_R0?AVruntime_error@std@@@8??0runtime_error@std@@QEAA@AEBV01@@Z24":
	.long	0                               # 0x0
	.long	"??_R0?AVruntime_error@std@@@8"@IMGREL
	.long	0                               # 0x0
	.long	4294967295                      # 0xffffffff
	.long	0                               # 0x0
	.long	24                              # 0x18
	.long	"??0runtime_error@std@@QEAA@AEBV01@@Z"@IMGREL

	.section	.data,"dw",discard,"??_R0?AVexception@std@@@8"
	.globl	"??_R0?AVexception@std@@@8"     # @"??_R0?AVexception@std@@@8"
	.p2align	4, 0x0
"??_R0?AVexception@std@@@8":
	.quad	"??_7type_info@@6B@"
	.quad	0
	.asciz	".?AVexception@std@@"
	.zero	4

	.section	.xdata,"dr",discard,"_CT??_R0?AVexception@std@@@8??0exception@std@@QEAA@AEBV01@@Z24"
	.globl	"_CT??_R0?AVexception@std@@@8??0exception@std@@QEAA@AEBV01@@Z24" # @"_CT??_R0?AVexception@std@@@8??0exception@std@@QEAA@AEBV01@@Z24"
	.p2align	4, 0x0
"_CT??_R0?AVexception@std@@@8??0exception@std@@QEAA@AEBV01@@Z24":
	.long	0                               # 0x0
	.long	"??_R0?AVexception@std@@@8"@IMGREL
	.long	0                               # 0x0
	.long	4294967295                      # 0xffffffff
	.long	0                               # 0x0
	.long	24                              # 0x18
	.long	"??0exception@std@@QEAA@AEBV01@@Z"@IMGREL

	.section	.xdata,"dr",discard,"_CTA2?AVruntime_error@std@@"
	.globl	"_CTA2?AVruntime_error@std@@"   # @"_CTA2?AVruntime_error@std@@"
	.p2align	3, 0x0
"_CTA2?AVruntime_error@std@@":
	.long	2                               # 0x2
	.long	"_CT??_R0?AVruntime_error@std@@@8??0runtime_error@std@@QEAA@AEBV01@@Z24"@IMGREL
	.long	"_CT??_R0?AVexception@std@@@8??0exception@std@@QEAA@AEBV01@@Z24"@IMGREL

	.section	.xdata,"dr",discard,"_TI2?AVruntime_error@std@@"
	.globl	"_TI2?AVruntime_error@std@@"    # @"_TI2?AVruntime_error@std@@"
	.p2align	3, 0x0
"_TI2?AVruntime_error@std@@":
	.long	0                               # 0x0
	.long	"??1runtime_error@std@@UEAA@XZ"@IMGREL
	.long	0                               # 0x0
	.long	"_CTA2?AVruntime_error@std@@"@IMGREL

	.section	.data,"dw",discard,"??_R0H@8"
	.globl	"??_R0H@8"                      # @"??_R0H@8"
	.p2align	4, 0x0
"??_R0H@8":
	.quad	"??_7type_info@@6B@"
	.quad	0
	.asciz	".H"
	.zero	5

	.section	.xdata,"dr",discard,"_CT??_R0H@84"
	.globl	"_CT??_R0H@84"                  # @"_CT??_R0H@84"
	.p2align	4, 0x0
"_CT??_R0H@84":
	.long	1                               # 0x1
	.long	"??_R0H@8"@IMGREL
	.long	0                               # 0x0
	.long	4294967295                      # 0xffffffff
	.long	0                               # 0x0
	.long	4                               # 0x4
	.long	0                               # 0x0

	.section	.xdata,"dr",discard,_CTA1H
	.globl	_CTA1H                          # @_CTA1H
	.p2align	3, 0x0
_CTA1H:
	.long	1                               # 0x1
	.long	"_CT??_R0H@84"@IMGREL

	.section	.xdata,"dr",discard,_TI1H
	.globl	_TI1H                           # @_TI1H
	.p2align	3, 0x0
_TI1H:
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	_CTA1H@IMGREL

	.section	.rdata,"dr",largest,"??_7runtime_error@std@@6B@"
	.p2align	4, 0x0                          # @0
.L__unnamed_1:
	.quad	"??_R4runtime_error@std@@6B@"
	.quad	"??_Eruntime_error@std@@UEAAPEAXI@Z"
	.quad	"?what@exception@std@@UEBAPEBDXZ"

	.section	.rdata,"dr",discard,"??_R4runtime_error@std@@6B@"
	.globl	"??_R4runtime_error@std@@6B@"   # @"??_R4runtime_error@std@@6B@"
	.p2align	4, 0x0
"??_R4runtime_error@std@@6B@":
	.long	1                               # 0x1
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	"??_R0?AVruntime_error@std@@@8"@IMGREL
	.long	"??_R3runtime_error@std@@8"@IMGREL
	.long	"??_R4runtime_error@std@@6B@"@IMGREL

	.section	.rdata,"dr",discard,"??_R3runtime_error@std@@8"
	.globl	"??_R3runtime_error@std@@8"     # @"??_R3runtime_error@std@@8"
	.p2align	3, 0x0
"??_R3runtime_error@std@@8":
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	2                               # 0x2
	.long	"??_R2runtime_error@std@@8"@IMGREL

	.section	.rdata,"dr",discard,"??_R2runtime_error@std@@8"
	.globl	"??_R2runtime_error@std@@8"     # @"??_R2runtime_error@std@@8"
	.p2align	2, 0x0
"??_R2runtime_error@std@@8":
	.long	"??_R1A@?0A@EA@runtime_error@std@@8"@IMGREL
	.long	"??_R1A@?0A@EA@exception@std@@8"@IMGREL
	.long	0                               # 0x0

	.section	.rdata,"dr",discard,"??_R1A@?0A@EA@runtime_error@std@@8"
	.globl	"??_R1A@?0A@EA@runtime_error@std@@8" # @"??_R1A@?0A@EA@runtime_error@std@@8"
	.p2align	4, 0x0
"??_R1A@?0A@EA@runtime_error@std@@8":
	.long	"??_R0?AVruntime_error@std@@@8"@IMGREL
	.long	1                               # 0x1
	.long	0                               # 0x0
	.long	4294967295                      # 0xffffffff
	.long	0                               # 0x0
	.long	64                              # 0x40
	.long	"??_R3runtime_error@std@@8"@IMGREL

	.section	.rdata,"dr",discard,"??_R1A@?0A@EA@exception@std@@8"
	.globl	"??_R1A@?0A@EA@exception@std@@8" # @"??_R1A@?0A@EA@exception@std@@8"
	.p2align	4, 0x0
"??_R1A@?0A@EA@exception@std@@8":
	.long	"??_R0?AVexception@std@@@8"@IMGREL
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	4294967295                      # 0xffffffff
	.long	0                               # 0x0
	.long	64                              # 0x40
	.long	"??_R3exception@std@@8"@IMGREL

	.section	.rdata,"dr",discard,"??_R3exception@std@@8"
	.globl	"??_R3exception@std@@8"         # @"??_R3exception@std@@8"
	.p2align	3, 0x0
"??_R3exception@std@@8":
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	1                               # 0x1
	.long	"??_R2exception@std@@8"@IMGREL

	.section	.rdata,"dr",discard,"??_R2exception@std@@8"
	.globl	"??_R2exception@std@@8"         # @"??_R2exception@std@@8"
	.p2align	2, 0x0
"??_R2exception@std@@8":
	.long	"??_R1A@?0A@EA@exception@std@@8"@IMGREL
	.long	0                               # 0x0

	.section	.rdata,"dr",largest,"??_7exception@std@@6B@"
	.p2align	4, 0x0                          # @1
.L__unnamed_2:
	.quad	"??_R4exception@std@@6B@"
	.quad	"??_Eexception@std@@UEAAPEAXI@Z"
	.quad	"?what@exception@std@@UEBAPEBDXZ"

	.section	.rdata,"dr",discard,"??_R4exception@std@@6B@"
	.globl	"??_R4exception@std@@6B@"       # @"??_R4exception@std@@6B@"
	.p2align	4, 0x0
"??_R4exception@std@@6B@":
	.long	1                               # 0x1
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	"??_R0?AVexception@std@@@8"@IMGREL
	.long	"??_R3exception@std@@8"@IMGREL
	.long	"??_R4exception@std@@6B@"@IMGREL

	.section	.rdata,"dr",discard,"??_C@_0BC@EOODALEL@Unknown?5exception?$AA@"
	.globl	"??_C@_0BC@EOODALEL@Unknown?5exception?$AA@" # @"??_C@_0BC@EOODALEL@Unknown?5exception?$AA@"
"??_C@_0BC@EOODALEL@Unknown?5exception?$AA@":
	.asciz	"Unknown exception"

	.section	.drectve,"yni"
	.ascii	" /FAILIFMISMATCH:\"_MSC_VER=1900\""
	.ascii	" /FAILIFMISMATCH:\"_ITERATOR_DEBUG_LEVEL=0\""
	.ascii	" /FAILIFMISMATCH:\"RuntimeLibrary=MT_StaticRelease\""
	.ascii	" /DEFAULTLIB:libcpmt.lib"
	.ascii	" /FAILIFMISMATCH:\"annotate_string=0\""
	.ascii	" /FAILIFMISMATCH:\"annotate_vector=0\""
	.ascii	" /FAILIFMISMATCH:\"annotate_optional=0\""
	.ascii	" /FAILIFMISMATCH:\"_CRT_STDIO_ISO_WIDE_SPECIFIERS=0\""
	.ascii	" /alternatename:_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue"
	.globl	"??_7runtime_error@std@@6B@"
"??_7runtime_error@std@@6B@" = .L__unnamed_1+8
	.globl	"??_7exception@std@@6B@"
"??_7exception@std@@6B@" = .L__unnamed_2+8
	.weak	"??_Eexception@std@@UEAAPEAXI@Z"
	.def	"??_Eexception@std@@UEAAPEAXI@Z";
	.scl	2;
	.type	32;
	.endef
"??_Eexception@std@@UEAAPEAXI@Z" = "??_Gexception@std@@UEAAPEAXI@Z"
	.weak	"??_Eruntime_error@std@@UEAAPEAXI@Z"
	.def	"??_Eruntime_error@std@@UEAAPEAXI@Z";
	.scl	2;
	.type	32;
	.endef
"??_Eruntime_error@std@@UEAAPEAXI@Z" = "??_Gruntime_error@std@@UEAAPEAXI@Z"
	.section	.debug$S,"dr"
	.p2align	2, 0x0
	.long	4                               # Debug section magic
	.long	241
	.long	.Ltmp13-.Ltmp12                 # Subsection size
.Ltmp12:
	.short	.Ltmp15-.Ltmp14                 # Record length
.Ltmp14:
	.short	4353                            # Record kind: S_OBJNAME
	.long	0                               # Signature
	.byte	0                               # Object name
	.p2align	2, 0x0
.Ltmp15:
	.short	.Ltmp17-.Ltmp16                 # Record length
.Ltmp16:
	.short	4412                            # Record kind: S_COMPILE3
	.long	1                               # Flags and language
	.short	208                             # CPUType
	.short	22                              # Frontend version
	.short	1
	.short	1
	.short	0
	.short	22011                           # Backend version
	.short	0
	.short	0
	.short	0
	.asciz	"clang version 22.1.1 (https://github.com/llvm/llvm-project fef02d48c08db859ef83f84232ed78bd9d1c323a)" # Null-terminated compiler version string
	.p2align	2, 0x0
.Ltmp17:
.Ltmp13:
	.p2align	2, 0x0
	.addrsig
	.addrsig_sym "?checked_divide@abi@@YAHHH@Z"
	.addrsig_sym _CxxThrowException
	.addrsig_sym __CxxFrameHandler3
	.addrsig_sym "?throw_int@abi@@YAHXZ"
	.addrsig_sym __std_exception_copy
	.addrsig_sym __std_terminate
	.addrsig_sym "??3@YAXPEAX_K@Z"
	.addrsig_sym __std_exception_destroy
	.addrsig_sym "??_7type_info@@6B@"
	.addrsig_sym "??_R0?AVruntime_error@std@@@8"
	.addrsig_sym __ImageBase
	.addrsig_sym "??_R0?AVexception@std@@@8"
	.addrsig_sym "??_R0H@8"
	.addrsig_sym "??_R4runtime_error@std@@6B@"
	.addrsig_sym "??_R3runtime_error@std@@8"
	.addrsig_sym "??_R2runtime_error@std@@8"
	.addrsig_sym "??_R1A@?0A@EA@runtime_error@std@@8"
	.addrsig_sym "??_R1A@?0A@EA@exception@std@@8"
	.addrsig_sym "??_R3exception@std@@8"
	.addrsig_sym "??_R2exception@std@@8"
	.addrsig_sym "??_R4exception@std@@6B@"
