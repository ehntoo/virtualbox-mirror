; $Id$
;; @file
; IPRT - No-CRT ceilf - AMD64 & X86.
;

;
; Copyright (C) 2006-2022 Oracle and/or its affiliates.
;
; This file is part of VirtualBox base platform packages, as
; available from https://www.virtualbox.org.
;
; This program is free software; you can redistribute it and/or
; modify it under the terms of the GNU General Public License
; as published by the Free Software Foundation, in version 3 of the
; License.
;
; This program is distributed in the hope that it will be useful, but
; WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
; General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, see <https://www.gnu.org/licenses>.
;
; The contents of this file may alternatively be used under the terms
; of the Common Development and Distribution License Version 1.0
; (CDDL), a copy of it is provided in the "COPYING.CDDL" file included
; in the VirtualBox distribution, in which case the provisions of the
; CDDL are applicable instead of those of the GPL.
;
; You may elect to license modified versions of this file under the
; terms and conditions of either the GPL or the CDDL or both.
;
; SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
;


%include "iprt/asmdefs.mac"

BEGINCODE

;;
; Compute the smallest integral value not less than r32.
; @returns st(0) / xmm0
; @param    r32      [rbp + 8] / xmm0
RT_NOCRT_BEGINPROC ceilf
    push    xBP
    mov     xBP, xSP
    sub     xSP, 10h

%ifdef RT_ARCH_AMD64 ;; @todo there is probably some sse instruction for this.
    movss   [xSP], xmm0
    fld     dword [xSP]
%else
    fld     dword [xBP + xCB*2]
%endif

    ; Make it round up by modifying the fpu control word.
    fstcw   [xBP - 10h]
    mov     eax, [xBP - 10h]
    or      eax, 00800h
    and     eax, 0fbffh
    mov     [xBP - 08h], eax
    fldcw   [xBP - 08h]

    ; Round ST(0) to integer.
    frndint

    ; Restore the fpu control word.
    fldcw   [xBP - 10h]

%ifdef RT_ARCH_AMD64
    fstp    dword [xSP]
    movss   xmm0, [xSP]
%endif
    leave
    ret
ENDPROC   RT_NOCRT(ceilf)

