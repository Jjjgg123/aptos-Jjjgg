/* jvm_inflate.c — 从 javm.c 拆出的独立模块：zlib/deflate 解码
 * 只依赖标准头与 aptos.h 的最小打印/内存设施，不碰 JVM 的类池与堆。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "aptos.h"

/* ---- 最小 inflate：zlib/deflate 解码（stored/fixed/dynamic Huffman + LZ77） ---- */
#define INF_MAXBITS 15
struct infl_state {
    const uint8_t *in; uint32_t in_len; uint32_t ip;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; uint32_t out_len; uint32_t out_cap;
    int err;
    int last;
};
static uint32_t infl_bits(struct infl_state *s, int n)
{
    while (s->bitcnt < n && s->ip < s->in_len) {
        s->bitbuf |= (uint32_t)s->in[s->ip++] << s->bitcnt;
        s->bitcnt += 8;
    }
    if (s->bitcnt < n) { s->err = 1; return 0; }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}
static void infl_build_syms(const uint8_t *lens, int nlens,
                            uint16_t *symbols, int *nsyms, uint16_t *counts)
{
    int i;
    for (i = 0; i <= INF_MAXBITS; i++) counts[i] = 0;
    for (i = 0; i < nlens; i++) if (lens[i] > 0) counts[lens[i]]++;
    *nsyms = 0;
    for (int len = 1; len <= INF_MAXBITS; len++)
        for (i = 0; i < nlens; i++)
            if (lens[i] == len) symbols[(*nsyms)++] = (uint16_t)i;
}
static int infl_huff(struct infl_state *s, const uint16_t *counts,
                     const uint16_t *symbols, uint32_t *out_sym)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= INF_MAXBITS; len++) {
        code |= (int)infl_bits(s, 1);
        if (s->err) return -1;
        int count = counts[len];
        if (code - first < count) {
            *out_sym = symbols[index + (code - first)];
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
static int infl_codes(struct infl_state *s,
                      const uint8_t *ll, int nll, const uint8_t *dl, int ndl)
{
    static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    uint16_t lsyms[288], dsyms[32], lcnt[INF_MAXBITS + 1], dcnt[INF_MAXBITS + 1];
    int nls, nds;
    infl_build_syms(ll, nll, lsyms, &nls, lcnt);
    infl_build_syms(dl, ndl, dsyms, &nds, dcnt);
    for (;;) {
        uint32_t sym;
        if (infl_huff(s, lcnt, lsyms, &sym) != 0) return -1;
        if (sym < 256) {
            if (s->out_len >= s->out_cap) { s->err = 1; return -1; }
            s->out[s->out_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int li = (int)sym - 257;
            if (li < 0 || li >= 29) return -1;
            int len = lbase[li] + (int)infl_bits(s, lext[li]);
            if (s->err) return -1;
            uint32_t ds;
            if (infl_huff(s, dcnt, dsyms, &ds) != 0) return -1;
            if (ds >= 30) return -1;
            int dist = dbase[ds] + (int)infl_bits(s, dext[ds]);
            if (s->err) return -1;
            if (dist <= 0 || dist > (int)s->out_len) return -1;
            for (int k = 0; k < len; k++) {
                if (s->out_len >= s->out_cap) { s->err = 1; return -1; }
                s->out[s->out_len] = s->out[s->out_len - dist];
                s->out_len++;
            }
        }
    }
}
static int infl_dyn_block(struct infl_state *s);

int javm_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    if (!in || in_len < 6) return -1;
    if ((in[0] & 0x0f) != 8) return -1;          /* CM=deflate */
    if (((in[0] << 8) | in[1]) % 31 != 0) return -1;
    struct infl_state s;
    memset(&s, 0, sizeof(s));
    s.in = in + 2;
    s.in_len = in_len - 2;   /* 数据 + adler32(4) */
    s.out = out;
    s.out_cap = out_cap;
    for (;;) {
        s.last = 0;
        uint8_t save_last = 0;
        (void)save_last;
        if (s.ip + 1 > s.in_len) { s.err = 1; break; }
        uint8_t bh = s.in[s.ip];
        s.ip++;
        s.last = bh & 1;
        int type = (bh >> 1) & 3;
        s.bitbuf = bh >> 3;
        s.bitcnt = 8 - 3;
        int r = -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.ip + 4 > s.in_len) { s.err = 1; break; }
            uint32_t len = s.in[s.ip] | ((uint32_t)s.in[s.ip + 1] << 8);
            uint32_t nlen = s.in[s.ip + 2] | ((uint32_t)s.in[s.ip + 3] << 8);
            s.ip += 4;
            if ((len & 0xffff) != (~nlen & 0xffff)) { s.err = 1; break; }
            for (uint32_t k = 0; k < len; k++) {
                if (s.ip >= s.in_len || s.out_len >= s.out_cap) { s.err = 1; break; }
                s.out[s.out_len++] = s.in[s.ip++];
            }
            if (s.err) break;
        } else if (type == 1) {
            uint8_t ll[288], dl[30];
            for (int i = 0; i < 144; i++) ll[i] = 8;
            for (int i = 144; i < 256; i++) ll[i] = 9;
            for (int i = 256; i < 280; i++) ll[i] = 7;
            for (int i = 280; i < 288; i++) ll[i] = 8;
            for (int i = 0; i < 30; i++) dl[i] = 5;
            r = infl_codes(&s, ll, 288, dl, 30);
        } else if (type == 2) {
            r = infl_dyn_block(&s);
        } else {
            s.err = 1; break;
        }
        if (r != 0 || s.err) { s.err = 1; break; }
        if (s.last) break;
    }
    if (s.err) return -1;
    *out_len = s.out_len;
    return 0;
}
static int infl_dyn_block(struct infl_state *s)
{
    int hlit = (int)infl_bits(s, 5) + 257;
    int hdist = (int)infl_bits(s, 5) + 1;
    int hclen = (int)infl_bits(s, 4) + 4;
    if (s->err) return -1;
    static const uint8_t clorder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t cl[19];
    for (int i = 0; i < 19; i++) cl[i] = 0;
    for (int i = 0; i < hclen; i++) cl[clorder[i]] = (uint8_t)infl_bits(s, 3);
    if (s->err) return -1;
    uint16_t clsyms[19], ccnt[INF_MAXBITS + 1];
    int ncs;
    infl_build_syms(cl, 19, clsyms, &ncs, ccnt);
    uint8_t ll[288], dl[32];
    int i = 0;
    while (i < hlit + hdist) {
        uint32_t sym;
        if (infl_huff(s, ccnt, clsyms, &sym) != 0) return -1;
        if (sym < 16) {
            if (i < hlit) ll[i] = (uint8_t)sym;
            else dl[i - hlit] = (uint8_t)sym;
            i++;
        } else if (sym == 16) {
            int rep = 3 + (int)infl_bits(s, 2);
            if (s->err || i == 0) return -1;
            uint8_t prev = (i - 1 < hlit) ? ll[i - 1] : dl[i - 1 - hlit];
            for (int k = 0; k < rep; k++) {
                if (i < hlit) ll[i] = prev;
                else dl[i - hlit] = prev;
                i++;
            }
        } else if (sym == 17) {
            int rep = 3 + (int)infl_bits(s, 3);
            if (s->err) return -1;
            for (int k = 0; k < rep; k++) {
                if (i < hlit) ll[i] = 0;
                else dl[i - hlit] = 0;
                i++;
            }
        } else {
            int rep = 11 + (int)infl_bits(s, 7);
            if (s->err) return -1;
            for (int k = 0; k < rep; k++) {
                if (i < hlit) ll[i] = 0;
                else dl[i - hlit] = 0;
                i++;
            }
        }
        if (i > hlit + hdist) return -1;
    }
    return infl_codes(s, ll, hlit, dl, hdist);
}

