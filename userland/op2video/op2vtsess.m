/* op2vtsess FILE: compressed samples (AVAssetReader passthrough) -> VTDecompressionSession; prints every callback. */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
static int gCalls, gImages;
static void out(void *r, void *f, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
	(void)r; (void)f; (void)dur;
	gCalls++; if (img) gImages++;
	if (gCalls <= 5 || st) printf("callback %d: status %d flags 0x%x image %p pts %lld\n", gCalls, (int)st, fl, img, pts.value);
}
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		AVURLAsset *a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
		AVAssetTrack *t = [a tracksWithMediaType:AVMediaTypeVideo].firstObject;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:a error:nil];
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:nil];
		[r addOutput:o];
		if (![r startReading]) { printf("startReading: %s\n", r.error.description.UTF8String); return 1; }
		CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
		VTDecompressionOutputCallbackRecord cb = { out, NULL };
		NSDictionary *attrs = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
		VTDecompressionSessionRef s = NULL;
		OSStatus st = VTDecompressionSessionCreate(NULL, fd, NULL, (__bridge CFDictionaryRef)attrs, &cb, &s);
		printf("VTDecompressionSessionCreate %d\n", (int)st);
		if (st) return 1;
		int n = 0;
		CMSampleBufferRef sb;
		while ((sb = [o copyNextSampleBuffer])) {
			VTDecodeInfoFlags info = 0;
			st = VTDecompressionSessionDecodeFrame(s, sb, 0, NULL, &info);
			if (n++ < 3 || st) printf("DecodeFrame %d -> %d info 0x%x\n", n, (int)st, info);
			CFRelease(sb);
		}
		st = VTDecompressionSessionFinishDelayedFrames(s);
		printf("FinishDelayedFrames %d\n", (int)st);
		VTDecompressionSessionWaitForAsynchronousFrames(s);
		printf("%d samples sent, %d callbacks, %d images\n", n, gCalls, gImages);
	}
	return 0;
}
