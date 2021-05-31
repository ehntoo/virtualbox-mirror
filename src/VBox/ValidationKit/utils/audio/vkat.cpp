/* $Id$ */
/** @file
 * Validation Kit Audio Test (VKAT) utility for testing and validating the audio stack.
 */

/*
 * Copyright (C) 2021 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <iprt/buildconfig.h>
#include <iprt/ctype.h>
#include <iprt/dir.h>
#include <iprt/errcore.h>
#include <iprt/initterm.h>
#include <iprt/getopt.h>
#include <iprt/message.h>
#include <iprt/path.h>
#include <iprt/process.h>
#include <iprt/rand.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/uuid.h>
#include <iprt/test.h>

#include <package-generated.h>
#include "product-generated.h"

#include <VBox/version.h>
#include <VBox/log.h>

#ifdef RT_OS_WINDOWS
# include <iprt/win/windows.h> /* for CoInitializeEx */
#endif

/**
 * Internal driver instance data
 * @note This must be put here as it's needed before pdmdrv.h is included.
 */
typedef struct PDMDRVINSINT
{
    /** The stack the drive belongs to. */
    struct AUDIOTESTDRVSTACK *pStack;
} PDMDRVINSINT;
#define PDMDRVINSINT_DECLARED

#include <VBox/vmm/pdmaudioinline.h>
#include <VBox/vmm/pdmaudiohostenuminline.h>

#include "Audio/AudioHlp.h"
#include "Audio/AudioTest.h"
#include "Audio/AudioTestService.h"
#include "Audio/AudioTestServiceClient.h"

#include "VBoxDD.h"

#include "vkatInternal.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** For use in the option switch to handle common options. */
#define AUDIO_TEST_COMMON_OPTION_CASES(a_ValueUnion) \
            case 'q': \
                g_uVerbosity = 0; \
                if (g_pRelLogger) \
                    RTLogGroupSettings(g_pRelLogger, "all=0 all.e"); \
                break; \
            \
            case 'v': \
                g_uVerbosity++; \
                if (g_pRelLogger) \
                    RTLogGroupSettings(g_pRelLogger, g_uVerbosity == 1 ? "all.e.l" : g_uVerbosity == 2 ? "all.e.l.f" : "all=~0"); \
                break; \
            \
            case 'V': \
                return audioTestVersion(); \
            \
            case 'h': \
                return audioTestUsage(g_pStdOut); \
            \
            case AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_ENABLE: \
                g_fDrvAudioDebug = true; \
                break; \
            \
            case AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_PATH: \
                g_pszDrvAudioDebug = (a_ValueUnion).psz; \
                break


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Enumeration specifying the current audio test mode.
 */
typedef enum AUDIOTESTMODE
{
    /** Unknown mode. */
    AUDIOTESTMODE_UNKNOWN = 0,
    /** VKAT is running on the guest side. */
    AUDIOTESTMODE_GUEST,
    /** VKAT is running on the host side. */
    AUDIOTESTMODE_HOST
} AUDIOTESTMODE;

struct AUDIOTESTENV;
/** Pointer a audio test environment. */
typedef AUDIOTESTENV *PAUDIOTESTENV;

struct AUDIOTESTDESC;
/** Pointer a audio test descriptor. */
typedef AUDIOTESTDESC *PAUDIOTESTDESC;

/**
 * Callback to set up the test parameters for a specific test.
 *
 * @returns IPRT status code.
 * @retval  VINF_SUCCESS    if setting the parameters up succeeded. Any other error code
 *                          otherwise indicating the kind of error.
 * @param   pszTest         Test name.
 * @param   pTstParmsAcq    The audio test parameters to set up.
 */
typedef DECLCALLBACKTYPE(int, FNAUDIOTESTSETUP,(PAUDIOTESTENV pTstEnv, PAUDIOTESTDESC pTstDesc, PAUDIOTESTPARMS pTstParmsAcq, void **ppvCtx));
/** Pointer to an audio test setup callback. */
typedef FNAUDIOTESTSETUP *PFNAUDIOTESTSETUP;

typedef DECLCALLBACKTYPE(int, FNAUDIOTESTEXEC,(PAUDIOTESTENV pTstEnv, void *pvCtx, PAUDIOTESTPARMS pTstParms));
/** Pointer to an audio test exec callback. */
typedef FNAUDIOTESTEXEC *PFNAUDIOTESTEXEC;

typedef DECLCALLBACKTYPE(int, FNAUDIOTESTDESTROY,(PAUDIOTESTENV pTstEnv, void *pvCtx));
/** Pointer to an audio test destroy callback. */
typedef FNAUDIOTESTDESTROY *PFNAUDIOTESTDESTROY;

/**
 * Structure for keeping an audio test audio stream.
 */
typedef struct AUDIOTESTSTREAM
{
    /** The PDM stream. */
    PPDMAUDIOSTREAM         pStream;
    /** The backend stream. */
    PPDMAUDIOBACKENDSTREAM  pBackend;
    /** The stream config. */
    PDMAUDIOSTREAMCFG       Cfg;
} AUDIOTESTSTREAM;
/** Pointer to audio test stream. */
typedef AUDIOTESTSTREAM *PAUDIOTESTSTREAM;

/**
 * Audio test environment parameters.
 * Not necessarily bound to a specific test (can be reused).
 */
typedef struct AUDIOTESTENV
{
    /** Audio testing mode. */
    AUDIOTESTMODE           enmMode;
    /** Output path for storing the test environment's final test files. */
    char                    szPathOut[RTPATH_MAX];
    /** Temporary path for this test environment. */
    char                    szPathTemp[RTPATH_MAX];
    /** The audio test driver stack. */
    AUDIOTESTDRVSTACK       DrvStack;
    /** The current (last) audio device enumeration to use. */
    PDMAUDIOHOSTENUM        DevEnum;
    /** Audio stream. */
    AUDIOTESTSTREAM         aStreams[AUDIOTESTENV_MAX_STREAMS];
    /** The audio test set to use. */
    AUDIOTESTSET            Set;
    union
    {
        struct
        {
            /** ATS instance to use. */
            ATSSERVER       Srv;
        } Guest;
        struct
        {
            ATSCLIENT       Client;
        } Host;
    } u;
} AUDIOTESTENV;

/**
 * Audio test descriptor.
 */
typedef struct AUDIOTESTDESC
{
    /** (Sort of) Descriptive test name. */
    const char             *pszName;
    /** Flag whether the test is excluded. */
    bool                    fExcluded;
    /** The setup callback. */
    PFNAUDIOTESTSETUP       pfnSetup;
    /** The exec callback. */
    PFNAUDIOTESTEXEC        pfnExec;
    /** The destruction callback. */
    PFNAUDIOTESTDESTROY     pfnDestroy;
} AUDIOTESTDESC;

/**
 * Structure for keeping a user context for the test service callbacks.
 */
typedef struct ATSCALLBACKCTX
{
    PAUDIOTESTENV pTstEnv;
} ATSCALLBACKCTX;
typedef ATSCALLBACKCTX *PATSCALLBACKCTX;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static int audioTestCombineParms(PAUDIOTESTPARMS pBaseParms, PAUDIOTESTPARMS pOverrideParms);
static int audioTestPlayTone(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream, PAUDIOTESTTONEPARMS pParms);
static int audioTestStreamDestroy(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream);

static RTEXITCODE audioTestUsage(PRTSTREAM pStrm);
static RTEXITCODE audioTestVersion(void);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/**
 * Common long options values.
 */
enum
{
    AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_ENABLE = 256,
    AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_PATH
};

/**
 * Long option values for the 'test' command.
 */
enum
{
    VKAT_TEST_OPT_COUNT = 900,
    VKAT_TEST_OPT_DEV,
    VKAT_TEST_OPT_ATS_ADDR,
    VKAT_TEST_OPT_ATS_PORT,
    VKAT_TEST_OPT_MODE,
    VKAT_TEST_OPT_OUTDIR,
    VKAT_TEST_OPT_PAUSE,
    VKAT_TEST_OPT_PCM_HZ,
    VKAT_TEST_OPT_PCM_BIT,
    VKAT_TEST_OPT_PCM_CHAN,
    VKAT_TEST_OPT_PCM_SIGNED,
    VKAT_TEST_OPT_TAG,
    VKAT_TEST_OPT_TEMPDIR,
    VKAT_TEST_OPT_VOL
};

/**
 * Long option values for the 'verify' command.
 */
enum
{
    VKAT_VERIFY_OPT_TAG = 900
};

/**
 * Long option values for the 'selftest' command.
 */
enum
{
    VKAT_SELFTEST_OPT_ATS_HOST = 900
};

/**
 * Common command line parameters.
 */
static const RTGETOPTDEF g_aCmdCommonOptions[] =
{
    { "--quiet",            'q',                                        RTGETOPT_REQ_NOTHING },
    { "--verbose",          'v',                                        RTGETOPT_REQ_NOTHING },
    { "--debug-audio",      AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_ENABLE,      RTGETOPT_REQ_NOTHING },
    { "--debug-audio-path", AUDIO_TEST_OPT_CMN_DEBUG_AUDIO_PATH,        RTGETOPT_REQ_STRING  },
};

/**
 * Command line parameters for test mode.
 */
static const RTGETOPTDEF g_aCmdTestOptions[] =
{
    { "--backend",          'b',                          RTGETOPT_REQ_STRING  },
    { "--drvaudio",         'd',                          RTGETOPT_REQ_NOTHING },
    { "--exclude",          'e',                          RTGETOPT_REQ_UINT32  },
    { "--exclude-all",      'a',                          RTGETOPT_REQ_NOTHING },
    { "--mode",             VKAT_TEST_OPT_MODE,           RTGETOPT_REQ_STRING  },
    { "--ats-address",      VKAT_TEST_OPT_ATS_ADDR,       RTGETOPT_REQ_STRING  },
    { "--ats-port",         VKAT_TEST_OPT_ATS_PORT,       RTGETOPT_REQ_UINT32  },
    { "--include",          'i',                          RTGETOPT_REQ_UINT32  },
    { "--outdir",           VKAT_TEST_OPT_OUTDIR,         RTGETOPT_REQ_STRING  },
    { "--count",            VKAT_TEST_OPT_COUNT,          RTGETOPT_REQ_UINT32  },
    { "--device",           VKAT_TEST_OPT_DEV,            RTGETOPT_REQ_STRING  },
    { "--pause",            VKAT_TEST_OPT_PAUSE,          RTGETOPT_REQ_UINT32  },
    { "--pcm-bit",          VKAT_TEST_OPT_PCM_BIT,        RTGETOPT_REQ_UINT8   },
    { "--pcm-chan",         VKAT_TEST_OPT_PCM_CHAN,       RTGETOPT_REQ_UINT8   },
    { "--pcm-hz",           VKAT_TEST_OPT_PCM_HZ,         RTGETOPT_REQ_UINT16  },
    { "--pcm-signed",       VKAT_TEST_OPT_PCM_SIGNED,     RTGETOPT_REQ_BOOL    },
    { "--tag",              VKAT_TEST_OPT_TAG,            RTGETOPT_REQ_STRING  },
    { "--tempdir",          VKAT_TEST_OPT_TEMPDIR,        RTGETOPT_REQ_STRING  },
    { "--volume",           VKAT_TEST_OPT_VOL,            RTGETOPT_REQ_UINT8   }
};

/**
 * Command line parameters for verification mode.
 */
static const RTGETOPTDEF g_aCmdVerifyOptions[] =
{
    { "--tag",              VKAT_VERIFY_OPT_TAG,          RTGETOPT_REQ_STRING  }
};

/**
 * Backends.
 *
 * @note The first backend in the array is the default one for the platform.
 */
static struct
{
    /** The driver registration structure. */
    PCPDMDRVREG pDrvReg;
    /** The backend name.
     * Aliases are implemented by having multiple entries for the same backend.  */
    const char *pszName;
} const g_aBackends[] =
{
#if defined(VBOX_WITH_AUDIO_ALSA) && defined(RT_OS_LINUX)
    {   &g_DrvHostALSAAudio,          "alsa" },
#endif
#ifdef VBOX_WITH_AUDIO_PULSE
    {   &g_DrvHostPulseAudio,         "pulseaudio" },
    {   &g_DrvHostPulseAudio,         "pulse" },
    {   &g_DrvHostPulseAudio,         "pa" },
#endif
#ifdef VBOX_WITH_AUDIO_OSS
    {   &g_DrvHostOSSAudio,           "oss" },
#endif
#if defined(RT_OS_DARWIN)
    {   &g_DrvHostCoreAudio,          "coreaudio" },
    {   &g_DrvHostCoreAudio,          "core" },
    {   &g_DrvHostCoreAudio,          "ca" },
#endif
#if defined(RT_OS_WINDOWS)
    {   &g_DrvHostAudioWas,           "wasapi" },
    {   &g_DrvHostAudioWas,           "was" },
    {   &g_DrvHostDSound,             "directsound" },
    {   &g_DrvHostDSound,             "dsound" },
    {   &g_DrvHostDSound,             "ds" },
#endif
    {   &g_DrvHostValidationKitAudio, "valkit" }
};
AssertCompile(sizeof(g_aBackends) > 0 /* port me */);

static volatile bool g_fTerminated = false;
/** The release logger. */
static PRTLOGGER    g_pRelLogger = NULL;


/** The test handle. */
RTTEST        g_hTest;
/** The current verbosity level. */
unsigned      g_uVerbosity = 0;
/** DrvAudio: Enable debug (or not). */
bool          g_fDrvAudioDebug = 0;
/** DrvAudio: The debug output path. */
const char   *g_pszDrvAudioDebug = NULL;


/*********************************************************************************************************************************
*   ATS Callback Implementations                                                                                                 *
*********************************************************************************************************************************/

/**
 * Note: Called within server (client serving) thread.
 */
static DECLCALLBACK(int) audioTestSvcTonePlayCallback(void const *pvUser, PPDMAUDIOSTREAMCFG pStreamCfg, PAUDIOTESTTONEPARMS pToneParms)
{
    PATSCALLBACKCTX pCtx    = (PATSCALLBACKCTX)pvUser;
    PAUDIOTESTENV   pTstEnv = pCtx->pTstEnv;

    AUDIOTESTTONE TstTone;
    AudioTestToneInitRandom(&TstTone, &pStreamCfg->Props);

    int rc;

    const PPDMAUDIOSTREAM pStream = pTstEnv->aStreams[0].pStream; /** @todo Make this dynamic. */

    if (audioTestDriverStackStreamIsOkay(&pTstEnv->DrvStack, pStream))
    {
        uint32_t cbBuf;
        uint8_t  abBuf[_4K];

        const uint64_t tsStartMs     = RTTimeMilliTS();
        const uint16_t cSchedulingMs = RTRandU32Ex(10, 80); /* Chose a random scheduling (in ms). */
        const uint32_t cbPerMs       = PDMAudioPropsMilliToBytes(&pStream->Props, cSchedulingMs);

        do
        {
            rc = AudioTestToneGenerate(&TstTone, abBuf, RT_MIN(cbPerMs, sizeof(abBuf)), &cbBuf);
            if (RT_SUCCESS(rc))
            {
                uint32_t cbWritten;
                rc = audioTestDriverStackStreamPlay(&pTstEnv->DrvStack, pStream, abBuf, cbBuf, &cbWritten);
            }

            if (RTTimeMilliTS() - tsStartMs >= pToneParms->msDuration)
                break;

            if (RT_FAILURE(rc))
                break;

            RTThreadSleep(cSchedulingMs);

        } while (RT_SUCCESS(rc));
    }
    else
        rc = VERR_AUDIO_STREAM_NOT_READY;

    return rc;
}


/*********************************************************************************************************************************
*   Implementation of audio test environment handling                                                                            *
*********************************************************************************************************************************/

/**
 * Initializes an audio test environment.
 *
 * @param   pTstEnv             Audio test environment to initialize.
 * @param   pDrvReg             Audio driver to use.
 * @param   fWithDrvAudio       Whether to include DrvAudio in the stack or not.
 * @param   pszTag              Tag name to use. If NULL, a generated UUID will be used.
 * @param   pszTcpAddr          TCP/IP address to connect to.
 * @param   uTcpPort            TCP/IP port to connect to.
 */
static int audioTestEnvInit(PAUDIOTESTENV pTstEnv,
                            PCPDMDRVREG pDrvReg, bool fWithDrvAudio, const char *pszTag,
                            const char *pszTcpAddr, uint32_t uTcpPort)
{
    PDMAudioHostEnumInit(&pTstEnv->DevEnum);

    int rc = audioTestDriverStackInit(&pTstEnv->DrvStack, pDrvReg, fWithDrvAudio);
    if (RT_FAILURE(rc))
        return rc;

    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Test mode is '%s'\n", pTstEnv->enmMode == AUDIOTESTMODE_HOST ? "host" : "guest");
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Using tag '%s'\n", pszTag);
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Output directory is '%s'\n", pTstEnv->szPathOut);
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Temp directory is '%s'\n", pTstEnv->szPathTemp);

    char szPathTemp[RTPATH_MAX];
    if (   !strlen(pTstEnv->szPathTemp)
        || !strlen(pTstEnv->szPathOut))
        rc = RTPathTemp(szPathTemp, sizeof(szPathTemp));

    if (   RT_SUCCESS(rc)
        && !strlen(pTstEnv->szPathTemp))
        rc = RTPathJoin(pTstEnv->szPathTemp, sizeof(pTstEnv->szPathTemp), szPathTemp, "vkat-temp");

    if (   RT_SUCCESS(rc)
        && !strlen(pTstEnv->szPathOut))
        rc = RTPathJoin(pTstEnv->szPathOut, sizeof(pTstEnv->szPathOut), szPathTemp, "vkat");

    if (RT_SUCCESS(rc))
        rc = AudioTestSetCreate(&pTstEnv->Set, pTstEnv->szPathTemp, pszTag);

    if (RT_FAILURE(rc))
        return rc;

    /** @todo Implement NAT mode like we do for TxS later? */
    if (pTstEnv->enmMode == AUDIOTESTMODE_GUEST)
    {
        ATSCALLBACKCTX Ctx;
        Ctx.pTstEnv = pTstEnv;

        ATSCALLBACKS Callbacks;
        Callbacks.pfnTonePlay = audioTestSvcTonePlayCallback;
        Callbacks.pvUser      = &Ctx;

        RTTestPrintf(g_hTest, RTTESTLVL_DEBUG, "Starting ATS ...\n");
        rc = AudioTestSvcInit(&pTstEnv->u.Guest.Srv, &Callbacks);
        if (RT_SUCCESS(rc))
            rc = AudioTestSvcStart(&pTstEnv->u.Guest.Srv);

        if (RT_FAILURE(rc))
        {
            RTTestFailed(g_hTest, "Initializing ATS failed with %Rrc\n", rc);
            return rc;
        }

        RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "ATS running\n");

        while (!g_fTerminated) /** @todo Implement signal handling. */
        {
            RTThreadSleep(100);
        }

        RTTestPrintf(g_hTest, RTTESTLVL_DEBUG, "Shutting down ATS ...\n");

        int rc2 = AudioTestSvcShutdown(&pTstEnv->u.Guest.Srv);
        if (RT_SUCCESS(rc))
            rc = rc2;
    }
    else
    {
        RTTestPrintf(g_hTest, RTTESTLVL_DEBUG, "Connecting to ATS at %s:%RU32 ...\n", pszTcpAddr, uTcpPort);
        rc = AudioTestSvcClientConnect(&pTstEnv->u.Host.Client, pszTcpAddr, uTcpPort);
        if (RT_FAILURE(rc))
        {
            RTTestFailed(g_hTest, "Connecting to ATS failed with %Rrc\n", rc);
            return rc;
        }
    }

    audioTestDriverStackDelete(&pTstEnv->DrvStack);

    return rc;
}

/**
 * Destroys an audio test environment.
 *
 * @param   pTstEnv             Audio test environment to destroy.
 */
static void audioTestEnvDestroy(PAUDIOTESTENV pTstEnv)
{
    if (!pTstEnv)
        return;

    PDMAudioHostEnumDelete(&pTstEnv->DevEnum);

    for (unsigned i = 0; i < RT_ELEMENTS(pTstEnv->aStreams); i++)
    {
        int rc2 = audioTestStreamDestroy(pTstEnv, &pTstEnv->aStreams[i]);
        if (RT_FAILURE(rc2))
            RTTestFailed(g_hTest, "Stream destruction for stream #%u failed with %Rrc\n", i, rc2);
    }

    AudioTestSetDestroy(&pTstEnv->Set);
    audioTestDriverStackDelete(&pTstEnv->DrvStack);
}

/**
 * Initializes an audio test parameters set.
 *
 * @param   pTstParms           Test parameters set to initialize.
 */
static void audioTestParmsInit(PAUDIOTESTPARMS pTstParms)
{
    RT_ZERO(*pTstParms);
}

/**
 * Destroys an audio test parameters set.
 *
 * @param   pTstParms           Test parameters set to destroy.
 */
static void audioTestParmsDestroy(PAUDIOTESTPARMS pTstParms)
{
    if (!pTstParms)
        return;

    return;
}


/*********************************************************************************************************************************
*   Device enumeration + handling.                                                                                               *
*********************************************************************************************************************************/

/**
 * Enumerates audio devices and optionally searches for a specific device.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test env to use for enumeration.
 * @param   pszDev              Device name to search for. Can be NULL if the default device shall be used.
 * @param   ppDev               Where to return the pointer of the device enumeration of \a pTstEnv when a
 *                              specific device was found.
 */
static int audioTestDevicesEnumerateAndCheck(PAUDIOTESTENV pTstEnv, const char *pszDev, PPDMAUDIOHOSTDEV *ppDev)
{
    RTTestSubF(g_hTest, "Enumerating audio devices and checking for device '%s'", pszDev ? pszDev : "<Default>");

    if (!pTstEnv->DrvStack.pIHostAudio->pfnGetDevices)
    {
        RTTestSkipped(g_hTest, "Backend does not support device enumeration, skipping");
        return VINF_NOT_SUPPORTED;
    }

    Assert(pszDev == NULL || ppDev);

    if (ppDev)
        *ppDev = NULL;

    int rc = pTstEnv->DrvStack.pIHostAudio->pfnGetDevices(pTstEnv->DrvStack.pIHostAudio, &pTstEnv->DevEnum);
    if (RT_SUCCESS(rc))
    {
        PPDMAUDIOHOSTDEV pDev;
        RTListForEach(&pTstEnv->DevEnum.LstDevices, pDev, PDMAUDIOHOSTDEV, ListEntry)
        {
            char szFlags[PDMAUDIOHOSTDEV_MAX_FLAGS_STRING_LEN];
            if (pDev->pszId)
                RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum: Device '%s' (ID '%s'):\n", pDev->szName, pDev->pszId);
            else
                RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum: Device '%s':\n", pDev->szName);
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum:   Usage           = %s\n",   PDMAudioDirGetName(pDev->enmUsage));
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum:   Flags           = %s\n",   PDMAudioHostDevFlagsToString(szFlags, pDev->fFlags));
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum:   Input channels  = %RU8\n", pDev->cMaxInputChannels);
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Enum:   Output channels = %RU8\n", pDev->cMaxOutputChannels);

            if (   pszDev
                && !RTStrCmp(pDev->szName, pszDev))
            {
                *ppDev = pDev;
            }
        }
    }
    else
        RTTestFailed(g_hTest, "Enumerating audio devices failed with %Rrc", rc);

    RTTestSubDone(g_hTest);

    if (   pszDev
        && *ppDev == NULL)
    {
        RTTestFailed(g_hTest, "Audio device '%s' not found", pszDev);
        return VERR_NOT_FOUND;
    }

    return VINF_SUCCESS;
}

/**
 * Opens an audio device.
 *
 * @returns VBox status code.
 * @param   pDev                Audio device to open.
 */
static int audioTestDeviceOpen(PPDMAUDIOHOSTDEV pDev)
{
    int rc = VINF_SUCCESS;

    RTTestSubF(g_hTest, "Opening audio device '%s' ...", pDev->szName);

    /** @todo Detect + open device here. */

    RTTestSubDone(g_hTest);

    return rc;
}

/**
 * Closes an audio device.
 *
 * @returns VBox status code.
 * @param   pDev                Audio device to close.
 */
static int audioTestDeviceClose(PPDMAUDIOHOSTDEV pDev)
{
    int rc = VINF_SUCCESS;

    RTTestSubF(g_hTest, "Closing audio device '%s' ...", pDev->szName);

    /** @todo Close device here. */

    RTTestSubDone(g_hTest);

    return rc;
}

/**
 * Destroys an audio test stream.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment the stream to destroy contains.
 * @param   pStream             Audio stream to destroy.
 */
static int audioTestStreamDestroy(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream)
{
    int rc = VINF_SUCCESS;
    if (pStream && pStream->pStream)
    {
        /** @todo Anything else to do here, e.g. test if there are left over samples or some such? */

        audioTestDriverStackStreamDestroy(&pTstEnv->DrvStack, pStream->pStream);
        pStream->pStream  = NULL;
        pStream->pBackend = NULL;
    }

    return rc;
}

/**
 * Creates an audio default input (recording) test stream.
 * Convenience function.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for creating the stream.
 * @param   pStream             Audio stream to create.
 * @param   pProps              PCM properties to use for creation.
 */
static int audioTestCreateStreamDefaultIn(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream, PPDMAUDIOPCMPROPS pProps)
{
    pStream->pBackend = NULL;
    int rc = audioTestDriverStackStreamCreateInput(&pTstEnv->DrvStack, pProps, 300 /*cMsBufferSize*/, 150 /*cMsPreBuffer*/,
                                                   10 /*cMsSchedulingHint*/, &pStream->pStream, &pStream->Cfg);
    if (RT_SUCCESS(rc) && !pTstEnv->DrvStack.pIAudioConnector)
        pStream->pBackend = &((PAUDIOTESTDRVSTACKSTREAM)pStream->pStream)->Backend;
    return rc;
}

/**
 * Records a test tone from a specific audio test stream.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for running the test.
 * @param   pStream             Stream to use for recording the tone.
 * @param   pParms              Tone parameters to use.
 *
 * @note    Blocking function.
 */
static int audioTestRecordTone(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream, PAUDIOTESTTONEPARMS pParms)
{
    const char *pcszPathOut = pTstEnv->Set.szPathAbs;

    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Recording test tone (for %RU32ms)\n", pParms->msDuration);
    RTTestPrintf(g_hTest, RTTESTLVL_DEBUG,  "Writing to '%s'\n", pcszPathOut);

    /** @todo Use .WAV here? */
    PAUDIOTESTOBJ pObj;
    int rc = AudioTestSetObjCreateAndRegister(&pTstEnv->Set, "tone-rec.pcm", &pObj);
    AssertRCReturn(rc, rc);

    if (audioTestDriverStackStreamIsOkay(&pTstEnv->DrvStack, pStream->pStream))
    {
        uint8_t abBuf[_4K];

        const uint64_t tsStartMs     = RTTimeMilliTS();
        const uint16_t cSchedulingMs = RTRandU32Ex(10, 80); /* Choose a random scheduling (in ms). */

        do
        {
            uint32_t cbRead = 0;
            rc = audioTestDriverStackStreamCapture(&pTstEnv->DrvStack, pStream->pStream, (void *)abBuf, sizeof(abBuf), &cbRead);
            if (RT_SUCCESS(rc))
                rc = AudioTestSetObjWrite(pObj, abBuf, cbRead);

            if (RT_FAILURE(rc))
                break;

            if (RTTimeMilliTS() - tsStartMs >= pParms->msDuration)
                break;

            RTThreadSleep(cSchedulingMs);

        } while (RT_SUCCESS(rc));
    }
    else
        rc = VERR_AUDIO_STREAM_NOT_READY;

    int rc2 = AudioTestSetObjClose(pObj);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

/**
 * Creates an audio default output (playback) test stream.
 * Convenience function.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for creating the stream.
 * @param   pStream             Audio stream to create.
 * @param   pProps              PCM properties to use for creation.
 */
static int audioTestCreateStreamDefaultOut(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream, PPDMAUDIOPCMPROPS pProps)
{
    pStream->pBackend = NULL;
    int rc = audioTestDriverStackStreamCreateOutput(&pTstEnv->DrvStack, pProps, 300 /*cMsBufferSize*/, 200 /*cMsPreBuffer*/,
                                                    10 /*cMsSchedulingHint*/, &pStream->pStream, &pStream->Cfg);
    if (RT_SUCCESS(rc) && !pTstEnv->DrvStack.pIAudioConnector)
        pStream->pBackend = &((PAUDIOTESTDRVSTACKSTREAM)pStream->pStream)->Backend;
    return rc;
}

/**
 * Plays a test tone on a specific audio test stream.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for running the test.
 * @param   pStream             Stream to use for playing the tone.
 * @param   pParms              Tone parameters to use.
 *
 * @note    Blocking function.
 */
static int audioTestPlayTone(PAUDIOTESTENV pTstEnv, PAUDIOTESTSTREAM pStream, PAUDIOTESTTONEPARMS pParms)
{
    AUDIOTESTTONE TstTone;
    AudioTestToneInit(&TstTone, &pParms->Props, pParms->dbFreqHz);

    const char *pcszPathOut = pTstEnv->Set.szPathAbs;

    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Playing test tone (tone frequency is %RU16Hz, %RU32ms)\n", (uint16_t)pParms->dbFreqHz, pParms->msDuration);
    RTTestPrintf(g_hTest, RTTESTLVL_DEBUG,  "Writing to '%s'\n", pcszPathOut);

    /** @todo Use .WAV here? */
    PAUDIOTESTOBJ pObj;
    int rc = AudioTestSetObjCreateAndRegister(&pTstEnv->Set, "tone-play.pcm", &pObj);
    AssertRCReturn(rc, rc);

    if (audioTestDriverStackStreamIsOkay(&pTstEnv->DrvStack, pStream->pStream))
    {
        uint32_t cbBuf;
        uint8_t  abBuf[_4K];

        const uint64_t tsStartMs     = RTTimeMilliTS();
        const uint16_t cSchedulingMs = RTRandU32Ex(10, 80); /* Choose a random scheduling (in ms). */
        const uint32_t cbPerMs       = PDMAudioPropsMilliToBytes(&pParms->Props, cSchedulingMs);

        do
        {
            rc = AudioTestToneGenerate(&TstTone, abBuf, RT_MIN(cbPerMs, sizeof(abBuf)), &cbBuf);
            if (RT_SUCCESS(rc))
            {
                /* Write stuff to disk before trying to play it. Help analysis later. */
                rc = AudioTestSetObjWrite(pObj, abBuf, cbBuf);
                if (RT_SUCCESS(rc))
                {
                    uint32_t cbWritten;
                    rc = audioTestDriverStackStreamPlay(&pTstEnv->DrvStack, pStream->pStream,
                                                        abBuf, cbBuf, &cbWritten);
                }
            }

            if (RTTimeMilliTS() - tsStartMs >= pParms->msDuration)
                break;

            if (RT_FAILURE(rc))
                break;

            RTThreadSleep(cSchedulingMs);

        } while (RT_SUCCESS(rc));
    }
    else
        rc = VERR_AUDIO_STREAM_NOT_READY;

    int rc2 = AudioTestSetObjClose(pObj);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

/**
 * Overrides audio test base parameters with another set.
 *
 * @returns VBox status code.
 * @param   pBaseParms          Base parameters to override.
 * @param   pOverrideParms      Override parameters to use for overriding the base parameters.
 *
 * @note    Overriding a parameter depends on its type / default values.
 */
static int audioTestCombineParms(PAUDIOTESTPARMS pBaseParms, PAUDIOTESTPARMS pOverrideParms)
{
    RT_NOREF(pBaseParms, pOverrideParms);

    /** @todo Implement parameter overriding. */
    return VERR_NOT_IMPLEMENTED;
}


/*********************************************************************************************************************************
*   Test callbacks                                                                                                               *
*********************************************************************************************************************************/

/**
 * @copydoc FNAUDIOTESTSETUP
 */
static DECLCALLBACK(int) audioTestPlayToneSetup(PAUDIOTESTENV pTstEnv, PAUDIOTESTDESC pTstDesc, PAUDIOTESTPARMS pTstParmsAcq, void **ppvCtx)
{
    RT_NOREF(pTstEnv, pTstDesc, ppvCtx);

    pTstParmsAcq->enmType     = AUDIOTESTTYPE_TESTTONE_PLAY;

    PDMAudioPropsInit(&pTstParmsAcq->Props, 16 /* bit */ / 8, true /* fSigned */, 2 /* Channels */, 44100 /* Hz */);

    pTstParmsAcq->enmDir      = PDMAUDIODIR_OUT;
#ifdef DEBUG
    pTstParmsAcq->cIterations = 2;
#else
    pTstParmsAcq->cIterations = RTRandU32Ex(1, 10);
#endif
    pTstParmsAcq->idxCurrent  = 0;

    return VINF_SUCCESS;
}

/**
 * @copydoc FNAUDIOTESTEXEC
 */
static DECLCALLBACK(int) audioTestPlayToneExec(PAUDIOTESTENV pTstEnv, void *pvCtx, PAUDIOTESTPARMS pTstParms)
{
    RT_NOREF(pvCtx);

    int rc = VINF_SUCCESS;

    PAUDIOTESTSTREAM pStream = &pTstEnv->aStreams[0];

    for (uint32_t i = 0; i < pTstParms->cIterations; i++)
    {
        AudioTestToneParamsInitRandom(&pTstParms->TestTone, &pTstParms->Props);

        PAUDIOTESTENTRY pTst;
        rc = AudioTestSetTestBegin(&pTstEnv->Set, "Playing test tone", pTstParms, &pTst);
        if (RT_SUCCESS(rc))
        {
            rc = audioTestCreateStreamDefaultOut(pTstEnv, pStream, &pTstParms->TestTone.Props);
            if (RT_SUCCESS(rc))
            {
                rc = audioTestPlayTone(pTstEnv, pStream, &pTstParms->TestTone);
            }

            int rc2 = audioTestStreamDestroy(pTstEnv, pStream);
            if (RT_SUCCESS(rc))
                rc = rc2;

            if (RT_SUCCESS(rc))
            {
               AudioTestSetTestDone(pTst);
            }
            else
                AudioTestSetTestFailed(pTst, rc, "Playing test tone failed");
        }

        if (RT_FAILURE(rc))
            RTTestFailed(g_hTest, "Playing tone failed\n");
    }

    return rc;
}

/**
 * @copydoc FNAUDIOTESTDESTROY
 */
static DECLCALLBACK(int) audioTestPlayToneDestroy(PAUDIOTESTENV pTstEnv, void *pvCtx)
{
    RT_NOREF(pTstEnv, pvCtx);

    return VINF_SUCCESS;
}

/**
 * @copydoc FNAUDIOTESTSETUP
 */
static DECLCALLBACK(int) audioTestRecordToneSetup(PAUDIOTESTENV pTstEnv, PAUDIOTESTDESC pTstDesc, PAUDIOTESTPARMS pTstParmsAcq, void **ppvCtx)
{
    RT_NOREF(pTstEnv, pTstDesc, ppvCtx);

    pTstParmsAcq->enmType     = AUDIOTESTTYPE_TESTTONE_RECORD;

    RT_ZERO(pTstParmsAcq->TestTone);
    PDMAudioPropsInit(&pTstParmsAcq->TestTone.Props, 16 /* bit */ / 8, true /* fSigned */, 2 /* Channels */, 44100 /* Hz */);

    pTstParmsAcq->enmDir      = PDMAUDIODIR_IN;
#ifdef DEBUG
    pTstParmsAcq->cIterations = 2;
#else
    pTstParmsAcq->cIterations = RTRandU32Ex(1, 10);
#endif
    pTstParmsAcq->idxCurrent  = 0;

    return VINF_SUCCESS;
}

/**
 * @copydoc FNAUDIOTESTEXEC
 */
static DECLCALLBACK(int) audioTestRecordToneExec(PAUDIOTESTENV pTstEnv, void *pvCtx, PAUDIOTESTPARMS pTstParms)
{
    RT_NOREF(pvCtx);

    int rc = VINF_SUCCESS;

    PAUDIOTESTSTREAM pStream = &pTstEnv->aStreams[0];

    for (uint32_t i = 0; i < pTstParms->cIterations; i++)
    {
        pTstParms->TestTone.msDuration = RTRandU32Ex(50 /* ms */, RT_MS_10SEC); /** @todo Record even longer? */

        PAUDIOTESTENTRY pTst;
        rc = AudioTestSetTestBegin(&pTstEnv->Set, "Recording test tone", pTstParms, &pTst);
        if (RT_SUCCESS(rc))
        {
            /** @todo  For now we're (re-)creating the recording stream for each iteration. Change that to be random. */
            rc = audioTestCreateStreamDefaultIn(pTstEnv, pStream, &pTstParms->TestTone.Props);
            if (RT_SUCCESS(rc))
                rc = audioTestRecordTone(pTstEnv, pStream, &pTstParms->TestTone);

            int rc2 = audioTestStreamDestroy(pTstEnv, pStream);
            if (RT_SUCCESS(rc))
                rc = rc2;

            if (RT_SUCCESS(rc))
            {
               AudioTestSetTestDone(pTst);
            }
            else
                AudioTestSetTestFailed(pTst, rc, "Recording test tone failed");
        }

        if (RT_FAILURE(rc))
            RTTestFailed(g_hTest, "Recording tone failed\n");
    }

    return rc;
}

/**
 * @copydoc FNAUDIOTESTDESTROY
 */
static DECLCALLBACK(int) audioTestRecordToneDestroy(PAUDIOTESTENV pTstEnv, void *pvCtx)
{
    RT_NOREF(pTstEnv, pvCtx);

    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Test execution                                                                                                               *
*********************************************************************************************************************************/

static AUDIOTESTDESC g_aTests[] =
{
    /* pszTest      fExcluded      pfnSetup */
    { "PlayTone",   false,         audioTestPlayToneSetup,       audioTestPlayToneExec,      audioTestPlayToneDestroy },
    { "RecordTone", false,         audioTestRecordToneSetup,     audioTestRecordToneExec,    audioTestRecordToneDestroy }
};

/**
 * Runs one specific audio test.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for running the test.
 * @param   pTstDesc            Test to run.
 * @param   uSeq                Test sequence # in case there are more tests.
 * @param   pOverrideParms      Test parameters for overriding the actual test parameters. Optional.
 */
static int audioTestOne(PAUDIOTESTENV pTstEnv, PAUDIOTESTDESC pTstDesc,
                        unsigned uSeq, PAUDIOTESTPARMS pOverrideParms)
{
    RT_NOREF(uSeq);

    int rc;

    AUDIOTESTPARMS TstParms;
    audioTestParmsInit(&TstParms);

    RTTestSub(g_hTest, pTstDesc->pszName);

    if (pTstDesc->fExcluded)
    {
        RTTestSkipped(g_hTest, "Excluded from list");
        return VINF_SUCCESS;
    }

    void *pvCtx = NULL; /* Test-specific opaque context. Optional and can be NULL. */

    if (pTstDesc->pfnSetup)
    {
        rc = pTstDesc->pfnSetup(pTstEnv, pTstDesc, &TstParms, &pvCtx);
        if (RT_FAILURE(rc))
        {
            RTTestFailed(g_hTest, "Test setup failed\n");
            return rc;
        }
    }

    audioTestCombineParms(&TstParms, pOverrideParms);

    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Test #%u: %RU32 iterations\n", uSeq, TstParms.cIterations);

    if (strlen(TstParms.Dev.szName)) /** @todo Refine this check. */
        rc = audioTestDeviceOpen(&TstParms.Dev);

    AssertPtr(pTstDesc->pfnExec);
    rc = pTstDesc->pfnExec(pTstEnv, pvCtx, &TstParms);

    RTTestSubDone(g_hTest);

    if (pTstDesc->pfnDestroy)
    {
        int rc2 = pTstDesc->pfnDestroy(pTstEnv, pvCtx);
        if (RT_SUCCESS(rc))
            rc = rc2;

        if (RT_FAILURE(rc2))
            RTTestFailed(g_hTest, "Test destruction failed\n");
    }

    rc = audioTestDeviceClose(&TstParms.Dev);

    audioTestParmsDestroy(&TstParms);

    return rc;
}

/**
 * Runs all specified tests in a row.
 *
 * @returns VBox status code.
 * @param   pTstEnv             Test environment to use for running all tests.
 * @param   pOverrideParms      Test parameters for (some / all) specific test parameters. Optional.
 */
static int audioTestWorker(PAUDIOTESTENV pTstEnv, PAUDIOTESTPARMS pOverrideParms)
{
    int rc = VINF_SUCCESS;

    unsigned uSeq = 0;
    for (unsigned i = 0; i < RT_ELEMENTS(g_aTests); i++)
    {
        int rc2 = audioTestOne(pTstEnv, &g_aTests[i], uSeq, pOverrideParms);
        if (RT_SUCCESS(rc))
            rc = rc2;

        if (!g_aTests[i].fExcluded)
            uSeq++;
    }

    return rc;
}

/** Option help for the 'test' command.   */
static DECLCALLBACK(const char *) audioTestCmdTestHelp(PCRTGETOPTDEF pOpt)
{
    switch (pOpt->iShort)
    {
        case 'd':                 return "Go via DrvAudio instead of directly interfacing with the backend.";
        case VKAT_TEST_OPT_DEV:   return "Use the specified audio device";
        case 'e':                 return "Exclude the given test id from the list";
        case 'a':                 return "Exclude all tests from the list (useful to enable single tests later with --include)";
        case 'i':                 return "Include the given test id in the list";
    }
    return NULL;
}

/**
 * Main (entry) function for the testing functionality of VKAT.
 *
 * @returns Program exit code.
 * @param   pGetState   RTGetOpt state.
 */
static DECLCALLBACK(RTEXITCODE) audioTestMain(PRTGETOPTSTATE pGetState)
{
    AUDIOTESTENV TstEnv;
    RT_ZERO(TstEnv);

    AUDIOTESTPARMS TstCust;
    audioTestParmsInit(&TstCust);

    const char *pszDevice     = NULL; /* Custom device to use. Can be NULL if not being used. */
    const char *pszTag        = NULL; /* Custom tag to use. Can be NULL if not being used. */
    PCPDMDRVREG pDrvReg       = g_aBackends[0].pDrvReg;
    bool        fWithDrvAudio = false;
    uint8_t     cPcmSampleBit = 0;
    uint8_t     cPcmChannels  = 0;
    uint32_t    uPcmHz        = 0;
    bool        fPcmSigned    = true;
    const char *pszTcpAddr    = NULL;
    uint16_t    uTcpPort      = 0;

    int           rc;
    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(pGetState, &ValueUnion)))
    {
        switch (rc)
        {
            case 'a':
                for (unsigned i = 0; i < RT_ELEMENTS(g_aTests); i++)
                    g_aTests[i].fExcluded = true;
                break;

            case 'b':
                pDrvReg = NULL;
                for (uintptr_t i = 0; i < RT_ELEMENTS(g_aBackends); i++)
                    if (   strcmp(ValueUnion.psz, g_aBackends[i].pszName) == 0
                        || strcmp(ValueUnion.psz, g_aBackends[i].pDrvReg->szName) == 0)
                    {
                        pDrvReg = g_aBackends[i].pDrvReg;
                        break;
                    }
                if (pDrvReg == NULL)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Unknown backend: '%s'", ValueUnion.psz);
                break;

            case 'd':
                fWithDrvAudio = true;
                break;

            case 'e':
                if (ValueUnion.u32 >= RT_ELEMENTS(g_aTests))
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid test number %u passed to --exclude", ValueUnion.u32);
                g_aTests[ValueUnion.u32].fExcluded = true;
                break;

            case VKAT_TEST_OPT_ATS_ADDR:
                if (TstEnv.enmMode == AUDIOTESTMODE_UNKNOWN)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Must specify a test mode first!");
                pszTcpAddr = ValueUnion.psz;
                break;

            case VKAT_TEST_OPT_ATS_PORT:
                if (TstEnv.enmMode == AUDIOTESTMODE_UNKNOWN)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Must specify a test mode first!");
                uTcpPort = ValueUnion.u32;
                break;

            case VKAT_TEST_OPT_MODE:
                if (TstEnv.enmMode != AUDIOTESTMODE_UNKNOWN)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Test mode (guest / host) already specified");
                TstEnv.enmMode = RTStrICmp(ValueUnion.psz, "guest") == 0 ? AUDIOTESTMODE_GUEST : AUDIOTESTMODE_HOST;
                break;

            case 'i':
                if (ValueUnion.u32 >= RT_ELEMENTS(g_aTests))
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Invalid test number %u passed to --include", ValueUnion.u32);
                g_aTests[ValueUnion.u32].fExcluded = false;
                break;

            case VKAT_TEST_OPT_COUNT:
                return RTMsgErrorExitFailure("Not yet implemented!");

            case VKAT_TEST_OPT_DEV:
                pszDevice = ValueUnion.psz;
                break;

            case VKAT_TEST_OPT_PAUSE:
                return RTMsgErrorExitFailure("Not yet implemented!");

            case VKAT_TEST_OPT_OUTDIR:
                rc = RTStrCopy(TstEnv.szPathOut, sizeof(TstEnv.szPathOut), ValueUnion.psz);
                if (RT_FAILURE(rc))
                    return RTMsgErrorExitFailure("Failed to copy out directory: %Rrc", rc);
                break;

            case VKAT_TEST_OPT_PCM_BIT:
                cPcmSampleBit = ValueUnion.u8;
                break;

            case VKAT_TEST_OPT_PCM_CHAN:
                cPcmChannels = ValueUnion.u8;
                break;

            case VKAT_TEST_OPT_PCM_HZ:
                uPcmHz = ValueUnion.u32;
                break;

            case VKAT_TEST_OPT_PCM_SIGNED:
                fPcmSigned = ValueUnion.f;
                break;

            case VKAT_TEST_OPT_TAG:
                pszTag = ValueUnion.psz;
                break;

            case VKAT_TEST_OPT_TEMPDIR:
                rc = RTStrCopy(TstEnv.szPathTemp, sizeof(TstEnv.szPathTemp), ValueUnion.psz);
                if (RT_FAILURE(rc))
                    return RTMsgErrorExit(RTEXITCODE_FAILURE, "Temp dir invalid, rc=%Rrc", rc);
                break;

            case VKAT_TEST_OPT_VOL:
                TstCust.TestTone.uVolumePercent = ValueUnion.u8;
                break;

            AUDIO_TEST_COMMON_OPTION_CASES(ValueUnion);

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    /*
     * Start testing.
     */
    RTTestBanner(g_hTest);

    /* Initialize the custom test parameters with sensible defaults if nothing else is given. */
    PDMAudioPropsInit(&TstCust.TestTone.Props,
                      cPcmSampleBit ? cPcmSampleBit / 8 : 2 /* 16-bit */, fPcmSigned, cPcmChannels ? cPcmChannels : 2,
                      uPcmHz ? uPcmHz : 44100);

    if (TstEnv.enmMode == AUDIOTESTMODE_UNKNOWN)
        return RTMsgErrorExit(RTEXITCODE_SYNTAX, "No test mode specified!\n");

    if (TstEnv.enmMode == AUDIOTESTMODE_HOST)
    {
        /* Use the default port is none is specified. */
        if (!uTcpPort)
            uTcpPort = ATS_DEFAULT_PORT;

        if (!pszTcpAddr)
            return RTMsgErrorExit(RTEXITCODE_SYNTAX, "--ats-address missing\n");
    }

    /* For now all tests have the same test environment. */
    rc = audioTestEnvInit(&TstEnv, pDrvReg, fWithDrvAudio, pszTag, pszTcpAddr, uTcpPort);
    if (RT_SUCCESS(rc))
    {
        PPDMAUDIOHOSTDEV pDev;
        rc = audioTestDevicesEnumerateAndCheck(&TstEnv, pszDevice, &pDev);
        if (RT_SUCCESS(rc))
            audioTestWorker(&TstEnv, &TstCust);

        /* Before destroying the test environment, pack up the test set so
         * that it's ready for transmission. */
        char szFileOut[RTPATH_MAX];
        rc = AudioTestSetPack(&TstEnv.Set, TstEnv.szPathOut, szFileOut, sizeof(szFileOut));
        if (RT_SUCCESS(rc))
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Test set packed up to '%s'\n", szFileOut);

#ifndef DEBUG_andy
        /* Clean up. */
        AudioTestSetClose(&TstEnv.Set); /* wipe fails on windows if the manifest file is open*/

        int rc2 = AudioTestSetWipe(&TstEnv.Set);
        AssertRC(rc2); /* Annoying, but not test-critical. */
#endif
        audioTestEnvDestroy(&TstEnv);
    }

    audioTestParmsDestroy(&TstCust);

    if (RT_FAILURE(rc)) /* Let us know that something went wrong in case we forgot to mention it. */
        RTTestFailed(g_hTest, "Tested failed with %Rrc\n", rc);

    /*
     * Print summary and exit.
     */
    return RTTestSummaryAndDestroy(g_hTest);
}


/*********************************************************************************************************************************
*   Command: verify                                                                                                              *
*********************************************************************************************************************************/

static int audioVerifyOpenTestSet(const char *pszPathSet, PAUDIOTESTSET pSet)
{
    int rc;

    char szPathExtracted[RTPATH_MAX];

    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Opening test set '%s'\n", pszPathSet);

    const bool fPacked = AudioTestSetIsPacked(pszPathSet);
    if (fPacked)
    {
        RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Test set is an archive and needs to be unpacked\n");

        char szPathTemp[RTPATH_MAX];
        rc = RTPathTemp(szPathTemp, sizeof(szPathTemp));
        if (RT_SUCCESS(rc))
        {
            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Using temporary directory '%s'\n", szPathTemp);

            rc = RTPathJoin(szPathExtracted, sizeof(szPathExtracted), szPathTemp, "vkat-XXXX");
            if (RT_SUCCESS(rc))
            {
                rc = RTDirCreateTemp(szPathExtracted, 0755);
                if (RT_SUCCESS(rc))
                {
                    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Unpacking archive to '%s'\n", szPathExtracted);
                    rc = AudioTestSetUnpack(pszPathSet, szPathExtracted);
                    if (RT_SUCCESS(rc))
                        RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Archive successfully unpacked\n");
                }
            }
        }
    }
    else
        rc = VINF_SUCCESS;

    if (RT_SUCCESS(rc))
        rc = AudioTestSetOpen(pSet, fPacked ? szPathExtracted : pszPathSet);

    if (RT_FAILURE(rc))
        RTTestFailed(g_hTest, "Unable to open / unpack test set archive: %Rrc", rc);

    return rc;
}

/**
 * Verifies one single test set.
 *
 * @returns VBox status code.
 * @param   pszPathSetA         Absolute path to test set A.
 * @param   pszPathSetB         Absolute path to test set B.
 */
static int audioVerifyOne(const char *pszPathSetA, const char *pszPathSetB)
{
    RTTestSubF(g_hTest, "Verifying");
    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Verifying test set '%s' with test set '%s'\n", pszPathSetA, pszPathSetB);

    AUDIOTESTSET SetA, SetB;
    int rc = audioVerifyOpenTestSet(pszPathSetA, &SetA);
    if (RT_SUCCESS(rc))
        rc = audioVerifyOpenTestSet(pszPathSetB, &SetB);

    if (RT_SUCCESS(rc))
    {
        AUDIOTESTERRORDESC errDesc;
        rc = AudioTestSetVerify(&SetA, &SetB, &errDesc);
        if (RT_SUCCESS(rc))
        {
            if (AudioTestErrorDescFailed(&errDesc))
            {
                /** @todo Use some AudioTestErrorXXX API for enumeration here later. */
                PAUDIOTESTERRORENTRY pErrEntry;
                RTListForEach(&errDesc.List, pErrEntry, AUDIOTESTERRORENTRY, Node)
                    RTTestFailed(g_hTest, pErrEntry->szDesc);
            }
            else
                RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Verification successful\n");

            AudioTestErrorDescDestroy(&errDesc);
        }
        else
            RTTestFailed(g_hTest, "Verification failed with %Rrc", rc);
    }

    AudioTestSetClose(&SetA);
    AudioTestSetClose(&SetB);

    RTTestSubDone(g_hTest);

    return rc;
}

/**
 * Main (entry) function for the verification functionality of VKAT.
 *
 * @returns Program exit code.
 * @param   pGetState   RTGetOpt state.
 */
static DECLCALLBACK(RTEXITCODE) audioVerifyMain(PRTGETOPTSTATE pGetState)
{
    /*
     * Parse options and process arguments.
     */
    char     *pszSetA = NULL;
    char     *pszSetB = NULL;
    unsigned iTestSet = 0;

    int           rc;
    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(pGetState, &ValueUnion)))
    {
        switch (rc)
        {
            case VKAT_VERIFY_OPT_TAG:
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                char **ppszSet = iTestSet == 0 ? &pszSetA : &pszSetB;

                if (iTestSet == 0)
                    RTTestBanner(g_hTest);

                *ppszSet = RTStrDup(ValueUnion.psz);
                AssertPtrReturn(*ppszSet, RTEXITCODE_FAILURE);

                iTestSet++;
                break;
            }

            AUDIO_TEST_COMMON_OPTION_CASES(ValueUnion);

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    if (!iTestSet)
        return RTMsgErrorExitFailure("At least one test set must be specified");

    if (iTestSet > 2)
        return RTMsgErrorExitFailure("Only two test sets can be verified at one time");

    /*
     * If only test set A is given, default to the current directory
     * for test set B.
     */
    if (iTestSet == 1)
    {
        char szDirCur[RTPATH_MAX];
        rc = RTPathGetCurrent(szDirCur, sizeof(szDirCur));
        if (RT_SUCCESS(rc))
        {
            Assert(pszSetB == NULL);
            pszSetB = RTStrDup(szDirCur);
            AssertPtrReturn(pszSetB, RTEXITCODE_FAILURE);
        }
        else
            RTTestFailed(g_hTest, "Failed to retrieve current directory: %Rrc", rc);
    }

    if (RT_SUCCESS(rc))
        rc = audioVerifyOne(pszSetA, pszSetB);

    RTStrFree(pszSetA);
    RTStrFree(pszSetB);

    /*
     * Print summary and exit.
     */
    return RTTestSummaryAndDestroy(g_hTest);
}


/*********************************************************************************************************************************
*   Command: play                                                                                                                *
*********************************************************************************************************************************/

/**
 * Worker for audioTestCmdPlayHandler that plays one file.
 */
static RTEXITCODE audioTestPlayOne(const char *pszFile, PCPDMDRVREG pDrvReg, const char *pszDevId, uint32_t cMsBufferSize,
                                   uint32_t cMsPreBuffer, uint32_t cMsSchedulingHint, bool fWithDrvAudio)
{
    /*
     * First we must open the file and determin the format.
     */
    RTERRINFOSTATIC ErrInfo;
    AUDIOTESTWAVEFILE WaveFile;
    int rc = AudioTestWaveFileOpen(pszFile, &WaveFile, RTErrInfoInitStatic(&ErrInfo));
    if (RT_FAILURE(rc))
        return RTMsgErrorExitFailure("Failed to open '%s': %Rrc%#RTeim", pszFile, rc, &ErrInfo.Core);

    if (g_uVerbosity > 0)
    {
        char szTmp[128];
        RTMsgInfo("Opened '%s' for playing\n", pszFile);
        RTMsgInfo("Format: %s\n", PDMAudioPropsToString(&WaveFile.Props, szTmp, sizeof(szTmp)));
        RTMsgInfo("Size:   %'RU32 bytes / %#RX32 / %'RU32 frames / %'RU64 ns\n",
                  WaveFile.cbSamples, WaveFile.cbSamples,
                  PDMAudioPropsBytesToFrames(&WaveFile.Props, WaveFile.cbSamples),
                  PDMAudioPropsBytesToNano(&WaveFile.Props, WaveFile.cbSamples));
    }

    /*
     * Construct the driver stack.
     */
    RTEXITCODE          rcExit = RTEXITCODE_FAILURE;
    AUDIOTESTDRVSTACK   DrvStack;
    rc = audioTestDriverStackInit(&DrvStack, pDrvReg, fWithDrvAudio);
    if (RT_SUCCESS(rc))
    {
        /*
         * Set the output device if one is specified.
         */
        rc = audioTestDriverStackSetDevice(&DrvStack, PDMAUDIODIR_OUT, pszDevId);
        if (RT_SUCCESS(rc))
        {
            /*
             * Open a stream for the output.
             */
            PDMAUDIOSTREAMCFG CfgAcq;
            PPDMAUDIOSTREAM   pStream = NULL;
            rc = audioTestDriverStackStreamCreateOutput(&DrvStack, &WaveFile.Props, cMsBufferSize,
                                                        cMsPreBuffer, cMsSchedulingHint, &pStream, &CfgAcq);
            if (RT_SUCCESS(rc))
            {
                rc = audioTestDriverStackStreamEnable(&DrvStack, pStream);
                if (RT_SUCCESS(rc))
                {
                    uint64_t const nsStarted = RTTimeNanoTS();

                    /*
                     * Transfer data as quickly as we're allowed.
                     */
                    for (;;)
                    {
                        /* Read a chunk from the wave file. */
                        uint8_t  abSamples[16384];
                        size_t   cbSamples = 0;
                        rc = AudioTestWaveFileRead(&WaveFile, abSamples, sizeof(abSamples), &cbSamples);
                        if (RT_SUCCESS(rc) && cbSamples > 0)
                        {
                            /* Transfer the data to the audio stream. */
                            for (uint32_t offSamples = 0; offSamples < cbSamples;)
                            {
                                uint32_t const cbCanWrite = audioTestDriverStackStreamGetWritable(&DrvStack, pStream);
                                if (cbCanWrite > 0)
                                {
                                    uint32_t const cbToPlay = RT_MIN(cbCanWrite, (uint32_t)cbSamples - offSamples);
                                    uint32_t       cbPlayed = 0;
                                    rc = audioTestDriverStackStreamPlay(&DrvStack, pStream, &abSamples[offSamples],
                                                                        cbToPlay, &cbPlayed);
                                    if (RT_SUCCESS(rc))
                                    {
                                        if (cbPlayed)
                                            offSamples += cbPlayed;
                                        else
                                        {
                                            rcExit = RTMsgErrorExitFailure("Played zero out of %#x bytes - %#x bytes reported playable!\n",
                                                                           cbToPlay, cbCanWrite);
                                            break;
                                        }
                                    }
                                    else
                                    {
                                        rcExit = RTMsgErrorExitFailure("Failed to play %#x bytes: %Rrc\n", cbToPlay, rc);
                                        break;
                                    }
                                }
                                else if (audioTestDriverStackStreamIsOkay(&DrvStack, pStream))
                                    RTThreadSleep(RT_MIN(RT_MAX(1, CfgAcq.Device.cMsSchedulingHint), 256));
                                else
                                {
                                    rcExit = RTMsgErrorExitFailure("Stream is not okay!\n");
                                    break;
                                }
                            }
                        }
                        else if (RT_SUCCESS(rc) && cbSamples == 0)
                        {
                            rcExit = RTEXITCODE_SUCCESS;
                            break;
                        }
                        else
                        {
                            rcExit = RTMsgErrorExitFailure("Error reading wav file '%s': %Rrc", pszFile, rc);
                            break;
                        }
                    }

                    /*
                     * Drain the stream.
                     */
                    if (rcExit == RTEXITCODE_SUCCESS)
                    {
                        if (g_uVerbosity > 0)
                            RTMsgInfo("%'RU64 ns: Draining...\n", RTTimeNanoTS() - nsStarted);
                        rc = audioTestDriverStackStreamDrain(&DrvStack, pStream, true /*fSync*/);
                        if (RT_SUCCESS(rc))
                        {
                            if (g_uVerbosity > 0)
                                RTMsgInfo("%'RU64 ns: Done\n", RTTimeNanoTS() - nsStarted);
                        }
                        else
                            rcExit = RTMsgErrorExitFailure("Draining failed: %Rrc", rc);
                    }
                }
                else
                    rcExit = RTMsgErrorExitFailure("Enabling the output stream failed: %Rrc", rc);
                audioTestDriverStackStreamDestroy(&DrvStack, pStream);
            }
            else
                rcExit = RTMsgErrorExitFailure("Creating output stream failed: %Rrc", rc);
        }
        else
            rcExit = RTMsgErrorExitFailure("Failed to set output device to '%s': %Rrc", pszDevId, rc);
        audioTestDriverStackDelete(&DrvStack);
    }
    else
        rcExit = RTMsgErrorExitFailure("Driver stack construction failed: %Rrc", rc);
    AudioTestWaveFileClose(&WaveFile);
    return rcExit;
}

/**
 * Command line parameters for test mode.
 */
static const RTGETOPTDEF g_aCmdPlayOptions[] =
{
    { "--backend",          'b',                          RTGETOPT_REQ_STRING  },
    { "--output-device",    'o',                          RTGETOPT_REQ_STRING  },
    { "--with-drv-audio",   'd',                          RTGETOPT_REQ_NOTHING },
};

/** the 'play' command option help. */
static DECLCALLBACK(const char *) audioTestCmdPlayHelp(PCRTGETOPTDEF pOpt)
{
    switch (pOpt->iShort)
    {
        case 'b': return "The audio backend to use.";
        case 'd': return "Go via DrvAudio instead of directly interfacing with the backend.";
        case 'o': return "The ID of the output device to use.";
        default:  return NULL;
    }
}

/**
 * The 'play' command handler.
 *
 * @returns Program exit code.
 * @param   pGetState   RTGetOpt state.
 */
static DECLCALLBACK(RTEXITCODE) audioTestCmdPlayHandler(PRTGETOPTSTATE pGetState)
{
    /* Option values: */
    PCPDMDRVREG pDrvReg           = g_aBackends[0].pDrvReg;
    uint32_t    cMsBufferSize     = UINT32_MAX;
    uint32_t    cMsPreBuffer      = UINT32_MAX;
    uint32_t    cMsSchedulingHint = UINT32_MAX;
    const char *pszDevId          = NULL;
    bool        fWithDrvAudio     = false;

    /* Argument processing loop: */
    int           rc;
    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(pGetState, &ValueUnion)) != 0)
    {
        switch (rc)
        {
            case 'b':
                pDrvReg = NULL;
                for (uintptr_t i = 0; i < RT_ELEMENTS(g_aBackends); i++)
                    if (   strcmp(ValueUnion.psz, g_aBackends[i].pszName) == 0
                        || strcmp(ValueUnion.psz, g_aBackends[i].pDrvReg->szName) == 0)
                    {
                        pDrvReg = g_aBackends[i].pDrvReg;
                        break;
                    }
                if (pDrvReg == NULL)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Unknown backend: '%s'", ValueUnion.psz);
                break;

            case 'd':
                fWithDrvAudio = true;
                break;

            case 'o':
                pszDevId = ValueUnion.psz;
                break;

            case VINF_GETOPT_NOT_OPTION:
            {
                RTEXITCODE rcExit = audioTestPlayOne(ValueUnion.psz, pDrvReg, pszDevId, cMsBufferSize, cMsPreBuffer,
                                                     cMsSchedulingHint, fWithDrvAudio);
                if (rcExit != RTEXITCODE_SUCCESS)
                    return rcExit;
                break;
            }

            AUDIO_TEST_COMMON_OPTION_CASES(ValueUnion);

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }
    return RTEXITCODE_SUCCESS;
}

/**
 * Command line parameters for self-test mode.
 */
static const RTGETOPTDEF g_aCmdSelftestOptions[] =
{
    { "--ats-host",         VKAT_SELFTEST_OPT_ATS_HOST,   RTGETOPT_REQ_STRING },
    { "--backend",          'b',                          RTGETOPT_REQ_STRING  },
    { "--with-drv-audio",   'd',                          RTGETOPT_REQ_NOTHING },
};

/** the 'selftest' command option help. */
static DECLCALLBACK(const char *) audioTestCmdSelftestHelp(PCRTGETOPTDEF pOpt)
{
    switch (pOpt->iShort)
    {
        case 'b': return "The audio backend to use.";
        case 'd': return "Go via DrvAudio instead of directly interfacing with the backend.";
        default:  return NULL;
    }
}

/**
 * Tests the Audio Test Service (ATS).
 *
 * @returns VBox status code.
 * @param   pDrvReg             Backend driver to use.
 * @param   pszAdr              Address of ATS server to connect to.
 *                              If NULL, an own (local) ATS server will be created.
 */
static int audioTestDoSelftestAts(PCPDMDRVREG pDrvReg, const char *pszAdr)
{
    AUDIOTESTENV TstEnv;
    int rc = audioTestDriverStackInit(&TstEnv.DrvStack, pDrvReg, true /* fWithDrvAudio */);
    if (RT_SUCCESS(rc))
    {
        /** @todo Make stream parameters configurable. */
        PDMAUDIOPCMPROPS  Props;
        PDMAudioPropsInit(&Props, 16 /* bit */ / 8, true /* fSigned */, 2 /* Channels */, 44100 /* Hz */);

        PDMAUDIOSTREAMCFG CfgAcq;
        PPDMAUDIOSTREAM   pStream = NULL;
        rc = audioTestDriverStackStreamCreateOutput(&TstEnv.DrvStack, &Props,
                                                    UINT32_MAX /* cMsBufferSize */,
                                                    UINT32_MAX /* cMsPreBuffer */,
                                                    UINT32_MAX /* cMsSchedulingHint */, &pStream, &CfgAcq);
        if (RT_SUCCESS(rc))
        {
            rc = audioTestDriverStackStreamEnable(&TstEnv.DrvStack, pStream);
            if (RT_SUCCESS(rc))
            {
                ATSCALLBACKCTX Ctx;
                Ctx.pTstEnv = &TstEnv;

                ATSCALLBACKS Callbacks;
                Callbacks.pfnTonePlay = audioTestSvcTonePlayCallback;
                Callbacks.pvUser      = &Ctx;

                /* Start an own ATS instance if no address to connect was specified. */
                ATSSERVER Srv;
                if (pszAdr == NULL)
                {
                    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Starting ATS ...\n");

                    rc = AudioTestSvcInit(&Srv, &Callbacks);
                    if (RT_SUCCESS(rc))
                    {
                        rc = AudioTestSvcStart(&Srv);
                        if (RT_SUCCESS(rc))
                            RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "ATS running\n");
                    }
                }
                else
                    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Connecting to ATS at '%s' ...\n", pszAdr);

                if (RT_SUCCESS(rc))
                {
                    ATSCLIENT Conn;
                    rc = AudioTestSvcClientConnect(&Conn, NULL, 0);
                    if (RT_SUCCESS(rc))
                    {
                        RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Connected to ATS, testing ...\n");

                        /* Do the bare minimum here to get a test tone out. */
                        AUDIOTESTTONEPARMS ToneParms;
                        RT_ZERO(ToneParms);
                        ToneParms.msDuration = RTRandU32Ex(250, 1000 * 5);
                        memcpy(&ToneParms.Props, &CfgAcq.Props, sizeof(PDMAUDIOPCMPROPS));

                        rc = AudioTestSvcClientTonePlay(&Conn, &CfgAcq, &ToneParms);

                        int rc2 = AudioTestSvcClientClose(&Conn);
                        if (RT_SUCCESS(rc))
                            rc = rc2;
                    }
                    else
                        RTTestFailed(g_hTest, "Connecting to ATS failed, rc=%Rrc\n", rc);

                    int rc2 = AudioTestSvcShutdown(&Srv);
                    if (RT_SUCCESS(rc))
                        rc = rc2;
                }

                if (pszAdr == NULL)
                {
                    RTTestPrintf(g_hTest, RTTESTLVL_ALWAYS, "Shutting down ATS ...\n");

                    int rc2 = AudioTestSvcDestroy(&Srv);
                        if (RT_SUCCESS(rc))
                            rc = rc2;
                }
            }
        }
    }

    if (RT_FAILURE(rc))
        RTTestFailed(g_hTest, "Testing ATS failed with %Rrc\n", rc);

    return rc;
}

/**
 * Main function for performing the self-tests.
 *
 * @returns VBox status code.
 * @param   pDrvReg             Backend driver to use.
 * @param   pszAtsAdr           Address of ATS server to connect to.
 *                              If NULL, an own (local) ATS server will be created.
 */
static int audioTestDoSelftest(PCPDMDRVREG pDrvReg, const char *pszAtsAdr)
{
    int rc = audioTestDoSelftestAts(pDrvReg, pszAtsAdr);
    if (RT_FAILURE(rc))
        RTTestFailed(g_hTest, "Self-test failed with: %Rrc", rc);

    return rc;
}

/**
 * The 'selftest' command handler.
 *
 * @returns Program exit code.
 * @param   pGetState   RTGetOpt state.
 */
static DECLCALLBACK(RTEXITCODE) audioTestCmdSelftestHandler(PRTGETOPTSTATE pGetState)
{
    /* Option values: */
    PCPDMDRVREG pDrvReg           = g_aBackends[0].pDrvReg;
    bool        fWithDrvAudio     = false;
    char       *pszAtsAddr        = NULL;

    /* Argument processing loop: */
    int           rc;
    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(pGetState, &ValueUnion)) != 0)
    {
        switch (rc)
        {
            case VKAT_SELFTEST_OPT_ATS_HOST:
            {
                pszAtsAddr = RTStrDup(ValueUnion.psz);
                break;
            }

            case 'b':
            {
                pDrvReg = NULL;
                for (uintptr_t i = 0; i < RT_ELEMENTS(g_aBackends); i++)
                    if (   strcmp(ValueUnion.psz, g_aBackends[i].pszName) == 0
                        || strcmp(ValueUnion.psz, g_aBackends[i].pDrvReg->szName) == 0)
                    {
                        pDrvReg = g_aBackends[i].pDrvReg;
                        break;
                    }
                if (pDrvReg == NULL)
                    return RTMsgErrorExit(RTEXITCODE_SYNTAX, "Unknown backend: '%s'", ValueUnion.psz);
                break;
            }

            case 'd':
            {
                fWithDrvAudio = true;
                break;
            }

            case VINF_GETOPT_NOT_OPTION:
            {
                break;
            }

            AUDIO_TEST_COMMON_OPTION_CASES(ValueUnion);

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    audioTestDoSelftest(pDrvReg, pszAtsAddr);

    RTStrFree(pszAtsAddr);

    /*
     * Print summary and exit.
     */
    return RTTestSummaryAndDestroy(g_hTest);
}


/**
 * Commands.
 */
static struct
{
    /** The command name. */
    const char     *pszCommand;
    /** The command handler.   */
    DECLCALLBACKMEMBER(RTEXITCODE, pfnHandler,(PRTGETOPTSTATE pGetState));

    /** Command description.   */
    const char     *pszDesc;
    /** Options array.  */
    PCRTGETOPTDEF   paOptions;
    /** Number of options in the option array. */
    size_t          cOptions;
    /** Gets help for an option. */
    DECLCALLBACKMEMBER(const char *, pfnOptionHelp,(PCRTGETOPTDEF pOpt));
} const g_aCommands[] =
{
    {
        "test",     audioTestMain,
        "Runs audio tests and creates an audio test set.",
        g_aCmdTestOptions,      RT_ELEMENTS(g_aCmdTestOptions),     audioTestCmdTestHelp
    },
    {
        "verify",   audioVerifyMain,
        "Verifies a formerly created audio test set.",
        g_aCmdVerifyOptions,    RT_ELEMENTS(g_aCmdVerifyOptions),   NULL,
    },
    {
        "play",     audioTestCmdPlayHandler,
        "Plays one or more wave files.",
        g_aCmdPlayOptions,      RT_ELEMENTS(g_aCmdPlayOptions),     audioTestCmdPlayHelp,
    },
    {
        "selftest", audioTestCmdSelftestHandler,
        "Performs self-tests.",
        g_aCmdSelftestOptions,  RT_ELEMENTS(g_aCmdSelftestOptions), audioTestCmdSelftestHelp,
    }
};

/**
 * Shows tool usage text.
 */
static RTEXITCODE audioTestUsage(PRTSTREAM pStrm)
{
    RTStrmPrintf(pStrm, "usage: %s [global options] <command> [command-options]\n",
                 RTPathFilename(RTProcExecutablePath()));
    RTStrmPrintf(pStrm,
                 "\n"
                 "Global Options:\n"
                 "  --debug-audio\n"
                 "    Enables DrvAudio debugging.\n"
                 "  --debug-audio-path=<path>\n"
                 "    Tells DrvAudio where to put its debug output (wav-files).\n"
                 "  -q, --quiet\n"
                 "    Sets verbosity to zero.\n"
                 "  -v, --verbose\n"
                 "    Increase verbosity.\n"
                 "  -V, --version\n"
                 "    Displays version.\n"
                 "  -h, -?, --help\n"
                 "    Displays help.\n"
                 );

    for (uintptr_t iCmd = 0; iCmd < RT_ELEMENTS(g_aCommands); iCmd++)
    {
        RTStrmPrintf(pStrm,
                     "\n"
                     "Command '%s':\n"
                     "    %s\n"
                     "Options for '%s':\n",
                     g_aCommands[iCmd].pszCommand, g_aCommands[iCmd].pszDesc, g_aCommands[iCmd].pszCommand);
        PCRTGETOPTDEF const paOptions = g_aCommands[iCmd].paOptions;
        for (unsigned i = 0; i < g_aCommands[iCmd].cOptions; i++)
        {
            if (RT_C_IS_PRINT(paOptions[i].iShort))
                RTStrmPrintf(pStrm, "  -%c, %s\n", paOptions[i].iShort, paOptions[i].pszLong);
            else
                RTStrmPrintf(pStrm, "  %s\n", paOptions[i].pszLong);

            const char *pszHelp = NULL;
            if (g_aCommands[iCmd].pfnOptionHelp)
                pszHelp = g_aCommands[iCmd].pfnOptionHelp(&paOptions[i]);
            if (pszHelp)
                RTStrmPrintf(pStrm, "    %s\n", pszHelp);
        }
    }
    return RTEXITCODE_SUCCESS;
}

/**
 * Shows tool version.
 */
static RTEXITCODE audioTestVersion(void)
{
    RTPrintf("v0.0.1\n");
    return RTEXITCODE_SUCCESS;
}

/**
 * Shows the logo.
 *
 * @param   pStream             Output stream to show logo on.
 */
static void audioTestShowLogo(PRTSTREAM pStream)
{
    RTStrmPrintf(pStream, VBOX_PRODUCT " VKAT (Validation Kit Audio Test) Version " VBOX_VERSION_STRING " - r%s\n"
                 "(C) " VBOX_C_YEAR " " VBOX_VENDOR "\n"
                 "All rights reserved.\n\n", RTBldCfgRevisionStr());
}

int main(int argc, char **argv)
{
    /*
     * Init IPRT and globals.
     */
    RTEXITCODE rcExit = RTTestInitAndCreate("AudioTest", &g_hTest);
    if (rcExit != RTEXITCODE_SUCCESS)
        return rcExit;

#ifdef RT_OS_WINDOWS
    HRESULT hrc = CoInitializeEx(NULL /*pReserved*/, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrc))
        RTMsgWarning("CoInitializeEx failed: %#x", hrc);
#endif

    /*
     * Configure release logging to go to stderr.
     */
    static const char * const g_apszLogGroups[] = VBOX_LOGGROUP_NAMES;
    int rc = RTLogCreate(&g_pRelLogger, RTLOGFLAGS_PREFIX_THREAD, "all.e.l", "VKAT_RELEASE_LOG",
                         RT_ELEMENTS(g_apszLogGroups), g_apszLogGroups, RTLOGDEST_STDERR, "vkat-release.log");
    if (RT_SUCCESS(rc))
        RTLogRelSetDefaultInstance(g_pRelLogger);
    else
        RTMsgWarning("Failed to create release logger: %Rrc", rc);

    /*
     * Process common options.
     */
    RTGETOPTSTATE GetState;
    rc = RTGetOptInit(&GetState, argc, argv, g_aCmdCommonOptions,
                      RT_ELEMENTS(g_aCmdCommonOptions), 1 /*idxFirst*/, 0 /*fFlags - must not sort! */);
    AssertRCReturn(rc, RTEXITCODE_INIT);

    RTGETOPTUNION ValueUnion;
    while ((rc = RTGetOpt(&GetState, &ValueUnion)) != 0)
    {
        switch (rc)
        {
            case 'q':
                g_uVerbosity = 0;
                if (g_pRelLogger)
                    RTLogGroupSettings(g_pRelLogger, "all=0 all.e");
                break;

            case 'v':
                g_uVerbosity++;
                if (g_pRelLogger)
                    RTLogGroupSettings(g_pRelLogger, g_uVerbosity == 1 ? "all.e.l" : g_uVerbosity == 2 ? "all.e.l.f" : "all=~0");
                break;

            case 'V':
                return audioTestVersion();

            case 'h':
                audioTestShowLogo(g_pStdOut);
                return audioTestUsage(g_pStdOut);

            case VINF_GETOPT_NOT_OPTION:
            {
                for (uintptr_t i = 0; i < RT_ELEMENTS(g_aCommands); i++)
                    if (strcmp(ValueUnion.psz, g_aCommands[i].pszCommand) == 0)
                    {
                        size_t const cCombinedOptions  = g_aCommands[i].cOptions + RT_ELEMENTS(g_aCmdCommonOptions);
                        PRTGETOPTDEF paCombinedOptions = (PRTGETOPTDEF)RTMemAlloc(cCombinedOptions * sizeof(RTGETOPTDEF));
                        if (paCombinedOptions)
                        {
                            memcpy(paCombinedOptions, g_aCmdCommonOptions, sizeof(g_aCmdCommonOptions));
                            memcpy(&paCombinedOptions[RT_ELEMENTS(g_aCmdCommonOptions)],
                                   g_aCommands[i].paOptions, g_aCommands[i].cOptions * sizeof(RTGETOPTDEF));

                            rc = RTGetOptInit(&GetState, argc, argv, paCombinedOptions, cCombinedOptions,
                                              GetState.iNext /*idxFirst*/, RTGETOPTINIT_FLAGS_OPTS_FIRST);
                            if (RT_SUCCESS(rc))
                            {

                                rcExit = g_aCommands[i].pfnHandler(&GetState);
                                RTMemFree(paCombinedOptions);
                                return rcExit;
                            }
                            return RTMsgErrorExitFailure("RTGetOptInit failed for '%s': %Rrc", ValueUnion.psz, rc);
                        }
                        return RTMsgErrorExitFailure("Out of memory!");
                    }
                RTMsgError("Unknown command '%s'!\n", ValueUnion.psz);
                audioTestUsage(g_pStdErr);
                return RTEXITCODE_SYNTAX;
            }

            default:
                return RTGetOptPrintError(rc, &ValueUnion);
        }
    }

    RTMsgError("No command specified!\n");
    audioTestUsage(g_pStdErr);
    return RTEXITCODE_SYNTAX;
}

