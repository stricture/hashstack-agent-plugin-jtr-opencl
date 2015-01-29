/* SIP cracker patch for JtR. Hacked together during March of 2012 by
 * Dhiru Kholia <dhiru.kholia at gmail.com> .
 *
 * Copyright (C) 2007  Martin J. Muench <mjm@codito.de>
 * SIP digest authentication password (hash) cracker
 * See doc/SIPcrack-LICENSE */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_sip;
#elif FMT_REGISTERS_H
john_register_one(&fmt_sip);
#else

#include "md5.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include "arch.h"
#include "crc32.h"
#include "misc.h"
#include "common.h"
#include "formats.h"
#include "params.h"
#include "options.h"
#include "sip_fmt_plug.h"
#ifdef _OPENMP
static int omp_t = 1;
#include <omp.h>
// Tuned on core i7 quad HT
//   1   4963K
//  16   8486K
//  32   8730K  ** this was chosen.
//  64   8791k
// 128   8908k
#define OMP_SCALE   32
#endif
#include "memdbg.h"

typedef struct sip_salt_t {
	int static_hash_data_len;
	MD5_CTX ctx_dyna_data;
	char static_hash_data[STATIC_HASH_SIZE+1];
} sip_salt;

static sip_salt *pSalt;


#define FORMAT_LABEL		"SIP"
#define FORMAT_NAME		""
#define ALGORITHM_NAME		"MD5 32/" ARCH_BITS_STR
#define BENCHMARK_COMMENT	""
#define BENCHMARK_LENGTH	0
#define PLAINTEXT_LENGTH	32
#define BINARY_SIZE		16
#define SALT_SIZE		sizeof(sip_salt)
#define BINARY_ALIGN	4
#define SALT_ALIGN		sizeof(int)
#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT	64

static struct fmt_tests sip_tests[] = {
	{"$sip$*192.168.1.111*192.168.1.104*200*asterisk*REGISTER*sip*192.168.1.104**46cce857****MD5*4dfc7515936a667565228dbaa0293dfc", "123456"},
	{"$sip$*10.0.1.20*10.0.1.10*1001*asterisk*REGISTER*sips*10.0.1.20*5061*0ef95b07****MD5*576e39e9de6a9ed053eb218f65fe470e", "q1XCLF0KaBObo797"},
	// generated with pass_gen.pl
	{"$sip$*192.168.163.238*192.168.163.239*50894*asterisk*REGISTER*sip*192.168.163.239**303535c9****MD5*e32c95d6ad0fecbc3967b7534d7b5b3b", "123456"},
	{"$sip$*192.168.196.105*192.168.196.192*81670*asterisk*REGISTER*sip*192.168.196.192**747f072a****MD5*d15c84b1bdc2155db12b721d7fb9445b", "password"},
	{"$sip$*192.168.119.6*192.168.119.154*65790*asterisk*REGISTER*sip*192.168.119.154**8d4e1a4b****MD5*dcc0d8a4c105dbf3ecf5b281f4c57356", "happy123"},
	{"$sip$*192.168.113.63*192.168.113.78*59810*asterisk*REGISTER*sip*192.168.113.78**b778256e****MD5*cb13933a5986df471265231d08206509", "aobdataeteag"},
	{NULL}
};

static char (*saved_key)[PLAINTEXT_LENGTH + 1];
static ARCH_WORD_32 (*crypt_key)[BINARY_SIZE/sizeof(ARCH_WORD_32)];
static char bin2hex_table[256][2]; /* table for bin<->hex mapping */

static void init(struct fmt_main *self)
{
#ifdef _OPENMP
	omp_t = omp_get_max_threads();
	self->params.min_keys_per_crypt *= omp_t;
	omp_t *= OMP_SCALE;
	self->params.max_keys_per_crypt *= omp_t;
#endif
	/* Init bin 2 hex table for faster conversions later */
	init_bin2hex(bin2hex_table);
	saved_key = mem_calloc_tiny(sizeof(*saved_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	crypt_key = mem_calloc_tiny(sizeof(*crypt_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *p = ciphertext, *q;
	int i,res = 0;
	if (strncmp(ciphertext, "$sip$*", 6))
		return 0;
	if (strlen(ciphertext) > 2048) // sizeof(saltBuf) in get_salt
		return 0;
	for(i = 0; i < strlen(ciphertext); i++)
		if(ciphertext[i] == '*')
			res++;
	if(res != 14)
		goto err;
	res = 0;
	p += 6;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > HOST_MAXLEN) /* host */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > HOST_MAXLEN) /* host */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > USER_MAXLEN) /* user */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > HOST_MAXLEN) /* realm */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > METHOD_MAXLEN) /* method */
		goto err;
	p = q + 1;
	/* uri stuff */
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	res += q - p;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	res += q - p;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	res += q - p;
	if (res > URI_MAXLEN) /* uri */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > NONCE_MAXLEN) /* nonce */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > NONCE_MAXLEN) /* cnonce */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > CNONCE_MAXLEN) /* nonce_count */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > QOP_MAXLEN) /* qop */
		goto err;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if ((q - p) > ALG_MAXLEN) /* algorithm */
		goto err;
	p = q + 1;
	if ((q = strchr(p, '*')) == NULL)
		goto err;
	if (strncmp("MD5*", p, 4))
		goto err;
	p = q + 1;
	if (strspn(p, "0123456789abcdef") != MD5_LEN_HEX) /* hash */
		goto err;
	return 1;
err:
	return 0;
}

static void *get_salt(char *ciphertext)
{
	static sip_salt salt;
	char saltBuf[2048];
	char *lines[16];
	login_t login;
	int num_lines;
	MD5_CTX md5_ctx;
	unsigned char md5_bin_hash[MD5_LEN];
	char static_hash[MD5_LEN_HEX+1];
	char *saltcopy = saltBuf;

	memset(&salt, 0, sizeof(salt));

	strcpy(saltBuf, ciphertext);
	saltcopy += 6;	/* skip over "$sip$*" */
	memset(&login, 0, sizeof(login_t));
	num_lines = stringtoarray(lines, saltcopy, '*');
	assert(num_lines == 14);
	strncpy(login.server,      lines[0], sizeof(login.server)      - 1 );
	strncpy(login.client,      lines[1], sizeof(login.client)      - 1 );
	strncpy(login.user,        lines[2], sizeof(login.user)        - 1 );
	strncpy(login.realm,       lines[3], sizeof(login.realm)       - 1 );
	strncpy(login.method,      lines[4], sizeof(login.method)      - 1 );
	/* special handling for uri */
	if (!strcmp(lines[7], ""))
		sprintf(login.uri, "%s:%s", lines[5], lines[6]);
	else
		sprintf(login.uri, "%s:%s:%s", lines[5], lines[6], lines[7]);

	strncpy(login.nonce,       lines[8], sizeof(login.nonce)       - 1 );
	strncpy(login.cnonce,      lines[9], sizeof(login.cnonce)      - 1 );
	strncpy(login.nonce_count, lines[10], sizeof(login.nonce_count) - 1 );
	strncpy(login.qop,         lines[11], sizeof(login.qop)        - 1 );
	strncpy(login.algorithm,   lines[12], sizeof(login.algorithm)  - 1 );
	strncpy(login.hash,        lines[13], sizeof(login.hash)       - 1 );
	if(strncmp(login.algorithm, "MD5", strlen(login.algorithm))) {
		printf("\n* Cannot crack '%s' hash, only MD5 supported so far...\n", login.algorithm);
		error();
	}

	/* Generating MD5 static hash: 'METHOD:URI' */
	MD5_Init(&md5_ctx);
	MD5_Update(&md5_ctx, (unsigned char*)login.method, strlen( login.method ));
	MD5_Update(&md5_ctx, (unsigned char*)":", 1);
	MD5_Update(&md5_ctx, (unsigned char*)login.uri, strlen( login.uri ));
	MD5_Final(md5_bin_hash, &md5_ctx);
	bin_to_hex(bin2hex_table, md5_bin_hash, MD5_LEN, static_hash, MD5_LEN_HEX);

	/* Constructing first part of dynamic hash: 'USER:REALM:' */
	MD5_Init(&md5_ctx);
	MD5_Update(&md5_ctx, login.user, strlen(login.user));
	MD5_Update(&md5_ctx, ":", 1);
	MD5_Update(&md5_ctx, login.realm, strlen(login.realm));
	MD5_Update(&md5_ctx, ":", 1);
	memcpy(&(salt.ctx_dyna_data), &md5_ctx, sizeof(md5_ctx));
	// we now construct the MD5_CTX with this data loaded. Thus we no longer store this buffer.
	//snprintf(salt.dynamic_hash_data, DYNAMIC_HASH_SIZE, "%s:%s:", login.user, login.realm);
	//salt.dynamic_hash_data_len = strlen(salt.dynamic_hash_data);

	/* Construct last part of final hash data: ':NONCE(:CNONCE:NONCE_COUNT:QOP):<static_hash>' */
	/* no qop */
	if(!strlen(login.qop))
		snprintf(salt.static_hash_data, STATIC_HASH_SIZE, ":%s:%s", login.nonce, static_hash);
	/* qop/conce/cnonce_count */
	else
		snprintf(salt.static_hash_data, STATIC_HASH_SIZE, ":%s:%s:%s:%s:%s",
				login.nonce, login.nonce_count, login.cnonce,
				login.qop, static_hash);
	/* Get lens of static buffers */
	salt.static_hash_data_len  = strlen(salt.static_hash_data);

	/* Begin brute force attack */
#ifdef SIP_DEBUG
	printf("Starting bruteforce against user '%s' (%s: '%s')\n",
			login.user, login.algorithm, login.hash);
#endif

	return &salt;
}

static void set_salt(void *salt)
{
	pSalt = (sip_salt*)salt;
}

static void * binary(char *ciphertext) {
	static char *bin_val;
	char *p;
	int i;

	if (!bin_val) bin_val = (char*)mem_alloc_tiny(BINARY_SIZE, MEM_ALIGN_WORD);
	p = strrchr(ciphertext, '*') + 1;
	for (i = 0; i < BINARY_SIZE; ++i) {
		bin_val[i] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}

	return (void *)bin_val;
}
static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index = 0;

#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (index = 0; index < count; index++)
	{
		/* password */
		MD5_CTX md5_ctx;
		unsigned char md5_bin_hash[MD5_LEN];
		char dynamic_hash[MD5_LEN_HEX+1];

		/* Generate dynamic hash including pw (see above) */
		//MD5_Init(&md5_ctx);
		//MD5_Update(&md5_ctx, (unsigned char*)pSalt->dynamic_hash_data, pSalt->dynamic_hash_data_len);
		// salt.ctx_dyna_data contains the ctx already loaded.
		memcpy(&md5_ctx, &(pSalt->ctx_dyna_data), sizeof(md5_ctx));

		MD5_Update(&md5_ctx, (unsigned char*)saved_key[index], strlen(saved_key[index]));
		MD5_Final(md5_bin_hash, &md5_ctx);
		bin_to_hex(bin2hex_table, md5_bin_hash, MD5_LEN, dynamic_hash, MD5_LEN_HEX);

		/* Generate digest response hash */
		MD5_Init(&md5_ctx);
		MD5_Update(&md5_ctx, (unsigned char*)dynamic_hash, MD5_LEN_HEX);
		MD5_Update(&md5_ctx, (unsigned char*)pSalt->static_hash_data, pSalt->static_hash_data_len);
		MD5_Final((unsigned char*)crypt_key[index], &md5_ctx);
	}
	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;
	for (index = 0; index < count; index++)
		if ( ((ARCH_WORD_32*)binary)[0] == ((ARCH_WORD_32*)&(crypt_key[index][0]))[0] )
			return 1;
	return 0;

}

static int cmp_one(void *binary, int index)
{
	return !memcmp(binary, crypt_key[index], BINARY_SIZE);
}

static int cmp_exact(char *source, int index)
{
    return 1;
}

static void sip_set_key(char *key, int index)
{
	int saved_key_length = strlen(key);
	memcpy(saved_key[index], key, saved_key_length);
	saved_key[index][saved_key_length] = 0;
}

static char *get_key(int index)
{
	return saved_key[index];
}

static int get_hash_0(int index) { return crypt_key[index][0] & 0xf; }
static int get_hash_1(int index) { return crypt_key[index][0] & 0xff; }
static int get_hash_2(int index) { return crypt_key[index][0] & 0xfff; }
static int get_hash_3(int index) { return crypt_key[index][0] & 0xffff; }
static int get_hash_4(int index) { return crypt_key[index][0] & 0xfffff; }
static int get_hash_5(int index) { return crypt_key[index][0] & 0xffffff; }
static int get_hash_6(int index) { return crypt_key[index][0] & 0x7ffffff; }

struct fmt_main fmt_sip = {
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
		{ NULL },
#endif
		sip_tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		binary,
		get_salt,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
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
		fmt_default_salt_hash,
		NULL,
		set_salt,
		sip_set_key,
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
