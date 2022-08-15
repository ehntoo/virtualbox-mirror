; $Id$
;; @file
; IPRT - No-CRT ldexpf - AMD64 & X86.
;

;
; Copyright (C) 2006-2022 Oracle Corporation
;
; This file is part of VirtualBox Open Source Edition (OSE), as
; available from http://www.virtualbox.org. This file is free software;
; you can redistribute it and/or modify it under the terms of the GNU
; General Public License (GPL) as published by the Free Software
; Foundation, in version 2 as it comes in the "COPYING" file of the
; VirtualBox OSE distribution. VirtualBox OSE is distributed in the
; hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
;
; The contents of this file may alternatively be used under the terms
; of the Common Development and Distribution License Version 1.0
; (CDDL) only, as it comes in the "COPYING.CDDL" file of the
; VirtualBox OSE distribution, in which case the provisions of the
; CDDL are applicable instead of those of the GPL.
;
; You may elect to license modified versions of this file under the
; terms and conditions of either the GPL or the CDDL or both.
;


%define RT_ASM_WITH_SEH64
%include "iprt/asmdefs.mac"


BEGINCODE

;;
; Computes r32 * 2^exp
; @returns st(0) / xmm0
; @param    r32     [rbp + xCB*2] / xmm0
; @param    exp     [ebp + 0ch]  gcc:edi  msc:edx
RT_NOCRT_BEGINPROC ldexpf
        push    xBP
        SEH64_PUSH_xBP
        mov     xBP, xSP
        SEH64_SET_FRAME_xBP 0
        sub     xSP, 10h
        SEH64_ALLOCATE_STACK 10h
        SEH64_END_PROLOGUE

        ;
        ; Load the value and scaling factor.
        ;
%ifdef RT_ARCH_AMD64
 %ifdef ASM_CALL64_GCC
        mov     [rbp - 8h], edi
 %else
        mov     [rbp - 8h], edx
 %endif
        fild    dword [rbp - 8h]
        movss   [rbp - 10h], xmm0
        fld     dword [rbp - 10h]
%else
        fild    dword [eBP + xCB*2 + 4]
        fld     dword [xBP + xCB*2]
%endif

        ;
        ; Do the scaling and return the result.
        ;
        fscale

        fstp    st1
%ifdef RT_ARCH_AMD64
        fstp    dword [rbp - 10h]
        movss   xmm0, [rbp - 10h]
%endif

        leave
        ret
ENDPROC   RT_NOCRT(ldexpf)

