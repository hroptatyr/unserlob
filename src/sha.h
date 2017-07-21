#if !defined INCLUDED_sha_h_
#define INCLUDED_sha_h_
#include <stdint.h>

typedef uint8_t sha_t[20U];

extern int sha(sha_t *restrict tgt, const char *buf, size_t bsz);

#endif	/* INCLUDED_sha_h_ */
