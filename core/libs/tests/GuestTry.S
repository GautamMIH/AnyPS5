// A guest frame that catches exceptions raised through libc.prx's guest C++ runtime.
//
// On Linux libc.prx serves guest code its own exception runtime (NID exports) and leaves the host
// C++ runtime to host code, so a host try/catch cannot catch what libc throws for guest callers
// (the guest unwinder stops at a frame with the host personality). Tests run the throwing call
// inside this frame instead, which uses the guest personality like relinked game code.
//
// int GuestTry(void* function, uintptr_t a, uintptr_t b, uintptr_t c)
//   Calls function(a, b, c). Returns 0 if it returned, 1 if it threw an exception matching
//   GuestTryCatchType (a std::type_info pointer; null catches everything). The caught object's
//   first eight bytes are stored in GuestTryCaughtWord, and GuestTryInspect, when set, is called
//   with the caught object before the catch ends. Other exceptions keep unwinding.
// Frames between this one and the throw must use the guest personality or none (plain host
// functions without cleanups are fine).
.cfi_sections .eh_frame
.text
.globl GuestTry
.type GuestTry, @function
GuestTry:
    .cfi_startproc
    .cfi_personality 0x9b, .Ltry_personality
    .cfi_lsda 0x1b, .Ltry_lsda
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq %rsp, %rbp
    .cfi_def_cfa_register %rbp
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
.Ltry_call:
    call *%rax
.Ltry_call_end:
    xorl %eax, %eax
    .cfi_remember_state
    popq %rbp
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_restore_state
.Ltry_landing:
    cmpq $1, %rdx
    jne .Ltry_bad
    movq %rax, %rdi
    call __cxa_begin_catch_nid_postfix
    movq (%rax), %rcx
    movq %rcx, GuestTryCaughtWord(%rip)
    movq GuestTryInspect(%rip), %rcx
    testq %rcx, %rcx
    jz .Ltry_caught
    movq %rax, %rdi
    call *%rcx
.Ltry_caught:
    call __cxa_end_catch_nid_postfix
    movl $1, %eax
    popq %rbp
    .cfi_def_cfa %rsp, 8
    ret
.Ltry_bad:
    ud2
    .cfi_endproc
.size GuestTry, .-GuestTry

.data
.p2align 3
.globl GuestTryCatchType
GuestTryCatchType:
    .quad 0
.globl GuestTryCaughtWord
GuestTryCaughtWord:
    .quad 0
.globl GuestTryInspect
GuestTryInspect:
    .quad 0

.section .data.rel.ro,"aw",@progbits
.p2align 3
.Ltry_personality:
    .quad __gxx_personality_v0_nid_postfix

.section .rodata
.Ltry_lsda:
    .byte 0xff
    .byte 0x9b
    .uleb128 .Ltry_types_end-.Ltry_types_offset
.Ltry_types_offset:
    .byte 0x01
    .uleb128 .Ltry_calls_end-.Ltry_calls
.Ltry_calls:
    .uleb128 .Ltry_call-GuestTry
    .uleb128 .Ltry_call_end-.Ltry_call
    .uleb128 .Ltry_landing-GuestTry
    .uleb128 1
.Ltry_calls_end:
    .byte 1, 0
    .long GuestTryCatchType-.
.Ltry_types_end:

.section .note.GNU-stack,"",@progbits
