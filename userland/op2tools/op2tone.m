/* op2tone: audio session + route + 1 s AudioQueue tone; prints every status (OP2_UID=501 to run as mobile). */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>
static double gPhase;
static void cb(void *u, AudioQueueRef q, AudioQueueBufferRef b) {
	(void)u;
	float *s = b->mAudioData;
	UInt32 n = b->mAudioDataBytesCapacity / 8;
	for (UInt32 i = 0; i < n; i++) { float v = 0.2f * sinf((float)gPhase); s[2*i] = s[2*i+1] = v; gPhase += 2 * M_PI * 440 / 48000; }
	b->mAudioDataByteSize = n * 8;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
int main(void) {
	@autoreleasepool {
		if (getenv("OP2_UID")) { int u = atoi(getenv("OP2_UID")); setgid(u); setuid(u); }
		NSError *e = nil;
		AVAudioSession *s = [AVAudioSession sharedInstance];
		BOOL ok = [s setCategory:AVAudioSessionCategoryPlayback error:&e];
		printf("setCategory %d %s\n", ok, e.description.UTF8String ?: "");
		e = nil; ok = [s setActive:YES error:&e];
		printf("setActive %d %s\n", ok, e.description.UTF8String ?: "");
		printf("route: %s\n", s.currentRoute.description.UTF8String);
		printf("sampleRate %.0f outputChannels %ld\n", s.sampleRate, (long)s.outputNumberOfChannels);
		AudioStreamBasicDescription f = { 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
		AudioQueueRef q = NULL;
		OSStatus st = AudioQueueNewOutput(&f, cb, NULL, NULL, NULL, 0, &q);
		printf("AudioQueueNewOutput %d\n", (int)st);
		if (st) return 1;
		for (int i = 0; i < 3; i++) { AudioQueueBufferRef b; AudioQueueAllocateBuffer(q, 8 * 2048, &b); cb(NULL, q, b); }
		st = AudioQueueStart(q, NULL);
		printf("AudioQueueStart %d\n", (int)st);
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
		AudioTimeStamp ts = {0};
		st = AudioQueueGetCurrentTime(q, NULL, &ts, NULL);
		printf("after 1 s: GetCurrentTime %d sampleTime %.0f (48000 = clock running)\n", (int)st, ts.mSampleTime);
		AudioQueueStop(q, true);
	}
	return 0;
}
