/* op2surfport: IOSurface -> mach port / XPC object -> IOSurface round trip (what remote VideoToolbox does). */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreVideo/CoreVideo.h>
typedef void *xpc_object_t;   /* xpc.h is not in the iOS SDK */
extern mach_port_t IOSurfaceCreateMachPort(IOSurfaceRef);
extern IOSurfaceRef IOSurfaceLookupFromMachPort(mach_port_t);
extern xpc_object_t IOSurfaceCreateXPCObject(IOSurfaceRef);
extern IOSurfaceRef IOSurfaceLookupFromXPCObject(xpc_object_t);
int main(void) {
	@autoreleasepool {
		CVPixelBufferRef pb = NULL;
		NSDictionary *a = @{ (id)kCVPixelBufferIOSurfacePropertiesKey : @{} };
		CVReturn r = CVPixelBufferCreate(NULL, 640, 360, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)a, &pb);
		IOSurfaceRef s = pb ? CVPixelBufferGetIOSurface(pb) : NULL;
		printf("CVPixelBufferCreate %d, surface %p id %u planes %zu bpr0 %zu bpr1 %zu\n", r, s, s ? IOSurfaceGetID(s) : 0,
		       s ? IOSurfaceGetPlaneCount(s) : 0, s ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : 0, s ? IOSurfaceGetBytesPerRowOfPlane(s, 1) : 0);
		if (!s) return 1;
		mach_port_t p = IOSurfaceCreateMachPort(s);
		IOSurfaceRef s2 = p ? IOSurfaceLookupFromMachPort(p) : NULL;
		printf("mach port 0x%x -> lookup %p id %u\n", p, s2, s2 ? IOSurfaceGetID(s2) : 0);
		xpc_object_t x = IOSurfaceCreateXPCObject(s);
		IOSurfaceRef s3 = x ? IOSurfaceLookupFromXPCObject(x) : NULL;
		printf("xpc object %p -> lookup %p id %u\n", x, s3, s3 ? IOSurfaceGetID(s3) : 0);
		CVPixelBufferRef pb2 = NULL;
		if (s3) r = CVPixelBufferCreateWithIOSurface(NULL, s3, NULL, &pb2);
		printf("CVPixelBufferCreateWithIOSurface %d -> %p %zux%zu\n", r, pb2, pb2 ? CVPixelBufferGetWidth(pb2) : 0, pb2 ? CVPixelBufferGetHeight(pb2) : 0);
	}
	return 0;
}
