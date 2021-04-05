// Copyright (c) 2021 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "common_utils.h"
#include "cmd_options.h"

static void usage(CmdOptionsCtx* ctx)
{
    printf(
        "Transcodes INPUT and optionally writes OUTPUT.\n"
        "\n"
        "Usage: %s [options] INPUT [OUTPUT]\n", ctx->program);
}

int main(int argc, char** argv)
{
    mfxStatus sts = MFX_ERR_NONE;
    bool bEnableOutput; // if true, removes all output bitsteam file writing and printing the progress
    CmdOptions options;

    // =====================================================================
    // Intel Media SDK transcode opaque pipeline setup
    // - In this example we are decoding and encoding an AVC (H.264) stream
    // - Opaque memory decode->vpp->encode pipeline. Media SDK selects suitable memory surface internally
    // - Asynchronous operation by executing more than one encode operation simultaneously
    //

    // Read options from the command line (if any is given)
    memset(&options, 0, sizeof(CmdOptions));
    options.ctx.options = OPTIONS_TRANSCODE;
    options.ctx.usage = usage;
    // Set default values:
    options.values.impl = MFX_IMPL_AUTO_ANY;

    // here we parse options
    ParseOptions(argc, argv, &options);

    if (!options.values.Bitrate) {
        printf("error: bitrate not set (mandatory)\n");
        return -1;
    }
    if (!options.values.FrameRateN || !options.values.FrameRateD) {
        printf("error: framerate not set (mandatory)\n");
        return -1;
    }
    if (!options.values.SourceName[0]) {
        printf("error: source file name not set (mandatory)\n");
        return -1;
    }

    bEnableOutput = (options.values.SinkName[0] != '\0');

    // Open input H.264 elementary stream (ES) file
    fileUniPtr fSource(OpenFile(options.values.SourceName, "rb"), &CloseFile);
    MSDK_CHECK_POINTER(fSource, MFX_ERR_NULL_PTR);

    // Create output elementary stream (ES) H.264 file
    fileUniPtr fSink(nullptr, &CloseFile);
    if (bEnableOutput) {
        fSink.reset(OpenFile(options.values.SinkName, "wb"));
        MSDK_CHECK_POINTER(fSink, MFX_ERR_NULL_PTR);
    }

    // Initialize Media SDK session
    // - MFX_IMPL_AUTO_ANY selects HW acceleration if available (on any adapter)
    // - Version 1.3 is selected since the opaque memory feature was added in this API release
    //   If more recent API features are needed, change the version accordingly
    mfxIMPL impl = options.values.impl;
    mfxVersion ver = { {3, 1} };      // Note: API 1.3 !
    MFXVideoSession session;
    sts = Initialize(impl, ver, &session, NULL);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Create Media SDK decoder & encoder
    MFXVideoDECODE mfxDEC(session);
    MFXVideoENCODE mfxENC(session);
    MFXVideoVPP mfxVPP(session);

    // Set required video parameters for decode
    // - In this example we are decoding an AVC (H.264) stream
    mfxVideoParam mfxDecParams;
    memset(&mfxDecParams, 0, sizeof(mfxDecParams));
    mfxDecParams.mfx.CodecId = MFX_CODEC_HEVC;
    mfxDecParams.IOPattern = MFX_IOPATTERN_OUT_OPAQUE_MEMORY;

    // Configure Media SDK to keep more operations in flight
    // - AsyncDepth represents the number of tasks that can be submitted, before synchronizing is required
    // - The choice of AsyncDepth = 4 is quite arbitrary but has proven to result in good performance
    mfxDecParams.AsyncDepth = 4;

    // Prepare Media SDK bit stream buffer for decoder
    // - Arbitrary buffer size for this example
    mfxBitstream mfxBS;
    memset(&mfxBS, 0, sizeof(mfxBS));
    mfxBS.MaxLength = 1024 * 1024;
    std::vector<mfxU8> bstData(mfxBS.MaxLength);
    mfxBS.Data = bstData.data();


    // Read a chunk of data from stream file into bit stream buffer
    // - Parse bit stream, searching for header and fill video parameters structure
    // - Abort if bit stream header is not found in the first bit stream buffer chunk
    sts = ReadBitStreamData(&mfxBS, fSource.get());
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    sts = mfxDEC.DecodeHeader(&mfxBS, &mfxDecParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize VPP parameters
    mfxVideoParam VPPParams;
    memset(&VPPParams, 0, sizeof(VPPParams));
    // Input data
    VPPParams.vpp.In.FourCC = MFX_FOURCC_P010;
    VPPParams.vpp.In.BitDepthLuma = 10;
    VPPParams.vpp.In.BitDepthChroma = 10;
    VPPParams.vpp.In.Shift = 1;
    VPPParams.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.In.CropX = 0;
    VPPParams.vpp.In.CropY = 0;
    VPPParams.vpp.In.CropW = mfxDecParams.mfx.FrameInfo.CropW;
    VPPParams.vpp.In.CropH = mfxDecParams.mfx.FrameInfo.CropH;
    VPPParams.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.In.FrameRateExtN = 30;
    VPPParams.vpp.In.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.In.Width = MSDK_ALIGN16(VPPParams.vpp.In.CropW);
    VPPParams.vpp.In.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.In.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.In.CropH);
    // Output data
    VPPParams.vpp.Out.FourCC = MFX_FOURCC_NV12;
    VPPParams.vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.Out.CropX = 0;
    VPPParams.vpp.Out.CropY = 0;
    VPPParams.vpp.Out.CropW = 1280;
    VPPParams.vpp.Out.CropH = 720;
    VPPParams.vpp.Out.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.Out.FrameRateExtN = 30;
    VPPParams.vpp.Out.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    VPPParams.vpp.Out.Width = MSDK_ALIGN16(VPPParams.vpp.Out.CropW);
    VPPParams.vpp.Out.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.Out.PicStruct) ?
        MSDK_ALIGN16(VPPParams.vpp.Out.CropH) :
        MSDK_ALIGN32(VPPParams.vpp.Out.CropH);

    VPPParams.IOPattern =
        MFX_IOPATTERN_IN_OPAQUE_MEMORY | MFX_IOPATTERN_OUT_OPAQUE_MEMORY;

    // Configure Media SDK to keep more operations in flight
    // - AsyncDepth represents the number of tasks that can be submitted, before synchronizing is required
    VPPParams.AsyncDepth = mfxDecParams.AsyncDepth;

    // Initialize encoder parameters
    // - In this example we are encoding an AVC (H.264) stream
    mfxVideoParam mfxEncParams;
    memset(&mfxEncParams, 0, sizeof(mfxEncParams));
    mfxEncParams.mfx.CodecId = MFX_CODEC_AVC;
    mfxEncParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    mfxEncParams.mfx.TargetKbps = options.values.Bitrate;
    mfxEncParams.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
    mfxEncParams.mfx.FrameInfo.FrameRateExtN = options.values.FrameRateN;
    mfxEncParams.mfx.FrameInfo.FrameRateExtD = options.values.FrameRateD;
    mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    mfxEncParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    mfxEncParams.mfx.FrameInfo.CropX = 0;
    mfxEncParams.mfx.FrameInfo.CropY = 0;
    mfxEncParams.mfx.FrameInfo.CropW = VPPParams.vpp.Out.CropW;     // Half the resolution of decode stream
    mfxEncParams.mfx.FrameInfo.CropH = VPPParams.vpp.Out.CropH;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    mfxEncParams.mfx.FrameInfo.Width = MSDK_ALIGN16(mfxEncParams.mfx.FrameInfo.CropW);
    mfxEncParams.mfx.FrameInfo.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == mfxEncParams.mfx.FrameInfo.PicStruct) ?
        MSDK_ALIGN16(mfxEncParams.mfx.FrameInfo.CropH) :
        MSDK_ALIGN32(mfxEncParams.mfx.FrameInfo.CropH);

    mfxEncParams.IOPattern = MFX_IOPATTERN_IN_OPAQUE_MEMORY;

    // Configure Media SDK to keep more operations in flight
    // - AsyncDepth represents the number of tasks that can be submitted, before synchronizing is required
    mfxEncParams.AsyncDepth = mfxDecParams.AsyncDepth;

    // Query number required surfaces for decoder
    mfxFrameAllocRequest DecRequest;
    memset(&DecRequest, 0, sizeof(DecRequest));
    sts = mfxDEC.QueryIOSurf(&mfxDecParams, &DecRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Query number required surfaces for encoder
    mfxFrameAllocRequest EncRequest;
    memset(&EncRequest, 0, sizeof(EncRequest));
    sts = mfxENC.QueryIOSurf(&mfxEncParams, &EncRequest);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Query number of required surfaces for VPP
    mfxFrameAllocRequest VPPRequest[2];     // [0] - in, [1] - out
    memset(&VPPRequest, 0, sizeof(mfxFrameAllocRequest) * 2);
    sts = mfxVPP.QueryIOSurf(&VPPParams, VPPRequest);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Determine the required number of surfaces for decoder output (VPP input) and for VPP output (encoder input)
    mfxU16 nSurfNumDecVPP = DecRequest.NumFrameSuggested + VPPRequest[0].NumFrameSuggested + VPPParams.AsyncDepth;
    mfxU16 nSurfNumVPPEnc = EncRequest.NumFrameSuggested + VPPRequest[1].NumFrameSuggested + VPPParams.AsyncDepth;

    // Initialize shared surfaces for decoder, VPP and encode
    // - Note that no buffer memory is allocated, for opaque memory this is handled by Media SDK internally
    // - Frame surface array keeps reference to all surfaces
    // - Opaque memory is configured with the mfxExtOpaqueSurfaceAlloc extended buffers

    // we need vector of raw pointers for opaque buffer;
    std::vector<mfxFrameSurface1*> pSurfaces(nSurfNumDecVPP);
    // additional vector for resource control
    std::vector<std::unique_ptr<mfxFrameSurface1>> pSurfacesPtrs(nSurfNumDecVPP + nSurfNumVPPEnc);
    for (int i = 0; i < nSurfNumDecVPP; i++) {
        pSurfacesPtrs[i].reset(new mfxFrameSurface1);
        pSurfaces[i] = pSurfacesPtrs[i].get();
        MSDK_CHECK_POINTER(pSurfaces[i], MFX_ERR_MEMORY_ALLOC);
        memset(pSurfaces[i], 0, sizeof(mfxFrameSurface1));
        pSurfaces[i]->Info = DecRequest.Info;
    }

    std::vector<mfxFrameSurface1*> pSurfaces2(nSurfNumVPPEnc);
    for (int i = 0; i < nSurfNumVPPEnc; i++) {
        pSurfacesPtrs[i + nSurfNumDecVPP].reset(new mfxFrameSurface1);
        pSurfaces2[i] = pSurfacesPtrs[i + nSurfNumDecVPP].get();
        MSDK_CHECK_POINTER(pSurfaces2[i], MFX_ERR_MEMORY_ALLOC);
        memset(pSurfaces2[i], 0, sizeof(mfxFrameSurface1));
        pSurfaces2[i]->Info = EncRequest.Info;
    }

    mfxExtOpaqueSurfaceAlloc extOpaqueAllocDec;
    memset(&extOpaqueAllocDec, 0, sizeof(extOpaqueAllocDec));
    extOpaqueAllocDec.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
    extOpaqueAllocDec.Header.BufferSz = sizeof(mfxExtOpaqueSurfaceAlloc);
    mfxExtBuffer* pExtParamsDec = (mfxExtBuffer*) & extOpaqueAllocDec;

    mfxExtOpaqueSurfaceAlloc extOpaqueAllocVPP;
    memset(&extOpaqueAllocVPP, 0, sizeof(extOpaqueAllocVPP));
    extOpaqueAllocVPP.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
    extOpaqueAllocVPP.Header.BufferSz = sizeof(mfxExtOpaqueSurfaceAlloc);
    mfxExtBuffer* pExtParamsVPP = (mfxExtBuffer*) & extOpaqueAllocVPP;

    mfxExtOpaqueSurfaceAlloc extOpaqueAllocEnc;
    memset(&extOpaqueAllocEnc, 0, sizeof(extOpaqueAllocEnc));
    extOpaqueAllocEnc.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
    extOpaqueAllocEnc.Header.BufferSz = sizeof(mfxExtOpaqueSurfaceAlloc);
    mfxExtBuffer* pExtParamsENC = (mfxExtBuffer*) & extOpaqueAllocEnc;

    extOpaqueAllocDec.Out.Surfaces = pSurfaces.data();
    extOpaqueAllocDec.Out.NumSurface = nSurfNumDecVPP;
    extOpaqueAllocDec.Out.Type = DecRequest.Type;

    memcpy(&extOpaqueAllocVPP.In, &extOpaqueAllocDec.Out, sizeof(extOpaqueAllocDec.Out));
    extOpaqueAllocVPP.Out.Surfaces = pSurfaces2.data();
    extOpaqueAllocVPP.Out.NumSurface = nSurfNumVPPEnc;
    extOpaqueAllocVPP.Out.Type = EncRequest.Type;

    memcpy(&extOpaqueAllocEnc.In, &extOpaqueAllocVPP.Out, sizeof(extOpaqueAllocVPP.Out));

    printf("read 3dlut file 3dlut_65cubic.dat and config MSDK parameters!\n");

    mfxU32 lut3DMemID = 0;
    mfxHDL hDevice = NULL;
    const char*  lut3dFileName = "3dlut_65cubic.dat";
    // Create 3DLut Memory to hold 3DLut data
    session.GetHandle(MFX_HANDLE_VA_DISPLAY, &hDevice);
    sts = Create3DLutMemory((void*)&lut3DMemID, hDevice, lut3dFileName);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize extended buffer for frame processing
    // - Denoise           VPP denoise filter
    // - mfxExtVPPDoUse:   Define the processing algorithm to be used
    // - mfxExtVPPDenoise: Denoise configuration
    // - mfxExtBuffer:     Add extended buffers to VPP parameter configuration
    mfxExtVPPDoUse extDoUse;
    memset(&extDoUse, 0, sizeof(extDoUse));
    mfxU32 tabDoUseAlg[2];
    extDoUse.Header.BufferId = MFX_EXTBUFF_VPP_DOUSE;
    extDoUse.Header.BufferSz = sizeof(mfxExtVPPDoUse);
    extDoUse.NumAlg = 2;
    extDoUse.AlgList = tabDoUseAlg;
    tabDoUseAlg[0] = MFX_EXTBUFF_VPP_3DLUT;
    tabDoUseAlg[1] = MFX_EXTBUFF_VPP_SCALING;

    mfxExtVPP3DLut lut3DConfig;
    memset(&lut3DConfig, 0, sizeof(lut3DConfig));
    lut3DConfig.Header.BufferId     = MFX_EXTBUFF_VPP_3DLUT;
    lut3DConfig.Header.BufferSz     = sizeof(mfxExtVPP3DLut);

    lut3DConfig.ChannelMapping              = MFX_3DLUT_CHANNEL_MAPPING_RGB_RGB;
    lut3DConfig.BufferType                  = MFX_RESOURCE_VA_SURFACE;
    lut3DConfig.VideoBuffer.DataType        = MFX_DATA_TYPE_U16;
    lut3DConfig.VideoBuffer.MemLayout       = MFX_3DLUT_MEMORY_LAYOUT_INTEL_65LUT;
    lut3DConfig.VideoBuffer.MemId           = &lut3DMemID;

    mfxExtVPPScaling scalingConfig;
    memset(&scalingConfig, 0, sizeof(scalingConfig));
    scalingConfig.Header.BufferId   = MFX_EXTBUFF_VPP_SCALING;
    scalingConfig.Header.BufferSz   = sizeof(mfxExtVPPScaling);
    scalingConfig.ScalingMode       = MFX_SCALING_MODE_LOWPOWER;

    mfxExtBuffer* ExtBuffer[4];
    ExtBuffer[0] = (mfxExtBuffer*) &extDoUse;
    ExtBuffer[1] = (mfxExtBuffer*) &lut3DConfig;
    ExtBuffer[2] = (mfxExtBuffer*) &scalingConfig;
    ExtBuffer[3] = (mfxExtBuffer*) pExtParamsVPP;

    mfxDecParams.ExtParam = &pExtParamsDec;
    mfxDecParams.NumExtParam = 1;
    VPPParams.ExtParam =  (mfxExtBuffer**) &ExtBuffer[0];
    VPPParams.NumExtParam = 4;
    mfxEncParams.ExtParam = &pExtParamsENC;
    mfxEncParams.NumExtParam = 1;

    printf("initialize MSDK decoder, vpp, encoder!\n");
    // Initialize the Media SDK decoder
    sts = mfxDEC.Init(&mfxDecParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize the Media SDK encoder
    sts = mfxENC.Init(&mfxEncParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Initialize Media SDK VPP
    sts = mfxVPP.Init(&VPPParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Retrieve video parameters selected by encoder.
    // - BufferSizeInKB parameter is required to set bit stream buffer size
    mfxVideoParam par;
    memset(&par, 0, sizeof(par));
    sts = mfxENC.GetVideoParam(&par);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Create task pool to improve asynchronous performance (greater GPU utilization)
    mfxU16 taskPoolSize = mfxEncParams.AsyncDepth;  // number of tasks that can be submitted, before synchronizing is required
    std::vector<Task> pTasks(taskPoolSize);
    std::vector<std::vector<mfxU8>> bstTaskData(taskPoolSize);
    for (int i = 0; i < taskPoolSize; i++) {
        // Prepare Media SDK bit stream buffer
        pTasks[i] = {};
        pTasks[i].mfxBS.MaxLength = par.mfx.BufferSizeInKB * 1000;
        bstTaskData[i].resize(pTasks[i].mfxBS.MaxLength);
        pTasks[i].mfxBS.Data = bstTaskData[i].data();
        MSDK_CHECK_POINTER(pTasks[i].mfxBS.Data, MFX_ERR_MEMORY_ALLOC);
    }

    // ===================================
    // Start transcoding the frames
    //
    printf("start transcoding the frames!\n");

    mfxTime tStart, tEnd;
    mfxGetTime(&tStart);

    mfxSyncPoint syncpD, syncpV;
    mfxFrameSurface1* pmfxOutSurface = NULL;
    mfxU32 nFrame = 0;
    int nIndex = 0;
    int nIndex2 = 0;
    int nFirstSyncTask = 0;
    int nTaskIdx = 0;

    sts = MFX_ERR_NONE;
    //
    // Stage 1: Main transcoding loop
    //
    while (MFX_ERR_NONE <= sts || MFX_ERR_MORE_DATA == sts || MFX_ERR_MORE_SURFACE == sts) {
        nTaskIdx = GetFreeTaskIndex(pTasks.data(), taskPoolSize);      // Find free task
        if (MFX_ERR_NOT_FOUND == nTaskIdx) {
            // No more free tasks, need to sync
            sts = session.SyncOperation(pTasks[nFirstSyncTask].syncp, 60000);
            MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            sts = WriteBitStreamFrame(&pTasks[nFirstSyncTask].mfxBS, fSink.get());
            MSDK_BREAK_ON_ERROR(sts);

            pTasks[nFirstSyncTask].syncp = NULL;
            pTasks[nFirstSyncTask].mfxBS.DataLength = 0;
            pTasks[nFirstSyncTask].mfxBS.DataOffset = 0;
            nFirstSyncTask = (nFirstSyncTask + 1) % taskPoolSize;

            ++nFrame;
            if (bEnableOutput && (nFrame%100 == 0)) {
                printf("Frame number: %d\r", nFrame);
                fflush(stdout);
            }
        } else {
            if (MFX_WRN_DEVICE_BUSY == sts)
                MSDK_SLEEP(1);  // just wait and then repeat the same call to DecodeFrameAsync

            if (MFX_ERR_MORE_DATA == sts) {
                sts = ReadBitStreamData(&mfxBS, fSource.get());       // Read more data to input bit stream
                MSDK_BREAK_ON_ERROR(sts);
            }

            if (MFX_ERR_MORE_SURFACE == sts || MFX_ERR_NONE == sts) {
                nIndex = GetFreeSurfaceIndex(pSurfaces.data(), nSurfNumDecVPP);        // Find free frame surface
                MSDK_CHECK_ERROR(MFX_ERR_NOT_FOUND, nIndex, MFX_ERR_MEMORY_ALLOC);
            }
            printf("decode a frame asychronously!\n");
            // Decode a frame asychronously (returns immediately)
            sts = mfxDEC.DecodeFrameAsync(&mfxBS, pSurfaces[nIndex], &pmfxOutSurface, &syncpD);

            // Ignore warnings if output is available,
            // if no output and no action required just repeat the DecodeFrameAsync call
            if (MFX_ERR_NONE < sts && syncpD)
                sts = MFX_ERR_NONE;

            if (MFX_ERR_NONE == sts) {
                nIndex2 = GetFreeSurfaceIndex(pSurfaces2.data(), nSurfNumVPPEnc);      // Find free frame surface
                MSDK_CHECK_ERROR(MFX_ERR_NOT_FOUND, nIndex2, MFX_ERR_MEMORY_ALLOC);

                for (;;) {
                    // Process a frame asychronously (returns immediately)
                    printf("process a frame asychronously!\n");
                    sts = mfxVPP.RunFrameVPPAsync(pmfxOutSurface, pSurfaces2[nIndex2], NULL, &syncpV);

                    if (MFX_ERR_NONE < sts && !syncpV) {    // repeat the call if warning and no output
                        if (MFX_WRN_DEVICE_BUSY == sts)
                            MSDK_SLEEP(1);  // wait if device is busy
                    } else if (MFX_ERR_NONE < sts && syncpV) {
                        sts = MFX_ERR_NONE;     // ignore warnings if output is available
                        break;
                    } else
                        break;  // not a warning
                }

                // VPP needs more data, let decoder decode another frame as input
                if (MFX_ERR_MORE_DATA == sts) {
                    continue;
                } else if (MFX_ERR_MORE_SURFACE == sts) {
                    // Not relevant for the illustrated workload! Therefore not handled.
                    // Relevant for cases when VPP produces more frames at output than consumes at input. E.g. framerate conversion 30 fps -> 60 fps
                    break;
                } else
                    MSDK_BREAK_ON_ERROR(sts);

                for (;;) {
                    // Encode a frame asychronously (returns immediately)
                    printf("encode a frame asychronously!\n");
                    sts = mfxENC.EncodeFrameAsync(NULL, pSurfaces2[nIndex2], &pTasks[nTaskIdx].mfxBS, &pTasks[nTaskIdx].syncp);

                    if (MFX_ERR_NONE < sts && !pTasks[nTaskIdx].syncp) {    // repeat the call if warning and no output
                        if (MFX_WRN_DEVICE_BUSY == sts)
                            MSDK_SLEEP(1);  // wait if device is busy
                    } else if (MFX_ERR_NONE < sts && pTasks[nTaskIdx].syncp) {
                        sts = MFX_ERR_NONE;     // ignore warnings if output is available
                        break;
                    } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
                        // Allocate more bitstream buffer memory here if needed...
                        break;
                    } else
                        break;
                }

                if (MFX_ERR_MORE_DATA == sts) {
                    // MFX_ERR_MORE_DATA indicates encoder need more input, request more surfaces from previous operation
                    sts = MFX_ERR_NONE;
                    continue;
                }
            }
        }
    }

    // MFX_ERR_MORE_DATA means that file has ended, need to go to buffering loop, exit in case of other errors
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    //
    // Stage 2: Retrieve the buffered decoded frames
    //
    while (MFX_ERR_NONE <= sts || MFX_ERR_MORE_SURFACE == sts) {
        nTaskIdx = GetFreeTaskIndex(pTasks.data(), taskPoolSize);      // Find free task
        if (MFX_ERR_NOT_FOUND == nTaskIdx) {
            // No more free tasks, need to sync
            sts = session.SyncOperation(pTasks[nFirstSyncTask].syncp, 60000);
            MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            sts = WriteBitStreamFrame(&pTasks[nFirstSyncTask].mfxBS, fSink.get());
            MSDK_BREAK_ON_ERROR(sts);

            pTasks[nFirstSyncTask].syncp = NULL;
            pTasks[nFirstSyncTask].mfxBS.DataLength = 0;
            pTasks[nFirstSyncTask].mfxBS.DataOffset = 0;
            nFirstSyncTask = (nFirstSyncTask + 1) % taskPoolSize;

            ++nFrame;
            if (bEnableOutput) {
                printf("Frame number: %d\r", nFrame);
                fflush(stdout);
            }
        } else {
            if (MFX_WRN_DEVICE_BUSY == sts)
                MSDK_SLEEP(1);

            nIndex = GetFreeSurfaceIndex(pSurfaces.data(), nSurfNumDecVPP);        // Find free frame surface
            MSDK_CHECK_ERROR(MFX_ERR_NOT_FOUND, nIndex, MFX_ERR_MEMORY_ALLOC);

            // Decode a frame asychronously (returns immediately)
            sts = mfxDEC.DecodeFrameAsync(NULL, pSurfaces[nIndex], &pmfxOutSurface, &syncpD);

            // Ignore warnings if output is available,
            // if no output and no action required just repeat the DecodeFrameAsync call
            if (MFX_ERR_NONE < sts && syncpD)
                sts = MFX_ERR_NONE;

            if (MFX_ERR_NONE == sts) {
                nIndex2 = GetFreeSurfaceIndex(pSurfaces2.data(), nSurfNumVPPEnc);      // Find free frame surface
                MSDK_CHECK_ERROR(MFX_ERR_NOT_FOUND, nIndex2, MFX_ERR_MEMORY_ALLOC);

                for (;;) {
                    // Process a frame asychronously (returns immediately)
                    sts = mfxVPP.RunFrameVPPAsync(pmfxOutSurface, pSurfaces2[nIndex2], NULL, &syncpV);

                    if (MFX_ERR_NONE < sts && !syncpV) {    // repeat the call if warning and no output
                        if (MFX_WRN_DEVICE_BUSY == sts)
                            MSDK_SLEEP(1);  // wait if device is busy
                    } else if (MFX_ERR_NONE < sts && syncpV) {
                        sts = MFX_ERR_NONE;     // ignore warnings if output is available
                        break;
                    } else
                        break;  // not a warning
                }

                // VPP needs more data, let decoder decode another frame as input
                if (MFX_ERR_MORE_DATA == sts) {
                    continue;
                } else if (MFX_ERR_MORE_SURFACE == sts) {
                    // Not relevant for the illustrated workload! Therefore not handled.
                    // Relevant for cases when VPP produces more frames at output than consumes at input. E.g. framerate conversion 30 fps -> 60 fps
                    break;
                } else
                    MSDK_BREAK_ON_ERROR(sts);

                for (;;) {
                    // Encode a frame asychronously (returns immediately)
                    sts = mfxENC.EncodeFrameAsync(NULL, pSurfaces2[nIndex2], &pTasks[nTaskIdx].mfxBS, &pTasks[nTaskIdx].syncp);

                    if (MFX_ERR_NONE < sts && !pTasks[nTaskIdx].syncp) {    // repeat the call if warning and no output
                        if (MFX_WRN_DEVICE_BUSY == sts)
                            MSDK_SLEEP(1);  // wait if device is busy
                    } else if (MFX_ERR_NONE < sts && pTasks[nTaskIdx].syncp) {
                        sts = MFX_ERR_NONE;     // ignore warnings if output is available
                        break;
                    } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
                        // Allocate more bitstream buffer memory here if needed...
                        break;
                    } else
                        break;
                }

                if (MFX_ERR_MORE_DATA == sts) {
                    // MFX_ERR_MORE_DATA indicates encoder need more input, request more surfaces from previous operation
                    sts = MFX_ERR_NONE;
                    continue;
                }
            }
        }
    }

    // MFX_ERR_MORE_DATA indicates that all decode buffers has been fetched, exit in case of other errors
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    //
    // Stage 3: Retrieve buffered frames from VPP
    //
    while (MFX_ERR_NONE <= sts || MFX_ERR_MORE_DATA == sts || MFX_ERR_MORE_SURFACE == sts) {
        nTaskIdx = GetFreeTaskIndex(pTasks.data(), taskPoolSize);      // Find free task
        if (MFX_ERR_NOT_FOUND == nTaskIdx) {
            // No more free tasks, need to sync
            sts = session.SyncOperation(pTasks[nFirstSyncTask].syncp, 60000);
            MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            sts = WriteBitStreamFrame(&pTasks[nFirstSyncTask].mfxBS, fSink.get());
            MSDK_BREAK_ON_ERROR(sts);

            pTasks[nFirstSyncTask].syncp = NULL;
            pTasks[nFirstSyncTask].mfxBS.DataLength = 0;
            pTasks[nFirstSyncTask].mfxBS.DataOffset = 0;
            nFirstSyncTask = (nFirstSyncTask + 1) % taskPoolSize;

            ++nFrame;
            if (bEnableOutput) {
                printf("Frame number: %d\r", nFrame);
                fflush(stdout);
            }
        } else {
            nIndex2 = GetFreeSurfaceIndex(pSurfaces2.data(), nSurfNumVPPEnc);      // Find free frame surface
            MSDK_CHECK_ERROR(MFX_ERR_NOT_FOUND, nIndex2, MFX_ERR_MEMORY_ALLOC);

            for (;;) {
                // Process a frame asychronously (returns immediately)
                sts = mfxVPP.RunFrameVPPAsync(NULL, pSurfaces2[nIndex2], NULL, &syncpV);

                if (MFX_ERR_NONE < sts && !syncpV) {    // repeat the call if warning and no output
                    if (MFX_WRN_DEVICE_BUSY == sts)
                        MSDK_SLEEP(1);  // wait if device is busy
                } else if (MFX_ERR_NONE < sts && syncpV) {
                    sts = MFX_ERR_NONE;     // ignore warnings if output is available
                    break;
                } else
                    break;  // not a warning
            }

            if (MFX_ERR_MORE_SURFACE == sts) {
                // Not relevant for the illustrated workload! Therefore not handled.
                // Relevant for cases when VPP produces more frames at output than consumes at input. E.g. framerate conversion 30 fps -> 60 fps
                break;
            } else
                MSDK_BREAK_ON_ERROR(sts);

            for (;;) {
                // Encode a frame asychronously (returns immediately)
                sts = mfxENC.EncodeFrameAsync(NULL, pSurfaces2[nIndex2], &pTasks[nTaskIdx].mfxBS, &pTasks[nTaskIdx].syncp);

                if (MFX_ERR_NONE < sts && !pTasks[nTaskIdx].syncp) {    // repeat the call if warning and no output
                    if (MFX_WRN_DEVICE_BUSY == sts)
                        MSDK_SLEEP(1);  // wait if device is busy
                } else if (MFX_ERR_NONE < sts && pTasks[nTaskIdx].syncp) {
                    sts = MFX_ERR_NONE;     // ignore warnings if output is available
                    break;
                } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
                    // Allocate more bitstream buffer memory here if needed...
                    break;
                } else
                    break;
            }

            if (MFX_ERR_MORE_DATA == sts) {
                // MFX_ERR_MORE_DATA indicates encoder need more input, request more surfaces from previous operation
                sts = MFX_ERR_NONE;
                continue;
            }
        }
    }

    // MFX_ERR_MORE_DATA indicates that all VPP buffers has been fetched, exit in case of other errors
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    //
    // Stage 4: Retrieve the buffered encoded frames
    //
    while (MFX_ERR_NONE <= sts) {
        nTaskIdx = GetFreeTaskIndex(pTasks.data(), taskPoolSize);      // Find free task
        if (MFX_ERR_NOT_FOUND == nTaskIdx) {
            // No more free tasks, need to sync
            sts = session.SyncOperation(pTasks[nFirstSyncTask].syncp, 60000);
            MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            sts = WriteBitStreamFrame(&pTasks[nFirstSyncTask].mfxBS, fSink.get());
            MSDK_BREAK_ON_ERROR(sts);

            pTasks[nFirstSyncTask].syncp = NULL;
            pTasks[nFirstSyncTask].mfxBS.DataLength = 0;
            pTasks[nFirstSyncTask].mfxBS.DataOffset = 0;
            nFirstSyncTask = (nFirstSyncTask + 1) % taskPoolSize;

            ++nFrame;
            if (bEnableOutput) {
                printf("Frame number: %d\r", nFrame);
                fflush(stdout);
            }
        } else {
            for (;;) {
                // Encode a frame asychronously (returns immediately)
                sts = mfxENC.EncodeFrameAsync(NULL, NULL, &pTasks[nTaskIdx].mfxBS, &pTasks[nTaskIdx].syncp);

                if (MFX_ERR_NONE < sts && !pTasks[nTaskIdx].syncp) {    // repeat the call if warning and no output
                    if (MFX_WRN_DEVICE_BUSY == sts)
                        MSDK_SLEEP(1);  // wait if device is busy
                } else if (MFX_ERR_NONE < sts && pTasks[nTaskIdx].syncp) {
                    sts = MFX_ERR_NONE;     // ignore warnings if output is available
                    break;
                } else
                    break;
            }
        }
    }

    // MFX_ERR_MORE_DATA indicates that there are no more buffered frames, exit in case of other errors
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    //
    // Stage 5: Sync all remaining tasks in task pool
    //
    while (pTasks[nFirstSyncTask].syncp) {
        sts = session.SyncOperation(pTasks[nFirstSyncTask].syncp, 60000);
        MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

        sts = WriteBitStreamFrame(&pTasks[nFirstSyncTask].mfxBS, fSink.get());
        MSDK_BREAK_ON_ERROR(sts);

        pTasks[nFirstSyncTask].syncp = NULL;
        pTasks[nFirstSyncTask].mfxBS.DataLength = 0;
        pTasks[nFirstSyncTask].mfxBS.DataOffset = 0;
        nFirstSyncTask = (nFirstSyncTask + 1) % taskPoolSize;

        ++nFrame;
        if (bEnableOutput) {
            printf("Frame number: %d\r", nFrame);
            fflush(stdout);
        }
    }

    mfxGetTime(&tEnd);
    double elapsed = TimeDiffMsec(tEnd, tStart) / 1000;
    double fps = ((double)nFrame / elapsed);
    printf("\nExecution time: %3.2f s (%3.2f fps)\n", elapsed, fps);

    // ===================================================================
    // Clean up resources
    //  - It is recommended to close Media SDK components first, before releasing allocated surfaces, since
    //    some surfaces may still be locked by internal Media SDK resources.

    mfxENC.Close();
    mfxDEC.Close();
    mfxVPP.Close();
    // session closed automatically on destruction

    sts = Release3DLutMemory(&lut3DMemID, &hDevice);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    Release();

    return 0;
}
