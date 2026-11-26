typedef const void *CFTypeRef; typedef const struct __CFString *CFStringRef; typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFDictionary *CFDictionaryRef; typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFAllocator *CFAllocatorRef; typedef CFTypeRef CFPropertyListRef; typedef unsigned char Boolean;
typedef long CFIndex; typedef unsigned int CFStringEncoding; enum { kCFStringEncodingUTF8 = 0x08000100 };
typedef struct { int x; } CFDictionaryKeyCallBacks, CFDictionaryValueCallBacks, CFArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks; extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
CFStringRef __CFStringMakeConstantString(const char *);
#define CFSTR(s) __CFStringMakeConstantString(s)
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef, const void **, const void **, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
CFArrayRef CFArrayCreate(CFAllocatorRef, const void **, CFIndex, const CFArrayCallBacks *);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding); void CFRelease(CFTypeRef);
