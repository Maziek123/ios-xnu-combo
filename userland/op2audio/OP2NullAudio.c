/*
 * OP2NullAudio: a silent built-in audio device for iOS 12 on the OnePlus 2 (AudioServerPlugIn).
 *
 * The phone has no audio driver yet (the real codec is a Qualcomm WCD9330 behind the ADSP), and iOS's own
 * built-in device stack (VirtualAudio over AppleEmbeddedAudio/IOAudio2 kernel drivers) has nothing under it, so
 * mediaserverd never got a usable device. This plug-in, loaded by CoreAudio from
 * /Library/Audio/Plug-Ins/HAL/OP2NullAudio.driver, publishes one built-in output device: 48 kHz, stereo,
 * 32-bit float. It keeps time from the host clock and discards the samples. Everything else in the system
 * (audio sessions, AVPlayer clocks) can then run as if there were a speaker.
 *
 * Objects: plug-in (1) -> device (2) -> output stream (3). No controls.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define kPlugIn       kAudioObjectPlugInObject   /* 1 */
#define kDevice       2
#define kStream       3
#define kRate         48000.0
#define kChannels     2
#define kPeriod       512                         /* zero time stamp period, frames */
#define kDeviceUID    "OP2NullAudio_Device"
#define kModelUID     "OP2NullAudio_Model"

static AudioServerPlugInHostRef gHost;
static atomic_uint gRefs = 1;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gIORunning;
static UInt64 gAnchorHost, gTimestampCount;
static Float64 gHostTicksPerFrame;

/* ---------------------------------------------------------------- format */

static AudioStreamBasicDescription
fmt(void)
{
	AudioStreamBasicDescription d = { 0 };
	d.mSampleRate = kRate;
	d.mFormatID = kAudioFormatLinearPCM;
	d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
	d.mBytesPerPacket = 4 * kChannels;
	d.mFramesPerPacket = 1;
	d.mBytesPerFrame = 4 * kChannels;
	d.mChannelsPerFrame = kChannels;
	d.mBitsPerChannel = 32;
	return d;
}

/* ---------------------------------------------------------------- COM boilerplate */

static HRESULT
QueryInterface(void *self, REFIID iid, LPVOID *out)
{
	CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
	Boolean ok = CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID);
	CFRelease(req);
	if (!ok) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	atomic_fetch_add(&gRefs, 1);
	*out = self;
	return S_OK;
}

static ULONG
AddRef(void *self)
{
	(void)self;
	return atomic_fetch_add(&gRefs, 1) + 1;
}

static ULONG
Release(void *self)
{
	(void)self;
	return atomic_fetch_sub(&gRefs, 1) - 1;
}

static OSStatus
Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
	(void)d;
	gHost = host;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	Float64 hostHz = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
	gHostTicksPerFrame = hostHz / kRate;
	return 0;
}

static OSStatus
CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{
	(void)d; (void)desc; (void)ci; (void)out;
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev)
{
	(void)d; (void)dev;
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{
	(void)d; (void)dev; (void)ci;
	return 0;
}

static OSStatus
RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{
	(void)d; (void)dev; (void)ci;
	return 0;
}

static OSStatus
PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
	(void)d; (void)dev; (void)action; (void)info;
	return 0;
}

static OSStatus
AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
	(void)d; (void)dev; (void)action; (void)info;
	return 0;
}

/* ---------------------------------------------------------------- properties */

/* Size of a property, or 0 when the object does not have it. */
static UInt32
propSize(AudioObjectID obj, const AudioObjectPropertyAddress *a)
{
	switch (obj) {
	case kPlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			return sizeof(AudioClassID);
		case kAudioObjectPropertyOwner:
			return sizeof(AudioObjectID);
		case kAudioObjectPropertyManufacturer:
		case kAudioPlugInPropertyResourceBundle:
			return sizeof(CFStringRef);
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
		case kAudioPlugInPropertyTranslateUIDToDevice:
			return sizeof(AudioObjectID);
		}
		return 0;
	case kDevice:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			return sizeof(AudioClassID);
		case kAudioObjectPropertyOwner:
		case kAudioDevicePropertyRelatedDevices:
			return sizeof(AudioObjectID);
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			return sizeof(CFStringRef);
		case kAudioObjectPropertyOwnedObjects:
			return (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : sizeof(AudioObjectID);
		case kAudioDevicePropertyStreams:
			return (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : sizeof(AudioObjectID);
		case kAudioObjectPropertyControlList:
			return 0 + 1;   /* present, empty: handled below */
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			return sizeof(UInt32);
		case kAudioDevicePropertyNominalSampleRate:
			return sizeof(Float64);
		case kAudioDevicePropertyAvailableNominalSampleRates:
			return sizeof(AudioValueRange);
		case kAudioDevicePropertyPreferredChannelsForStereo:
			return 2 * sizeof(UInt32);
		}
		return 0;
	case kStream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			return sizeof(AudioClassID);
		case kAudioObjectPropertyOwner:
			return sizeof(AudioObjectID);
		case kAudioObjectPropertyOwnedObjects:
			return 0 + 1;   /* present, empty */
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
			return sizeof(UInt32);
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			return sizeof(AudioStreamBasicDescription);
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			return sizeof(AudioStreamRangedDescription);
		}
		return 0;
	}
	return 0;
}

static Boolean
HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
	(void)d; (void)pid;
	return propSize(obj, a) != 0;
}

static OSStatus
IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
    Boolean *out)
{
	(void)d; (void)pid;
	if (propSize(obj, a) == 0)
		return kAudioHardwareUnknownPropertyError;
	/* sample rate and formats accept their only value */
	*out = (obj == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
	    (obj == kStream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
	    a->mSelector == kAudioStreamPropertyPhysicalFormat));
	return 0;
}

static OSStatus
GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 *out)
{
	(void)d; (void)pid; (void)qs; (void)q;
	UInt32 n = propSize(obj, a);
	if (n == 0)
		return kAudioHardwareUnknownPropertyError;
	*out = (n == 1) ? 0 : n;   /* 1 = present but empty */
	return 0;
}

#define PUT(type, value) do { if (inSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
	*(type *)out = (value); *outSize = sizeof(type); return 0; } while (0)

static OSStatus
GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 inSize, UInt32 *outSize, void *out)
{
	(void)d; (void)pid;
	if (propSize(obj, a) == 0)
		return kAudioHardwareUnknownPropertyError;
	switch (obj) {
	case kPlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
		case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("OnePlus 2 port"));
		case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList: PUT(AudioObjectID, kDevice);
		case kAudioPlugInPropertyTranslateUIDToDevice: {
			CFStringRef uid = (qs == sizeof(CFStringRef) && q) ? *(const CFStringRef *)q : NULL;
			PUT(AudioObjectID, (uid && CFEqual(uid, CFSTR(kDeviceUID))) ? kDevice : kAudioObjectUnknown);
		}
		}
		break;
	case kDevice:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kPlugIn);
		case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR("Speaker"));
		case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("OnePlus 2 port"));
		case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(kDeviceUID));
		case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kModelUID));
		case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, kDevice);
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams: PUT(AudioObjectID, kStream);
		case kAudioObjectPropertyControlList: *outSize = 0; return 0;
		case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeBuiltIn);
		case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
		case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
		case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, gIORunning > 0);
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 1);
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
		case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kPeriod);
		case kAudioDevicePropertyNominalSampleRate: PUT(Float64, kRate);
		case kAudioDevicePropertyAvailableNominalSampleRates: {
			AudioValueRange r = { kRate, kRate };
			PUT(AudioValueRange, r);
		}
		case kAudioDevicePropertyPreferredChannelsForStereo:
			if (inSize < 2 * sizeof(UInt32))
				return kAudioHardwareBadPropertySizeError;
			((UInt32 *)out)[0] = 1;
			((UInt32 *)out)[1] = 2;
			*outSize = 2 * sizeof(UInt32);
			return 0;
		}
		break;
	case kStream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
		case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
		case kAudioObjectPropertyOwner: PUT(AudioObjectID, kDevice);
		case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
		case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
		case kAudioStreamPropertyDirection: PUT(UInt32, 0);   /* 0 = output */
		case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeSpeaker);
		case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
		case kAudioStreamPropertyLatency: PUT(UInt32, 0);
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, fmt());
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats: {
			AudioStreamRangedDescription r;
			r.mFormat = fmt();
			r.mSampleRateRange.mMinimum = r.mSampleRateRange.mMaximum = kRate;
			PUT(AudioStreamRangedDescription, r);
		}
		}
		break;
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus
SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 inSize, const void *in)
{
	(void)d; (void)pid; (void)qs; (void)q;
	if (obj == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate && inSize == sizeof(Float64))
		return (*(const Float64 *)in == kRate) ? 0 : kAudioHardwareIllegalOperationError;
	if (obj == kStream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
	    a->mSelector == kAudioStreamPropertyPhysicalFormat) && inSize == sizeof(AudioStreamBasicDescription)) {
		const AudioStreamBasicDescription *f = in;
		return (f->mSampleRate == kRate && f->mChannelsPerFrame == kChannels) ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	return kAudioHardwareUnsupportedOperationError;
}

/* ---------------------------------------------------------------- IO: keep time, drop samples */

static OSStatus
StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	(void)d; (void)dev; (void)client;
	pthread_mutex_lock(&gLock);
	if (gIORunning++ == 0) {
		gAnchorHost = mach_absolute_time();
		gTimestampCount = 0;
	}
	pthread_mutex_unlock(&gLock);
	return 0;
}

static OSStatus
StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	(void)d; (void)dev; (void)client;
	pthread_mutex_lock(&gLock);
	if (gIORunning > 0)
		gIORunning--;
	pthread_mutex_unlock(&gLock);
	return 0;
}

static OSStatus
GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *sampleTime,
    UInt64 *hostTime, UInt64 *seed)
{
	(void)d; (void)dev; (void)client;
	pthread_mutex_lock(&gLock);
	UInt64 now = mach_absolute_time();
	Float64 ticksPerPeriod = gHostTicksPerFrame * kPeriod;
	/* advance to the latest period boundary not after now */
	while ((Float64)(now - gAnchorHost) >= ticksPerPeriod * (Float64)(gTimestampCount + 1))
		gTimestampCount++;
	*sampleTime = (Float64)gTimestampCount * kPeriod;
	*hostTime = gAnchorHost + (UInt64)(ticksPerPeriod * (Float64)gTimestampCount);
	*seed = 1;
	pthread_mutex_unlock(&gLock);
	return 0;
}

static OSStatus
WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will,
    Boolean *inPlace)
{
	(void)d; (void)dev; (void)client;
	*will = (op == kAudioServerPlugInIOOperationWriteMix);
	*inPlace = true;
	return 0;
}

static OSStatus
BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
    const AudioServerPlugInIOCycleInfo *ci)
{
	(void)d; (void)dev; (void)client; (void)op; (void)frames; (void)ci;
	return 0;
}

static OSStatus
DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary)
{
	(void)d; (void)dev; (void)stream; (void)client; (void)op; (void)frames; (void)ci; (void)main; (void)secondary;
	return 0;   /* the samples go nowhere */
}

static OSStatus
EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
    const AudioServerPlugInIOCycleInfo *ci)
{
	(void)d; (void)dev; (void)client; (void)op; (void)frames; (void)ci;
	return 0;
}

/* ---------------------------------------------------------------- factory */

static AudioServerPlugInDriverInterface gInterface = {
	NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient,
	RemoveDeviceClient, PerformConfigChange, AbortConfigChange, HasProperty, IsPropertySettable,
	GetPropertyDataSize, GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation,
	BeginIOOperation, DoIOOperation, EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

__attribute__((visibility("default"))) void *
OP2NullAudio_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
	(void)alloc;
	if (!CFEqual(type, kAudioServerPlugInTypeUUID))
		return NULL;
	return &gInterfacePtr;
}
