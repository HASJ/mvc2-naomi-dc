/*
 * ed25519_verify.c  SHA-256, SHA-512, and RFC 8032 Ed25519 signature verification
 * Pure C; field arithmetic derived from public-domain TweetNaCl with canonical S < L check.
 */

#include <stdio.h>
#include <string.h>

#include "ed25519_verify.h"

/* ---- SHA-256 ---- */

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static uint32_t load32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void sha256_block(uint32_t s[8], const uint8_t b[64]) {
    uint32_t w[64], a, bv, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) w[i] = load32be(b + i * 4);
    for (; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s[0]; bv = s[1]; c = s[2]; d = s[3];
    e = s[4]; f = s[5]; g = s[6]; h = s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & bv) ^ (a & c) ^ (bv & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = bv; bv = a; a = t1 + t2;
    }
    s[0] += a; s[1] += bv; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void sha256_init(SHA256_CTX* ctx) {
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    ctx->bits = 0;
    ctx->len = 0;
    memcpy(ctx->state, iv, sizeof iv);
}

void sha256_update(SHA256_CTX* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->bits += (uint64_t)len * 8ULL;
    while (len > 0) {
        size_t take = 64u - ctx->len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->len, p, take);
        ctx->len += take;
        p += take;
        len -= take;
        if (ctx->len == 64u) {
            sha256_block(ctx->state, ctx->buf);
            ctx->len = 0;
        }
    }
}

void sha256_final(SHA256_CTX* ctx, uint8_t out[32]) {
    int i;
    ctx->buf[ctx->len++] = 0x80u;
    if (ctx->len > 56u) {
        memset(ctx->buf + ctx->len, 0, 64u - ctx->len);
        sha256_block(ctx->state, ctx->buf);
        ctx->len = 0;
    }
    memset(ctx->buf + ctx->len, 0, 56u - ctx->len);
    for (i = 0; i < 8; i++) {
        ctx->buf[56 + i] = (uint8_t)(ctx->bits >> ((7 - i) * 8));
    }
    sha256_block(ctx->state, ctx->buf);
    for (i = 0; i < 8; i++) {
        store32be(out + i * 4, ctx->state[i]);
    }
}

void sha256_bytes(const void* data, size_t len, uint8_t out[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

int sha256_file(const char* path, uint8_t out[32]) {
    uint8_t buf[16384];
    SHA256_CTX ctx;
    FILE* f;
    size_t n;
    if (path == NULL) return 0;
    f = fopen(path, "rb");
    if (f == NULL) return 0;
    sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    sha256_final(&ctx, out);
    return 1;
}

void sha256_hex(const uint8_t hash[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(hash[i] >> 4) & 15];
        out_hex[i * 2 + 1] = hex[hash[i] & 15];
    }
    out_hex[64] = 0;
}

/* ---- SHA-512 ---- */

static inline uint64_t rotr64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

static uint64_t load64be(const uint8_t* p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void store64be(uint8_t* p, uint64_t v) {
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

typedef struct SHA512_CTX {
    uint64_t total_lo, total_hi;
    uint64_t state[8];
    uint8_t buf[128];
    size_t len;
} SHA512_CTX;

static void sha512_block(uint64_t s[8], const uint8_t b[128]) {
    uint64_t w[80], a, bv, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) w[i] = load64be(b + i * 8);
    for (; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s[0]; bv = s[1]; c = s[2]; d = s[3];
    e = s[4]; f = s[5]; g = s[6]; h = s[7];
    for (i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & bv) ^ (a & c) ^ (bv & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = bv; bv = a; a = t1 + t2;
    }
    s[0] += a; s[1] += bv; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

static void sha512_init(SHA512_CTX* ctx) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    ctx->total_lo = 0;
    ctx->total_hi = 0;
    ctx->len = 0;
    memcpy(ctx->state, iv, sizeof iv);
}

static void sha512_update(SHA512_CTX* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t bits = (uint64_t)len * 8ULL;
    uint64_t prev = ctx->total_lo;
    ctx->total_lo += bits;
    if (ctx->total_lo < prev) ctx->total_hi++;
    while (len > 0) {
        size_t take = 128u - ctx->len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->len, p, take);
        ctx->len += take;
        p += take;
        len -= take;
        if (ctx->len == 128u) {
            sha512_block(ctx->state, ctx->buf);
            ctx->len = 0;
        }
    }
}

static void sha512_final(SHA512_CTX* ctx, uint8_t out[64]) {
    int i;
    ctx->buf[ctx->len++] = 0x80u;
    if (ctx->len > 112u) {
        memset(ctx->buf + ctx->len, 0, 128u - ctx->len);
        sha512_block(ctx->state, ctx->buf);
        ctx->len = 0;
    }
    memset(ctx->buf + ctx->len, 0, 112u - ctx->len);
    store64be(ctx->buf + 112, ctx->total_hi);
    store64be(ctx->buf + 120, ctx->total_lo);
    sha512_block(ctx->state, ctx->buf);
    for (i = 0; i < 8; i++) {
        store64be(out + i * 8, ctx->state[i]);
    }
}

void sha512_bytes(const void* data, size_t len, uint8_t out[64]) {
    SHA512_CTX ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

/* ---- Ed25519 verification ---- */

typedef int64_t gf[16];

static const gf gf0 = { 0 };
static const gf gf1 = { 1 };
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

static const uint8_t ED_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

static void set25519(gf r, const gf a) {
    int i;
    for (i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    int i;
    for (i = 0; i < 16; i++) {
        int64_t c;
        o[i] += (1LL << 16);
        c = (int64_t)(((uint64_t)(o[i] + (1LL << 60))) >> 16) - (1LL << 44);
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (int64_t)((uint64_t)c << 16);
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t t, i, c = ~(int64_t)(b - 1);
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t* o, const gf n) {
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return memcmp(c, d, 32) != 0;
}

static uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1u;
}

static void unpack25519(gf o, const uint8_t* n) {
    int i;
    for (i = 0; i < 16; i++) o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) {
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b) {
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    int64_t t[31];
    int i, j;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) {
    M(o, a, a);
}

static void inv25519(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void pow2523(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void ed_add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], uint8_t b) {
    int i;
    for (i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void scalarmult(gf p[4], gf q[4], const uint8_t* s) {
    int i;
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (i = 255; i >= 0; --i) {
        uint8_t b = (uint8_t)((s[i / 8] >> (i & 7)) & 1u);
        cswap(p, q, b);
        ed_add(q, p);
        ed_add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t* s) {
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static void ed_pack(uint8_t r[32], gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

static void modL(uint8_t r[32], int64_t x[64]) {
    int64_t carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)ED_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (int64_t)ED_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * (int64_t)ED_L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t r[64]) {
    int64_t x[64];
    int i;
    for (i = 0; i < 64; i++) x[i] = (uint64_t)r[i];
    for (i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

static int s_lt_L(const uint8_t s[32]) {
    int i;
    for (i = 31; i >= 0; i--) {
        if (s[i] < ED_L[i]) return 1;
        if (s[i] > ED_L[i]) return 0;
    }
    return 0;
}

int ed25519_verify(const uint8_t sig[64], const void* msg, size_t msg_len, const uint8_t pk[32]) {
    uint8_t t[32], h[64];
    gf p[4], q[4];
    SHA512_CTX ctx;
    if (sig == NULL || pk == NULL || (msg == NULL && msg_len > 0)) {
        return 0;
    }
    if (!s_lt_L(sig + 32)) {
        return 0;
    }
    if (unpackneg(q, pk) != 0) {
        return 0;
    }
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    if (msg_len > 0) {
        sha512_update(&ctx, msg, msg_len);
    }
    sha512_final(&ctx, h);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    ed_add(p, q);
    ed_pack(t, p);
    return memcmp(sig, t, 32) == 0;
}

int ed25519_self_test(void) {
    /* RFC 8032 Section 7.1 TEST 1 (empty message) & TEST 3 (2-byte message 0xaf 0x82) */
    static const uint8_t pk1[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
    };
    static const uint8_t sig1[64] = {
        0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
        0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
        0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
        0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b,
    };
    static const uint8_t pk3[32] = {
        0xfc, 0x51, 0xcd, 0x8e, 0x62, 0x18, 0xa1, 0xa3, 0x8d, 0xa4, 0x7e, 0xd0, 0x02, 0x30, 0xf0, 0x58,
        0x08, 0x16, 0xed, 0x13, 0xba, 0x33, 0x03, 0xac, 0x5d, 0xeb, 0x91, 0x15, 0x48, 0x90, 0x80, 0x25,
    };
    static const uint8_t msg3[2] = { 0xaf, 0x82 };
    static const uint8_t sig3[64] = {
        0x62, 0x91, 0xd6, 0x57, 0xde, 0xec, 0x24, 0x02, 0x48, 0x27, 0xe6, 0x9c, 0x3a, 0xbe, 0x01, 0xa3,
        0x0c, 0xe5, 0x48, 0xa2, 0x84, 0x74, 0x3a, 0x44, 0x5e, 0x36, 0x80, 0xd7, 0xdb, 0x5a, 0xc3, 0xac,
        0x18, 0xff, 0x9b, 0x53, 0x8d, 0x16, 0xf2, 0x90, 0xae, 0x67, 0xf7, 0x60, 0x98, 0x4d, 0xc6, 0x59,
        0x4a, 0x7c, 0x15, 0xe9, 0x71, 0x6e, 0xd2, 0x8d, 0xc0, 0x27, 0xbe, 0xce, 0xea, 0x1e, 0xc4, 0x0a,
    };
    uint8_t bad_sig[64];
    memcpy(bad_sig, sig3, sizeof bad_sig);
    bad_sig[0] ^= 0x01u;
    if (!ed25519_verify(sig1, "", 0, pk1)) return 0;
    if (!ed25519_verify(sig3, msg3, sizeof msg3, pk3)) return 0;
    if (ed25519_verify(bad_sig, msg3, sizeof msg3, pk3)) return 0;
    if (ed25519_verify(sig3, "ab", 2, pk3)) return 0;
    return 1;
}
