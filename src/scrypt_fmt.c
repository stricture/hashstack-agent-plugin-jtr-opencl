/*
 * This file is part of John the Ripper password cracker,
 * Copyright (c) 2013 by Solar Designer
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * There's ABSOLUTELY NO WARRANTY, express or implied.
 */

#include <stdio.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "escrypt/crypto_scrypt.h"

#include "arch.h"
#include "misc.h"
#include "common.h"
#include "formats.h"
#include "base64_convert.h"
#include "memdbg.h"

#define FORMAT_LABEL			"scrypt"
#define FORMAT_NAME			""
#define FMT_CISCO9              "$9$"
#define FMT_SCRYPTKDF			"$ScryptKDF.pm$"
#ifdef __XOP__
#define ALGORITHM_NAME			"Salsa20/8 128/128 XOP"
#elif defined(__AVX__)
#define ALGORITHM_NAME			"Salsa20/8 128/128 AVX"
#elif defined(__SSE2__)
#define ALGORITHM_NAME			"Salsa20/8 128/128 SSE2"
#else
#define ALGORITHM_NAME			"Salsa20/8 32/" ARCH_BITS_STR
#endif

#define BENCHMARK_COMMENT		" (16384, 8, 1)"
#define BENCHMARK_LENGTH		-1

#define PLAINTEXT_LENGTH		125

#define BINARY_SIZE			256
#define BINARY_ALIGN			1
#define SALT_SIZE			BINARY_SIZE
#define SALT_ALIGN			1

#define MIN_KEYS_PER_CRYPT		1
#define MAX_KEYS_PER_CRYPT		1

static struct fmt_tests tests[] = {
	{"$7$C6..../....SodiumChloride$kBGj9fHznVYFQMEn/qDCfrDevf9YDtcDdKvEqHJLV8D", "pleaseletmein"},
	{"$7$C6..../....\x01\x09\x0a\x0d\x20\x7f\x80\xff$b7cKqzsQk7txdc9As1WZBHjUPNWQWJW8A.UUUTA5eD1", "\x01\x09\x0a\x0d\x20\x7f\x80\xff"},
	{"$7$2/..../....$rNxJWVHNv/mCNcgE/f6/L4zO6Fos5c2uTzhyzoisI62", ""},
	{"$7$86....E....NaCl$xffjQo7Bm/.SKRS4B2EuynbOLjAmXU5AbDbRXhoBl64", "password"},
	// cisco type 9 hashes.  .  They are $7$C/..../.... type  (N=16384, r=1, p=1) different base-64 (same as WPA).  salt used RAW
	{"$9$nhEmQVczB7dqsO$X.HsgL6x1il0RxkOSSvyQYwucySCt7qFm4v7pqCxkKM", "cisco"},
	{"$9$cvWdfQlRRDKq/U$VFTPha5VHTCbSgSUAo.nPoh50ZiXOw1zmljEjXkaq1g", "123456"},
	{"$9$X9fA8mypebLFVj$Klp6X9hxNhkns0kwUIinvLRSIgWOvCwDhVTZqjsycyU", "JtR"},
	// 3rd type ScryptKDF.pm format (we saw this in CMIYC 2013)
	// Generate in perl with scrypt_hash($_[1],$salt,1<<$N,$r,$p,$bytes)
	// to put into proper format, we mime->raw the salt and mime->cryptBS the hash hash, and fixup $N,$r,$p
	// For this hash we replace the default ':' chars in the hash with '*' so they will end up as 1
	// field, and change the SCRYPT into $ScryptKDF.pm$.  So this hash
	// SCRYPT:16384:8:1:VHRuaXZOZ05INWJs:JjrOzA8pdPhLvLh8sY64fLLaAjFUwYCXMmS16NXcn0A=
	// gets change into (by ScryptKDF2john)
	// $ScryptKDF.pm$16384*8*1*VHRuaXZOZ05INWJs*JjrOzA8pdPhLvLh8sY64fLLaAjFUwYCXMmS16NXcn0A=
	// and then in prepare, this becomes (which is canonical for this format)
	// $7$C6..../....TtnivNgNH5bl$acXnAzE8oVzGwW9Tlu6iw7fq021J/1sZmEKhcLBrT02
	{"$ScryptKDF.pm$16384*8*1*VHRuaXZOZ05INWJs*JjrOzA8pdPhLvLh8sY64fLLaAjFUwYCXMmS16NXcn0A=","password"},
	{"$ScryptKDF.pm$16384*8*1*bjZkemVmZ3lWVi42*cmBflTPsqGIbg9ZIJRTQdbic8OCUH+904TFmNPBkuEA=","test123"},
	{"$ScryptKDF.pm$16384*8*1*VlVYUzBhQmlNbk5J*bJhm6VUS2UQRwMRqLTvSsljDeq193Ge4aqQDtb94bKg=","hello"},
	{"$ScryptKDF.pm$16384*8*1*VHRuaXZOZ05INWJs*JjrOzA8pdPhLvLh8sY64fLLaAjFUwYCXMmS16NXcn0BhlHpZJ3J2jcozCDM7t+sfjkgQ894R+f+ldVWM5atlkA==","password"},
	{NULL}
};

// from crypt_scrypt-common.c  (removed static from that file on these 3 functions)
extern const uint8_t * decode64_uint32(uint32_t * dst, uint32_t dstbits, const uint8_t * src);
extern uint8_t * encode64_uint32(uint8_t * dst, size_t dstlen, uint32_t src, uint32_t srcbits);
extern int decode64_one(uint32_t * dst, uint8_t src);

static int max_threads;
static escrypt_local_t *local;

static char saved_salt[SALT_SIZE];
static struct {
	char key[PLAINTEXT_LENGTH + 1];
	char out[BINARY_SIZE];
} *buffer;

static void init(struct fmt_main *self)
{
	int i;

#ifdef _OPENMP
	max_threads = omp_get_max_threads();
	self->params.min_keys_per_crypt *= max_threads;
	self->params.max_keys_per_crypt *= max_threads;
#else
	max_threads = 1;
#endif

	local = mem_alloc(sizeof(*local) * max_threads);
	for (i = 0; i < max_threads; i++)
		escrypt_init_local(&local[i]);

	buffer = mem_alloc(sizeof(*buffer) * self->params.max_keys_per_crypt);
}

static char N_to_c(int N) {
	int b=0;
	while (N>>=1) ++b;
	return itoa64[b];
}

static char *prepare(char *fields[10], struct fmt_main *self)
{
	static char Buf[256];
	char tmp[512], tmp2[512], tmp4[256], tmp5[6], tmp6[6], *cp, *cp2;
	int N, r, p;

	if (!strncmp(fields[1], FMT_CISCO9, sizeof(FMT_CISCO9)-1)) {
		// cisco type 9 hashes.  scrypt params: N=16384, r=1, p=1 hash in crypt format.  Change it to CryptBS.
		// salt is 14 byte RAW, we can use it as is.
		//from: {"$9$nhEmQVczB7dqsO$X.HsgL6x1il0RxkOSSvyQYwucySCt7qFm4v7pqCxkKM", "cisco"},
		//to:   {"$7$C/..../....nhEmQVczB7dqsO$AG.yl8LDCkiErlh4ttizmxYCXSiXYrNY6vKmLDKj/P4", "cisco"},
		if (strlen(fields[1]) != 4+14+43)
			return fields[1];
		N=1<<14; r=1; p=1;
		encode64_uint32((uint8_t*)tmp5, sizeof(tmp5), r, 30);
		tmp5[5]=0;
		encode64_uint32((uint8_t*)tmp6, sizeof(tmp6), p, 30);
		tmp6[5]=0;
		sprintf (Buf, "$7$%c%s%s%14.14s$%s", N_to_c(N), tmp5, tmp6, &(fields[1][3]),
			base64_convert_cp(&(fields[1][3+14+1]), e_b64_crypt, 43, tmp, e_b64_cryptBS, sizeof(tmp), flg_Base64_NO_FLAGS));
	}
	else if (!strncmp(fields[1], FMT_SCRYPTKDF, sizeof(FMT_SCRYPTKDF)-1))
	{
		// ScryptKDF.pm (perl) format scrypt, generated by: scrypt_hash($_[1],$salt,$N,$r,$p,$bytes); Since N, r, p
		// AND bytes are variable, we have to handle computing all of them.  NOTE, we may have to make changes to
		// the crypto_scrypt-common.c to handle the variable number of bytes.
		// to put into proper format, we mime->raw the salt and mime->cryptBS the hash hash, and fixup $N,$r,$p
		//from: {"$ScryptKDF.pm$*16384*8*1*VHRuaXZOZ05INWJs*JjrOzA8pdPhLvLh8sY64fLLaAjFUwYCXMmS16NXcn0A=","password"},
		//to:   {"$7$C6..../....TtnivNgNH5bl$acXnAzE8oVzGwW9Tlu6iw7fq021J/1sZmEKhcLBrT02","password"},
		int N, r, p;
		if (strlen(fields[1]) > sizeof(tmp)+sizeof(FMT_SCRYPTKDF)-1)
			return fields[1];
		strcpy(tmp, &fields[1][sizeof(FMT_SCRYPTKDF)-1]);
		cp = strtok(tmp, "*");
		if (!isdec(cp)) return fields[1];
		N = atoi(cp);
		cp = strtok(NULL, "*");
		if (!isdec(cp)) return fields[1];
		r = atoi(cp);
		cp = strtok(NULL, "*");
		if (!isdec(cp)) return fields[1];
		p = atoi(cp);
		cp = strtok(NULL, "*");
		cp2 = strtok(NULL, "*");
		if (base64_valid_length(cp, e_b64_mime, flg_Base64_MIME_TRAIL_EQ_CNT) != strlen(cp))
			return fields[1];
		if (base64_valid_length(cp2, e_b64_mime, flg_Base64_MIME_TRAIL_EQ_CNT) != strlen(cp2))
			return fields[1];
		encode64_uint32((uint8_t*)tmp5, sizeof(tmp5), r, 30);
		tmp5[5]=0;
		encode64_uint32((uint8_t*)tmp6, sizeof(tmp6), p, 30);
		tmp6[5]=0;
		memset(tmp4, 0, sizeof(tmp4));
		base64_convert_cp(cp, e_b64_mime, strlen(cp), tmp4, e_b64_raw, sizeof(tmp4), flg_Base64_NO_FLAGS);
		memset(tmp2, 0, sizeof(tmp2));
		base64_convert_cp(cp2, e_b64_mime, strlen(cp2), tmp2, e_b64_cryptBS, sizeof(tmp2),flg_Base64_NO_FLAGS);
		cp = &tmp2[strlen(tmp2)-1];
		while (cp > tmp2 && *cp == '.') *cp-- = 0;
		cp = &tmp4[strlen(tmp)-1];
		while (cp > tmp4 && *cp == '.') *cp-- = 0;
		sprintf (Buf, "$7$%c%s%s%s$%s", N_to_c(N), tmp5, tmp6, tmp4, tmp2);
	} else
		return fields[1];
	return Buf;
}

static void done(void)
{
	int i;

	for (i = 0; i < max_threads; i++)
		escrypt_free_local(&local[i]);

	MEM_FREE(local);
	MEM_FREE(buffer);
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *p;
	int length;
	unsigned tmp;

	if (strncmp(ciphertext, "$7$", 3))
		return 0;

	for (p = ciphertext + 3; p < ciphertext + (3 + 1 + 5 + 5); p++)
		if (atoi64[ARCH_INDEX(*p)] == 0x7F)
			return 0;

	p = strrchr(ciphertext, '$');
	if (!p)
		return 0;

	if (p - ciphertext > BINARY_SIZE - (1 + 43))
		return 0;

	++p;
	length = base64_valid_length(p, e_b64_cryptBS, flg_Base64_NO_FLAGS);

	decode64_one(&tmp, ciphertext[3]);
	if (!tmp)
		return 0;
	decode64_uint32(&tmp, 30, (const uint8_t *)&ciphertext[4]);
	if (!tmp)
		return 0;
	decode64_uint32(&tmp, 30, (const uint8_t *)&ciphertext[4+5]);
	if (!tmp)
		return 0;

	// we want the hash to use 32 bytes OR more.  43 base64 bytes is 32 raw bytes
	return p[length]==0 && length >= 43;
}

static void *binary(char *ciphertext)
{
	static char out[BINARY_SIZE];
	strncpy(out, ciphertext, sizeof(out)); /* NUL padding is required */
	return out;
}

static void *salt(char *ciphertext)
{
	static char out[SALT_SIZE];
	char *cp;
	/* NUL padding is required */
	memset(out, 0, sizeof(out));
	if (strlen(ciphertext) > SALT_SIZE-1)
		memcpy(out, ciphertext, SALT_SIZE-1);
	else
		strcpy(out, ciphertext);
	cp = strchr(&out[8], '$');
	while (cp && *cp) {
		*cp++ = 0;
	}
	return out;
}

#define H(s, i) \
	((int)(unsigned char)(atoi64[ARCH_INDEX((s)[(i)])] ^ (s)[(i) - 1]))
/*
 * original Hx() macros simple looked at length-2 (last byte, and last byte -2)
 * now we look at bytes 40 and 38 from the hash, so that longer hashes can
 * be compared to shorter ones.  The last byte may be different, so we
 * do NOT use that one.  This new method works for any number of bytes in
 * the scrypt 32 or more.
#define H0(s) \
	int i = strlen(s) - 2; \
	return i > 0 ? H((s), i) & 0xF : 0
*/

#define H0(s) \
	char *cp = strrchr(s,'$')+40; \
	int i = cp-s; \
	return i > 0 ? H((s), i) & 0xF : 0
#define H1(s) \
	char *cp = strrchr(s,'$')+40; \
	int i = cp-s; \
	return i > 2 ? (H((s), i) ^ (H((s), i - 2) << 4)) & 0xFF : 0
#define H2(s) \
	char *cp = strrchr(s,'$')+40; \
	int i = cp-s; \
	return i > 2 ? (H((s), i) ^ (H((s), i - 2) << 6)) & 0xFFF : 0
#define H3(s) \
	char *cp = strrchr(s,'$')+40; \
	int i = cp-s; \
	return i > 4 ? (H((s), i) ^ (H((s), i - 2) << 5) ^ \
	    (H((s), i - 4) << 10)) & 0xFFFF : 0
#define H4(s) \
	char *cp = strrchr(s,'$')+40; \
	int i = cp-s; \
	return i > 6 ? (H((s), i) ^ (H((s), i - 2) << 5) ^ \
	    (H((s), i - 4) << 10) ^ (H((s), i - 6) << 15)) & 0xFFFFF : 0

static int binary_hash_0(void *binary)
{
	H0((char *)binary);
}

static int binary_hash_1(void *binary)
{
	H1((char *)binary);
}

static int binary_hash_2(void *binary)
{
	H2((char *)binary);
}

static int binary_hash_3(void *binary)
{
	H3((char *)binary);
}

static int binary_hash_4(void *binary)
{
	H4((char *)binary);
}

static int get_hash_0(int index)
{
	H0(buffer[index].out);
}

static int get_hash_1(int index)
{
	H1(buffer[index].out);
}

static int get_hash_2(int index)
{
	H2(buffer[index].out);
}

static int get_hash_3(int index)
{
	H3(buffer[index].out);
}

static int get_hash_4(int index)
{
	H4(buffer[index].out);
}

static int salt_hash(void *salt)
{
	int i, h;

	i = strlen((char *)salt) - 1;
	if (i > 1) i--;

	h = (unsigned char)atoi64[ARCH_INDEX(((char *)salt)[i])];
	h ^= ((unsigned char *)salt)[i - 1];
	h <<= 6;
	h ^= (unsigned char)atoi64[ARCH_INDEX(((char *)salt)[i - 1])];
	h ^= ((unsigned char *)salt)[i];

	return h & (SALT_HASH_SIZE - 1);
}

static void set_salt(void *salt)
{
	strcpy(saved_salt, salt);
}

static void set_key(char *key, int index)
{
	strnzcpy(buffer[index].key, key, PLAINTEXT_LENGTH + 1);
}

static char *get_key(int index)
{
	return buffer[index].key;
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index;
	int failed = 0;

#ifdef _OPENMP
#pragma omp parallel for default(none) private(index) shared(count, failed, local, saved_salt, buffer)
#endif
	for (index = 0; index < count; index++) {
		uint8_t *hash;
		hash = escrypt_r(&(local[index]),
		    (const uint8_t *)(buffer[index].key),
		    strlen(buffer[index].key),
		    (const uint8_t *)saved_salt,
		    (uint8_t *)&(buffer[index].out),
		    sizeof(buffer[index].out));
		if (!hash) {
			failed = 1;
			buffer[index].out[0] = 0;
		}
	}

	if (failed) {
		fprintf(stderr, "scrypt memory allocation failed\n");
		error();
	}

	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;
	// binary was created as 32 bytes. It will always be
	// <= length of buffer.out. So we use the binary as
	// our hash indication lentth (and avoid looking at last byte)
	int len = strlen(buffer[0].out)-2;

	for (index = 0; index < count; index++)
		if (!strncmp((char *)binary, buffer[index].out, len))
			return 1;

	return 0;
}

static int cmp_one(void *binary, int index)
{
	int len = strlen(buffer[index].out)-2;
	return !strncmp((char *)binary, buffer[index].out,len);
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

#if FMT_MAIN_VERSION > 11

static unsigned int tunable_cost_N(void *salt)
{
	const uint8_t * setting;
	const uint8_t * src;
	uint64_t N;

	setting = salt;
	if (setting[0] != '$' || setting[1] != '7' || setting[2] != '$')
		return 0;
	src = setting + 3;
	{
		uint32_t N_log2;

		if (decode64_one(&N_log2, *src))
			return 0;
		src++;
		N = (uint64_t)1 << N_log2;
	}

	return (unsigned int) N;
}
static unsigned int tunable_cost_r(void *salt)
{
	const uint8_t * setting;
	const uint8_t * src;
	uint32_t r;

	setting = salt;
	if (setting[0] != '$' || setting[1] != '7' || setting[2] != '$')
		return 0;
	src = setting + 3;
	{
		uint32_t N_log2;

		if (decode64_one(&N_log2, *src))
			return 0;
		src++;
	}
	src = decode64_uint32(&r, 30, src);
	if (!src)
		return 0;

	return (unsigned int) r;
}

static unsigned int tunable_cost_p(void *salt)
{
	const uint8_t * setting;
	const uint8_t * src;
	uint32_t r, p;

	setting = salt;
	if (setting[0] != '$' || setting[1] != '7' || setting[2] != '$')
		return 0;
	src = setting + 3;
	{
		uint32_t N_log2;

		if (decode64_one(&N_log2, *src))
			return 0;
		src++;
	}
	src = decode64_uint32(&r, 30, src);
	if (!src)
		return 0;
	src = decode64_uint32(&p, 30, src);
	if (!src)
		return 0;

	return (unsigned int) p;
}
#endif

struct fmt_main fmt_scrypt = {
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
		FMT_CASE | FMT_8_BIT | FMT_OMP,
#if FMT_MAIN_VERSION > 11
		{
			"N",
			"r",
			"p"
		},
#endif
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		prepare,
		valid,
		fmt_default_split,
		binary,
		salt,
#if FMT_MAIN_VERSION > 11
		{
			tunable_cost_N,
			tunable_cost_r,
			tunable_cost_p
		},
#endif
		fmt_default_source,
		{
			binary_hash_0,
			binary_hash_1,
			binary_hash_2,
			binary_hash_3,
			binary_hash_4,
			NULL,
			NULL
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
			NULL,
			NULL
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};
