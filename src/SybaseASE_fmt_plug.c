/*
 * Unicode conversion enhancements by magnum, 2011. Licensed as below.
 *
 * Sybase ASE hash support for version 15.0.2 and above, based on hmailserver
 * patch by James Nobis.
 * Hash format description : http://marcellmajor.com/sybase_sha256.html
 * Hacked together by Dhiru Kholia in February, 2011.
 *
 * This patch Copyright (C) 2010 by James Nobis - quel
 * quel NOSPAM quelrod NOSPAM net, and it is herby released to the general
 * public under the follow terms:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.

 * Inspiration from the generic sha-1 and md5 (Copyright (c) 2010 by Solar Designer)
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_SybaseASE;
#elif FMT_REGISTERS_H
john_register_one(&fmt_SybaseASE);
#else

#include "arch.h"

//#undef _OPENMP
//#undef SIMD_COEF_32
//#undef SIMD_COEF_32
//#undef SIMD_PARA_SHA256
//
//#define FORCE_GENERIC_SHA2 2
#include "sha2.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "unicode.h"
#include "johnswap.h"
#include "simd-intrinsics.h"
#ifdef _OPENMP
#include <omp.h>
#endif
#include "memdbg.h"

#define FORMAT_LABEL        "SybaseASE"
#define FORMAT_NAME         "Sybase ASE"

#define ALGORITHM_NAME      "SHA256 " SHA256_ALGORITHM_NAME

#define BENCHMARK_COMMENT   ""
#define BENCHMARK_LENGTH    0

#define PLAINTEXT_LENGTH    64
#define CIPHERTEXT_LENGTH   (6 + 16 + 64)
#define PREFIX_LENGTH       6

#define BINARY_SIZE         32
#define BINARY_ALIGN        4
#define SALT_SIZE           8
#define SALT_ALIGN          4

#ifdef SIMD_COEF_32
#define MIN_KEYS_PER_CRYPT  (SIMD_COEF_32*SIMD_PARA_SHA256)
#define MAX_KEYS_PER_CRYPT	(SIMD_COEF_32*SIMD_PARA_SHA256)
#ifdef __MIC__
#ifndef OMP_SCALE
#define OMP_SCALE           64
#endif
#else
#ifndef OMP_SCALE
#define OMP_SCALE           512
#endif
#endif // __MIC__
#else
#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT  1
#ifndef OMP_SCALE
#define OMP_SCALE           256
#endif
#endif

static struct fmt_tests SybaseASE_tests[] = {
    {"0xc0074f9cc8c0d55d9803b0c0816e127f2a56ee080230af5b4ce3da1f3d9fcc5449fcfcf3fb9595eb8ea6", "test12"},
    {"0xc0074BE393C06BE420AD541671aa5e6f1a19a4a73bb51c59f45790f0887cfb70e0599747c6844d4556b3", "a"},
    {NULL}
};

#ifdef SIMD_COEF_32
// note, elements 3-7 are 'nulls', and are not in this array.
static UTF16 (*prep_key)[4][MAX_KEYS_PER_CRYPT][64 / sizeof(UTF16)];
static unsigned char *NULL_LIMB;
static int (*last_len);
#else
static UTF16 (*prep_key)[518 / sizeof(UTF16)];
#endif
static ARCH_WORD_32 (*crypt_out)[BINARY_SIZE/4];
static int kpc;

extern struct fmt_main fmt_SybaseASE;
static void init(struct fmt_main *self)
{
#if _OPENMP || SIMD_COEF_32
	int i;
#endif
#ifdef _OPENMP
	i = omp_get_max_threads();
	self->params.min_keys_per_crypt *= i;
	i *= OMP_SCALE;
	self->params.max_keys_per_crypt *= i;
#endif
	kpc = self->params.max_keys_per_crypt;

	prep_key = mem_calloc_align(sizeof(*prep_key),
		self->params.max_keys_per_crypt, MEM_ALIGN_CACHE);
	crypt_out = mem_calloc_align(sizeof(*crypt_out),
		self->params.max_keys_per_crypt, MEM_ALIGN_CACHE);

	if (options.target_enc == UTF_8)
		fmt_SybaseASE.params.plaintext_length = 125;
	// will simply set SIMD stuff here, even if not 'used'
#ifdef SIMD_COEF_32
	NULL_LIMB = mem_calloc_align(64, MAX_KEYS_PER_CRYPT, MEM_ALIGN_CACHE);
	last_len = mem_calloc_align(sizeof(*last_len), self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	for (i = 0; i < kpc/MAX_KEYS_PER_CRYPT; ++i) {
		int j;
		for (j = 0; j < MAX_KEYS_PER_CRYPT; ++j) {
			prep_key[i][3][j][3] = 0x80;
			prep_key[i][3][j][30] = 518<<3;
		}
	}
#endif
}

static void done(void)
{
#ifdef SIMD_COEF_32
	MEM_FREE(last_len);
	MEM_FREE(NULL_LIMB);
#endif
	MEM_FREE(crypt_out);
	MEM_FREE(prep_key);
}

static int valid(char *ciphertext, struct fmt_main *self)
{
    if(strncmp(ciphertext, "0xc007", 6)!=0)
        return 0;
    if(hexlen(&ciphertext[6]) != CIPHERTEXT_LENGTH - 6)
        return 0;
    return 1;
}

static char *split(char *ciphertext, int index, struct fmt_main *self)
{
	static char out[CIPHERTEXT_LENGTH+1];
	strnzcpy(out, ciphertext, CIPHERTEXT_LENGTH+1);
	strlwr(out);
	return out;
}

static void *get_binary(char *ciphertext)
{
    static unsigned char *out;
    int i;
    char *p = ciphertext + PREFIX_LENGTH + SALT_SIZE * 2;

    if (!out) out = mem_alloc_tiny(BINARY_SIZE, MEM_ALIGN_WORD);

    for (i = 0; i < BINARY_SIZE; i++) {
        out[i] = (atoi16[ARCH_INDEX(*p)] << 4) | atoi16[ARCH_INDEX(p[1])];
        p += 2;
    }
    return out;
}

static void *get_salt(char *ciphertext)
{
	static union {
		unsigned char u8[SALT_SIZE];
		ARCH_WORD_32 u32;
	} out;
	int i;
	char *p = ciphertext + PREFIX_LENGTH;

	for (i = 0; i < sizeof(out.u8); i++) {
		out.u8[i] = (atoi16[ARCH_INDEX(*p)] << 4) |atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}
	return out.u8;
}

static int get_hash_0(int index)
{
    return crypt_out[index][0] & PH_MASK_0;
}

static int get_hash_1(int index)
{
    return crypt_out[index][0] & PH_MASK_1;
}

static int get_hash_2(int index)
{
    return crypt_out[index][0] & PH_MASK_2;
}

static int get_hash_3(int index)
{
    return crypt_out[index][0] & PH_MASK_3;
}

static int get_hash_4(int index)
{
    return crypt_out[index][0] & PH_MASK_4;
}

static int get_hash_5(int index)
{
    return crypt_out[index][0] & PH_MASK_5;
}

static int get_hash_6(int index)
{
    return crypt_out[index][0] & PH_MASK_6;
}

static void set_salt(void *salt)
{
	int index;

	for(index = 0; index < kpc; index++)
	{
		/* append salt at offset 510 */
#ifdef SIMD_COEF_32
		int idx1=index/MAX_KEYS_PER_CRYPT, idx2=index%MAX_KEYS_PER_CRYPT;
		memcpy(&prep_key[idx1][2][idx2][31], salt, 2);
		memcpy(prep_key[idx1][3][idx2], &((unsigned char*)salt)[2], 6);
#else
		memcpy((unsigned char*)prep_key[index] + 510,
		       (unsigned char*)salt, 8);
#endif
	}
}

static void set_key(char *key, int index)
{
#ifdef SIMD_COEF_32
	UTF16 tmp[PLAINTEXT_LENGTH+1];
	int len2, len = enc_to_utf16_be(tmp, PLAINTEXT_LENGTH, (UTF8*)key, strlen(key));
	int idx1=index/MAX_KEYS_PER_CRYPT, idx2=index%MAX_KEYS_PER_CRYPT;

	if (len < 0)
		len = strlen16(tmp);

	if (len > 32)
		memcpy(prep_key[idx1][1][idx2], &tmp[32], (len-32)<<1);
	len2 = len;
	if (len2 > 32) len2 = 32;
	memcpy(prep_key[idx1][0][idx2], tmp, len2<<1);
	len2 = len;
	while (len < last_len[index]) {
		if (len < 32)
			prep_key[idx1][0][idx2][len] = 0;
		else
			prep_key[idx1][1][idx2][len-32] = 0;
		++len;
	}
	last_len[index] = len2;
#else
	/* Clean slate */
    memset(prep_key[index], 0, 2 * PLAINTEXT_LENGTH);

    /* convert key to UTF-16BE, --encoding aware */
    enc_to_utf16_be(prep_key[index], PLAINTEXT_LENGTH, (UTF8*)key,
                    strlen(key));
#endif
}

static char *get_key(int index)
{
    UTF16 key_le[PLAINTEXT_LENGTH + 1];

#ifdef SIMD_COEF_32
	int j, idx1=index/MAX_KEYS_PER_CRYPT, idx2=index%MAX_KEYS_PER_CRYPT;

	if (last_len[index] < 32) {
		for (j = 0; j < last_len[index]; ++j)
			key_le[j] = JOHNSWAP(prep_key[idx1][0][idx2][j])>>16;
	} else {
		for (j = 0; j < 32; ++j)
			key_le[j] = JOHNSWAP(prep_key[idx1][0][idx2][j])>>16;
		for (; j < last_len[index]; ++j)
			key_le[j] = JOHNSWAP(prep_key[idx1][1][idx2][j-32])>>16;
	}
	key_le[j] = 0;
#else
	UTF16 *d = key_le;
	UTF16 *s = prep_key[index];

    // Byte-swap back to UTF-16LE
    while ((*d++ = *s >> 8 | *s << 8))
        s++;
#endif
    return (char*)utf16_to_enc(key_le);
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index = 0;

#ifdef _OPENMP
#ifndef SIMD_COEF_32
#pragma omp parallel for default(none) private(index) shared(count, crypt_out, prep_key)
#else
#pragma omp parallel for default(none) private(index) shared(count, crypt_out, prep_key, NULL_LIMB)
#endif
#endif
	for(index = 0; index < count; index += MAX_KEYS_PER_CRYPT)
	{
#ifndef SIMD_COEF_32
		SHA256_CTX ctx;

		SHA256_Init(&ctx);
		SHA256_Update(&ctx, prep_key[index], 518);
		SHA256_Final((unsigned char *)crypt_out[index], &ctx);
#else
		unsigned char _OBuf[32*MAX_KEYS_PER_CRYPT+MEM_ALIGN_CACHE], *crypt;
		uint32_t *crypt32;
		crypt = (unsigned char*)mem_align(_OBuf, MEM_ALIGN_CACHE);
		crypt32 = (uint32_t*)crypt;

		SIMDSHA256body(prep_key[index/MAX_KEYS_PER_CRYPT], crypt32, NULL, SSEi_FLAT_IN|SSEi_FLAT_RELOAD_SWAPLAST);
		SIMDSHA256body(&(prep_key[index/MAX_KEYS_PER_CRYPT][1]), crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD|SSEi_FLAT_RELOAD_SWAPLAST);
		SIMDSHA256body(NULL_LIMB, crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD);
		SIMDSHA256body(NULL_LIMB, crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD);
		SIMDSHA256body(NULL_LIMB, crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD);
		SIMDSHA256body(NULL_LIMB, crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD);
		SIMDSHA256body(NULL_LIMB, crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD);
		SIMDSHA256body(&(prep_key[index/MAX_KEYS_PER_CRYPT][2]), crypt32, crypt32, SSEi_FLAT_IN|SSEi_RELOAD|SSEi_FLAT_RELOAD_SWAPLAST);
		// Last one with FLAT_OUT
		SIMDSHA256body(&(prep_key[index/MAX_KEYS_PER_CRYPT][3]), crypt_out[index], crypt32, SSEi_FLAT_IN|SSEi_RELOAD|SSEi_FLAT_OUT);
#endif
	}
	return count;
}

static int cmp_all(void *binary, int count)
{
    int index = 0;

    for (index = 0; index < count; index++)
        if (*(ARCH_WORD_32 *)binary == *(ARCH_WORD_32 *)crypt_out[index])
            return 1;
    return 0;
}

static int cmp_one(void *binary, int index)
{
    return !memcmp((char *)binary, (const char*)crypt_out[index], BINARY_SIZE);
}

static int cmp_exact(char *source, int index)
{
    return 1;
}

static int salt_hash(void *salt)
{
	return *(ARCH_WORD_32*)salt & (SALT_HASH_SIZE - 1);
}

struct fmt_main fmt_SybaseASE = {
    {
        FORMAT_LABEL,
        FORMAT_NAME,
        ALGORITHM_NAME,
        BENCHMARK_COMMENT,
        BENCHMARK_LENGTH,
        0,
        PLAINTEXT_LENGTH,
        BINARY_SIZE,
        BINARY_ALIGN,
        SALT_SIZE,
        SALT_ALIGN,
        MIN_KEYS_PER_CRYPT,
        MAX_KEYS_PER_CRYPT,
        FMT_CASE | FMT_8_BIT | FMT_OMP | FMT_UNICODE | FMT_UTF8 | FMT_SPLIT_UNIFIES_CASE,
		{ NULL },
        SybaseASE_tests
    }, {
        init,
        done,
        fmt_default_reset,
        fmt_default_prepare,
        valid,
        split,
        get_binary,
        get_salt,
		{ NULL },
        fmt_default_source,
        {
		fmt_default_binary_hash_0,
		fmt_default_binary_hash_1,
		fmt_default_binary_hash_2,
		fmt_default_binary_hash_3,
		fmt_default_binary_hash_4,
		fmt_default_binary_hash_5,
		fmt_default_binary_hash_6
        },
        salt_hash,
        NULL,
        set_salt,
        set_key,
        get_key,
        fmt_default_clear_keys,
        crypt_all,
        {
		get_hash_0,
		get_hash_1,
		get_hash_2,
		get_hash_3,
		get_hash_4,
		get_hash_5,
		get_hash_6
        },
        cmp_all,
        cmp_one,
        cmp_exact
    }
};

#endif /* plugin stanza */
