; MSVC x64 syscall stub for jm::detail::syscall
; int32_t jm_do_syscall(uint32_t id, uint64_t argc, const uint64_t* argv);

PUBLIC jm_do_syscall
.CODE

jm_do_syscall PROC
    push rbx
    push rsi
    push rdi
    sub rsp, 0A0h

    mov eax, ecx
    mov rbx, r8
    mov r11, rdx

    xor r10, r10
    xor rdx, rdx
    xor r8, r8
    xor r9, r9

    test r11, r11
    jz dosys
    mov r10, qword ptr [rbx]
    cmp r11, 1
    jbe dosys
    mov rdx, qword ptr [rbx+8]
    cmp r11, 2
    jbe dosys
    mov r8, qword ptr [rbx+16]
    cmp r11, 3
    jbe dosys
    mov r9, qword ptr [rbx+24]
    cmp r11, 4
    jbe dosys

    mov rcx, r11
    sub rcx, 4
    lea rsi, [rbx+32]
    lea rdi, [rsp+28h]
    rep movsq

dosys:
    syscall

    add rsp, 0A0h
    pop rdi
    pop rsi
    pop rbx
    ret
jm_do_syscall ENDP

END
