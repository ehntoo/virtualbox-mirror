/* $Id$ */
/** @file
 * Host audio driver - Pulse Audio.
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_HOST_AUDIO
#include <VBox/log.h>
#include <VBox/vmm/pdmaudioifs.h>
#include <VBox/vmm/pdmaudioinline.h>
#include <VBox/vmm/pdmaudiohostenuminline.h>

#include <stdio.h>

#include <iprt/alloc.h>
#include <iprt/mem.h>
#include <iprt/uuid.h>
#include <iprt/semaphore.h>

#include "DrvHostAudioPulseAudioStubsMangling.h"
#include "DrvHostAudioPulseAudioStubs.h"

#include <pulse/pulseaudio.h>
#ifndef PA_STREAM_NOFLAGS
# define PA_STREAM_NOFLAGS  (pa_context_flags_t)0x0000U /* since 0.9.19 */
#endif
#ifndef PA_CONTEXT_NOFLAGS
# define PA_CONTEXT_NOFLAGS (pa_context_flags_t)0x0000U /* since 0.9.19 */
#endif

#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defines                                                                                                                      *
*********************************************************************************************************************************/
/** Max number of errors reported by drvHostAudioPaError per instance.
 * @todo Make this configurable thru driver config. */
#define VBOX_PULSEAUDIO_MAX_LOG_REL_ERRORS  64


/** @name PULSEAUDIOENUMCBFLAGS_XXX
 * @{ */
/** No flags specified. */
#define PULSEAUDIOENUMCBFLAGS_NONE          0
/** (Release) log found devices. */
#define PULSEAUDIOENUMCBFLAGS_LOG           RT_BIT(0)
/** Only do default devices. */
#define PULSEAUDIOENUMCBFLAGS_DEFAULT_ONLY  RT_BIT(1)
/** @} */


/*********************************************************************************************************************************
*   Structures                                                                                                                   *
*********************************************************************************************************************************/
/** Pointer to the instance data for a pulse audio host audio driver. */
typedef struct DRVHOSTPULSEAUDIO *PDRVHOSTPULSEAUDIO;


/**
 * Callback context for the server init context state changed callback.
 */
typedef struct PULSEAUDIOSTATECHGCTX
{
    /** The event semaphore. */
    RTSEMEVENT                  hEvtInit;
    /** The returned context state. */
    pa_context_state_t volatile enmCtxState;
} PULSEAUDIOSTATECHGCTX;
/** Pointer to a server init context state changed callback context. */
typedef PULSEAUDIOSTATECHGCTX *PPULSEAUDIOSTATECHGCTX;


/**
 * Enumeration callback context used by the pfnGetConfig code.
 */
typedef struct PULSEAUDIOENUMCBCTX
{
    /** Pointer to PulseAudio's threaded main loop. */
    pa_threaded_mainloop   *pMainLoop;
    /** Enumeration flags, PULSEAUDIOENUMCBFLAGS_XXX. */
    uint32_t                fFlags;
    /** VBox status code for the operation.
     * The caller sets this to VERR_AUDIO_ENUMERATION_FAILED, the callback never
     * uses that status code. */
    int32_t                 rcEnum;
    /** Name of default sink being used. Must be free'd using RTStrFree(). */
    char                   *pszDefaultSink;
    /** Name of default source being used. Must be free'd using RTStrFree(). */
    char                   *pszDefaultSource;
    /** The device enumeration to fill, NULL if pfnGetConfig context.   */
    PPDMAUDIOHOSTENUM       pDeviceEnum;
} PULSEAUDIOENUMCBCTX;
/** Pointer to an enumeration callback context. */
typedef PULSEAUDIOENUMCBCTX *PPULSEAUDIOENUMCBCTX;


/**
 * Pulse audio device enumeration entry.
 */
typedef struct PULSEAUDIODEVENTRY
{
    /** The part we share with others. */
    PDMAUDIOHOSTDEV         Core;
    /** The pulse audio name.
     * @note Kind of must use fixed size field here as that allows
     *       PDMAudioHostDevDup() and PDMAudioHostEnumCopy() to work. */
    RT_FLEXIBLE_ARRAY_EXTENSION
    char                    szPulseName[RT_FLEXIBLE_ARRAY];
} PULSEAUDIODEVENTRY;
/** Pointer to a pulse audio device enumeration entry. */
typedef PULSEAUDIODEVENTRY *PPULSEAUDIODEVENTRY;


/**
 * Pulse audio stream data.
 */
typedef struct PULSEAUDIOSTREAM
{
    /** The stream's acquired configuration. */
    PPDMAUDIOSTREAMCFG     pCfg;
    /** Pointer to driver instance. */
    PDRVHOSTPULSEAUDIO     pDrv;
    /** Pointer to opaque PulseAudio stream. */
    pa_stream             *pStream;
    /** Pulse sample format and attribute specification. */
    pa_sample_spec         SampleSpec;
    /** Pulse playback and buffer metrics. */
    pa_buffer_attr         BufAttr;
    int                    fOpSuccess;
    /** Pointer to Pulse sample peeking buffer. */
    const uint8_t         *pu8PeekBuf;
    /** Current size (in bytes) of peeking data in
     *  buffer. */
    size_t                 cbPeekBuf;
    /** Our offset (in bytes) in peeking buffer. */
    size_t                 offPeekBuf;
    pa_operation          *pDrainOp;
    /** Number of occurred audio data underflows. */
    uint32_t               cUnderflows;
    /** Current latency (in us). */
    uint64_t               curLatencyUs;
#ifdef LOG_ENABLED
    /** Start time stamp (in us) of stream playback / recording. */
    pa_usec_t              tsStartUs;
    /** Time stamp (in us) when last read from / written to the stream. */
    pa_usec_t              tsLastReadWrittenUs;
#endif
} PULSEAUDIOSTREAM;
/** Pointer to pulse audio stream data. */
typedef PULSEAUDIOSTREAM *PPULSEAUDIOSTREAM;


/**
 * Pulse audio host audio driver instance data.
 * @implements PDMIAUDIOCONNECTOR
 */
typedef struct DRVHOSTPULSEAUDIO
{
    /** Pointer to the driver instance structure. */
    PPDMDRVINS              pDrvIns;
    /** Pointer to PulseAudio's threaded main loop. */
    pa_threaded_mainloop   *pMainLoop;
    /**
     * Pointer to our PulseAudio context.
     * @note We use a pMainLoop in a separate thread (pContext).
     *       So either use callback functions or protect these functions
     *       by pa_threaded_mainloop_lock() / pa_threaded_mainloop_unlock().
     */
    pa_context             *pContext;
    /** Shutdown indicator. */
    volatile bool           fAbortLoop;
    /** Error count for not flooding the release log.
     *  Specify UINT32_MAX for unlimited logging. */
    uint32_t                cLogErrors;
    /** The stream (base) name; needed for distinguishing
     *  streams in the PulseAudio mixer controls if multiple
     *  VMs are running at the same time. */
    char                    szStreamName[64];
    /** Don't want to put this on the stack... */
    PULSEAUDIOSTATECHGCTX   InitStateChgCtx;
    /** Pointer to host audio interface. */
    PDMIHOSTAUDIO           IHostAudio;
} DRVHOSTPULSEAUDIO;



/*
 * Glue to make the code work systems with PulseAudio < 0.9.11.
 */
#if !defined(PA_CONTEXT_IS_GOOD) && PA_API_VERSION < 12 /* 12 = 0.9.11 where PA_STREAM_IS_GOOD was added */
DECLINLINE(bool) PA_CONTEXT_IS_GOOD(pa_context_state_t enmState)
{
    return enmState == PA_CONTEXT_CONNECTING
        || enmState == PA_CONTEXT_AUTHORIZING
        || enmState == PA_CONTEXT_SETTING_NAME
        || enmState == PA_CONTEXT_READY;
}
#endif

#if !defined(PA_STREAM_IS_GOOD) && PA_API_VERSION < 12 /* 12 = 0.9.11 where PA_STREAM_IS_GOOD was added */
DECLINLINE(bool) PA_STREAM_IS_GOOD(pa_stream_state_t enmState)
{
    return enmState == PA_STREAM_CREATING
        || enmState == PA_STREAM_READY;
}
#endif



/** @todo Implement va handling. */
static int drvHostAudioPaError(PDRVHOSTPULSEAUDIO pThis, const char *szMsg)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(szMsg, VERR_INVALID_POINTER);

    if (   pThis->cLogErrors < VBOX_PULSEAUDIO_MAX_LOG_REL_ERRORS
        && LogRelIs2Enabled())
    {
        pThis->cLogErrors++;
        int rc2 = pa_context_errno(pThis->pContext);
        LogRel2(("PulseAudio: %s: %s\n", szMsg, pa_strerror(rc2)));
    }

    /** @todo Implement some PulseAudio -> IPRT mapping here. */
    return VERR_GENERAL_FAILURE;
}


/**
 * Signal the main loop to abort. Just signalling isn't sufficient as the
 * mainloop might not have been entered yet.
 */
static void drvHostAudioPaSignalWaiter(PDRVHOSTPULSEAUDIO pThis)
{
    if (pThis)
    {
        pThis->fAbortLoop = true;
        pa_threaded_mainloop_signal(pThis->pMainLoop, 0);
    }
}



/**
 * Pulse audio callback for context status changes, init variant.
 */
static void drvHostAudioPaCtxCallbackStateChanged(pa_context *pCtx, void *pvUser)
{
    AssertPtrReturnVoid(pCtx);

    PDRVHOSTPULSEAUDIO pThis = (PDRVHOSTPULSEAUDIO)pvUser;
    AssertPtrReturnVoid(pThis);

    switch (pa_context_get_state(pCtx))
    {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            drvHostAudioPaSignalWaiter(pThis);
            break;

        default:
            break;
    }
}


/**
 * Callback used with pa_stream_cork() in a number of places.
 */
static void drvHostAudioPaStreamSuccessCallback(pa_stream *pStream, int fSuccess, void *pvUser)
{
    AssertPtrReturnVoid(pStream);
    PPULSEAUDIOSTREAM pStrm = (PPULSEAUDIOSTREAM)pvUser;
    AssertPtrReturnVoid(pStrm);

    pStrm->fOpSuccess = fSuccess;

    if (fSuccess)
        drvHostAudioPaSignalWaiter(pStrm->pDrv);
    else
        drvHostAudioPaError(pStrm->pDrv, "Failed to finish stream operation");
}


/**
 * Synchronously wait until an operation completed.
 */
static int drvHostAudioPaWaitForEx(PDRVHOSTPULSEAUDIO pThis, pa_operation *pOP, RTMSINTERVAL cMsTimeout)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pOP,   VERR_INVALID_POINTER);

    int rc = VINF_SUCCESS;

    uint64_t u64StartMs = RTTimeMilliTS();
    while (pa_operation_get_state(pOP) == PA_OPERATION_RUNNING)
    {
        if (!pThis->fAbortLoop) /** @todo r=bird: I do _not_ get the logic behind this fAbortLoop mechanism, it looks more
                                 * than a little mixed up and too much generalized see drvHostAudioPaSignalWaiter. */
        {
            AssertPtr(pThis->pMainLoop);
            pa_threaded_mainloop_wait(pThis->pMainLoop);
            if (   !pThis->pContext
                || pa_context_get_state(pThis->pContext) != PA_CONTEXT_READY)
            {
                pa_operation_cancel(pOP);
                LogRel(("PulseAudio: pa_context_get_state context not ready\n"));
                rc = VERR_INVALID_STATE;
                break;
            }
        }
        pThis->fAbortLoop = false;

        uint64_t u64ElapsedMs = RTTimeMilliTS() - u64StartMs;
        if (u64ElapsedMs >= cMsTimeout)
        {
            pa_operation_cancel(pOP);
            rc = VERR_TIMEOUT;
            break;
        }
    }

    pa_operation_unref(pOP);

    return rc;
}


static int drvHostAudioPaWaitFor(PDRVHOSTPULSEAUDIO pThis, pa_operation *pOP)
{
    return drvHostAudioPaWaitForEx(pThis, pOP, 10 * RT_MS_1SEC);
}



/*********************************************************************************************************************************
*   PDMIHOSTAUDIO                                                                                                                *
*********************************************************************************************************************************/

/**
 * Worker for drvHostAudioPaEnumSourceCallback() and
 * drvHostAudioPaEnumSinkCallback() that adds an entry to the enumeration
 * result.
 */
static void drvHostAudioPaEnumAddDevice(PPULSEAUDIOENUMCBCTX pCbCtx, PDMAUDIODIR enmDir, const char *pszName,
                                        const char *pszDesc, uint8_t cChannelsInput, uint8_t cChannelsOutput,
                                        const char *pszDefaultName)
{
    size_t const cchName = strlen(pszName);
    PPULSEAUDIODEVENTRY pDev = (PPULSEAUDIODEVENTRY)PDMAudioHostDevAlloc(RT_UOFFSETOF(PULSEAUDIODEVENTRY, szPulseName)
                                                                         + RT_ALIGN_Z(cchName + 1, 16));
    if (pDev != NULL)
    {
        memcpy(pDev->szPulseName, pszName, cchName);
        pDev->szPulseName[cchName] = '\0';

        pDev->Core.enmUsage           = enmDir;
        pDev->Core.enmType            = RTStrIStr(pszDesc, "built-in") != NULL
                                      ? PDMAUDIODEVICETYPE_BUILTIN : PDMAUDIODEVICETYPE_UNKNOWN;
        pDev->Core.fFlags             = RTStrCmp(pszName, pszDefaultName) == 0
                                      ? PDMAUDIOHOSTDEV_F_DEFAULT  : PDMAUDIOHOSTDEV_F_NONE;
        pDev->Core.cMaxInputChannels  = cChannelsInput;
        pDev->Core.cMaxOutputChannels = cChannelsOutput;
        RTStrCopy(pDev->Core.szName, sizeof(pDev->Core.szName),
                  pszDesc && *pszDesc ? pszDesc : pszName);

        PDMAudioHostEnumAppend(pCbCtx->pDeviceEnum, &pDev->Core);
    }
    else
        pCbCtx->rcEnum = VERR_NO_MEMORY;
}


/**
 * Enumeration callback - source info.
 *
 * @param   pCtx        The context (DRVHOSTPULSEAUDIO::pContext).
 * @param   pInfo       The info.  NULL when @a eol is not zero.
 * @param   eol         Error-or-last indicator or something like that:
 *                          -  0: Normal call with info.
 *                          -  1: End of list, no info.
 *                          - -1: Error callback, no info.
 */
static void drvHostAudioPaEnumSourceCallback(pa_context *pCtx, const pa_source_info *pInfo, int eol, void *pvUserData)
{
    LogFlowFunc(("pCtx=%p pInfo=%p eol=%d pvUserData=%p\n", pCtx, pInfo, eol, pvUserData));
    PPULSEAUDIOENUMCBCTX pCbCtx = (PPULSEAUDIOENUMCBCTX)pvUserData;
    AssertPtrReturnVoid(pCbCtx);
    Assert((pInfo == NULL) == (eol != 0));
    RT_NOREF(pCtx);

    if (eol == 0 && pInfo != NULL)
    {
        LogRel2(("Pulse Audio: Source #%u: %u Hz %uch format=%u name='%s' desc='%s' driver='%s' flags=%#x\n",
                 pInfo->index, pInfo->sample_spec.rate, pInfo->sample_spec.channels, pInfo->sample_spec.format,
                 pInfo->name, pInfo->description, pInfo->driver, pInfo->flags));
        drvHostAudioPaEnumAddDevice(pCbCtx, PDMAUDIODIR_IN, pInfo->name, pInfo->description,
                                    pInfo->sample_spec.channels, 0 /*cChannelsOutput*/, pCbCtx->pszDefaultSource);
    }
    else if (eol == 1 && !pInfo && pCbCtx->rcEnum == VERR_AUDIO_ENUMERATION_FAILED)
        pCbCtx->rcEnum = VINF_SUCCESS;

    /* Wake up the calling thread when done: */
    if (eol != 0)
        pa_threaded_mainloop_signal(pCbCtx->pMainLoop, 0);
}


/**
 * Enumeration callback - sink info.
 *
 * @param   pCtx        The context (DRVHOSTPULSEAUDIO::pContext).
 * @param   pInfo       The info.  NULL when @a eol is not zero.
 * @param   eol         Error-or-last indicator or something like that:
 *                          -  0: Normal call with info.
 *                          -  1: End of list, no info.
 *                          - -1: Error callback, no info.
 */
static void drvHostAudioPaEnumSinkCallback(pa_context *pCtx, const pa_sink_info *pInfo, int eol, void *pvUserData)
{
    LogFlowFunc(("pCtx=%p pInfo=%p eol=%d pvUserData=%p\n", pCtx, pInfo, eol, pvUserData));
    PPULSEAUDIOENUMCBCTX pCbCtx = (PPULSEAUDIOENUMCBCTX)pvUserData;
    AssertPtrReturnVoid(pCbCtx);
    Assert((pInfo == NULL) == (eol != 0));
    RT_NOREF(pCtx);

    if (eol == 0 && pInfo != NULL)
    {
        LogRel2(("Pulse Audio: Sink #%u: %u Hz %uch format=%u name='%s' desc='%s' driver='%s' flags=%#x\n",
                 pInfo->index, pInfo->sample_spec.rate, pInfo->sample_spec.channels, pInfo->sample_spec.format,
                 pInfo->name, pInfo->description, pInfo->driver, pInfo->flags));
        drvHostAudioPaEnumAddDevice(pCbCtx, PDMAUDIODIR_OUT, pInfo->name, pInfo->description,
                                    0 /*cChannelsInput*/, pInfo->sample_spec.channels, pCbCtx->pszDefaultSink);
    }
    else if (eol == 1 && !pInfo && pCbCtx->rcEnum == VERR_AUDIO_ENUMERATION_FAILED)
        pCbCtx->rcEnum = VINF_SUCCESS;

    /* Wake up the calling thread when done: */
    if (eol != 0)
        pa_threaded_mainloop_signal(pCbCtx->pMainLoop, 0);
}


/**
 * Enumeration callback - service info.
 *
 * Copy down the default names.
 */
static void drvHostAudioPaEnumServerCallback(pa_context *pCtx, const pa_server_info *pInfo, void *pvUserData)
{
    LogFlowFunc(("pCtx=%p pInfo=%p pvUserData=%p\n", pCtx, pInfo, pvUserData));
    PPULSEAUDIOENUMCBCTX pCbCtx = (PPULSEAUDIOENUMCBCTX)pvUserData;
    AssertPtrReturnVoid(pCbCtx);
    RT_NOREF(pCtx);

    if (pInfo)
    {
        LogRel2(("PulseAudio: Server info: user=%s host=%s ver=%s name=%s defsink=%s defsrc=%s spec: %d %uHz %uch\n",
                 pInfo->user_name, pInfo->host_name, pInfo->server_version, pInfo->server_name,
                 pInfo->default_sink_name, pInfo->default_source_name,
                 pInfo->sample_spec.format, pInfo->sample_spec.rate, pInfo->sample_spec.channels));

        Assert(!pCbCtx->pszDefaultSink);
        Assert(!pCbCtx->pszDefaultSource);
        Assert(pCbCtx->rcEnum == VERR_AUDIO_ENUMERATION_FAILED);
        pCbCtx->rcEnum = VINF_SUCCESS;

        if (pInfo->default_sink_name)
        {
            Assert(RTStrIsValidEncoding(pInfo->default_sink_name));
            pCbCtx->pszDefaultSink = RTStrDup(pInfo->default_sink_name);
            AssertStmt(pCbCtx->pszDefaultSink, pCbCtx->rcEnum = VERR_NO_STR_MEMORY);
        }

        if (pInfo->default_source_name)
        {
            Assert(RTStrIsValidEncoding(pInfo->default_source_name));
            pCbCtx->pszDefaultSource = RTStrDup(pInfo->default_source_name);
            AssertStmt(pCbCtx->pszDefaultSource, pCbCtx->rcEnum = VERR_NO_STR_MEMORY);
        }
    }
    else
        pCbCtx->rcEnum = VERR_INVALID_POINTER;

    pa_threaded_mainloop_signal(pCbCtx->pMainLoop, 0);
}


/**
 * @note Called with the PA main loop locked.
 */
static int drvHostAudioPaEnumerate(PDRVHOSTPULSEAUDIO pThis, uint32_t fEnum, PPDMAUDIOHOSTENUM pDeviceEnum)
{
    PULSEAUDIOENUMCBCTX CbCtx        = { pThis->pMainLoop, fEnum, VERR_AUDIO_ENUMERATION_FAILED, NULL, NULL, pDeviceEnum };
    bool const          fLog         = (fEnum & PULSEAUDIOENUMCBFLAGS_LOG);
    bool const          fOnlyDefault = (fEnum & PULSEAUDIOENUMCBFLAGS_DEFAULT_ONLY);
    int                 rc;

    /*
     * Check if server information is available and bail out early if it isn't.
     * This should give us a default (playback) sink and (recording) source.
     */
    LogRel(("PulseAudio: Retrieving server information ...\n"));
    CbCtx.rcEnum = VERR_AUDIO_ENUMERATION_FAILED;
    pa_operation *paOpServerInfo = pa_context_get_server_info(pThis->pContext, drvHostAudioPaEnumServerCallback, &CbCtx);
    if (paOpServerInfo)
        rc = drvHostAudioPaWaitFor(pThis, paOpServerInfo);
    else
    {
        LogRel(("PulseAudio: Server information not available, skipping enumeration.\n"));
        return VINF_SUCCESS;
    }
    if (RT_SUCCESS(rc))
        rc = CbCtx.rcEnum;
    if (RT_FAILURE(rc))
    {
        if (fLog)
            LogRel(("PulseAudio: Error enumerating PulseAudio server properties: %Rrc\n", rc));
        return rc;
    }

    /*
     * Get info about the playback sink.
     */
    if (fLog && CbCtx.pszDefaultSink)
        LogRel2(("PulseAudio: Default output sink is '%s'\n", CbCtx.pszDefaultSink));
    else if (fLog)
        LogRel2(("PulseAudio: No default output sink found\n"));

    if (CbCtx.pszDefaultSink || !fOnlyDefault)
    {
        CbCtx.rcEnum = VERR_AUDIO_ENUMERATION_FAILED;
        if (!fOnlyDefault)
            rc = drvHostAudioPaWaitFor(pThis,
                                       pa_context_get_sink_info_list(pThis->pContext, drvHostAudioPaEnumSinkCallback, &CbCtx));
        else
            rc = drvHostAudioPaWaitFor(pThis, pa_context_get_sink_info_by_name(pThis->pContext, CbCtx.pszDefaultSink,
                                                                               drvHostAudioPaEnumSinkCallback, &CbCtx));
        if (RT_SUCCESS(rc))
            rc = CbCtx.rcEnum;
        if (fLog && RT_FAILURE(rc))
            LogRel(("PulseAudio: Error enumerating properties for default output sink '%s': %Rrc\n",
                    CbCtx.pszDefaultSink, rc));
    }

    /*
     * Get info about the recording source.
     */
    if (fLog && CbCtx.pszDefaultSource)
        LogRel2(("PulseAudio: Default input source is '%s'\n", CbCtx.pszDefaultSource));
    else if (fLog)
        LogRel2(("PulseAudio: No default input source found\n"));
    if (CbCtx.pszDefaultSource || !fOnlyDefault)
    {
        CbCtx.rcEnum = VERR_AUDIO_ENUMERATION_FAILED;
        int rc2;
        if (!fOnlyDefault)
            rc2 = drvHostAudioPaWaitFor(pThis, pa_context_get_source_info_list(pThis->pContext,
                                                                               drvHostAudioPaEnumSourceCallback, &CbCtx));
        else
            rc2 = drvHostAudioPaWaitFor(pThis, pa_context_get_source_info_by_name(pThis->pContext, CbCtx.pszDefaultSource,
                                                                                  drvHostAudioPaEnumSourceCallback, &CbCtx));
        if (RT_SUCCESS(rc2))
            rc2 = CbCtx.rcEnum;
        if (fLog && RT_FAILURE(rc2))
            LogRel(("PulseAudio: Error enumerating properties for default input source '%s': %Rrc\n",
                    CbCtx.pszDefaultSource, rc));
        if (RT_SUCCESS(rc))
            rc = rc2;
    }

    /* clean up */
    RTStrFree(CbCtx.pszDefaultSink);
    RTStrFree(CbCtx.pszDefaultSource);

    LogFlowFuncLeaveRC(rc);
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetConfig}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_GetConfig(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pBackendCfg)
{
    PDRVHOSTPULSEAUDIO pThis = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    AssertPtrReturn(pBackendCfg, VERR_INVALID_POINTER);

    /*
     * The configuration.
     */
    RTStrCopy(pBackendCfg->szName, sizeof(pBackendCfg->szName), "PulseAudio");
    pBackendCfg->cbStreamOut    = sizeof(PULSEAUDIOSTREAM);
    pBackendCfg->cbStreamIn     = sizeof(PULSEAUDIOSTREAM);
    pBackendCfg->cMaxStreamsOut = UINT32_MAX;
    pBackendCfg->cMaxStreamsIn  = UINT32_MAX;

#if 0
    /*
     * In case we want to gather info about default devices, we can do this:
     */
    PDMAUDIOHOSTENUM DeviceEnum;
    PDMAudioHostEnumInit(&DeviceEnum);
    pa_threaded_mainloop_lock(pThis->pMainLoop);
    int rc = drvHostAudioPaEnumerate(pThis, PULSEAUDIOENUMCBFLAGS_DEFAULT_ONLY | PULSEAUDIOENUMCBFLAGS_LOG, &DeviceEnum);
    pa_threaded_mainloop_unlock(pThis->pMainLoop);
    AssertRCReturn(rc, rc);
    /** @todo do stuff with DeviceEnum. */
    PDMAudioHostEnumDelete(&DeviceEnum);
#else
    RT_NOREF(pThis);
#endif
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetDevices}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_GetDevices(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHOSTENUM pDeviceEnum)
{
    PDRVHOSTPULSEAUDIO pThis = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    AssertPtrReturn(pDeviceEnum, VERR_INVALID_POINTER);
    PDMAudioHostEnumInit(pDeviceEnum);

    /* Refine it or something (currently only some LogRel2 stuff): */
    pa_threaded_mainloop_lock(pThis->pMainLoop);
    int rc = drvHostAudioPaEnumerate(pThis, PULSEAUDIOENUMCBFLAGS_NONE, pDeviceEnum);
    pa_threaded_mainloop_unlock(pThis->pMainLoop);
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetStatus}
 */
static DECLCALLBACK(PDMAUDIOBACKENDSTS) drvHostAudioPaHA_GetStatus(PPDMIHOSTAUDIO pInterface, PDMAUDIODIR enmDir)
{
    RT_NOREF(pInterface, enmDir);
    return PDMAUDIOBACKENDSTS_RUNNING;
}


static pa_sample_format_t drvHostAudioPaPropsToPulse(PPDMAUDIOPCMPROPS pProps)
{
    switch (PDMAudioPropsSampleSize(pProps))
    {
        case 1:
            if (!pProps->fSigned)
                return PA_SAMPLE_U8;
            break;

        case 2:
            if (pProps->fSigned)
                return PDMAudioPropsIsLittleEndian(pProps) ? PA_SAMPLE_S16LE : PA_SAMPLE_S16BE;
            break;

#ifdef PA_SAMPLE_S32LE
        case 4:
            if (pProps->fSigned)
                return PDMAudioPropsIsLittleEndian(pProps) ? PA_SAMPLE_S32LE : PA_SAMPLE_S32BE;
            break;
#endif
    }

    AssertMsgFailed(("%RU8%s not supported\n", PDMAudioPropsSampleSize(pProps), pProps->fSigned ? "S" : "U"));
    return PA_SAMPLE_INVALID;
}


static int drvHostAudioPaToAudioProps(PPDMAUDIOPCMPROPS pProps, pa_sample_format_t pulsefmt, uint8_t cChannels, uint32_t uHz)
{
    AssertReturn(cChannels > 0, VERR_INVALID_PARAMETER);
    AssertReturn(cChannels < 16, VERR_INVALID_PARAMETER);

    switch (pulsefmt)
    {
        case PA_SAMPLE_U8:
            PDMAudioPropsInit(pProps, 1 /*8-bit*/, false /*signed*/, cChannels, uHz);
            break;

        case PA_SAMPLE_S16LE:
            PDMAudioPropsInitEx(pProps, 2 /*16-bit*/, true /*signed*/, cChannels, uHz, true /*fLittleEndian*/, false /*fRaw*/);
            break;

        case PA_SAMPLE_S16BE:
            PDMAudioPropsInitEx(pProps, 2 /*16-bit*/, true /*signed*/, cChannels, uHz, false /*fLittleEndian*/, false /*fRaw*/);
            break;

#ifdef PA_SAMPLE_S32LE
        case PA_SAMPLE_S32LE:
            PDMAudioPropsInitEx(pProps, 4 /*32-bit*/, true /*signed*/, cChannels, uHz, true /*fLittleEndian*/, false /*fRaw*/);
            break;
#endif

#ifdef PA_SAMPLE_S32BE
        case PA_SAMPLE_S32BE:
            PDMAudioPropsInitEx(pProps, 4 /*32-bit*/, true /*signed*/, cChannels, uHz, false /*fLittleEndian*/, false /*fRaw*/);
            break;
#endif

        default:
            AssertLogRelMsgFailed(("PulseAudio: Format (%d) not supported\n", pulsefmt));
            return VERR_NOT_SUPPORTED;
    }

    return VINF_SUCCESS;
}


/**
 * Stream status changed.
 */
static void drvHostAudioPaStreamStateChangedCallback(pa_stream *pStream, void *pvUser)
{
    AssertPtrReturnVoid(pStream);

    PDRVHOSTPULSEAUDIO pThis = (PDRVHOSTPULSEAUDIO)pvUser;
    AssertPtrReturnVoid(pThis);

    switch (pa_stream_get_state(pStream))
    {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            drvHostAudioPaSignalWaiter(pThis);
            break;

        default:
            break;
    }
}

#ifdef DEBUG

static void drvHostAudioPaStreamReqWriteDebugCallback(pa_stream *pStream, size_t cbLen, void *pvContext)
{
    RT_NOREF(cbLen, pvContext);

    PPULSEAUDIOSTREAM pStrm = (PPULSEAUDIOSTREAM)pvContext;
    AssertPtrReturnVoid(pStrm);

    pa_usec_t usec = 0;
    int neg = 0;
    pa_stream_get_latency(pStream, &usec, &neg);

    Log2Func(("Requested %zu bytes -- Current latency is %RU64ms\n", cbLen, usec / 1000));
}


static void drvHostAudioPaStreamUnderflowDebugCallback(pa_stream *pStream, void *pvContext)
{
    PPULSEAUDIOSTREAM pStrm = (PPULSEAUDIOSTREAM)pvContext;
    AssertPtrReturnVoid(pStrm);

    pStrm->cUnderflows++;

    LogRel2(("PulseAudio: Warning: Hit underflow #%RU32\n", pStrm->cUnderflows));

    if (   pStrm->cUnderflows  >= 6                /** @todo Make this check configurable. */
        && pStrm->curLatencyUs < 2000000 /* 2s */)
    {
        pStrm->curLatencyUs = (pStrm->curLatencyUs * 3) / 2;

        LogRel2(("PulseAudio: Output latency increased to %RU64ms\n", pStrm->curLatencyUs / 1000 /* ms */));

        pStrm->BufAttr.maxlength = pa_usec_to_bytes(pStrm->curLatencyUs, &pStrm->SampleSpec);
        pStrm->BufAttr.tlength   = pa_usec_to_bytes(pStrm->curLatencyUs, &pStrm->SampleSpec);

        pa_stream_set_buffer_attr(pStream, &pStrm->BufAttr, NULL, NULL);

        pStrm->cUnderflows = 0;
    }

    pa_usec_t curLatencyUs = 0;
    pa_stream_get_latency(pStream, &curLatencyUs, NULL /* Neg */);

    LogRel2(("PulseAudio: Latency now is %RU64ms\n", curLatencyUs / 1000 /* ms */));

# ifdef LOG_ENABLED
    const pa_timing_info *pTInfo = pa_stream_get_timing_info(pStream);
    const pa_sample_spec *pSpec  = pa_stream_get_sample_spec(pStream);

    pa_usec_t curPosWritesUs = pa_bytes_to_usec(pTInfo->write_index, pSpec);
    pa_usec_t curPosReadsUs  = pa_bytes_to_usec(pTInfo->read_index, pSpec);
    pa_usec_t curTsUs        = pa_rtclock_now() - pStrm->tsStartUs;

    Log2Func(("curPosWrite=%RU64ms, curPosRead=%RU64ms, curTs=%RU64ms, curLatency=%RU64ms (%RU32Hz, %RU8 channels)\n",
              curPosWritesUs / RT_US_1MS_64, curPosReadsUs / RT_US_1MS_64,
              curTsUs / RT_US_1MS_64, curLatencyUs / RT_US_1MS_64, pSpec->rate, pSpec->channels));
# endif
}


static void drvHostAudioPaStreamOverflowDebugCallback(pa_stream *pStream, void *pvContext)
{
    RT_NOREF(pStream, pvContext);

    Log2Func(("Warning: Hit overflow\n"));
}

#endif /* DEBUG */


static int drvHostAudioPaStreamOpen(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM pStreamPA, bool fIn, const char *pszName)
{
    AssertPtrReturn(pThis,     VERR_INVALID_POINTER);
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName,   VERR_INVALID_POINTER);

    int rc = VERR_AUDIO_STREAM_COULD_NOT_CREATE;
    pa_stream *pStream = NULL;

    pa_threaded_mainloop_lock(pThis->pMainLoop);

    do /* goto avoidance non-loop */
    {
        pa_sample_spec *pSampleSpec = &pStreamPA->SampleSpec;

        LogFunc(("Opening '%s', rate=%dHz, channels=%d, format=%s\n",
                 pszName, pSampleSpec->rate, pSampleSpec->channels,
                 pa_sample_format_to_string(pSampleSpec->format)));

        if (!pa_sample_spec_valid(pSampleSpec))
        {
            LogRel(("PulseAudio: Unsupported sample specification for stream '%s'\n", pszName));
            break;
        }

        pa_buffer_attr *pBufAttr = &pStreamPA->BufAttr;

        /** @todo r=andy Use pa_stream_new_with_proplist instead. */
        if (!(pStream = pa_stream_new(pThis->pContext, pszName, pSampleSpec, NULL /* pa_channel_map */)))
        {
            LogRel(("PulseAudio: Could not create stream '%s'\n", pszName));
            rc = VERR_NO_MEMORY;
            break;
        }

#ifdef DEBUG
        pa_stream_set_write_callback(       pStream, drvHostAudioPaStreamReqWriteDebugCallback,  pStreamPA);
        pa_stream_set_underflow_callback(   pStream, drvHostAudioPaStreamUnderflowDebugCallback, pStreamPA);
        if (!fIn) /* Only for output streams. */
            pa_stream_set_overflow_callback(pStream, drvHostAudioPaStreamOverflowDebugCallback,  pStreamPA);
#endif
        pa_stream_set_state_callback(       pStream, drvHostAudioPaStreamStateChangedCallback,   pThis);

        uint32_t flags = PA_STREAM_NOFLAGS;
#if PA_API_VERSION >= 12
        /* XXX */
        flags |= PA_STREAM_ADJUST_LATENCY;
#endif
        /* For using pa_stream_get_latency() and pa_stream_get_time(). */
        flags |= PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE;

        /* No input/output right away after the stream was started. */
        flags |= PA_STREAM_START_CORKED;

        if (fIn)
        {
            LogFunc(("Input stream attributes: maxlength=%d fragsize=%d\n",
                     pBufAttr->maxlength, pBufAttr->fragsize));

            if (pa_stream_connect_record(pStream, /*dev=*/NULL, pBufAttr, (pa_stream_flags_t)flags) < 0)
            {
                LogRel(("PulseAudio: Could not connect input stream '%s': %s\n",
                        pszName, pa_strerror(pa_context_errno(pThis->pContext))));
                break;
            }
        }
        else
        {
            LogFunc(("Output buffer attributes: maxlength=%d tlength=%d prebuf=%d minreq=%d\n",
                     pBufAttr->maxlength, pBufAttr->tlength, pBufAttr->prebuf, pBufAttr->minreq));

            if (pa_stream_connect_playback(pStream, /*dev=*/NULL, pBufAttr, (pa_stream_flags_t)flags,
                                           /*cvolume=*/NULL, /*sync_stream=*/NULL) < 0)
            {
                LogRel(("PulseAudio: Could not connect playback stream '%s': %s\n",
                        pszName, pa_strerror(pa_context_errno(pThis->pContext))));
                break;
            }
        }

        /* Wait until the stream is ready. */
        pa_stream_state_t enmStreamState;
        for (;;)
        {
            enmStreamState = pa_stream_get_state(pStream);
            if (   enmStreamState == PA_STREAM_READY
                || !PA_STREAM_IS_GOOD(enmStreamState))
                break;

            if (!pThis->fAbortLoop)
                pa_threaded_mainloop_wait(pThis->pMainLoop);
            pThis->fAbortLoop = false;
        }
        if (!PA_STREAM_IS_GOOD(enmStreamState))
        {
            LogRel(("PulseAudio: Failed to initialize stream '%s' (state %ld)\n", pszName, enmStreamState));
            break;
        }

#ifdef LOG_ENABLED
        pStreamPA->tsStartUs = pa_rtclock_now();
#endif
        const pa_buffer_attr *pBufAttrObtained = pa_stream_get_buffer_attr(pStream);
        AssertPtrBreak(pBufAttrObtained);
        memcpy(pBufAttr, pBufAttrObtained, sizeof(pa_buffer_attr));

        LogFunc(("Obtained %s buffer attributes: tLength=%RU32, maxLength=%RU32, minReq=%RU32, fragSize=%RU32, preBuf=%RU32\n",
                 fIn ? "capture" : "playback",
                 pBufAttr->tlength, pBufAttr->maxlength, pBufAttr->minreq, pBufAttr->fragsize, pBufAttr->prebuf));

        pStreamPA->pStream = pStream;

        pa_threaded_mainloop_unlock(pThis->pMainLoop);
        LogFlowFuncLeaveRC(VINF_SUCCESS);
        return VINF_SUCCESS;

    } while (0);

    /* We failed. */
    if (pStream)
        pa_stream_disconnect(pStream);

    pa_threaded_mainloop_unlock(pThis->pMainLoop);

    if (pStream)
        pa_stream_unref(pStream);
    LogFlowFuncLeaveRC(rc);
    return rc;
}


static int drvHostAudioPaCreateStreamOut(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM pStreamPA,
                                         PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    pStreamPA->pDrainOp            = NULL;

    pStreamPA->SampleSpec.format   = drvHostAudioPaPropsToPulse(&pCfgReq->Props);
    pStreamPA->SampleSpec.rate     = PDMAudioPropsHz(&pCfgReq->Props);
    pStreamPA->SampleSpec.channels = PDMAudioPropsChannels(&pCfgReq->Props);

    pStreamPA->curLatencyUs        = PDMAudioPropsFramesToMilli(&pCfgReq->Props, pCfgReq->Backend.cFramesBufferSize) * RT_US_1MS;

    const uint32_t cbLatency = pa_usec_to_bytes(pStreamPA->curLatencyUs, &pStreamPA->SampleSpec);

    LogRel2(("PulseAudio: Initial output latency is %RU64ms (%RU32 bytes)\n", pStreamPA->curLatencyUs / RT_US_1MS, cbLatency));

    pStreamPA->BufAttr.tlength     = cbLatency;
    pStreamPA->BufAttr.maxlength   = -1; /* Let the PulseAudio server choose the biggest size it can handle. */
    pStreamPA->BufAttr.prebuf      = cbLatency;
    pStreamPA->BufAttr.minreq      = PDMAudioPropsFramesToBytes(&pCfgReq->Props, pCfgReq->Backend.cFramesPeriod);

    LogFunc(("Requested: BufAttr tlength=%RU32, maxLength=%RU32, minReq=%RU32\n",
             pStreamPA->BufAttr.tlength, pStreamPA->BufAttr.maxlength, pStreamPA->BufAttr.minreq));

    Assert(pCfgReq->enmDir == PDMAUDIODIR_OUT);

    char szName[256];
    RTStrPrintf(szName, sizeof(szName), "VirtualBox %s [%s]", PDMAudioPlaybackDstGetName(pCfgReq->u.enmDst), pThis->szStreamName);

    /* Note that the struct BufAttr is updated to the obtained values after this call! */
    int rc = drvHostAudioPaStreamOpen(pThis, pStreamPA, false /* fIn */, szName);
    if (RT_FAILURE(rc))
        return rc;

    rc = drvHostAudioPaToAudioProps(&pCfgAcq->Props, pStreamPA->SampleSpec.format,
                                  pStreamPA->SampleSpec.channels, pStreamPA->SampleSpec.rate);
    if (RT_FAILURE(rc))
    {
        LogRel(("PulseAudio: Cannot find audio output format %ld\n", pStreamPA->SampleSpec.format));
        return rc;
    }

    LogFunc(("Acquired: BufAttr tlength=%RU32, maxLength=%RU32, minReq=%RU32\n",
             pStreamPA->BufAttr.tlength, pStreamPA->BufAttr.maxlength, pStreamPA->BufAttr.minreq));

    pCfgAcq->Backend.cFramesPeriod     = PDMAUDIOSTREAMCFG_B2F(pCfgAcq, pStreamPA->BufAttr.minreq);
    pCfgAcq->Backend.cFramesBufferSize = PDMAUDIOSTREAMCFG_B2F(pCfgAcq, pStreamPA->BufAttr.tlength);
    pCfgAcq->Backend.cFramesPreBuffering     = PDMAUDIOSTREAMCFG_B2F(pCfgAcq, pStreamPA->BufAttr.prebuf);

    pStreamPA->pDrv = pThis;

    return rc;
}


static int drvHostAudioPaCreateStreamIn(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM  pStreamPA,
                                        PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    pStreamPA->SampleSpec.format   = drvHostAudioPaPropsToPulse(&pCfgReq->Props);
    pStreamPA->SampleSpec.rate     = PDMAudioPropsHz(&pCfgReq->Props);
    pStreamPA->SampleSpec.channels = PDMAudioPropsChannels(&pCfgReq->Props);

    pStreamPA->BufAttr.fragsize    = PDMAudioPropsFramesToBytes(&pCfgReq->Props, pCfgReq->Backend.cFramesPeriod);
    pStreamPA->BufAttr.maxlength   = -1; /* Let the PulseAudio server choose the biggest size it can handle. */

    Assert(pCfgReq->enmDir == PDMAUDIODIR_IN);

    char szName[256];
    RTStrPrintf(szName, sizeof(szName), "VirtualBox %s [%s]", PDMAudioRecSrcGetName(pCfgReq->u.enmSrc), pThis->szStreamName);

    /* Note: Other members of BufAttr are ignored for record streams. */
    int rc = drvHostAudioPaStreamOpen(pThis, pStreamPA, true /* fIn */, szName);
    if (RT_FAILURE(rc))
        return rc;

    rc = drvHostAudioPaToAudioProps(&pCfgAcq->Props, pStreamPA->SampleSpec.format,
                                  pStreamPA->SampleSpec.channels, pStreamPA->SampleSpec.rate);
    if (RT_FAILURE(rc))
    {
        LogRel(("PulseAudio: Cannot find audio capture format %ld\n", pStreamPA->SampleSpec.format));
        return rc;
    }

    pStreamPA->pDrv       = pThis;
    pStreamPA->pu8PeekBuf = NULL;

    pCfgAcq->Backend.cFramesPeriod       = PDMAUDIOSTREAMCFG_B2F(pCfgAcq, pStreamPA->BufAttr.fragsize);
    pCfgAcq->Backend.cFramesBufferSize   = pStreamPA->BufAttr.maxlength != UINT32_MAX /* paranoia */
                                         ? PDMAUDIOSTREAMCFG_B2F(pCfgAcq, pStreamPA->BufAttr.maxlength)
                                         : pCfgAcq->Backend.cFramesPeriod * 2 /* whatever */;
    pCfgAcq->Backend.cFramesPreBuffering = pCfgAcq->Backend.cFramesPeriod;

    LogFlowFuncLeaveRC(rc);
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamCreate}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_StreamCreate(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                       PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    PDRVHOSTPULSEAUDIO pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    PPULSEAUDIOSTREAM  pStreamPA = (PPULSEAUDIOSTREAM)pStream;
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgReq, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgAcq, VERR_INVALID_POINTER);


    int rc;
    if (pCfgReq->enmDir == PDMAUDIODIR_IN)
        rc = drvHostAudioPaCreateStreamIn (pThis, pStreamPA, pCfgReq, pCfgAcq);
    else if (pCfgReq->enmDir == PDMAUDIODIR_OUT)
        rc = drvHostAudioPaCreateStreamOut(pThis, pStreamPA, pCfgReq, pCfgAcq);
    else
        AssertFailedReturn(VERR_NOT_IMPLEMENTED);

    if (RT_SUCCESS(rc))
    {
        pStreamPA->pCfg = PDMAudioStrmCfgDup(pCfgAcq);
        if (!pStreamPA->pCfg)
            rc = VERR_NO_MEMORY;
    }

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamDestroy}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_StreamDestroy(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    PDRVHOSTPULSEAUDIO pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    PPULSEAUDIOSTREAM  pStreamPA = (PPULSEAUDIOSTREAM)pStream;
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);

    if (pStreamPA->pStream)
    {
        pa_threaded_mainloop_lock(pThis->pMainLoop);

        /* Make sure to cancel a pending draining operation, if any. */
        if (pStreamPA->pDrainOp)
        {
            pa_operation_cancel(pStreamPA->pDrainOp);
            pStreamPA->pDrainOp = NULL;
        }

        pa_stream_disconnect(pStreamPA->pStream);
        pa_stream_unref(pStreamPA->pStream);

        pStreamPA->pStream = NULL;

        pa_threaded_mainloop_unlock(pThis->pMainLoop);
    }

    if (pStreamPA->pCfg)
    {
        PDMAudioStrmCfgFree(pStreamPA->pCfg);
        pStreamPA->pCfg = NULL;
    }

    return VINF_SUCCESS;
}


/**
 * Pulse audio pa_stream_drain() completion callback.
 */
static void drvHostAudioPaStreamDrainCompletionCallback(pa_stream *pStream, int fSuccess, void *pvUser)
{
    AssertPtrReturnVoid(pStream);
    PPULSEAUDIOSTREAM pStreamPA = (PPULSEAUDIOSTREAM)pvUser;
    AssertPtrReturnVoid(pStreamPA);

    pStreamPA->fOpSuccess = fSuccess;
    if (fSuccess)
        pa_operation_unref(pa_stream_cork(pStream, 1, drvHostAudioPaStreamSuccessCallback, pvUser));
    else
        drvHostAudioPaError(pStreamPA->pDrv, "Failed to drain stream");

    if (pStreamPA->pDrainOp)
    {
        pa_operation_unref(pStreamPA->pDrainOp);
        pStreamPA->pDrainOp = NULL;
    }
}


static int drvHostAudioPaStreamControlOut(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM pStreamPA, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    int rc = VINF_SUCCESS;

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
            pa_threaded_mainloop_lock(pThis->pMainLoop);

            if (   pStreamPA->pDrainOp
                && pa_operation_get_state(pStreamPA->pDrainOp) != PA_OPERATION_DONE)
            {
                pa_operation_cancel(pStreamPA->pDrainOp);
                pa_operation_unref(pStreamPA->pDrainOp);

                pStreamPA->pDrainOp = NULL;
            }
            else
            {
                /* Uncork (resume) stream. */
                rc = drvHostAudioPaWaitFor(pThis, pa_stream_cork(pStreamPA->pStream, 0 /* Uncork */, drvHostAudioPaStreamSuccessCallback, pStreamPA));
            }

            pa_threaded_mainloop_unlock(pThis->pMainLoop);
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            /* Pause audio output (the Pause bit of the AC97 x_CR register is set).
             * Note that we must return immediately from here! */
            pa_threaded_mainloop_lock(pThis->pMainLoop);
            if (!pStreamPA->pDrainOp)
            {
                rc = drvHostAudioPaWaitFor(pThis, pa_stream_trigger(pStreamPA->pStream, drvHostAudioPaStreamSuccessCallback, pStreamPA));
                if (RT_SUCCESS(rc))
                    pStreamPA->pDrainOp = pa_stream_drain(pStreamPA->pStream, drvHostAudioPaStreamDrainCompletionCallback, pStreamPA);
            }
            pa_threaded_mainloop_unlock(pThis->pMainLoop);
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}


static int drvHostAudioPaStreamControlIn(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM pStreamPA, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("enmStreamCmd=%ld\n", enmStreamCmd));

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
            pa_threaded_mainloop_lock(pThis->pMainLoop);
            rc = drvHostAudioPaWaitFor(pThis, pa_stream_cork(pStreamPA->pStream, 0 /* Play / resume */, drvHostAudioPaStreamSuccessCallback, pStreamPA));
            pa_threaded_mainloop_unlock(pThis->pMainLoop);
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            pa_threaded_mainloop_lock(pThis->pMainLoop);
            if (pStreamPA->pu8PeekBuf) /* Do we need to drop the peek buffer?*/
            {
                pa_stream_drop(pStreamPA->pStream);
                pStreamPA->pu8PeekBuf = NULL;
            }

            rc = drvHostAudioPaWaitFor(pThis, pa_stream_cork(pStreamPA->pStream, 1 /* Stop / pause */, drvHostAudioPaStreamSuccessCallback, pStreamPA));
            pa_threaded_mainloop_unlock(pThis->pMainLoop);
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamControl}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_StreamControl(PPDMIHOSTAUDIO pInterface,
                                                        PPDMAUDIOBACKENDSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    PDRVHOSTPULSEAUDIO pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    PPULSEAUDIOSTREAM  pStreamPA = (PPULSEAUDIOSTREAM)pStream;
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);

    if (!pStreamPA->pCfg) /* Not (yet) configured? Skip. */
        return VINF_SUCCESS;

    int rc;
    if (pStreamPA->pCfg->enmDir == PDMAUDIODIR_IN)
        rc = drvHostAudioPaStreamControlIn (pThis, pStreamPA, enmStreamCmd);
    else if (pStreamPA->pCfg->enmDir == PDMAUDIODIR_OUT)
        rc = drvHostAudioPaStreamControlOut(pThis, pStreamPA, enmStreamCmd);
    else
        AssertFailedStmt(rc = VERR_NOT_IMPLEMENTED);

    return rc;
}


static uint32_t drvHostAudioPaStreamGetAvailable(PDRVHOSTPULSEAUDIO pThis, PPULSEAUDIOSTREAM pStreamPA)
{
    pa_threaded_mainloop_lock(pThis->pMainLoop);

    uint32_t cbAvail = 0;

    if (PA_STREAM_IS_GOOD(pa_stream_get_state(pStreamPA->pStream)))
    {
        if (pStreamPA->pCfg->enmDir == PDMAUDIODIR_IN)
        {
            cbAvail = (uint32_t)pa_stream_readable_size(pStreamPA->pStream);
            Log3Func(("cbReadable=%RU32\n", cbAvail));
        }
        else if (pStreamPA->pCfg->enmDir == PDMAUDIODIR_OUT)
        {
            size_t cbWritable = pa_stream_writable_size(pStreamPA->pStream);

            Log3Func(("cbWritable=%zu, maxLength=%RU32, minReq=%RU32\n",
                      cbWritable, pStreamPA->BufAttr.maxlength, pStreamPA->BufAttr.minreq));

            /* Don't report more writable than the PA server can handle. */
            if (cbWritable > pStreamPA->BufAttr.maxlength)
                cbWritable = pStreamPA->BufAttr.maxlength;

            cbAvail = (uint32_t)cbWritable;
        }
        else
            AssertFailed();
    }

    pa_threaded_mainloop_unlock(pThis->pMainLoop);

    return cbAvail;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetReadable}
 */
static DECLCALLBACK(uint32_t) drvHostAudioPaHA_StreamGetReadable(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    return drvHostAudioPaStreamGetAvailable(RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio),
                                            (PPULSEAUDIOSTREAM)pStream);
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetWritable}
 */
static DECLCALLBACK(uint32_t) drvHostAudioPaHA_StreamGetWritable(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    return drvHostAudioPaStreamGetAvailable(RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio),
                                            (PPULSEAUDIOSTREAM)pStream);
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetStatus}
 */
static DECLCALLBACK(PDMAUDIOSTREAMSTS) drvHostAudioPaHA_StreamGetStatus(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    PDRVHOSTPULSEAUDIO pThis = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    RT_NOREF(pStream);

    /* Check PulseAudio's general status. */
    PDMAUDIOSTREAMSTS fStrmSts = PDMAUDIOSTREAMSTS_FLAGS_NONE;
    if (   pThis->pContext
        && PA_CONTEXT_IS_GOOD(pa_context_get_state(pThis->pContext)))
       fStrmSts = PDMAUDIOSTREAMSTS_FLAGS_INITIALIZED | PDMAUDIOSTREAMSTS_FLAGS_ENABLED;

    return fStrmSts;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamPlay}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_StreamPlay(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                     const void *pvBuf, uint32_t cbBuf, uint32_t *pcbWritten)
{
    PDRVHOSTPULSEAUDIO pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    PPULSEAUDIOSTREAM  pStreamPA = (PPULSEAUDIOSTREAM)pStream;
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    AssertReturn(cbBuf, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbWritten, VERR_INVALID_POINTER);

    pa_threaded_mainloop_lock(pThis->pMainLoop);

#ifdef LOG_ENABLED
    const pa_usec_t tsNowUs         = pa_rtclock_now();
    const pa_usec_t tsDeltaPlayedUs = tsNowUs - pStreamPA->tsLastReadWrittenUs;
    pStreamPA->tsLastReadWrittenUs  = tsNowUs;
    Log3Func(("tsDeltaPlayedMs=%RU64\n", tsDeltaPlayedUs / RT_US_1MS));
#endif

    int          rc;
    size_t const cbWriteable = pa_stream_writable_size(pStreamPA->pStream);
    if (cbWriteable != (size_t)-1)
    {
        size_t cbLeft = RT_MIN(cbWriteable, cbBuf);
        Assert(cbLeft > 0 /* At this point we better have *something* to write (DrvAudio checked before calling). */);
        if (pa_stream_write(pStreamPA->pStream, pvBuf, cbLeft, NULL /*pfnFree*/, 0 /*offset*/, PA_SEEK_RELATIVE) >= 0)
        {
            *pcbWritten = (uint32_t)cbLeft;
            rc = VINF_SUCCESS;
        }
        else
            rc = drvHostAudioPaError(pStreamPA->pDrv, "Failed to write to output stream");
    }
    else
        rc = drvHostAudioPaError(pStreamPA->pDrv, "Failed to determine output data size");

    pa_threaded_mainloop_unlock(pThis->pMainLoop);
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamCapture}
 */
static DECLCALLBACK(int) drvHostAudioPaHA_StreamCapture(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                        void *pvBuf, uint32_t cbBuf, uint32_t *pcbRead)
{
    PDRVHOSTPULSEAUDIO pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTPULSEAUDIO, IHostAudio);
    PPULSEAUDIOSTREAM  pStreamPA = (PPULSEAUDIOSTREAM)pStream;
    AssertPtrReturn(pStreamPA, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    AssertReturn(cbBuf, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbRead, VERR_INVALID_POINTER);

    /* We should only call pa_stream_readable_size() once and trust the first value. */
    pa_threaded_mainloop_lock(pThis->pMainLoop);
    size_t cbAvail = pa_stream_readable_size(pStreamPA->pStream);
    pa_threaded_mainloop_unlock(pThis->pMainLoop);

    if (cbAvail == (size_t)-1)
        return drvHostAudioPaError(pStreamPA->pDrv, "Failed to determine input data size");

    /* If the buffer was not dropped last call, add what remains. */
    if (pStreamPA->pu8PeekBuf)
    {
        Assert(pStreamPA->cbPeekBuf >= pStreamPA->offPeekBuf);
        cbAvail += (pStreamPA->cbPeekBuf - pStreamPA->offPeekBuf);
    }

    Log3Func(("cbAvail=%zu\n", cbAvail));

    if (!cbAvail) /* No data? Bail out. */
    {
        *pcbRead = 0;
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;

    size_t cbToRead = RT_MIN(cbAvail, cbBuf);

    Log3Func(("cbToRead=%zu, cbAvail=%zu, offPeekBuf=%zu, cbPeekBuf=%zu\n",
              cbToRead, cbAvail, pStreamPA->offPeekBuf, pStreamPA->cbPeekBuf));

    uint32_t cbReadTotal = 0;

    while (cbToRead)
    {
        /* If there is no data, do another peek. */
        if (!pStreamPA->pu8PeekBuf)
        {
            pa_threaded_mainloop_lock(pThis->pMainLoop);
            pa_stream_peek(pStreamPA->pStream,
                           (const void**)&pStreamPA->pu8PeekBuf, &pStreamPA->cbPeekBuf);
            pa_threaded_mainloop_unlock(pThis->pMainLoop);

            pStreamPA->offPeekBuf = 0;

            /* No data anymore?
             * Note: If there's a data hole (cbPeekBuf then contains the length of the hole)
             *       we need to drop the stream lateron. */
            if (   !pStreamPA->pu8PeekBuf
                && !pStreamPA->cbPeekBuf)
            {
                break;
            }
        }

        Assert(pStreamPA->cbPeekBuf >= pStreamPA->offPeekBuf);
        size_t cbToWrite = RT_MIN(pStreamPA->cbPeekBuf - pStreamPA->offPeekBuf, cbToRead);

        Log3Func(("cbToRead=%zu, cbToWrite=%zu, offPeekBuf=%zu, cbPeekBuf=%zu, pu8PeekBuf=%p\n",
                  cbToRead, cbToWrite,
                  pStreamPA->offPeekBuf, pStreamPA->cbPeekBuf, pStreamPA->pu8PeekBuf));

        if (   cbToWrite
            /* Only copy data if it's not a data hole (see above). */
            && pStreamPA->pu8PeekBuf
            && pStreamPA->cbPeekBuf)
        {
            memcpy((uint8_t *)pvBuf + cbReadTotal, pStreamPA->pu8PeekBuf + pStreamPA->offPeekBuf, cbToWrite);

            Assert(cbToRead >= cbToWrite);
            cbToRead          -= cbToWrite;
            cbReadTotal       += cbToWrite;

            pStreamPA->offPeekBuf += cbToWrite;
            Assert(pStreamPA->offPeekBuf <= pStreamPA->cbPeekBuf);
        }

        if (/* Nothing to write anymore? Drop the buffer. */
               !cbToWrite
            /* Was there a hole in the peeking buffer? Drop it. */
            || !pStreamPA->pu8PeekBuf
            /* If the buffer is done, drop it. */
            || pStreamPA->offPeekBuf == pStreamPA->cbPeekBuf)
        {
            pa_threaded_mainloop_lock(pThis->pMainLoop);
            pa_stream_drop(pStreamPA->pStream);
            pa_threaded_mainloop_unlock(pThis->pMainLoop);

            pStreamPA->pu8PeekBuf = NULL;
        }
    }

    if (RT_SUCCESS(rc))
        *pcbRead = cbReadTotal;
    return rc;
}


/*********************************************************************************************************************************
*   PDMIBASE                                                                                                                     *
*********************************************************************************************************************************/

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvHostAudioPaQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    AssertPtrReturn(pInterface, NULL);
    AssertPtrReturn(pszIID, NULL);

    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTPULSEAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTPULSEAUDIO);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudio);

    return NULL;
}


/*********************************************************************************************************************************
*   PDMDRVREG                                                                                                                    *
*********************************************************************************************************************************/

/**
 * @interface_method_impl{PDMDRVREG,pfnPowerOff}
 */
static DECLCALLBACK(void) drvHostAudioPaPowerOff(PPDMDRVINS pDrvIns)
{
    PDRVHOSTPULSEAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTPULSEAUDIO);
    LogFlowFuncEnter();

    if (pThis->pMainLoop)
        pa_threaded_mainloop_stop(pThis->pMainLoop);

    if (pThis->pContext)
    {
        pa_context_disconnect(pThis->pContext);
        pa_context_unref(pThis->pContext);
        pThis->pContext = NULL;
    }

    if (pThis->pMainLoop)
    {
        pa_threaded_mainloop_free(pThis->pMainLoop);
        pThis->pMainLoop = NULL;
    }

    LogFlowFuncLeave();
}


/**
 * Destructs a PulseAudio Audio driver instance.
 *
 * @copydoc FNPDMDRVDESTRUCT
 */
static DECLCALLBACK(void) drvHostAudioPaDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    LogFlowFuncEnter();
    drvHostAudioPaPowerOff(pDrvIns);
    LogFlowFuncLeave();
}


/**
 * Pulse audio callback for context status changes, init variant.
 *
 * Signalls our event semaphore so we can do a timed wait from
 * drvHostAudioPaConstruct().
 */
static void drvHostAudioPaCtxCallbackStateChangedInit(pa_context *pCtx, void *pvUser)
{
    AssertPtrReturnVoid(pCtx);
    PPULSEAUDIOSTATECHGCTX pStateChgCtx = (PPULSEAUDIOSTATECHGCTX)pvUser;
    pa_context_state_t     enmCtxState  = pa_context_get_state(pCtx);
    switch (enmCtxState)
    {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            AssertPtrReturnVoid(pStateChgCtx);
            pStateChgCtx->enmCtxState = enmCtxState;
            RTSemEventSignal(pStateChgCtx->hEvtInit);
            break;

        default:
            break;
    }
}


/**
 * Constructs a PulseAudio Audio driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvHostAudioPaConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(pCfg, fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    AssertPtrReturn(pDrvIns, VERR_INVALID_POINTER);

    PDRVHOSTPULSEAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTPULSEAUDIO);
    LogRel(("Audio: Initializing PulseAudio driver\n"));

    /*
     * Initialize instance data.
     */
    pThis->pDrvIns                   = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = drvHostAudioPaQueryInterface;
    /* IHostAudio */
    pThis->IHostAudio.pfnGetConfig          = drvHostAudioPaHA_GetConfig;
    pThis->IHostAudio.pfnGetDevices         = drvHostAudioPaHA_GetDevices;
    pThis->IHostAudio.pfnGetStatus          = drvHostAudioPaHA_GetStatus;
    pThis->IHostAudio.pfnStreamCreate       = drvHostAudioPaHA_StreamCreate;
    pThis->IHostAudio.pfnStreamDestroy      = drvHostAudioPaHA_StreamDestroy;
    pThis->IHostAudio.pfnStreamControl      = drvHostAudioPaHA_StreamControl;
    pThis->IHostAudio.pfnStreamGetReadable  = drvHostAudioPaHA_StreamGetReadable;
    pThis->IHostAudio.pfnStreamGetWritable  = drvHostAudioPaHA_StreamGetWritable;
    pThis->IHostAudio.pfnStreamGetPending   = NULL;
    pThis->IHostAudio.pfnStreamGetStatus    = drvHostAudioPaHA_StreamGetStatus;
    pThis->IHostAudio.pfnStreamPlay         = drvHostAudioPaHA_StreamPlay;
    pThis->IHostAudio.pfnStreamCapture      = drvHostAudioPaHA_StreamCapture;

    /*
     * Read configuration.
     */
    int rc2 = CFGMR3QueryString(pCfg, "StreamName", pThis->szStreamName, sizeof(pThis->szStreamName));
    AssertMsgRCReturn(rc2, ("Confguration error: No/bad \"StreamName\" value, rc=%Rrc\n", rc2), rc2);

    /*
     * Load the pulse audio library.
     */
    int rc = audioLoadPulseLib();
    if (RT_SUCCESS(rc))
        LogRel(("PulseAudio: Using version %s\n", pa_get_library_version()));
    else
    {
        LogRel(("PulseAudio: Failed to load the PulseAudio shared library! Error %Rrc\n", rc));
        return rc;
    }

    /*
     * Set up the basic pulse audio bits (remember the destructore is always called).
     */
    //pThis->fAbortLoop = false;
    pThis->pMainLoop = pa_threaded_mainloop_new();
    if (!pThis->pMainLoop)
    {
        LogRel(("PulseAudio: Failed to allocate main loop: %s\n", pa_strerror(pa_context_errno(pThis->pContext))));
        return VERR_NO_MEMORY;
    }

    pThis->pContext = pa_context_new(pa_threaded_mainloop_get_api(pThis->pMainLoop), "VirtualBox");
    if (!pThis->pContext)
    {
        LogRel(("PulseAudio: Failed to allocate context: %s\n", pa_strerror(pa_context_errno(pThis->pContext))));
        return VERR_NO_MEMORY;
    }

    if (pa_threaded_mainloop_start(pThis->pMainLoop) < 0)
    {
        LogRel(("PulseAudio: Failed to start threaded mainloop: %s\n", pa_strerror(pa_context_errno(pThis->pContext))));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /*
     * Connect to the pulse audio server.
     *
     * We install an init state callback so we can do a timed wait in case
     * connecting to the pulseaudio server should take too long.
     */
    pThis->InitStateChgCtx.hEvtInit    = NIL_RTSEMEVENT;
    pThis->InitStateChgCtx.enmCtxState = PA_CONTEXT_UNCONNECTED;
    rc = RTSemEventCreate(&pThis->InitStateChgCtx.hEvtInit);
    AssertLogRelRCReturn(rc, rc);

    pa_threaded_mainloop_lock(pThis->pMainLoop);
    pa_context_set_state_callback(pThis->pContext, drvHostAudioPaCtxCallbackStateChangedInit, &pThis->InitStateChgCtx);
    if (!pa_context_connect(pThis->pContext, NULL /* pszServer */, PA_CONTEXT_NOFLAGS, NULL))
    {
        pa_threaded_mainloop_unlock(pThis->pMainLoop);

        rc = RTSemEventWait(pThis->InitStateChgCtx.hEvtInit, RT_MS_10SEC); /* 10 seconds should be plenty. */
        if (RT_SUCCESS(rc))
        {
            if (pThis->InitStateChgCtx.enmCtxState == PA_CONTEXT_READY)
            {
                /* Install the main state changed callback to know if something happens to our acquired context. */
                pa_threaded_mainloop_lock(pThis->pMainLoop);
                pa_context_set_state_callback(pThis->pContext, drvHostAudioPaCtxCallbackStateChanged, pThis /* pvUserData */);
                pa_threaded_mainloop_unlock(pThis->pMainLoop);
            }
            else
            {
                LogRel(("PulseAudio: Failed to initialize context (state %d, rc=%Rrc)\n", pThis->InitStateChgCtx.enmCtxState, rc));
                rc = VERR_AUDIO_BACKEND_INIT_FAILED;
            }
        }
        else
        {
            LogRel(("PulseAudio: Waiting for context to become ready failed: %Rrc\n", rc));
            rc = VERR_AUDIO_BACKEND_INIT_FAILED;
        }
    }
    else
    {
        pa_threaded_mainloop_unlock(pThis->pMainLoop);
        LogRel(("PulseAudio: Failed to connect to server: %s\n", pa_strerror(pa_context_errno(pThis->pContext))));
        rc = VERR_AUDIO_BACKEND_INIT_FAILED; /* bird: This used to be VINF_SUCCESS. */
    }

    RTSemEventDestroy(pThis->InitStateChgCtx.hEvtInit);
    pThis->InitStateChgCtx.hEvtInit = NIL_RTSEMEVENT;

    return rc;
}


/**
 * Pulse audio driver registration record.
 */
const PDMDRVREG g_DrvHostPulseAudio =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "PulseAudio",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Pulse Audio host driver",
    /* fFlags */
     PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVHOSTPULSEAUDIO),
    /* pfnConstruct */
    drvHostAudioPaConstruct,
    /* pfnDestruct */
    drvHostAudioPaDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    drvHostAudioPaPowerOff,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

