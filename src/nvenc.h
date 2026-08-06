#ifndef NVENC_H
#define NVENC_H

#include <ffnvcodec/nvEncodeAPI.h>
#include <ffnvcodec/dynlink_loader.h>
#include <va/va.h>
#include <stdbool.h>
#include <stdint.h>
#include "vabackend.h"

// Encode-specific context, stored in NVContext->encodeData
// when created with VAEntrypointEncSlice.

typedef struct {
    NV_ENC_OUTPUT_PTR       bitstreamBuffer;
    bool                    allocated;
    void                   *lockedPtr;      //locked bitstream pointer
    uint32_t                lockedSize;
    bool                    locked;
} NVENCOutputBuffer;

typedef struct {
    void                           *encoder;        //NVENC session handle
    NV_ENCODE_API_FUNCTION_LIST     funcs;
    bool                            initialized;
    GUID                            codecGuid;
    GUID                            profileGuid;
    NV_ENC_CONFIG                   encodeConfig;
    NV_ENC_INITIALIZE_PARAMS        initParams;
    uint32_t                        width;
    uint32_t                        height;
    NV_ENC_BUFFER_FORMAT            inputFormat;
    bool                            seqParamSet;
    uint32_t                        rcMode;         //VA-API rate control mode
    uint32_t                        bitrate;        //bits/sec
    uint32_t                        maxBitrate;
    uint32_t                        frameRateNum;
    uint32_t                        frameRateDen;
    uint32_t                        intraPeriod;    //GOP length
    uint32_t                        ipPeriod;
    uint32_t                        qualityLevel;   //VA-API quality level (1-7 -> P1-P7)
    uint32_t                        vbvBufferSize;  //HRD buffer size (bits)
    uint32_t                        vbvInitialDelay; //HRD initial fullness (bits)
    uint64_t                        frameCount;
    NVENCOutputBuffer               outputBuffer;
    VABufferID                      currentCodedBufId;
    bool                            forceIDR;       //from idr_pic_flag
    NV_ENC_PIC_TYPE                 picType;        //from slice params
    bool                            useIPC;         //encode via 64-bit helper
    int                             ipcFd;          //socket fd, -1 if not connected
    void                           *shmPtr;         //mmap'd shared memory for frame data
    uint32_t                        shmSize;
    int                             shmFd;
    uint32_t                        maxWidth;
    uint32_t                        maxHeight;
    uint32_t                        temporalId;
    uint32_t                        numTemporalLayers; //temporal SVC layers (0/1 = disabled)
    bool                            allowBframes;
    /*
     * Persistent linear staging buffer + NVENC-registered resource, reused across
     * frames for the lifetime of the session instead of being allocated/registered
     * and freed/unregistered on every single frame. Repeatedly churning device
     * memory allocations and NVENC resource registrations was found to gradually
     * destabilize long-running encode sessions (crash after a few minutes).
     */
    CUdeviceptr                     linearBuffer;
    uint32_t                        linearBufferSize;
    NV_ENC_REGISTERED_PTR           registeredRes;
    uint32_t                        registeredWidth;
    uint32_t                        registeredHeight;
    uint32_t                        registeredPitch;
    /*
     * Last values actually programmed into NVENC via init/reconfigure. When
     * Chrome sends new rate-control or framerate values through misc params,
     * we call nvEncReconfigureEncoder only if these differ from the stored
     * requests (bitrate / maxBitrate / frameRateNum / frameRateDen). Without
     * this, WebRTC's BWE bitrate reductions are silently ignored, encoder
     * keeps emitting at the initial (much higher) bitrate, saturating the
     * uplink and causing the peer to see stalls / frozen frames.
     */
    uint32_t                        appliedBitrate;
    uint32_t                        appliedMaxBitrate;
    uint32_t                        appliedFrameRateNum;
    uint32_t                        appliedFrameRateDen;
    /*
     * QP hints. VAEncMiscParameterRateControl carries initial_qp / min_qp /
     * max_qp for CBR/VBR (bound the encoder's QP range) and CQP (initial_qp
     * seeds the constant QP). VAEncPictureParameterBuffer's pic_init_qp
     * (H.264) and init_qp (HEVC) can override per-picture for CQP.
     * 0 means "unset / use NVENC default".
     */
    uint32_t                        initialQP;
    uint32_t                        minQP;
    uint32_t                        maxQP;
    /* Per-picture QP override for the next frame (CQP path). */
    uint32_t                        picQP;
    /* Last CONSTQP value actually programmed via init/reconfigure; used to
     * detect per-picture QP changes and issue nvEncReconfigureEncoder. */
    uint32_t                        appliedConstQP;
} NVENCContext;

// Wraps VACodedBufferSegment with NVENC bitstream storage
typedef struct {
    VACodedBufferSegment    segment;
    void                   *bitstreamData;
    uint32_t                bitstreamSize;
    uint32_t                bitstreamAlloc;
    bool                    hasData;
} NVCodedBuffer;

bool nvenc_load(NvencFunctions **nvenc_dl);
void nvenc_unload(NvencFunctions **nvenc_dl);

bool nvenc_open_session(NVENCContext *nvencCtx, NvencFunctions *nvenc_dl, CUcontext cudaCtx);
void nvenc_close_session(NVENCContext *nvencCtx);

struct _NVDriver;
/*
 * Probe NVENC for supported codec GUIDs, profile GUIDs, input formats and
 * per-codec caps (10-bit, YUV444, YUV422). Opens a temporary NVENC session,
 * queries via nvEncGetEncodeGUIDs / nvEncGetEncodeProfileGUIDs /
 * nvEncGetSupportedInputFormats / nvEncGetEncodeCaps, then closes it.
 * Populates drv->nvenc*Supports* fields and sets drv->nvencCapsProbed=true.
 * Idempotent — a second call returns immediately.
 *
 * Requires drv->cudaAvailable == true. On CUDA-less builds (32-bit / IPC
 * fallback) probing is skipped and all caps stay false; the fallback
 * hardcoded list in nvenc_is_encode_profile still applies.
 *
 * Called lazily from nvenc_dispatch_create_config the first time an encode
 * profile is queried.
 */
bool nvenc_probe_caps(struct _NVDriver *drv);

bool nvenc_init_encoder(NVENCContext *nvencCtx, uint32_t width, uint32_t height,
                        GUID codecGuid, GUID profileGuid,
                        NV_ENC_TUNING_INFO tuningInfo);

/*
 * Apply any pending bitrate/framerate changes (as recorded in nvencCtx via
 * the codec misc-param handlers) to the running NVENC session using
 * nvEncReconfigureEncoder. No-op when nothing changed since the last
 * successful init/reconfigure. Returns false on a real API error; a mismatch
 * that can't be reconfigured is logged and treated as best-effort.
 */
bool nvenc_reconfigure_if_needed(NVENCContext *nvencCtx);

bool nvenc_alloc_output_buffer(NVENCContext *nvencCtx);
void nvenc_free_output_buffer(NVENCContext *nvencCtx);

bool nvenc_register_cuda_resource(NVENCContext *nvencCtx, CUdeviceptr devPtr,
                                  uint32_t width, uint32_t height, uint32_t pitch,
                                  NV_ENC_BUFFER_FORMAT format,
                                  NV_ENC_REGISTERED_PTR *outRegistered);
bool nvenc_map_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered,
                        NV_ENC_INPUT_PTR *outMapped, NV_ENC_BUFFER_FORMAT *outFmt);
bool nvenc_unmap_resource(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR mapped);
bool nvenc_unregister_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered);

int nvenc_encode_frame(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR inputBuffer,
                       NV_ENC_BUFFER_FORMAT bufferFmt,
                       uint32_t inputWidth, uint32_t inputHeight, uint32_t inputPitch,
                       NV_ENC_PIC_TYPE picType, uint32_t picFlags);

bool nvenc_lock_bitstream(NVENCContext *nvencCtx, void **outPtr, uint32_t *outSize);
bool nvenc_unlock_bitstream(NVENCContext *nvencCtx);

/*
 * Static allowlist of encode profiles the fork implements. Says nothing
 * about whether the underlying hardware/driver actually accepts them —
 * that requires nvenc_is_encode_profile_supported() plus a probed caps
 * struct.
 */
bool nvenc_is_encode_profile(VAProfile profile);
/*
 * True iff (a) profile is in the static allowlist AND (b) the runtime
 * capability probe found that NVENC on this box actually supports it —
 * codec GUID present, profile GUID present, required input format
 * present. Falls back to the static allowlist behavior when caps aren't
 * probed (drv->nvencCapsProbed == false), which happens on CUDA-less /
 * IPC-only builds.
 */
bool nvenc_is_encode_profile_supported(struct _NVDriver *drv, VAProfile profile);
GUID nvenc_va_profile_to_codec_guid(VAProfile profile);
GUID nvenc_va_profile_to_profile_guid(VAProfile profile);
/*
 * Chroma layout advertised by a VA-API profile (YUV420 / YUV422 /
 * YUV444). Used to advertise the correct VAConfigAttribRTFormat mask and
 * to choose the right NVENC input buffer format.
 */
typedef enum {
    NVENC_CHROMA_420 = 0,
    NVENC_CHROMA_422,
    NVENC_CHROMA_444,
} NvencChromaFormat;
NvencChromaFormat nvenc_profile_chroma(VAProfile profile);
NV_ENC_BUFFER_FORMAT nvenc_surface_format(VAProfile profile, int bitDepth);

/*
 * Encode-side VA-API entry points that used to live inline in vabackend.c and
 * are now split into src/nvenc_dispatch.c. vabackend.c calls these when the
 * VA config attribute query targets an encode profile (nvGetConfigAttributesEncode)
 * or the active context is an encode context (nvRenderPictureEncode / nvEndPictureEncode).
 */
void nvGetConfigAttributesEncode(VAProfile profile,
                                 VAConfigAttrib *attrib_list,
                                 int num_attribs);
void nvRenderPictureEncode(NVContext *nvCtx, NVBuffer *buf);
VAStatus nvEndPictureEncode(NVDriver *drv, NVContext *nvCtx);
/* Called from nvCreateConfig when entrypoint == VAEntrypointEncSlice: rejects
 * profiles NVENC does not handle, allocates a fresh NVConfig, and pre-fills
 * it with the fork's encode-config defaults + rate-control + bit-depth from
 * the client's attribute list. */
VAStatus nvenc_dispatch_create_config(NVDriver *drv, VAProfile profile,
                                      VAEntrypoint entrypoint,
                                      VAConfigAttrib *attrib_list,
                                      int num_attribs,
                                      VAConfigID *config_id_out);
/* Called from nvQueryConfigAttributes when cfg->isEncode: writes the
 * VAConfigAttribRTFormat entry back out for the already-created config. */
VAStatus nvenc_dispatch_query_config_attributes(NVConfig *cfg,
                                                VAConfigAttrib *attrib_list,
                                                int *num_attribs);
/* Called from nvCreateContext when cfg->isEncode: allocates NVENCContext,
 * opens the NVENC session (or falls back to the IPC path if CUDA is
 * unavailable) and hands back a fresh VAContextID via *context_out. */
VAStatus nvenc_dispatch_create_context(NVDriver *drv, NVConfig *cfg,
                                       uint32_t picture_width,
                                       uint32_t picture_height,
                                       VAContextID *context_out);
/* Called from destroyContext when nvCtx->isEncode: tears down the NVENC
 * session or IPC channel and releases NVENCContext. The caller is
 * responsible for wrapping this in the cuCtxPushCurrent/PopCurrent pair
 * (encode teardown may touch CUDA resources on the direct path). */
void nvenc_dispatch_destroy_context(NVDriver *drv, NVContext *nvCtx);
/* Called from nvBeginPicture when nvCtx->isEncode: records the render
 * target and resets per-frame encode state (pic type, forceIDR). */
VAStatus nvenc_dispatch_begin_picture(NVContext *nvCtx, NVSurface *surface);
/* Called from nvDeriveImage in IPC-encode-only mode (!drv->cudaAvailable):
 * allocates a host-memory NV12/P010 backing for the surface and hands the
 * caller a VAImage that points straight at it, so a client (Steam's
 * ffmpeg) can write captured frames through vaMapBuffer with no GPU
 * roundtrip. */
VAStatus nvenc_dispatch_derive_image_hostmem(NVDriver *drv, NVSurface *surfaceObj,
                                             VASurfaceID surface, VAImage *image);
/* Called from nvPutImage in IPC-encode-only mode (!drv->cudaAvailable):
 * copies the image buffer into the surface's host-memory pixel store,
 * for later IPC transmission to the encode helper. */
VAStatus nvenc_dispatch_put_image_hostmem(NVSurface *surfaceObj, NVImage *imageObj);

void h264enc_handle_sequence_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_picture_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_slice_params(NVENCContext *ctx, NVBuffer *buf);
void h264enc_handle_misc_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_sequence_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_picture_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_slice_params(NVENCContext *ctx, NVBuffer *buf);
void hevcenc_handle_misc_params(NVENCContext *ctx, NVBuffer *buf);
void av1enc_handle_sequence_params(NVENCContext *ctx, NVBuffer *buf);
void av1enc_handle_picture_params(NVENCContext *ctx, NVBuffer *buf);
void av1enc_handle_slice_params(NVENCContext *ctx, NVBuffer *buf);
void av1enc_handle_misc_params(NVENCContext *ctx, NVBuffer *buf);

#endif // NVENC_H
