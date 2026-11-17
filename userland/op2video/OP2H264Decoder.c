/*
 * OP2H264Decoder: a software H.264 decoder for iOS 12's VideoToolbox (OnePlus 2 port, 2026-11-17).
 *
 * The iPhone decodes H.264 in hardware (H264H6.videodecoder, A8 video block); this phone has none of that, so
 * H264H6 does not register and no decoder serves 'avc1'. This library registers one: FFmpeg's H.264 decoder
 * (libavcodec, LGPL, built by userland/op2video/build-ffmpeg.sh) behind VideoToolbox's private decoder plug-in
 * interface, which was read off H264H6 in the iOS 12.5.7 shared cache:
 *   VTRegisterVideoDecoder('avc1', create)  ->  create: CMDerivedObjectCreate(alloc, &vtable, VTVideoDecoderGetClassID())
 *   vtable = { NULL, &CMBaseClass (version 1, storage size, finalize, ...), &VTVideoDecoderClass (version 3) }
 *   VTVideoDecoderClass: +0x08 startSession(decoder, session, formatDesc)
 *                        +0x10 decodeFrame(decoder, frame, sampleBuffer, flags, infoFlagsOut)
 *                        +0x18 copySupportedPropertyDictionary(decoder, dictOut)
 *   frames go back through VTDecoderSessionEmitDecodedFrame(session, frame, status, infoFlags, imageBuffer).
 * The private functions are looked up with dlsym (the iOS SDK does not list them).
 *
 * Output: NV12 video range ('420v'), IOSurface-backed, from the session's pixel buffer pool.
 * Load it into the decoding process and call op2_h264_register() (or let the constructor do it).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>

typedef void *CMBaseObjectRef;
typedef void *VTDecoderSessionRef;
typedef void *VTDecoderFrameRef;

/* ---------------------------------------------------------------- private API, by dlsym */

static OSStatus (*pCMDerivedObjectCreate)(CFAllocatorRef, const void *vtable, CFTypeID classID, CMBaseObjectRef *out);
static void *(*pCMBaseObjectGetDerivedStorage)(CMBaseObjectRef);
static CFTypeID (*pVTVideoDecoderGetClassID)(void);
static OSStatus (*pVTRegisterVideoDecoder)(FourCharCode, OSStatus (*)(FourCharCode, CFAllocatorRef, CMBaseObjectRef *));
static OSStatus (*pVTDecoderSessionSetPixelBufferAttributes)(VTDecoderSessionRef, CFDictionaryRef);
static CVPixelBufferPoolRef (*pVTDecoderSessionGetPixelBufferPool)(VTDecoderSessionRef);
static OSStatus (*pVTDecoderSessionEmitDecodedFrame)(VTDecoderSessionRef, VTDecoderFrameRef, OSStatus, uint32_t,
    CVImageBufferRef);

static void
log_line(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char b[256];
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	fprintf(stderr, "OP2H264: %s\n", b);
	FILE *f = fopen("/private/var/tmp/op2h264.log", "a");
	if (f) {
		fprintf(f, "[%d] %s\n", getpid(), b);
		fclose(f);
	}
}

static int
resolve(void)
{
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
	void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW);
	if (!vt || !cm)
		return -1;
	pCMDerivedObjectCreate = dlsym(cm, "CMDerivedObjectCreate");
	pCMBaseObjectGetDerivedStorage = dlsym(cm, "CMBaseObjectGetDerivedStorage");
	pVTVideoDecoderGetClassID = dlsym(vt, "VTVideoDecoderGetClassID");
	pVTRegisterVideoDecoder = dlsym(vt, "VTRegisterVideoDecoder");
	pVTDecoderSessionSetPixelBufferAttributes = dlsym(vt, "VTDecoderSessionSetPixelBufferAttributes");
	pVTDecoderSessionGetPixelBufferPool = dlsym(vt, "VTDecoderSessionGetPixelBufferPool");
	pVTDecoderSessionEmitDecodedFrame = dlsym(vt, "VTDecoderSessionEmitDecodedFrame");
	if (!pCMDerivedObjectCreate || !pCMBaseObjectGetDerivedStorage || !pVTVideoDecoderGetClassID ||
	    !pVTRegisterVideoDecoder || !pVTDecoderSessionSetPixelBufferAttributes ||
	    !pVTDecoderSessionGetPixelBufferPool || !pVTDecoderSessionEmitDecodedFrame)
		return -2;
	return 0;
}

/* ---------------------------------------------------------------- decoder state */

struct dec {
	VTDecoderSessionRef session;
	AVCodecContext *ctx;
	AVPacket *pkt;
	AVFrame *frame;
	pthread_mutex_t lock;
	int width, height;
	uint64_t decoded, emitted;
};

static struct dec *
D(CMBaseObjectRef o)
{
	return (struct dec *)pCMBaseObjectGetDerivedStorage(o);
}

/* yuv420p (FFmpeg) -> NV12 (CVPixelBuffer) */
static OSStatus
emit(struct dec *d, AVFrame *f)
{
	VTDecoderFrameRef handle = (VTDecoderFrameRef)f->opaque;
	CVPixelBufferPoolRef pool = pVTDecoderSessionGetPixelBufferPool(d->session);
	CVPixelBufferRef pb = NULL;
	OSStatus st = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) : -1;
	if (st != 0 || !pb) {
		log_line("no pixel buffer (%d)", (int)st);
		return pVTDecoderSessionEmitDecodedFrame(d->session, handle, kVTVideoDecoderMalfunctionErr, 0, NULL);
	}
	CVPixelBufferLockBaseAddress(pb, 0);
	int w = f->width < (int)CVPixelBufferGetWidth(pb) ? f->width : (int)CVPixelBufferGetWidth(pb);
	int h = f->height < (int)CVPixelBufferGetHeight(pb) ? f->height : (int)CVPixelBufferGetHeight(pb);
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
	uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	for (int r = 0; r < h; r++)
		memcpy(y + r * ys, f->data[0] + r * f->linesize[0], w);
	for (int r = 0; r < h / 2; r++) {
		const uint8_t *u = f->data[1] + r * f->linesize[1], *v = f->data[2] + r * f->linesize[2];
		uint8_t *o = uv + r * uvs;
		for (int c = 0; c < w / 2; c++) {
			o[2 * c] = u[c];
			o[2 * c + 1] = v[c];
		}
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	st = pVTDecoderSessionEmitDecodedFrame(d->session, handle, 0, 0, pb);
	if (d->emitted < 3)
		log_line("emit %p %dx%d -> %d", handle, w, h, (int)st);
	CVPixelBufferRelease(pb);
	d->emitted++;
	return st;
}

static void
drain(struct dec *d)
{
	while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
		d->decoded++;
		emit(d, d->frame);
		av_frame_unref(d->frame);
	}
}

/* ---------------------------------------------------------------- VTVideoDecoderClass */

static OSStatus
startSession(CMBaseObjectRef o, VTDecoderSessionRef session, CMVideoFormatDescriptionRef fd)
{
	struct dec *d = D(o);
	d->session = session;
	CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fd);
	d->width = dim.width;
	d->height = dim.height;

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	d->ctx = avcodec_alloc_context3(codec);
	if (!d->ctx)
		return kVTVideoDecoderMalfunctionErr;
	/* avcC from the format description's sample description extension atoms */
	CFDictionaryRef atoms = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
	CFDataRef avcC = atoms ? CFDictionaryGetValue(atoms, CFSTR("avcC")) : NULL;
	if (avcC && CFGetTypeID(avcC) == CFDataGetTypeID()) {
		CFIndex n = CFDataGetLength(avcC);
		d->ctx->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(d->ctx->extradata, CFDataGetBytePtr(avcC), n);
		d->ctx->extradata_size = (int)n;
	}
	/* Every frame leaves decodeFrame() before it returns: VideoToolbox's default is decode-order output, and the
	 * decoder class's delayed-frame slots are left empty (as H264H6 does), so nothing would ever flush frames held
	 * back by frame threading or B-frame reordering (the session waited forever). One thread, low delay. */
	d->ctx->thread_count = 1;
	d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	d->ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;   /* packet->opaque comes back as frame->opaque */
	int r = avcodec_open2(d->ctx, codec, NULL);
	if (r < 0) {
		log_line("avcodec_open2 failed %d", r);
		return kVTVideoDecoderMalfunctionErr;
	}
	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	pthread_mutex_init(&d->lock, NULL);

	int32_t fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &d->width);
	CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &d->height);
	CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
		kCVPixelBufferIOSurfacePropertiesKey };
	const void *v[] = { nf, nw, nh, ios };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	OSStatus st = pVTDecoderSessionSetPixelBufferAttributes(session, attrs);
	CFRelease(attrs); CFRelease(ios); CFRelease(nf); CFRelease(nw); CFRelease(nh);
	log_line("session %dx%d avcC %ld bytes, pixel buffer attributes -> %d", d->width, d->height,
	    avcC ? (long)CFDataGetLength(avcC) : 0L, (int)st);
	return 0;
}

static unsigned sCallsPts;

static OSStatus
decodeFrame(CMBaseObjectRef o, VTDecoderFrameRef frame, CMSampleBufferRef sb, uint32_t flags, uint32_t *infoOut)
{
	struct dec *d = D(o);
	if (infoOut)
		*infoOut = 0;
	CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
	size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
	pthread_mutex_lock(&d->lock);
	if (len == 0 || av_new_packet(d->pkt, (int)len) < 0) {
		pthread_mutex_unlock(&d->lock);
		return pVTDecoderSessionEmitDecodedFrame(d->session, frame, 0, kVTDecodeInfo_FrameDropped, NULL);
	}
	CMBlockBufferCopyDataBytes(bb, 0, len, d->pkt->data);
	CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
	d->pkt->pts = CMTIME_IS_VALID(pts) ? pts.value : AV_NOPTS_VALUE;
	if (CMSampleBufferGetNumSamples(sb) > 0 && sCallsPts++ < 600)
		log_line("  pts %lld flags 0x%x frame %p", (long long)d->pkt->pts, flags, frame);
	d->pkt->opaque = frame;
	int r = avcodec_send_packet(d->ctx, d->pkt);
	av_packet_unref(d->pkt);
	static unsigned sCalls;
	uint64_t before = d->emitted;
	if (r < 0) {
		pthread_mutex_unlock(&d->lock);
		if (sCalls++ < 600)
			log_line("decodeFrame #%u: %zu bytes, send_packet %d -> bad data", sCalls, len, r);
		return pVTDecoderSessionEmitDecodedFrame(d->session, frame, kVTVideoDecoderBadDataErr, 0, NULL);
	}
	drain(d);
	if (sCalls++ < 600)
		log_line("decodeFrame #%u: %zu bytes, send_packet %d, %llu frame(s) out", sCalls, len, r,
		    (unsigned long long)(d->emitted - before));
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/* +0x38: VTDecompressionSessionFinishDelayedFrames. Nothing is ever held (FFmpeg is patched for decode-order output,
 * patches/h264-decode-order.patch, AV_CODEC_FLAG_LOW_DELAY), so there is nothing to release. It must NOT drain and
 * flush FFmpeg: avcodec_flush_buffers() drops the reference frames, and every following P/B frame then decoded to
 * nothing (video stuck on its first frame after preroll and after every seek, 2026-11-17). */
static OSStatus
finishDelayedFrames(CMBaseObjectRef o)
{
	(void)o;
	return 0;
}

static OSStatus
copySupportedPropertyDictionary(CMBaseObjectRef o, CFDictionaryRef *out)
{
	(void)o;
	*out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	return 0;
}

/* ---------------------------------------------------------------- CMBaseClass */

static void
finalize(CMBaseObjectRef o)
{
	struct dec *d = D(o);
	if (d->ctx) {
		pthread_mutex_lock(&d->lock);
		avcodec_send_packet(d->ctx, NULL);   /* flush: frames still in the reorder delay */
		drain(d);
		pthread_mutex_unlock(&d->lock);
		log_line("finalize: %llu frames decoded, %llu emitted", d->decoded, d->emitted);
		avcodec_free_context(&d->ctx);
		av_packet_free(&d->pkt);
		av_frame_free(&d->frame);
		pthread_mutex_destroy(&d->lock);
	}
}

static CFStringRef
copyDebugDescription(CMBaseObjectRef o)
{
	(void)o;
	return CFStringCreateCopy(NULL, CFSTR("<OP2H264Decoder (FFmpeg software H.264)>"));
}

static OSStatus
copyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	(void)o; (void)a;
	if (CFEqual(key, CFSTR("UsingHardwareAcceleratedVideoDecoder"))) {
		*(CFBooleanRef *)out = kCFBooleanFalse;
		return 0;
	}
	return -12782;   /* kCMBaseObjectError_ValueNotAvailable */
}

static OSStatus
setProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o; (void)key; (void)value;
	return -12784;   /* kCMBaseObjectError_PropertyNotSupported */
}

struct base_class {
	uint64_t version;
	uint64_t derivedStorageSize;
	void *equal, *invalidate;
	void (*finalize)(CMBaseObjectRef);
	CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
	OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	void *notificationBarrier, *protocolVTables;
};

struct decoder_class {
	uint64_t version;
	OSStatus (*startSession)(CMBaseObjectRef, VTDecoderSessionRef, CMVideoFormatDescriptionRef);
	OSStatus (*decodeFrame)(CMBaseObjectRef, VTDecoderFrameRef, CMSampleBufferRef, uint32_t, uint32_t *);
	OSStatus (*copySupportedPropertyDictionary)(CMBaseObjectRef, CFDictionaryRef *);
	void *setProperties, *copySerializableProperties, *canAcceptFormatDescription;   /* +0x20..+0x30: not provided */
	OSStatus (*finishDelayedFrames)(CMBaseObjectRef);   /* +0x38 (VTDecompressionSessionFinishDelayedFrames) */
	void *reserved[8];
};

static const struct base_class gBase = { 1, sizeof(struct dec), NULL, NULL, finalize, copyDebugDescription,
	copyProperty, setProperty, NULL, NULL };
static const struct decoder_class gDecoder = { 3, startSession, decodeFrame, copySupportedPropertyDictionary, NULL, NULL,
	NULL, finishDelayedFrames, { 0 } };
static const struct { void *reserved; const struct base_class *base; const struct decoder_class *derived; }
	gVTable = { NULL, &gBase, &gDecoder };

static OSStatus
create(FourCharCode codec, CFAllocatorRef alloc, CMBaseObjectRef *out)
{
	(void)codec;
	OSStatus st = pCMDerivedObjectCreate(alloc, &gVTable, pVTVideoDecoderGetClassID(), out);
	if (st == 0)
		memset(D(*out), 0, sizeof(struct dec));
	log_line("create '%c%c%c%c' -> %d", (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8), (char)codec, (int)st);
	return st;
}

__attribute__((visibility("default"))) int
op2_h264_register(void)
{
	static int done;
	if (done)
		return 0;
	int r = resolve();
	if (r) {
		log_line("private VideoToolbox/CoreMedia functions missing (%d)", r);
		return r;
	}
	OSStatus st = pVTRegisterVideoDecoder('avc1', create);
	done = 1;
	log_line("registered for avc1 -> %d", (int)st);
	return (int)st;
}

__attribute__((constructor)) static void
op2_h264_init(void)
{
	op2_h264_register();
}
