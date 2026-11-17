/* op2vtdec FILE [DECODER.dylib]: decode a movie's video through VideoToolbox (AVAssetReader), report frames + speed. */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <dlfcn.h>
#import <VideoToolbox/VideoToolbox.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		if (argc > 3) {   /* VideoToolbox first (its registry initialises on first use), then the decoder */
			CMVideoFormatDescriptionRef fd = NULL;
			CMVideoFormatDescriptionCreate(NULL, 'avc1', 16, 16, NULL, &fd);
			VTDecompressionSessionRef ds = NULL;
			OSStatus st = VTDecompressionSessionCreate(NULL, fd, NULL, NULL, NULL, &ds);
			printf("pre-init VTDecompressionSessionCreate -> %d\n", (int)st);
		}
		if (argc > 2 && !dlopen(argv[2], RTLD_NOW)) { printf("dlopen: %s\n", dlerror()); return 1; }
		AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
		AVAssetTrack *t = [a tracksWithMediaType:AVMediaTypeVideo].firstObject;
		if (!t) { printf("no video track\n"); return 1; }
		NSError *e = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:a error:&e];
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
			outputSettings:@{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) }];
		[r addOutput:o];
		if (![r startReading]) { printf("startReading failed: %s\n", r.error.description.UTF8String); return 1; }
		NSDate *t0 = [NSDate date];
		int n = 0, total = 0;
		CMSampleBufferRef sb;
		while ((sb = [o copyNextSampleBuffer])) {
			if (total++ < 3)
				printf("sample %d: image %p, data %p, numSamples %ld, valid %d\n", total, CMSampleBufferGetImageBuffer(sb),
				       CMSampleBufferGetDataBuffer(sb), (long)CMSampleBufferGetNumSamples(sb), CMSampleBufferIsValid(sb));
			CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
			if (pb && n == 30) {
				CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
				uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
				printf("frame 30: %zux%zu fmt %.4s Y[100,100]=%d bpr %zu/%zu\n", CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
				       (char *)&(uint32_t){ CFSwapInt32(CVPixelBufferGetPixelFormatType(pb)) },
				       y ? y[100 * CVPixelBufferGetBytesPerRowOfPlane(pb, 0) + 100] : -1,
				       CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1));
				CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
			}
			if (pb) n++;
			CFRelease(sb);
		}
		double s = -[t0 timeIntervalSinceNow];
		printf("%d sample buffers; status %ld, %d frames in %.2f s (%.0f fps) %s\n", total, (long)r.status, n, s, n / s, r.error.description.UTF8String ?: "");
	}
	return 0;
}
