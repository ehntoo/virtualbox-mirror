/** @file
 * VirtualBox X11 Additions graphics driver utility functions
 */

/*
 * Copyright (C) 2006-2007 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include <VBox/VMMDev.h>
#include <VBox/VBoxGuestLib.h>

#ifndef PCIACCESS
# include <xf86Pci.h>
# include <Pci.h>
#endif

#include "xf86.h"
#define NEED_XF86_TYPES
#include <iprt/string.h>
#include "compiler.h"
#include "cursorstr.h"

#include "vboxvideo.h"

#define VBOX_MAX_CURSOR_WIDTH 64
#define VBOX_MAX_CURSOR_HEIGHT 64

/**************************************************************************
* Debugging functions and macros                                          *
**************************************************************************/

/* #define DEBUG_POINTER */

#ifdef DEBUG
# define PUT_PIXEL(c) ErrorF ("%c", c)
#else /* DEBUG_VIDEO not defined */
# define PUT_PIXEL(c) do { } while(0)
#endif /* DEBUG_VIDEO not defined */

/** Macro to printf an error message and return from a function */
#define RETERROR(scrnIndex, RetVal, ...) \
    do \
    { \
        xf86DrvMsg(scrnIndex, X_ERROR, __VA_ARGS__); \
        return RetVal; \
    } \
    while (0)

/** Structure to pass cursor image data between realise_cursor() and
 * load_cursor_image().  The members match the parameters to
 * @a VBoxHGSMIUpdatePointerShape(). */
struct vboxCursorImage
{
    uint32_t fFlags;
    uint32_t cHotX;
    uint32_t cHotY;
    uint32_t cWidth;
    uint32_t cHeight;
    uint8_t *pPixels;
    uint32_t cbLength;
};

#ifdef DEBUG_POINTER
static void
vbox_show_shape(unsigned short w, unsigned short h, CARD32 bg, unsigned char *image)
{
    size_t x, y;
    unsigned short pitch;
    CARD32 *color;
    unsigned char *mask;
    size_t sizeMask;

    image    += sizeof(struct vboxCursorImage);
    mask      = image;
    pitch     = (w + 7) / 8;
    sizeMask  = (pitch * h + 3) & ~3;
    color     = (CARD32 *)(image + sizeMask);

    TRACE_ENTRY();
    for (y = 0; y < h; ++y, mask += pitch, color += w)
    {
        for (x = 0; x < w; ++x)
        {
            if (mask[x / 8] & (1 << (7 - (x % 8))))
                ErrorF (" ");
            else
            {
                CARD32 c = color[x];
                if (c == bg)
                    ErrorF("Y");
                else
                    ErrorF("X");
            }
        }
        ErrorF("\n");
    }
}
#endif

/**************************************************************************
* Helper functions and macros                                             *
**************************************************************************/

/* This is called by the X server every time it loads a new cursor to see
 * whether our "cursor hardware" can handle the cursor.  This provides us with
 * a mechanism (the only one!) to switch back from a software to a hardware
 * cursor. */
static Bool
vbox_host_uses_hwcursor(ScrnInfoPtr pScrn)
{
    Bool rc = TRUE;
    uint32_t fFeatures = 0;
    VBOXPtr pVBox = pScrn->driverPrivate;

    /* We may want to force the use of a software cursor.  Currently this is
     * needed if the guest uses a large virtual resolution, as in this case
     * the host and guest tend to disagree about the pointer location. */
    if (pVBox->forceSWCursor)
        rc = FALSE;
    /* Query information about mouse integration from the host. */
    if (rc) {
        int vrc = VbglR3GetMouseStatus(&fFeatures, NULL, NULL);
        if (RT_FAILURE(vrc)) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                     "Unable to determine whether the virtual machine supports mouse pointer integration - request initialization failed with return code %d\n", vrc);
            rc = FALSE;
        }
    }
    /* If we got the information from the host then make sure the host wants
     * to draw the pointer. */
    if (rc)
    {
        if (   (fFeatures & VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE)
#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) >= 5
                /* As of this version (server 1.6) all major Linux releases
                 * are known to handle USB tablets correctly. */
            || (fFeatures & VMMDEV_MOUSE_HOST_HAS_ABS_DEV)
#endif
            )
            /* Assume this will never be unloaded as long as the X session is
             * running. */
            pVBox->guestCanAbsolute = TRUE;
        if (   (fFeatures & VMMDEV_MOUSE_HOST_CANNOT_HWPOINTER)
            || !pVBox->guestCanAbsolute
            || !(fFeatures & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE)
           )
            rc = FALSE;
    }
    return rc;
}

/**************************************************************************
* Main functions                                                          *
**************************************************************************/

void
vbox_close(ScrnInfoPtr pScrn, VBOXPtr pVBox)
{
    TRACE_ENTRY();

    xf86DestroyCursorInfoRec(pVBox->pCurs);
    pVBox->pCurs = NULL;
    TRACE_EXIT();
}

Bool
vbox_init(int scrnIndex, VBOXPtr pVBox)
{
    Bool rc = TRUE;
    int vrc;
    uint32_t fMouseFeatures = 0;

    TRACE_ENTRY();
    vrc = VbglR3Init();
    if (RT_FAILURE(vrc))
    {
        xf86DrvMsg(scrnIndex, X_ERROR,
                   "Failed to initialize the VirtualBox device (rc=%d) - make sure that the VirtualBox guest additions are properly installed.  If you are not sure, try reinstalling them.  The X Window graphics drivers will run in compatibility mode.\n",
                   vrc);
        rc = FALSE;
    }
    pVBox->useDevice = rc;
    return rc;
}

static void
vbox_vmm_hide_cursor(ScrnInfoPtr pScrn, VBOXPtr pVBox)
{
    int rc;

    rc = VBoxHGSMIUpdatePointerShape(&pVBox->guestCtx, 0, 0, 0, 0, 0, NULL, 0);
    if (RT_FAILURE(rc))
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Could not hide the virtual mouse pointer, VBox error %d.\n", rc);
        /* Play safe, and disable the hardware cursor until the next mode
         * switch, since obviously something happened that we didn't
         * anticipate. */
        pVBox->forceSWCursor = TRUE;
    }
}

static void
vbox_vmm_show_cursor(ScrnInfoPtr pScrn, VBOXPtr pVBox)
{
    int rc;

    if (!vbox_host_uses_hwcursor(pScrn))
        return;
    rc = VBoxHGSMIUpdatePointerShape(&pVBox->guestCtx, VBOX_MOUSE_POINTER_VISIBLE,
                                     0, 0, 0, 0, NULL, 0);
    if (RT_FAILURE(rc)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Could not unhide the virtual mouse pointer.\n");
        /* Play safe, and disable the hardware cursor until the next mode
         * switch, since obviously something happened that we didn't
         * anticipate. */
        pVBox->forceSWCursor = TRUE;
    }
}

static void
vbox_vmm_load_cursor_image(ScrnInfoPtr pScrn, VBOXPtr pVBox,
                           unsigned char *pvImage)
{
    int rc;
    struct vboxCursorImage *pImage;
    pImage = (struct vboxCursorImage *)pvImage;

#ifdef DEBUG_POINTER
    vbox_show_shape(pImage->cWidth, pImage->cHeight, 0, pvImage);
#endif

    rc = VBoxHGSMIUpdatePointerShape(&pVBox->guestCtx, pImage->fFlags,
             pImage->cHotX, pImage->cHotY, pImage->cWidth, pImage->cHeight,
             pImage->pPixels, pImage->cbLength);
    if (RT_FAILURE(rc)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,  "Unable to set the virtual mouse pointer image.\n");
        /* Play safe, and disable the hardware cursor until the next mode
         * switch, since obviously something happened that we didn't
         * anticipate. */
        pVBox->forceSWCursor = TRUE;
    }
}

static void
vbox_set_cursor_colors(ScrnInfoPtr pScrn, int bg, int fg)
{
    NOREF(pScrn);
    NOREF(bg);
    NOREF(fg);
    /* ErrorF("vbox_set_cursor_colors NOT IMPLEMENTED\n"); */
}


static void
vbox_set_cursor_position(ScrnInfoPtr pScrn, int x, int y)
{
    /* Nothing to do here, as we are telling the guest where the mouse is,
     * not vice versa. */
    NOREF(pScrn);
    NOREF(x);
    NOREF(y);
}

static void
vbox_hide_cursor(ScrnInfoPtr pScrn)
{
    VBOXPtr pVBox = pScrn->driverPrivate;

    vbox_vmm_hide_cursor(pScrn, pVBox);
}

static void
vbox_show_cursor(ScrnInfoPtr pScrn)
{
    VBOXPtr pVBox = pScrn->driverPrivate;

    vbox_vmm_show_cursor(pScrn, pVBox);
}

static void
vbox_load_cursor_image(ScrnInfoPtr pScrn, unsigned char *image)
{
    VBOXPtr pVBox = pScrn->driverPrivate;

    vbox_vmm_load_cursor_image(pScrn, pVBox, image);
}

static Bool
vbox_use_hw_cursor(ScreenPtr pScreen, CursorPtr pCurs)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    return vbox_host_uses_hwcursor(pScrn);
}

static unsigned char
color_to_byte(unsigned c)
{
    return (c >> 8) & 0xff;
}

static unsigned char *
vbox_realize_cursor(xf86CursorInfoPtr infoPtr, CursorPtr pCurs)
{
    VBOXPtr pVBox;
    CursorBitsPtr bitsp;
    unsigned short w, h, x, y;
    unsigned char *c, *p, *pm, *ps, *m;
    size_t sizeRequest, sizeRgba, sizeMask, srcPitch, dstPitch;
    CARD32 fc, bc, *cp;
    int rc, scrnIndex = infoPtr->pScrn->scrnIndex;
    struct vboxCursorImage *pImage;

    pVBox = infoPtr->pScrn->driverPrivate;
    bitsp = pCurs->bits;
    w = bitsp->width;
    h = bitsp->height;

    if (!w || !h || w > VBOX_MAX_CURSOR_WIDTH || h > VBOX_MAX_CURSOR_HEIGHT)
        RETERROR(scrnIndex, NULL,
            "Error invalid cursor dimensions %dx%d\n", w, h);

    if ((bitsp->xhot > w) || (bitsp->yhot > h))
        RETERROR(scrnIndex, NULL,
            "Error invalid cursor hotspot location %dx%d (max %dx%d)\n",
            bitsp->xhot, bitsp->yhot, w, h);

    srcPitch = PixmapBytePad (bitsp->width, 1);
    dstPitch = (w + 7) / 8;
    sizeMask = ((dstPitch * h) + 3) & (size_t) ~3;
    sizeRgba = w * h * 4;
    sizeRequest = sizeMask + sizeRgba + sizeof(*pImage);

    p = c = calloc (1, sizeRequest);
    if (!c)
        RETERROR(scrnIndex, NULL,
                 "Error failed to alloc %lu bytes for cursor\n",
                 (unsigned long) sizeRequest);

    pImage = (struct vboxCursorImage *)p;
    pImage->pPixels = m = p + sizeof(*pImage);
    cp = (CARD32 *)(m + sizeMask);

    TRACE_LOG ("w=%d h=%d sm=%d sr=%d p=%d\n",
           w, h, (int) sizeMask, (int) sizeRgba, (int) dstPitch);
    TRACE_LOG ("m=%p c=%p cp=%p\n", m, c, (void *)cp);

    fc = color_to_byte (pCurs->foreBlue)
      | (color_to_byte (pCurs->foreGreen) << 8)
      | (color_to_byte (pCurs->foreRed)   << 16);

    bc = color_to_byte (pCurs->backBlue)
      | (color_to_byte (pCurs->backGreen) << 8)
      | (color_to_byte (pCurs->backRed)   << 16);

    /*
     * Convert the Xorg source/mask bits to the and/xor bits VBox needs.
     * Xorg:
     *   The mask is a bitmap indicating which parts of the cursor are
     *   transparent and which parts are drawn. The source is a bitmap
     *   indicating which parts of the non-transparent portion of the
     *   the cursor should be painted in the foreground color and which
     *   should be painted in the background color. By default, set bits
     *   indicate the opaque part of the mask bitmap and clear bits
     *   indicate the transparent part.
     * VBox:
     *   The color data is the XOR mask. The AND mask bits determine
     *   which pixels of the color data (XOR mask) will replace (overwrite)
     *   the screen pixels (AND mask bit = 0) and which ones will be XORed
     *   with existing screen pixels (AND mask bit = 1).
     *   For example when you have the AND mask all 0, then you see the
     *   correct mouse pointer image surrounded by black square.
     */
    for (pm = bitsp->mask, ps = bitsp->source, y = 0;
         y < h;
         ++y, pm += srcPitch, ps += srcPitch, m += dstPitch)
    {
        for (x = 0; x < w; ++x)
        {
            if (pm[x / 8] & (1 << (x % 8)))
            {
                /* opaque, leave AND mask bit at 0 */
                if (ps[x / 8] & (1 << (x % 8)))
                {
                    *cp++ = fc;
                    PUT_PIXEL('X');
                }
                else
                {
                    *cp++ = bc;
                    PUT_PIXEL('*');
                }
            }
            else
            {
                /* transparent, set AND mask bit */
                m[x / 8] |= 1 << (7 - (x % 8));
                /* don't change the screen pixel */
                *cp++ = 0;
                PUT_PIXEL(' ');
            }
        }
        PUT_PIXEL('\n');
    }

    pImage->cWidth   = w;
    pImage->cHeight  = h;
    pImage->cHotX    = bitsp->xhot;
    pImage->cHotY    = bitsp->yhot;
    pImage->fFlags   = VBOX_MOUSE_POINTER_VISIBLE | VBOX_MOUSE_POINTER_SHAPE;
    pImage->cbLength = sizeRequest - sizeof(*pImage);

#ifdef DEBUG_POINTER
    ErrorF("shape = %p\n", p);
    vbox_show_shape(w, h, bc, c);
#endif

    return p;
}

#ifdef ARGB_CURSOR
static Bool
vbox_use_hw_cursor_argb(ScreenPtr pScreen, CursorPtr pCurs)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    Bool rc = TRUE;

    if (!vbox_host_uses_hwcursor(pScrn))
        rc = FALSE;
    if (   rc
        && (   (pCurs->bits->height > VBOX_MAX_CURSOR_HEIGHT)
            || (pCurs->bits->width > VBOX_MAX_CURSOR_WIDTH)
            || (pScrn->bitsPerPixel <= 8)
           )
       )
        rc = FALSE;
#ifndef VBOXVIDEO_13
    /* Evil hack - we use this as another way of poking the driver to update
     * our list of video modes. */
    vboxWriteHostModes(pScrn, pScrn->currentMode);
#endif
    return rc;
}


static void
vbox_load_cursor_argb(ScrnInfoPtr pScrn, CursorPtr pCurs)
{
    VBOXPtr pVBox;
    VMMDevReqMousePointer *reqp;
    CursorBitsPtr bitsp;
    unsigned short w, h;
    unsigned short cx, cy;
    unsigned char *pm;
    CARD32 *pc;
    size_t sizeData, sizeMask;
    CARD8 *p;
    int scrnIndex;
    uint32_t fFlags =   VBOX_MOUSE_POINTER_VISIBLE | VBOX_MOUSE_POINTER_SHAPE
                      | VBOX_MOUSE_POINTER_ALPHA;
    int rc;

    pVBox = pScrn->driverPrivate;
    bitsp = pCurs->bits;
    w     = bitsp->width;
    h     = bitsp->height;
    scrnIndex = pScrn->scrnIndex;

    /* Mask must be generated for alpha cursors, that is required by VBox. */
    /* note: (michael) the next struct must be 32bit aligned. */
    sizeMask  = ((w + 7) / 8 * h + 3) & ~3;

    if (!w || !h || w > VBOX_MAX_CURSOR_WIDTH || h > VBOX_MAX_CURSOR_HEIGHT)
        RETERROR(scrnIndex, ,
                 "Error invalid cursor dimensions %dx%d\n", w, h);

    if ((bitsp->xhot > w) || (bitsp->yhot > h))
        RETERROR(scrnIndex, ,
                 "Error invalid cursor hotspot location %dx%d (max %dx%d)\n",
                 bitsp->xhot, bitsp->yhot, w, h);

    sizeData = w * h * 4 + sizeMask;
    p = calloc(1, sizeData);
    if (!p)
        RETERROR(scrnIndex, ,
                 "Error failed to alloc %lu bytes for cursor\n",
                 (unsigned long)sizeData);

    memcpy(p + sizeMask, bitsp->argb, w * h * 4);

    /* Emulate the AND mask. */
    pm = p;
    pc = bitsp->argb;

    /* Init AND mask to 1 */
    memset(pm, 0xFF, sizeMask);

    /*
     * The additions driver must provide the AND mask for alpha cursors. The host frontend
     * which can handle alpha channel, will ignore the AND mask and draw an alpha cursor.
     * But if the host does not support ARGB, then it simply uses the AND mask and the color
     * data to draw a normal color cursor.
     */
    for (cy = 0; cy < h; cy++)
    {
        unsigned char bitmask = 0x80;

        for (cx = 0; cx < w; cx++, bitmask >>= 1)
        {
            if (bitmask == 0)
                bitmask = 0x80;

            if (pc[cx] >= 0xF0000000)
                pm[cx / 8] &= ~bitmask;
        }

        /* Point to next source and dest scans */
        pc += w;
        pm += (w + 7) / 8;
    }

    rc = VBoxHGSMIUpdatePointerShape(&pVBox->guestCtx, fFlags, bitsp->xhot,
                                     bitsp->yhot, w, h, p, sizeData);
    free(p);
}
#endif

Bool
vbox_cursor_init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    VBOXPtr pVBox = pScrn->driverPrivate;
    xf86CursorInfoPtr pCurs = NULL;
    Bool rc = TRUE;

    TRACE_ENTRY();
    if (!pVBox->fHaveHGSMI)
        return FALSE;
    pVBox->pCurs = pCurs = xf86CreateCursorInfoRec();
    if (!pCurs) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create X Window cursor information structures for virtual mouse.\n");
        rc = FALSE;
    }
    if (rc) {
        pCurs->MaxWidth = VBOX_MAX_CURSOR_WIDTH;
        pCurs->MaxHeight = VBOX_MAX_CURSOR_HEIGHT;
        pCurs->Flags =   HARDWARE_CURSOR_TRUECOLOR_AT_8BPP
                       | HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_1
                       | HARDWARE_CURSOR_BIT_ORDER_MSBFIRST;

        pCurs->SetCursorColors   = vbox_set_cursor_colors;
        pCurs->SetCursorPosition = vbox_set_cursor_position;
        pCurs->LoadCursorImage   = vbox_load_cursor_image;
        pCurs->HideCursor        = vbox_hide_cursor;
        pCurs->ShowCursor        = vbox_show_cursor;
        pCurs->UseHWCursor       = vbox_use_hw_cursor;
        pCurs->RealizeCursor     = vbox_realize_cursor;

#ifdef ARGB_CURSOR
        pCurs->UseHWCursorARGB   = vbox_use_hw_cursor_argb;
        pCurs->LoadCursorARGB    = vbox_load_cursor_argb;
#endif

        /* Hide the host cursor before we initialise if we wish to use a
         * software cursor. */
        if (pVBox->forceSWCursor)
            vbox_vmm_hide_cursor(pScrn, pVBox);
        rc = xf86InitCursor(pScreen, pCurs);
    }
    if (!rc)
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to enable mouse pointer integration.\n");
    if (!rc && (pCurs != NULL))
        xf86DestroyCursorInfoRec(pCurs);
    return rc;
}
