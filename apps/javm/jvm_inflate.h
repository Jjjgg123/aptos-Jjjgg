#ifndef JVM_INFLATE_H
#define JVM_INFLATE_H

#include <stdint.h>

/* zlib(deflate) 解压：out_cap 为输出容量，*out_len 返回实际解压长度。
 * 返回 0 成功，非 0 失败。 */
int javm_inflate(const uint8_t *in, uint32_t in_len,
                 uint8_t *out, uint32_t out_cap, uint32_t *out_len);

#endif
