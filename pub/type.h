#ifndef _PUB_TYPE_H_
#define _PUB_TYPE_H_

#include <stdint.h>
#include <stdlib.h>

typedef uint8_t byte_t;
typedef signed long ssize_t;

#ifdef __GNUC__
	#if (__GNUC__ > 3) || ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1))
		#define INLINE static __inline__ __attribute__((always_inline))
	#else
		#define INLINE static __inline__
	#endif
#elif defined(_MSC_VER)
	#define INLINE static __forceinline
#elif (defined(__BORLANDC__) || defined(__WATCOMC__))
	#define INLINE static __inline
#else
	#define INLINE static inline
#endif

#define B_AT(n, i) (((n) >> (i) * 8) & 0xff)
#define BE_GEN(bit) \
    INLINE uint##bit##_t be##bit(uint##bit##_t n) \
    { \
        uint##bit##_t ret; \
        for (int i = 0; i < sizeof(ret); i++) { \
            ((byte_t *)&ret)[i] = B_AT(n, sizeof(ret) - i - 1); \
        } \
        return ret; \
    }

BE_GEN(16)
BE_GEN(32)
BE_GEN(64)

#undef B_AT

#endif
