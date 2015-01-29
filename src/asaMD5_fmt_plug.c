/*
 * This software is Copyright (c) 2004 bartavelle, <bartavelle at bandecon.com>, and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 *
 * Converted to thin format, into $dynamic_20$ format.
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_asaMD5;
#elif FMT_REGISTERS_H
john_register_one(&fmt_asaMD5);
#else

#include <string.h>

#include "common.h"
#include "formats.h"
#include "dynamic.h"
#include "memdbg.h"

#define FORMAT_LABEL            "asa-md5"
#define FORMAT_NAME             "Cisco ASA"
#define ALGORITHM_NAME          "?" /* filled in by md5-gen */
#define BENCHMARK_COMMENT       ""
#define BENCHMARK_LENGTH        0

#define PLAINTEXT_LENGTH        32
#define CIPHERTEXT_LENGTH       21
#define BINARY_SIZE             16
#define BINARY_ALIGN            MEM_ALIGN_WORD

#define SALT_SIZE               (sizeof(char*))
#define SALT_ALIGN              MEM_ALIGN_WORD

static struct fmt_tests tests[] = {
	{"$dynamic_20$h3mJrcH0901pqX/m$alex","ripper"},
	{"$dynamic_20$3USUcOPFUiMCO4Jk$cisc","cisco"},
	{"$dynamic_20$lZt7HSIXw3.QP7.R$admc","CscFw-ITC!"},
	{"$dynamic_20$hN7LzeyYjw12FSIU$john","cisco"},
	{"$dynamic_20$7DrfeZ7cyOj/PslD$jack","cisco"},
	{NULL}
};

static char Conv_Buf[80];
static struct fmt_main *pDynamic_20;
static void init(struct fmt_main *self);
static void get_ptr();

/* this function converts a 'native' asamd5 signature string into a Dynamic_20 syntax string */
static char *Convert(char *Buf, char *ciphertext) {
	// 2KFQnbNIdI.2KYOU -> $dynamic_20$2KFQnbNIdI.2KYOU
	if (text_in_dynamic_format_already(pDynamic_20, ciphertext))
		return ciphertext;

	if (strlen(ciphertext) == CIPHERTEXT_LENGTH) {
		sprintf(Buf, "$dynamic_20$%s", ciphertext);
		return Buf;
	}
	return ciphertext;
}

static char *our_split(char *ciphertext, int index, struct fmt_main *self)
{
	get_ptr();
	return pDynamic_20->methods.split(Convert(Conv_Buf, ciphertext), index, self);
}

static void *our_salt(char *ciphertext) {
	get_ptr();
	return pDynamic_20->methods.salt(Convert(Conv_Buf, ciphertext));
}

static int valid(char *ciphertext, struct fmt_main *self) {
	int i;

	if (!ciphertext)
		return 0;
	get_ptr();
	i = strlen(ciphertext);
	if (i > CIPHERTEXT_LENGTH)
		return pDynamic_20->methods.valid(ciphertext, pDynamic_20);
	if (i == CIPHERTEXT_LENGTH)
		return pDynamic_20->methods.valid(Convert(Conv_Buf, ciphertext), pDynamic_20);
	return 0;
}

/* this function converts username:hash to $dynamic_20$hash$user,
   where user is username truncated to four characters */
static char *prepare(char *split_fields[10], struct fmt_main *self)
{
	static char out[11+1+16+1+4+1];

	if (!valid(split_fields[1], self)) {
		if (split_fields[1] && strlen(split_fields[1]) == 16) {
			char username[4+1] = "";

			strncat(username, split_fields[0], 4);
			sprintf(out, "$dynamic_20$%s$%s", split_fields[1],
			        username);
			if (valid(out,self))
				return out;
		}
	}
	return split_fields[1];
}

static void *our_binary(char *ciphertext) {
	get_ptr();
	return pDynamic_20->methods.binary(Convert(Conv_Buf, ciphertext));
}

struct fmt_main fmt_asaMD5 = {
	{
		// setup the labeling and stuff. NOTE the max and min crypts are set to 1
		// here, but will be reset within our init() function.
		FORMAT_LABEL, FORMAT_NAME, ALGORITHM_NAME, BENCHMARK_COMMENT, BENCHMARK_LENGTH,
		0, 12, BINARY_SIZE, BINARY_ALIGN, SALT_SIZE, SALT_ALIGN, 1, 1, FMT_CASE | FMT_8_BIT | FMT_DYNAMIC,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		tests
	},
	{
		/* All we setup here, is the pointer to valid, and the pointer to init */
		/* within the call to init, we will properly set this full object */
		init,
		fmt_default_done,
		fmt_default_reset,
		prepare,
		valid,
		our_split
	}
};

static void link_funcs() {
	fmt_asaMD5.methods.salt = our_salt;
	fmt_asaMD5.methods.binary = our_binary;
	fmt_asaMD5.methods.split = our_split;
}

static void init(struct fmt_main *self)
{
	get_ptr();
	if (self->private.initialized == 0) {
		pDynamic_20 = dynamic_THIN_FORMAT_LINK(&fmt_asaMD5, Convert(Conv_Buf, tests[0].ciphertext), "asa-md5", 1);
		link_funcs();
		fmt_asaMD5.params.algorithm_name = pDynamic_20->params.algorithm_name;
		self->private.initialized = 1;
	}
}

static void get_ptr() {
	if (!pDynamic_20) {
		pDynamic_20 = dynamic_THIN_FORMAT_LINK(&fmt_asaMD5, Convert(Conv_Buf, tests[0].ciphertext), "asa-md5", 0);
		link_funcs();
	}
}

#endif /* plugin stanza */
