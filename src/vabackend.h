#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <stdio.h>
#include <pthread.h>
#include "list.h"
#include "direct/nv-driver.h"
#include "common.h"
#include "stats.h"

#define SURFACE_QUEUE_SIZE 16
#define MAX_IMAGE_COUNT 64
#define MAX_PROFILES 32

typedef struct {
    void        *buf;
    uint64_t    size;
    uint64_t    allocated;
} AppendableBuffer;

typedef enum
{
    OBJECT_TYPE_CONFIG,
    OBJECT_TYPE_CONTEXT,
    OBJECT_TYPE_SURFACE,
    OBJECT_TYPE_BUFFER,
    OBJECT_TYPE_IMAGE
} ObjectType;

typedef struct Object_t
{
    ObjectType      type;
    VAGenericID     id;
    void            *obj;
} *Object;

typedef struct
{
    unsigned int    elements;
    size_t          size;
    VABufferType    bufferType;
    void            *ptr;
    size_t          offset;
} NVBuffer;

struct _NVContext;
struct _BackingImage;

typedef struct
{
    uint32_t                width;
    uint32_t                height;
    cudaVideoSurfaceFormat  format;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    int                     pictureIdx;
    uint64_t                pictureIdxLastUsed; // drv->pictureIdxUseCounter value when last decoded into or referenced
    VAContextID             contextId; // last context to use this target; remains valid as an ID after destruction
    /* The fork keeps a direct pointer as well: the encode paths and the
     * picture-index bookkeeping need the context itself, not just its ID.
     * nvDestroyContext() clears it on every surface it owned, so "context is
     * gone" is representable as NULL rather than a dangling pointer. */
    struct _NVContext       *context;
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    int                     order_hint; //needed for AV1
    uint32_t                av1FrameWidth;  //AV1 frame size last decoded into this surface,
    uint32_t                av1FrameHeight; //which can be smaller than the surface
    VAProcColorStandardType colorStandard;
    bool                    colorRangeFull;
    struct _BackingImage    *backingImage;
    int                     resolving;
    // Number of in-flight vaRenderPicture() blits currently reading this
    // surface as a source or writing it as the render target. vaDestroySurfaces()
    // waits for this to drain before detaching the backing image, so a client
    // that destroys a surface while a VideoProc blit is still using it cannot
    // pull the memory out from under the copy.
    atomic_uint             videoProcReads;
    // Protected by drv->objectCreationMutex. Once set, VideoProc calls may
    // no longer take a new read reference to this surface.
    bool                    destroying;
    int                     fourcc;
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    bool                    decodeFailed;
    /* Host-memory pixel buffer for encode-only IPC path (no CUDA) */
    void                   *hostPixelData;
    uint32_t                hostPixelSize;
    bool                    hostPixelIsShm; /* true if hostPixelData points to SHM (don't free) */
    /* Imported DMA-BUF for IPC encode (fd from Steam's GPU capture) */
    int                     importedDmaBufFd;
    uint32_t                importedPitches[4];
    uint32_t                importedOffsets[4];
    uint32_t                importedNumPlanes;
    uint32_t                importedDataSize;
    // Set once the surface has been handed out as a dma-buf. The client can
    // then read the frame without a driver call that would wait for it.
    atomic_bool             exported;
} NVSurface;

typedef enum
{
    NV_FORMAT_NONE,
    NV_FORMAT_NV12,
    NV_FORMAT_P010,
    NV_FORMAT_P012,
    NV_FORMAT_P016,
    NV_FORMAT_444P,
    NV_FORMAT_Q416,
    NV_FORMAT_ARGB
} NVFormat;

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    NVFormat    format;
    NVBuffer    *imageBuffer;
} NVImage;

typedef struct {
    CUexternalMemory extMem;
    CUmipmappedArray mipmapArray;
} NVCudaImage;

typedef struct _BackingImage {
    NVSurface   *surface;
    EGLImage    image;
    CUarray     arrays[3];
    uint32_t    width;
    uint32_t    height;
    int         fourcc;
    int         fds[4];
    dev_t       st_dev[4];
    ino_t       st_ino[4];
    int         offsets[4];
    int         strides[4];
    uint64_t    mods[4];
    uint32_t    size[4];
    //direct backend only
    NVCudaImage cudaImages[3];
    NVFormat    format;
    /* NVIDIA opaque fds for CUDA import (IPC encode path) */
    int         nvFds[4];
    uint32_t    memorySizes[4];
    VAProcColorStandardType colorStandard;
    bool        colorRangeFull;
    uint32_t    totalSize;
    CUexternalMemory extMem;
    bool        isSingleBuffer;
    bool        isExternalBuffer;
    bool        borrowedCudaResources;
    struct _BackingImage *borrowedBackingImage;
    atomic_uint borrowCount;
    bool        syncInitialized;
    bool        resolving;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    void        *externalMapping;
    uint32_t    externalMappingSize;
    CUdeviceptr externalDevicePtr;
    uint32_t    externalDeviceSize;
    uint64_t    detachedSerial;
} BackingImage;

struct _NVDriver;

typedef struct {
    const char *name;
    bool (*initExporter)(struct _NVDriver *drv);
    void (*releaseExporter)(struct _NVDriver *drv);
    bool (*exportCudaPtr)(struct _NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
    void (*detachBackingImageFromSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*realiseSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*fillExportDescriptor)(struct _NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
    void (*destroyAllBackingImage)(struct _NVDriver *drv);
} NVBackend;

/*
 * Per-call log state for the two sites a CUDA-less client drives once per
 * frame. Same idea as the NVENC*Log family in nvenc.h -- keep what was last
 * printed, print only on a difference -- but these hang off NVDriver rather
 * than an encode context, so they have to be declared here: nvenc.h includes
 * this header, not the other way round.
 *
 * vaDeriveImage and vaCreateSurfaces2 are once-per-session calls on the CUDA
 * path, which is why they were logged unconditionally. In encode-only mode a
 * client derives an image and allocates a surface for every frame, so the same
 * two lines turn into the bulk of the log.
 */
typedef struct {
    uint32_t width, height, size;
    int32_t  format;
} NVENCHostImageLog;

typedef struct {
    uint32_t width, height, format, numSurfaces, memType;
    uint32_t numAttribs;
} NVENCSurfaceRequestLog;

typedef struct _NVDriver
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    NvencFunctions          *nv;
    CUcontext               cudaContext;
    CUvideoctxlock          vidLock;
    Array/*<Object>*/       objects;
    pthread_mutex_t         objectCreationMutex;
    VAGenericID             nextObjId;
    uint64_t                pictureIdxUseCounter; // updated atomically
    bool                    useCorrectNV12Format;
    bool                    supports16BitSurface;
    bool                    supports444Surface;
    int                     cudaGpuId;
    int                     drmFd;
    pthread_mutex_t         exportMutex;
    pthread_mutex_t         imagesMutex;
    Array/*<NVEGLImage>*/   images;
    const NVBackend         *backend;
    //fields for direct backend
    NVDriverContext         driverContext;
    //fields for egl backend
    EGLDeviceEXT            eglDevice;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    int                     numFramesPresented;
    int                     profileCount;
    VAProfile               profiles[MAX_PROFILES];
    bool                    nvencAvailable;
    bool                    cudaAvailable;  /* false when 32-bit CUDA fails */
    /*
     * NVENC capability probe results. Populated once, lazily, on the first
     * encode-config query. Used to gate profile advertisement so we don't
     * advertise (e.g.) HEVC Main422_10 on hardware that lacks YUV422
     * encode. All bools default to false.
     *
     * nvencCapsProbed means "we tried", nvencCapsValid means "and got an
     * answer worth believing". They are separate because a failed probe must
     * not read as "this GPU supports nothing" -- in encode-only mode the probe
     * has to go out to the 64-bit helper and can legitimately come back empty,
     * and treating that as a negative answer strips the encode entrypoint from
     * every profile. Callers fall back to the built-in list when !valid.
     */
    bool                    nvencCapsProbed;
    bool                    nvencCapsValid;
    /* Last-logged shapes for the two per-call sites that a CUDA-less client
     * drives once per frame -- see the typedefs above nvCreateSurfaces2's
     * cousins in nvenc.h for the rest of this family. */
    NVENCHostImageLog       loggedHostImage;
    NVENCSurfaceRequestLog  loggedSurfaceRequest;
    bool                    nvencSupportsH264;
    bool                    nvencSupportsH264High10;
    bool                    nvencSupportsHEVC;
    bool                    nvencSupportsHEVCMain10;
    bool                    nvencSupportsHEVCFrext;      /* umbrella cap for 422/444 profiles */
    bool                    nvencSupportsAV1;
    bool                    nvencSupportsAV1_10bit;
    /* Input format caps — from NV_ENC_CAPS_SUPPORT_10BIT_ENCODE /
     * SUPPORT_YUV444_ENCODE per codec, plus SupportedInputFormat scan. */
    bool                    nvencSupportsInputYUV444;
    bool                    nvencSupportsInputYUV444_10;
    bool                    nvencSupportsInputYUV422;    /* NV16 / P210 */
    bool                    nvencSupportsInputYUV422_10;
    /* Maximum encode dimensions, per codec, from NV_ENC_CAPS_WIDTH_MAX /
     * HEIGHT_MAX. These differ by codec and generation (H.264 is capped at
     * 4096 on every current part, while HEVC and AV1 reach 8192), so a single
     * hardcoded number either under-reports 8K HEVC/AV1 or over-promises
     * H.264. Zero means "not probed" and callers fall back to a safe default. */
    uint32_t                nvencMaxWidthH264,  nvencMaxHeightH264;
    uint32_t                nvencMaxWidthHEVC,  nvencMaxHeightHEVC;
    uint32_t                nvencMaxWidthAV1,   nvencMaxHeightAV1;
    CUmodule                videoProcModule;
    CUfunction              nv12ToArgbKernel;
    CUfunction              p010ToArgbKernel;
    CUmodule                videoProcModuleP010;
    bool                    videoProcKernelP010Failed;
    bool                    videoProcKernelFailed;
    CUdeviceptr             videoProcYBuffer;
    CUdeviceptr             videoProcUVBuffer;
    CUdeviceptr             videoProcArgbBuffer;
    size_t                  videoProcYBufferSize;
    size_t                  videoProcUVBufferSize;
    size_t                  videoProcArgbBufferSize;
    void                    *cpuVideoProcYBuffer;
    void                    *cpuVideoProcUVBuffer;
    void                    *cpuVideoProcArgbBuffer;
    size_t                  cpuVideoProcYBufferSize;
    size_t                  cpuVideoProcUVBufferSize;
    size_t                  cpuVideoProcArgbBufferSize;
    bool                    statsEnabled;
    uint64_t                statsLogInterval;
    atomic_uint_fast64_t    stats[NV_STAT_COUNT];
    uint64_t                maxDetachedBackingImageBytes;
    uint32_t                maxDetachedBackingImages;
    uint64_t                detachedBackingImageSerial;
} NVDriver;

struct _NVCodec;

typedef struct _NVContext
{
    NVDriver            *drv;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    uint32_t            width;
    uint32_t            height;
    CUvideodecoder      decoder;
    NVSurface           *renderTarget;
    NVSurface           *displayTarget;
    void                *codecData;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    bitstreamBuffer;
    AppendableBuffer    sliceOffsets;
    bool                av1SequenceEnableRestoration;
    uint32_t            av1TileOffsetsSeen;
    uint32_t            av1TileMinOffset;
    uint32_t            av1TileMaxEnd;
    bool                av1BitstreamCompacted;
    /* AV1 frames may be coded smaller than the sequence maximum the context was
     * created with (frame_size_override_flag). NVDEC scales the decoded frame
     * to fill the decoder's display area, so the display area has to follow
     * the frame size: requested is what the current picture needs, applied is
     * what the decoder was last configured with (0 = the context size). */
    uint32_t            requestedDisplayWidth;
    uint32_t            requestedDisplayHeight;
    uint32_t            appliedDisplayWidth;
    uint32_t            appliedDisplayHeight;
    bool                decoderHasDecoded;
    NVSurface           *lastQueuedSurface; //most recent surface handed to the resolve thread
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    cudaVideoCodec      cudaCodec;
    cudaVideoSurfaceFormat decoderSurfaceFormat;
    cudaVideoChromaFormat decoderChromaFormat;
    int                 decoderBitDepth;
    // NVDEC can address at most 32 decode surfaces, but VA-API lets a client
    // render into as many surfaces as it likes. pictureIdxOwners maps each
    // index to the surface currently holding it.
    NVSurface          *pictureIdxOwners[32]; // protected by drv->objectCreationMutex
    bool                pictureIdxAssigned; // an index has been handed out, so the decoder's format is fixed
    pthread_t           resolveThread;
    bool                resolveThreadStarted;
    bool                resolveThreadFailed; // protected by resolveMutex
    pthread_mutex_t     resolveMutex;
    pthread_cond_t      resolveCondition;
    pthread_cond_t      videoProcCondition; // protected by drv->objectCreationMutex
    unsigned int        activeVideoProcCalls;
    unsigned int        activeVideoProcRenders;
    bool                videoProcDestroying;
    unsigned int        activeDecodeCalls; // protected by drv->objectCreationMutex
    bool                decodeDestroying;
    NVSurface**         surfaceQueue; // protected by resolveMutex
    size_t              surfaceQueueCapacity;
    size_t              surfaceQueueReadIdx;
    size_t              surfaceQueueWriteIdx;
    volatile bool       exiting;
    pthread_mutex_t     surfaceCreationMutex;
    int                 surfaceCount;
    bool                isEncode;
    void               *encodeData; /* NVENCContext* for encode contexts */
} NVContext;

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
    bool                    isEncode;
    uint32_t                rcMode;
    bool                    allowBframes;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);
typedef enum {
    NV_PICTURE_BEGIN,
    NV_PICTURE_RENDER,
    NV_PICTURE_END,
} NVPictureOperation;
typedef void (*CodecBeginPictureFunc)(NVContext*, VASurfaceID);

// Internals exposed for the stats subsystem (src/stats.c).
pid_t nv_gettid(void);
FILE *nvStatsOutput(void);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
struct _NVCodec {
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
    CodecBeginPictureFunc beginPicture;
    // Optional lifecycle hooks. checkPicture and surfaceDestroyed run with
    // objectCreationMutex held; the other hooks run during an active decode call.
    // checkPicture may reject a call before the backend changes its target.
    VAStatus (*checkPicture)(NVContext*, NVPictureOperation);
    // Override buffer dispatch when a codec needs submission error reporting.
    VAStatus (*renderPicture)(NVContext*, VABufferID*, int);
    // Validate the assembled picture before CUDA submission; release any
    // codec-owned pending output on failure before returning the error.
    VAStatus (*prepareDecode)(NVContext*);
    // Called after decode with default field order set. May adjust metadata;
    // return false to defer resolution or release an incomplete failed output.
    bool (*finishDecode)(NVContext*, VAStatus);
    // Release codec-owned pending output when CUDA context push/pop fails.
    void (*abortPicture)(NVContext*);
    void (*surfaceDestroyed)(NVContext*, VASurfaceID, NVSurface*);
};

typedef struct _NVCodec NVCodec;

// Codec helpers. Surface lookup must stay under objectCreationMutex while the
// returned pointer is used; buffer lookup has the same lifetime as RenderPicture.
NVSurface *nvGetSurface(NVDriver *drv, VASurfaceID id);
NVBuffer *nvGetBuffer(NVDriver *drv, VABufferID id);
void nvSetSurfaceResolving(NVSurface *surface, bool resolving);

typedef struct
{
    uint32_t bppc; // bytes per pixel per channel
    uint32_t numPlanes;
    uint32_t fourcc;
    bool     is16bits;
    bool     isYuv444;
    NVFormatPlane plane[3];
    VAImageFormat vaFormat;
} NVFormatInfo;

extern const NVFormatInfo formatsInfo[];

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf);

uint32_t nvExportableFourcc(uint32_t fourcc);
// Cross-TU lookup / allocation helpers used by the moved encode dispatch code
// (src/nvenc_dispatch.c) as well as vabackend.c itself.
void *nvGetObjectPtr(NVDriver *drv, ObjectType type, VAGenericID id);
Object nvAllocateObject(NVDriver *drv, ObjectType type, size_t allocatePtrSize);
const char *nvColorStandardName(VAProcColorStandardType colorStandard);
VAProcColorStandardType nvColorStandardFromMatrixCoefficients(uint8_t matrixCoefficients);
void nvSurfaceResetColorMetadata(NVSurface *surface);
void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull);
void nvSurfaceCopyColorMetadata(NVSurface *dst, const NVSurface *src);
void nvSurfaceCopyColorMetadataFromBackingImage(NVSurface *surface, const BackingImage *img);
void nvBackingImageStoreSurfaceColorMetadata(BackingImage *img, const NVSurface *surface);
void nvBackingImageCopyColorMetadata(BackingImage *dst, const BackingImage *src);
bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
bool nvdLogDebugEnabled(void);
bool nvdSingleBufferForced(void);

/* True when NVD_LOG selected a destination. Exposed as a plain global rather
 * than an accessor so LOG_ENABLED() compiles to a single load and can be used
 * to skip work on hot paths without paying for a cross-TU call.
 *
 * LOG() itself is intentionally left unguarded — 76 call sites rely on the
 * macro supplying its own trailing semicolon, so it cannot become a
 * do/while(0) block. logger() early-returns when logging is off, which is
 * fine for the once-per-session sites. What LOG_ENABLED() is for is guarding
 * the *arguments*: any per-frame site that has to compute or compare something
 * before it can decide whether to log must do that work behind this check, or
 * it costs every frame whether or not anyone is listening. */
extern bool nvdLoggingEnabled;
#define LOG_ENABLED() (nvdLoggingEnabled)
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define CHECK_CUDA_RESULT_RETURN(err, ret) if (checkCudaErrors(err, __FILE__, __func__, __LINE__)) { return ret; }
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define LOG_DEBUG(...) do { if (nvdLogDebugEnabled()) { logger(__FILE__, __func__, __LINE__, __VA_ARGS__); } } while (0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTROFF(base, bytes) ((void *)((unsigned char *)(base) + (bytes)))
#define DECLARE_CODEC(name) \
    __attribute__((used)) \
    __attribute__((retain)) \
    __attribute__((section("nvd_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#define DECLARE_DISABLED_CODEC(name) \
    __attribute__((section("nvd_disabled_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#endif // VABACKEND_H
