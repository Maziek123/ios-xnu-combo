extern "C" { int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf,3,4))); size_t strlcpy(char *, const char *, size_t); }
extern "C" { void *memcpy(void *, const void *, size_t); void bzero(void *, size_t); int memcmp(const void *, const void *, size_t); }
extern "C" int strncmp(const char *, const char *, size_t);
