/* op2shot OUT.png: screenshot through the render server (CARenderServerRenderDisplay into an IOSurface). */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		const char *outPath = argc > 1 ? argv[1] : "/private/var/tmp/shot.png";
		int w = argc > 3 ? atoi(argv[2]) : 1242, h = argc > 3 ? atoi(argv[3]) : 2208;
		void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
		void (*render)(mach_port_t, CFStringRef, IOSurfaceRef, int, int) = dlsym(qc, "CARenderServerRenderDisplay");
		if (!render) { printf("no CARenderServerRenderDisplay\n"); return 1; }
		NSDictionary *p = @{ (id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfaceBytesPerElement : @4,
		                     (id)kIOSurfaceBytesPerRow : @(w * 4), (id)kIOSurfacePixelFormat : @('BGRA'),
		                     (id)kIOSurfaceAllocSize : @(w * h * 4) };
		IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
		if (!s) { printf("IOSurfaceCreate failed\n"); return 1; }
		render(0, CFSTR("LCD"), s, 0, 0);
		IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
		void *base = IOSurfaceGetBaseAddress(s);
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef c = CGBitmapContextCreate(base, w, h, 8, IOSurfaceGetBytesPerRow(s), cs,
		    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
		CGImageRef img = CGBitmapContextCreateImage(c);
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)outPath, strlen(outPath), false);
		CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
		CGImageDestinationAddImage(d, img, NULL);
		bool ok = CGImageDestinationFinalize(d);
		printf("%s %dx%d -> %s\n", ok ? "saved" : "FAILED", w, h, outPath);
	}
	return 0;
}
