/*
 * This software was written by Jim Fougeron jfoug AT cox dot net
 * in 2012. No copyright is claimed, and the software is hereby
 * placed in the public domain. In case this attempt to disclaim
 * copyright and place the software in the public domain is deemed
 * null and void, then the software is Copyright (c) 2012 Jim Fougeron
 * and it is hereby released to the general public under the following
 * terms:
 *
 * This software may be modified, redistributed, and used for any
 * purpose, in source and binary forms, with or without modification.
 *
 *
 * This implements the SRP protocol, with Blizzard's (battlenet) documented
 * implementation specifics.
 *
 * U = username in upper case
 * P = password in upper case
 * s = random salt value.
 *
 * x = SHA1(s . SHA1(U . ":" . P));
 * v = 47^x % 112624315653284427036559548610503669920632123929604336254260115573677366691719
 *
 * v is the 'verifier' value (256 bit value).
 *
 * Added OMP.  Added 'default' oSSL BigNum exponentiation.
 * GMP exponentation (faster) is optional, and controled with HAVE_LIBGMP in autoconfig.h
 *
 * NOTE, big fix required. The incoming binary may be 64 bytes OR LESS.  It
 * can also be 64 bytes (or less), and have left padded 0's.  We have to adjust
 * several things to handle this properly. First, valid must handle it. Then
 * binary and salt both must handle this. Also, crypt must handle this.  NOTE,
 * the string 'could' be an odd length. If so, then only 1 byte of hex is put
 * into the first binary byte. all of these problems were found once I got
 * jtrts.pl working with wowsrp. There now are 2 input files for wowsrp. One
 * bytes of precision, then only 61 bytes will be in the string). The other
 * file left pads the numbers with 0's to an even 64 bytes long, so all are
 * 64 bytes. the format MUST handle both, since at this momement, we are not
 * exactly sure which type will be seen in the wild.  NOTE, the byte swapped
 * method (GMP) within is no longer valid, and was removed.
 * NOTE, we need to add split() to canonize this format (remove LPad 0's)
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_blizzard;
#elif FMT_REGISTERS_H
john_register_one(&fmt_blizzard);
#else

#if AC_BUILT
/* we need to know if HAVE_LIBGMP is defined */
#include "autoconfig.h"
#endif

#include <string.h>
#include "sha.h"
#include "sha2.h"

#include "arch.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "unicode.h" /* For encoding-aware uppercasing */
#ifdef HAVE_LIBGMP
#if HAVE_GMP_GMP_H
#include <gmp/gmp.h>
#else
#include <gmp.h>
#endif
#define EXP_STR " GMP-exp"
#else
#include <openssl/bn.h>
#define EXP_STR " oSSL-exp"
#endif
#include "johnswap.h"
#ifdef _OPENMP
#include <omp.h>
#define OMP_SCALE               64
#endif
#include "memdbg.h"


#define FORMAT_LABEL		"WoWSRP"
#define FORMAT_NAME		"Battlenet"
#define ALGORITHM_NAME		"SHA1 32/" ARCH_BITS_STR EXP_STR

#define BENCHMARK_COMMENT	""
#define BENCHMARK_LENGTH	-1

#define WOWSIG			"$WoWSRP$"
#define WOWSIGLEN		8
// min plaintext len is 8  PW's are only alpha-num uppercase
#define PLAINTEXT_LENGTH	16
#define CIPHERTEXT_LENGTH	64

#define BINARY_SIZE		4
#define BINARY_ALIGN		4
#define FULL_BINARY_SIZE	32
#define SALT_SIZE		(64+3)
#define SALT_ALIGN		1
#define USERNAMELEN             32

#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT	4

// salt is in hex  (salt and salt2)
static struct fmt_tests tests[] = {
	{WOWSIG"6D00CD214C8473C7F4E9DC77AE8FC6B3944298C48C7454E6BB8296952DCFE78D$73616C74", "PASSWORD", {"SOLAR"}},
	{WOWSIG"A35DCC134159A34F1D411DA7F38AB064B617D5DBDD9258FE2F23D5AB1CF3F685$73616C7432", "PASSWORD2", {"DIZ"}},
	{WOWSIG"A35DCC134159A34F1D411DA7F38AB064B617D5DBDD9258FE2F23D5AB1CF3F685$73616C7432*DIZ", "PASSWORD2"},
	// this one has a leading 0
	{"$WoWSRP$01C7F618E4589F3229D764580FDBF0D579D7CB1C071F11C856BDDA9E41946530$36354172646F744A366A7A58386D4D6E*JOHN", "PASSWORD"},
	// same hash, but without 0 (only 63 byte hash).
	{"$WoWSRP$1C7F618E4589F3229D764580FDBF0D579D7CB1C071F11C856BDDA9E41946530$36354172646F744A366A7A58386D4D6E*JOHN", "PASSWORD"},
	{NULL}
};

#ifdef HAVE_LIBGMP
typedef struct t_SRP_CTX {
	mpz_t z_mod, z_base, z_exp, z_rop;
} SRP_CTX;
#else
typedef struct t_SRP_CTX {
	BIGNUM *z_mod, *z_base, *z_exp, *z_rop;
	BN_CTX *BN_ctx;
}SRP_CTX;
#endif

static SRP_CTX *pSRP_CTX;
static unsigned char saved_salt[SALT_SIZE];
static unsigned char user_id[USERNAMELEN];
static char (*saved_key)[PLAINTEXT_LENGTH + 1];
static ARCH_WORD_32 (*crypt_out)[8];


static void init(struct fmt_main *self)
{
	int i;
#if defined (_OPENMP)
	int omp_t = omp_get_max_threads();
	self->params.min_keys_per_crypt *= omp_t;
	omp_t *= OMP_SCALE;
	self->params.max_keys_per_crypt *= omp_t;
#endif
	saved_key = mem_calloc_tiny(sizeof(*saved_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	crypt_out = mem_calloc_tiny(sizeof(*crypt_out) * self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	pSRP_CTX = mem_calloc_tiny(sizeof(*pSRP_CTX) * self->params.max_keys_per_crypt, MEM_ALIGN_WORD);

	for (i = 0; i < self->params.max_keys_per_crypt; ++i) {
#ifdef HAVE_LIBGMP
		mpz_init_set_str(pSRP_CTX[i].z_mod, "112624315653284427036559548610503669920632123929604336254260115573677366691719", 10);
		mpz_init_set_str(pSRP_CTX[i].z_base, "47", 10);
		mpz_init_set_str(pSRP_CTX[i].z_exp, "1", 10);
		mpz_init(pSRP_CTX[i].z_rop);
		// Now, properly initialzed mpz_exp, so it is 'large enough' to hold any SHA1 value
		// we need to put into it. Then we simply need to copy in the data, and possibly set
		// the limb count size.
		mpz_mul_2exp(pSRP_CTX[i].z_exp, pSRP_CTX[i].z_exp, 159);
#else
		pSRP_CTX[i].z_mod=BN_new();
		BN_dec2bn(&pSRP_CTX[i].z_mod, "112624315653284427036559548610503669920632123929604336254260115573677366691719");
		pSRP_CTX[i].z_base=BN_new();
		BN_set_word(pSRP_CTX[i].z_base, 47);
		pSRP_CTX[i].z_exp=BN_new();
		pSRP_CTX[i].z_rop=BN_new();
		pSRP_CTX[i].BN_ctx = BN_CTX_new();
#endif
	}
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *p, *q;

	if (strncmp(ciphertext, WOWSIG, WOWSIGLEN))
		return 0;

	q = p = &ciphertext[WOWSIGLEN];
	while (atoi16[ARCH_INDEX(*q)] != 0x7F)
		q++;
	if (q-p > CIPHERTEXT_LENGTH)
		return 0;
	if (*q != '$')
		return 0;
	++q;
	p = strchr(q, '*');
	if (!p)
		return 0;
	if (((p - q) & 1))
		return 0;
	if (p - q > 2 * SALT_SIZE)
		return 0;
	while (atoi16[ARCH_INDEX(*q)] != 0x7F)
		q++;
	if (q != p)
		return 0;
	if (strlen(&p[1]) > USERNAMELEN)
		return 0;
	return 1;
}

static void StripZeros(const char *ct, char *ct2) {
	int i;
	for (i = 0; i < WOWSIGLEN; ++i)
		*ct2++ = *ct++;
	while (*ct == '0')
		++ct;
	while (*ct)
		*ct2++ = *ct++;
	*ct2 = 0;
}

static char *prepare(char *split_fields[10], struct fmt_main *pFmt) {
	// if user name not there, then add it
	static char ct[128+32+1];
	char *cp;

	if (!split_fields[1][0] || strncmp(split_fields[1], WOWSIG, WOWSIGLEN))
		return split_fields[1];
	cp = strchr(split_fields[1], '*');
	if (cp) {
		if (split_fields[1][WOWSIGLEN] == '0') {
			StripZeros(split_fields[1], ct);
			return ct;
		}
		return split_fields[1];
	}
	strnzcpy(ct, split_fields[1], 128);
	cp = &ct[strlen(ct)];
	*cp++ = '*';
	strnzcpy(cp, split_fields[0], USERNAMELEN);
	// upcase user name
	enc_strupper(cp);
	// Ok, if there are leading 0's for that binary resultant value, then remove them.
	if (ct[WOWSIGLEN] == '0') {
		char ct2[128+32+1];
		StripZeros(ct, ct2);
		strcpy(ct, ct2);
	}
	return ct;
}

static char *split(char *ciphertext, int index, struct fmt_main *pFmt) {
	static char ct[128+32+1];
	char *cp;

	strnzcpy(ct, ciphertext, 128+32+1);
	cp = strchr(ct, '*');
	if (cp) *cp = 0;
	strupr(&ct[WOWSIGLEN]);
	if (cp) *cp = '*';
	if (ct[WOWSIGLEN] == '0') {
		char ct2[128+32+1];
		StripZeros(ct, ct2);
		strcpy(ct, ct2);
	}
	return ct;
}


static void *get_binary(char *ciphertext)
{
	static union {
		unsigned char b[FULL_BINARY_SIZE];
		ARCH_WORD_32 dummy[1];
	} out;
	char *p, *q;
	int i;

	p = &ciphertext[WOWSIGLEN];
	q = strchr(p, '$');
	memset(out.b, 0, sizeof(out.b));
	while (*p == '0')
		++p;
	if ((q-p)&1) {
		out.b[0] = atoi16[ARCH_INDEX(*p)];
		++p;
	} else {
		out.b[0] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}
	for (i = 1; i < FULL_BINARY_SIZE; i++) {
		out.b[i] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
		if (p >= q)
			break;
	}
	//dump_stuff_msg("binary", out.b, 32);
	return out.b;
}

static void *salt(char *ciphertext)
{
	static union {
		unsigned char b[SALT_SIZE];
		ARCH_WORD_32 dummy;
	} out;
	char *p;
	int length=0;

	memset(out.b, 0, SALT_SIZE);
	p = strchr(&ciphertext[WOWSIGLEN], '$') + 1;

	while (atoi16[ARCH_INDEX(*p)] != 0x7f) {
		out.b[++length] =
		    (atoi16[ARCH_INDEX(*p)] << 4) |
		    atoi16[ARCH_INDEX(p[1])];
		p += 2;
	}
	++p;
	out.b[0] = length;
	memcpy(out.b + length+1, p, strlen(p)+1);

	return out.b;
}

static int get_hash_0(int index)       { return crypt_out[index][0] & 0xF; }
static int get_hash_1(int index)       { return crypt_out[index][0] & 0xFF; }
static int get_hash_2(int index)       { return crypt_out[index][0] & 0xFFF; }
static int get_hash_3(int index)       { return crypt_out[index][0] & 0xFFFF; }
static int get_hash_4(int index)       { return crypt_out[index][0] & 0xFFFFF; }
static int get_hash_5(int index)       { return crypt_out[index][0] & 0xFFFFFF; }
static int get_hash_6(int index)       { return crypt_out[index][0] & 0x7FFFFFF; }

static int salt_hash(void *salt)
{
	unsigned int hash = 0;
	char *p = (char *)salt;

	while (*p) {
		hash <<= 1;
		hash += (unsigned char)*p++;
		if (hash >> SALT_HASH_LOG) {
			hash ^= hash >> SALT_HASH_LOG;
			hash &= (SALT_HASH_SIZE - 1);
		}
	}

	hash ^= hash >> SALT_HASH_LOG;
	hash &= (SALT_HASH_SIZE - 1);

	return hash;
}

static void set_salt(void *salt)
{
	unsigned char *cp = (unsigned char*)salt;
	memcpy(saved_salt, &cp[1], *cp);
	saved_salt[*cp] = 0;
	strcpy((char*)user_id, (char*)&cp[*cp+1]);
}

static void set_key(char *key, int index)
{
	strnzcpy(saved_key[index], key, PLAINTEXT_LENGTH+1);
	enc_strupper(saved_key[index]);
}

static char *get_key(int index)
{
	return saved_key[index];
}

// x = SHA1(s, H(U, ":", P));
// v = 47^x % 112624315653284427036559548610503669920632123929604336254260115573677366691719

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int j;
#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (j = 0; j < count; ++j) {
		SHA_CTX ctx;
		unsigned char Tmp[20];

		memset(crypt_out[j], 0, sizeof(crypt_out[j]));
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, user_id, strlen((char*)user_id));
		SHA1_Update(&ctx, ":", 1);
		SHA1_Update(&ctx, saved_key[j], strlen(saved_key[j]));
		SHA1_Final(Tmp, &ctx);
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, saved_salt, strlen((char*)saved_salt));
		SHA1_Update(&ctx, Tmp, 20);
		SHA1_Final(Tmp, &ctx);
		// Ok, now Tmp is v

		//if (!strcmp(saved_key[j], "ENTERNOW__1") && !strcmp((char*)user_id, "DIP")) {
		//	printf ("salt=%s user=%s  pass=%s, ", (char*)saved_salt, (char*)user_id, saved_key[j]);
		//	dump_stuff_msg("sha$h  ", Tmp, 20);
		//}

#ifdef HAVE_LIBGMP
	{
		unsigned char HashStr[80], *p;
		int i, todo;
		p = HashStr;
		for (i = 0; i < 20; ++i) {
			*p++ = itoa16[Tmp[i]>>4];
			*p++ = itoa16[Tmp[i]&0xF];
		}
		*p = 0;

		mpz_set_str(pSRP_CTX[j].z_exp, (char*)HashStr, 16);
		mpz_powm (pSRP_CTX[j].z_rop, pSRP_CTX[j].z_base, pSRP_CTX[j].z_exp, pSRP_CTX[j].z_mod );
		mpz_get_str ((char*)HashStr, 16, pSRP_CTX[j].z_rop);

		p = HashStr;
		todo = strlen((char*)p);
		if (todo&1) {
			((unsigned char*)(crypt_out[j]))[0] = atoi16[ARCH_INDEX(*p)];
			++p;
			--todo;
		} else {
			((unsigned char*)(crypt_out[j]))[0] =
				(atoi16[ARCH_INDEX(*p)] << 4) |
				atoi16[ARCH_INDEX(p[1])];
			p += 2;
			todo -= 2;
		}
		todo >>= 1;
		for (i = 1; i <= todo; i++) {
			((unsigned char*)(crypt_out[j]))[i] =
				(atoi16[ARCH_INDEX(*p)] << 4) |
				atoi16[ARCH_INDEX(p[1])];
			p += 2;
		}
		//if (!strcmp(saved_key[j], "ENTERNOW__1") && !strcmp((char*)user_id, "DIP")) {
		//	dump_stuff_msg("crypt ", crypt_out[j], 32);
		//}
	}
#else
		// using oSSL's BN to do expmod.
		pSRP_CTX[j].z_exp = BN_bin2bn(Tmp,20,pSRP_CTX[j].z_exp);
		BN_mod_exp(pSRP_CTX[j].z_rop, pSRP_CTX[j].z_base, pSRP_CTX[j].z_exp, pSRP_CTX[j].z_mod, pSRP_CTX[j].BN_ctx);
		BN_bn2bin(pSRP_CTX[j].z_rop, (unsigned char*)(crypt_out[j]));
		//if (!strcmp(saved_key[j], "ENTERNOW__1") && !strcmp((char*)user_id, "DIP")) {
		//	dump_stuff_msg("crypt ", crypt_out[j], 32);
		//}
#endif
	}
	return count;
}

static int cmp_all(void *binary, int count)
{
	int i;
	for (i = 0; i < count; ++i) {
		if (*((ARCH_WORD_32*)binary) == *((ARCH_WORD_32*)(crypt_out[i])))
			return 1;
	}
	return 0;
}
static int cmp_one(void *binary, int index)
{
	return *((ARCH_WORD_32*)binary) == *((ARCH_WORD_32*)(crypt_out[index]));
}

static int cmp_exact(char *source, int index)
{
	return !memcmp(get_binary(source), crypt_out[index], BINARY_SIZE);
}

struct fmt_main fmt_blizzard = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		8,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_8_BIT | FMT_SPLIT_UNIFIES_CASE | FMT_OMP,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		prepare,
		valid,
		split,
		get_binary,
		salt,
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
