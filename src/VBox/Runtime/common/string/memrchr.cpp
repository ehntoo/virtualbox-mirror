/* $Id$ */
/** @file
 * IPRT - CRT Strings, memrchr().
 */

/*
 * Copyright (C) 2018-2022 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL), a copy of it is provided in the "COPYING.CDDL" file included
 * in the VirtualBox distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include "internal/iprt.h"
#include <iprt/string.h>


/**
 * Search for a given byte starting at the end of the block.
 *
 * @returns Pointer on a match or NULL otherwise.
 * @param   pv      Pointer to the block.
 * @param   ch      The char to search for.
 * @param   cb      The size of the block.
 */
#undef memrchr
void *RT_NOCRT(memrchr)(const void *pv, int ch, size_t cb)
{
    if (cb)
    {
        const char *pbCur = (const char *)pv + cb - 1;

        while (cb)
        {
            if (*pbCur == ch)
                return (void *)pbCur;
            pbCur--;
            cb--;
        }
    }

    return NULL;
}
RT_ALIAS_AND_EXPORT_NOCRT_SYMBOL(memrchr);

