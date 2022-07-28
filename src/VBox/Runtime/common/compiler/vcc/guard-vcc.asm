; $Id$
;; @file
; IPRT - Control Flow Guard related Visual C++ support routines.
;

;
; Copyright (C) 2022 Oracle Corporation
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



;*********************************************************************************************************************************
;*      Header Files                                                                                                             *
;*********************************************************************************************************************************
%include "iprt/asmdefs.mac"
%include "iprt/x86.mac"



;*********************************************************************************************************************************
;*  Global Variables                                                                                                             *
;*********************************************************************************************************************************

;
; Points to a NOP icall checker by default.
; The loader will replace this when we start advertising it in the load config.
;
section .0000cfg rdata align=8

GLOBALNAME __guard_check_icall_fptr
        RTCCPTR_DEF     NAME(__guard_check_icall_nop)

%ifdef RT_ARCH_AMD64
GLOBALNAME __guard_dispatch_icall_fptr
        RTCCPTR_DEF     __guard_dispatch_icall_nop

; xfg stuff (haven't seen it generated or used yet).
GLOBALNAME __guard_xfg_check_icall_fptr
        RTCCPTR_DEF     __guard_check_icall_nop

GLOBALNAME __guard_xfg_dispatch_icall_fptr
        RTCCPTR_DEF     __guard_xfg_dispatch_icall_nop

GLOBALNAME __guard_xfg_table_dispatch_icall_fptr
        RTCCPTR_DEF     __guard_xfg_dispatch_icall_nop

%endif


BEGINCODE
;;
; Check that doesn't do anything.
;
; This is for older windows versions which doesn't support call flow guard stuff.
;
BEGINPROC __guard_check_icall_nop
        ret
ENDPROC   __guard_check_icall_nop

%ifdef RT_ARCH_AMD64
;;
; Target function in RAX
;
; This is for older windows versions which doesn't support call flow guard stuff.
;
BEGINPROC __guard_dispatch_icall_nop
        jmp     rax
ENDPROC   __guard_dispatch_icall_nop
%endif

%ifdef RT_ARCH_AMD64
;;
; Target function in RAX
;
; This is for windows versions which doesn't support extended call flow guard stuff.
;
BEGINPROC __guard_xfg_dispatch_icall_nop
        jmp     [__guard_dispatch_icall_nop wrt RIP]
ENDPROC   __guard_xfg_dispatch_icall_nop
%endif

