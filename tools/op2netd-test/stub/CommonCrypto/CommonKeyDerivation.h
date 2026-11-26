#include <stddef.h>
#include <stdint.h>
enum { kCCPBKDF2 = 2, kCCPRFHmacAlgSHA1 = 1 };
int CCKeyDerivationPBKDF(int alg, const char *pw, size_t pwl, const uint8_t *salt, size_t sl, int prf, unsigned rounds, uint8_t *dk, size_t dkl);
