/* op2fbshot OUT.png: screenshot of what the panel shows (AppleCLCD debug selector 96 copies the scanout buffer). */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
kern_return_t IOConnectCallMethod(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, size_t, uint64_t *,
    uint32_t *, void *, size_t *);
int main(int argc, char **argv) {
	@autoreleasepool {
		const char *outPath = argc > 1 ? argv[1] : "/private/var/tmp/fb.png";
		io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("AppleCLCD"));
		io_connect_t c = 0;
		kern_return_t kr = svc ? IOServiceOpen(svc, mach_task_self(), 0, &c) : KERN_FAILURE;
		if (kr) { printf("IOServiceOpen AppleCLCD: 0x%x\n", kr); return 1; }
		size_t cap = 1088 * 4 * 1920 + 65536, sz = cap;
		uint8_t *buf = malloc(cap);
		uint64_t out[2] = { 0, 0 }; uint32_t outCnt = 2;
		kr = IOConnectCallMethod(c, 96, NULL, 0, NULL, 0, out, &outCnt, buf, &sz);
		if (kr) { printf("copy panel: 0x%x\n", kr); return 1; }
		int w = (int)out[0], bpr = (int)out[1], h = (int)(sz / bpr);
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, bpr, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
		CGImageRef img = CGBitmapContextCreateImage(ctx);
		CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)outPath, strlen(outPath), false);
		CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
		CGImageDestinationAddImage(d, img, NULL);
		printf("%s %dx%d (bpr %d) -> %s\n", CGImageDestinationFinalize(d) ? "saved" : "FAILED", w, h, bpr, outPath);
	}
	return 0;
}
