/*
 * This file is part of John the Ripper password cracker,
 * Copyright (c) 2013 by Solar Designer
 * Copyright (c) 2013-2014 by magnum
 * Copyright (c) 2014 by Sayantan Datta
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * There's ABSOLUTELY NO WARRANTY, express or implied.
 */

//#define MASK_DEBUG

#include <stdio.h> /* for fprintf(stderr, ...) */
#include <string.h>
#include <ctype.h>

#include "misc.h" /* for error() */
#include "logger.h"
#include "recovery.h"
#include "os.h"
#include "signals.h"
#include "status.h"
#include "options.h"
#include "config.h"
#include "external.h"
#include "cracker.h"
#include "john.h"
#include "mask.h"
#include "unicode.h"
#include "encoding_data.h"
#include "memdbg.h"
#include "mask_ext.h"

static parsed_ctx parsed_mask;
static cpu_mask_context cpu_mask_ctx, rec_ctx;
static int *template_key_offsets;
static char *mask = NULL, *template_key;
static int max_keylen, fmt_maxlen, rec_len, rec_cl, restored_len, restored = 1;
static unsigned long long cand_length;
int mask_add_len, mask_num_qw, mask_cur_len;

/*
 * This keeps track of whether we have any 8-bit in our non-hybrid mask.
 * If we do not, we can skip expensive encoding conversions
 */
static int mask_has_8bit;

/*
 * cand and rec_cand is the number of remaining candidates.
 * So, its value decreases as cracking progress.
 */
static unsigned long long cand, rec_cand;

unsigned long long mask_tot_cand;
unsigned long long mask_parent_keys;

#define BUILT_IN_CHARSET "ludshaLUDSHA123456789"

#define store_op(k, i) \
	parsed_mask->stack_op_br[k] = i;

#define store_cl(k, i) \
	parsed_mask->stack_cl_br[k] = i;

#define load_op(i) \
	parsed_mask->stack_op_br[i]

#define load_cl(i) \
	parsed_mask->stack_cl_br[i]

#define load_qtn(i) \
	parsed_mask->stack_qtn[i]

/*
 * Converts \xHH notation to characters. The original buffer is modified -
 * we are guaranteed the new string is shorter or same length.
 *
 * This function must pass escaped characters on, as-is (still escaped),
 * including "\\" which may escape "\\xHH" from being parsed as \xHH.
 */
static void parse_hex(char *string)
{
	static int warned;
	unsigned char *s = (unsigned char*)string;
	unsigned char *d = s;

	if (!string || !*string)
		return;

	while (*s)
	if (*s == '\\' && s[1] != 'x') {
		*d++ = *s++;
		*d++ = *s++;
	} else if (*s == '\\' && s[1] == 'x' &&
	    atoi16[s[2]] != 0x7f && atoi16[s[3]] != 0x7f) {
		char c = (atoi16[s[2]] << 4) + atoi16[s[3]];
		if (!c && !warned++)
		if (john_main_process)
			fprintf(stderr, "Warning: \\x00 in mask terminates "
			        "the string\n");
		if (strchr("\\[]?-", c))
			*d++ = '\\';
		*d++ = c;
		s += 4;
	} else
		*d++ = *s++;

	*d = 0;
}

/*
 * Expands custom placeholders in string and returns a new resulting string.
 * with -1=?u?l, "A?1abc[3-6]" will expand to "A[?u?l]abc[3-6]"
 *
 * This function must pass any escaped characters on, as-is (still escaped).
 */
static char* expand_cplhdr(char *string)
{
	static char out[0x8000];
	unsigned char *s = (unsigned char*)string;
	char *d = out;

	if (!string || !*string)
		return string;

	//fprintf(stderr, "%s(%s)\n", __FUNCTION__, string);
	while (*s && d < &out[sizeof(out) - 2]) {
		if (*s == '\\') {
			*d++ = *s++;
			*d++ = *s++;
		} else
		if (*s == '?' && s[1] >= '1' && s[1] <= '9') {
			int ab = 0;
			char *cs = options.custom_mask[s[1] - '1'];
			if (*cs != '[') {
				*d++ = '[';
				ab = 1;
			}
			while (*cs && d < &out[sizeof(out) - 2])
				*d++ = *cs++;
			if (ab)
				*d++ = ']';
			s += 2;
		} else
			*d++ = *s++;
	}
	*d = '\0';

#ifdef MASK_DEBUG
	fprintf(stderr, "%s(%s) return: %s\n", __FUNCTION__, string, out);
#endif
	return out;
}

/*
 * Convert a single placeholder like ?l (given as 'l' char arg.) to a string.
 * plhdr2string('d', n) will return "0123456789"
 *
 * This function never has to deal with escapes (would not be called).
 */
static char* plhdr2string(char p, int fmt_case)
{
	static char out[256];
	char *s, *o = out;
	int j;

#define add_range(a, b)	for (j = a; j <= b; j++) *o++ = j
#define add_string(str)	for (s = (char*)str; *s; s++) *o++ = *s

	if ((pers_opts.internal_enc == ASCII ||
	     pers_opts.internal_enc == UTF_8) &&
	    (p == 'L' || p == 'U' || p == 'D' || p == 'S')) {
		if (john_main_process)
		fprintf(stderr, "Can't use ?%c placeholder with %s encoding\n",
			        p, cp_id2name(pers_opts.internal_enc));
		error();
	}

	switch(p) {
	case 'l': /* lower-case letters */
		add_range('a', 'z');
		break;
	case 'L': /* lower-case letters, non-ASCII only */
		switch (pers_opts.internal_enc) {
		case CP437:
			add_string(CHARS_LOWER_CP437
			           CHARS_LOW_ONLY_CP437);
			break;
		case CP737:
			add_string(CHARS_LOWER_CP737
			           CHARS_LOW_ONLY_CP737);
			break;
		case CP850:
			add_string(CHARS_LOWER_CP850
			           CHARS_LOW_ONLY_CP850);
			break;
		case CP852:
			add_string(CHARS_LOWER_CP852
			           CHARS_LOW_ONLY_CP852);
			break;
		case CP858:
			add_string(CHARS_LOWER_CP858
			           CHARS_LOW_ONLY_CP858);
			break;
		case CP866:
			add_string(CHARS_LOWER_CP866
			           CHARS_LOW_ONLY_CP866);
			break;
		case CP1250:
			add_string(CHARS_LOWER_CP1250
			           CHARS_LOW_ONLY_CP1250);
			break;
		case CP1251:
			add_string(CHARS_LOWER_CP1251
			           CHARS_LOW_ONLY_CP1251);
			break;
		case CP1252:
			add_string(CHARS_LOWER_CP1252
			           CHARS_LOW_ONLY_CP1252);
			break;
		case CP1253:
			add_string(CHARS_LOWER_CP1253
			           CHARS_LOW_ONLY_CP1253);
			break;
		case ISO_8859_1:
			add_string(CHARS_LOWER_ISO_8859_1
			           CHARS_LOW_ONLY_ISO_8859_1);
			break;
		case ISO_8859_2:
			add_string(CHARS_LOWER_ISO_8859_2
			           CHARS_LOW_ONLY_ISO_8859_2);
			break;
		case ISO_8859_7:
			add_string(CHARS_LOWER_ISO_8859_7
			           CHARS_LOW_ONLY_ISO_8859_7);
			break;
		case ISO_8859_15:
			add_string(CHARS_LOWER_ISO_8859_15
			           CHARS_LOW_ONLY_ISO_8859_15);
			break;
		case KOI8_R:
			add_string(CHARS_LOWER_KOI8_R
			           CHARS_LOW_ONLY_KOI8_R);
			break;
		}
		break;
	case 'u': /* upper-case letters */
		add_range('A', 'Z');
		break;
	case 'U': /* upper-case letters, non-ASCII only */
		switch (pers_opts.internal_enc) {
		case CP437:
			add_string(CHARS_UPPER_CP437
			           CHARS_UP_ONLY_CP437);
			break;
		case CP737:
			add_string(CHARS_UPPER_CP737
			           CHARS_UP_ONLY_CP737);
			break;
		case CP850:
			add_string(CHARS_UPPER_CP850
			           CHARS_UP_ONLY_CP850);
			break;
		case CP852:
			add_string(CHARS_UPPER_CP852
			           CHARS_UP_ONLY_CP852);
			break;
		case CP858:
			add_string(CHARS_UPPER_CP858
			           CHARS_UP_ONLY_CP858);
			break;
		case CP866:
			add_string(CHARS_UPPER_CP866
			           CHARS_UP_ONLY_CP866);
			break;
		case CP1250:
			add_string(CHARS_UPPER_CP1250
			           CHARS_UP_ONLY_CP1250);
			break;
		case CP1251:
			add_string(CHARS_UPPER_CP1251
			           CHARS_UP_ONLY_CP1251);
			break;
		case CP1252:
			add_string(CHARS_UPPER_CP1252
			           CHARS_UP_ONLY_CP1252);
			break;
		case CP1253:
			add_string(CHARS_UPPER_CP1253
			           CHARS_UP_ONLY_CP1253);
			break;
		case ISO_8859_1:
			add_string(CHARS_UPPER_ISO_8859_1
			           CHARS_UP_ONLY_ISO_8859_1);
			break;
		case ISO_8859_2:
			add_string(CHARS_UPPER_ISO_8859_2
			           CHARS_UP_ONLY_ISO_8859_2);
			break;
		case ISO_8859_7:
			add_string(CHARS_UPPER_ISO_8859_7
			           CHARS_UP_ONLY_ISO_8859_7);
			break;
		case ISO_8859_15:
			add_string(CHARS_UPPER_ISO_8859_15
			           CHARS_UP_ONLY_ISO_8859_15);
			break;
		case KOI8_R:
			add_string(CHARS_UPPER_KOI8_R
			           CHARS_UP_ONLY_KOI8_R);
			break;
		}
		break;
	case 'd': /* digits */
		add_range('0', '9');
		break;
	case 'D': /* digits, non-ASCII only */
		switch (pers_opts.internal_enc) {
		case CP437:
			add_string(CHARS_DIGITS_CP437);
			break;
		case CP737:
			add_string(CHARS_DIGITS_CP737);
			break;
		case CP850:
			add_string(CHARS_DIGITS_CP850);
			break;
		case CP852:
			add_string(CHARS_DIGITS_CP852);
			break;
		case CP858:
			add_string(CHARS_DIGITS_CP858);
			break;
		case CP866:
			add_string(CHARS_DIGITS_CP866);
			break;
		case CP1250:
			add_string(CHARS_DIGITS_CP1250);
			break;
		case CP1251:
			add_string(CHARS_DIGITS_CP1251);
			break;
		case CP1252:
			add_string(CHARS_DIGITS_CP1252);
			break;
		case CP1253:
			add_string(CHARS_DIGITS_CP1253);
			break;
		case ISO_8859_1:
			add_string(CHARS_DIGITS_ISO_8859_1);
			break;
		case ISO_8859_2:
			add_string(CHARS_DIGITS_ISO_8859_2);
			break;
		case ISO_8859_7:
			add_string(CHARS_DIGITS_ISO_8859_7);
			break;
		case ISO_8859_15:
			add_string(CHARS_DIGITS_ISO_8859_15);
			break;
		case KOI8_R:
			add_string(CHARS_DIGITS_KOI8_R);
			break;
		}
		break;
	case 's': /* specials */
		add_range(32, 47);
		add_range(58, 64);
		add_range(91, 96);
		add_range(123, 126);
		break;
	case 'S': /* specials, non-ASCII only */
		switch (pers_opts.internal_enc) {
		case CP437:
			add_string(CHARS_PUNCTUATION_CP437
			           CHARS_SPECIALS_CP437
			           CHARS_WHITESPACE_CP437);
			break;
		case CP737:
			add_string(CHARS_PUNCTUATION_CP737
			           CHARS_SPECIALS_CP737
			           CHARS_WHITESPACE_CP737);
			break;
		case CP850:
			add_string(CHARS_PUNCTUATION_CP850
			           CHARS_SPECIALS_CP850
			           CHARS_WHITESPACE_CP850);
			break;
		case CP852:
			add_string(CHARS_PUNCTUATION_CP852
			           CHARS_SPECIALS_CP852
			           CHARS_WHITESPACE_CP852);
			break;
		case CP858:
			add_string(CHARS_PUNCTUATION_CP858
			           CHARS_SPECIALS_CP858
			           CHARS_WHITESPACE_CP858);
			break;
		case CP866:
			add_string(CHARS_PUNCTUATION_CP866
			           CHARS_SPECIALS_CP866
			           CHARS_WHITESPACE_CP866);
			break;
		case CP1250:
			add_string(CHARS_PUNCTUATION_CP1250
			           CHARS_SPECIALS_CP1250
			           CHARS_WHITESPACE_CP1250);
			break;
		case CP1251:
			add_string(CHARS_PUNCTUATION_CP1251
			           CHARS_SPECIALS_CP1251
			           CHARS_WHITESPACE_CP1251);
			break;
		case CP1252:
			add_string(CHARS_PUNCTUATION_CP1252
			           CHARS_SPECIALS_CP1252
			           CHARS_WHITESPACE_CP1252);
			break;
		case CP1253:
			add_string(CHARS_PUNCTUATION_CP1253
			           CHARS_SPECIALS_CP1253
			           CHARS_WHITESPACE_CP1253);
			break;
		case ISO_8859_1:
			add_string(CHARS_PUNCTUATION_ISO_8859_1
			           CHARS_SPECIALS_ISO_8859_1
			           CHARS_WHITESPACE_ISO_8859_1);
			break;
		case ISO_8859_2:
			add_string(CHARS_PUNCTUATION_ISO_8859_2
			           CHARS_SPECIALS_ISO_8859_2
			           CHARS_WHITESPACE_ISO_8859_2);
			break;
		case ISO_8859_7:
			add_string(CHARS_PUNCTUATION_ISO_8859_7
			           CHARS_SPECIALS_ISO_8859_7
			           CHARS_WHITESPACE_ISO_8859_7);
			break;
		case ISO_8859_15:
			add_string(CHARS_PUNCTUATION_ISO_8859_15
			           CHARS_SPECIALS_ISO_8859_15
			           CHARS_WHITESPACE_ISO_8859_15);
			break;
		case KOI8_R:
			add_string(CHARS_PUNCTUATION_KOI8_R
			           CHARS_SPECIALS_KOI8_R
			           CHARS_WHITESPACE_KOI8_R);
			break;
		}
		break;
	case 'h': /* All high-bit */
		add_range(0x80, 0xff);
		break;
	case 'H': /* All (except NULL which we can't handle) */
		add_range(0x01, 0xff);
		break;
	case 'a': /* Printable ASCII */
		if (fmt_case)
			add_range(0x20, 0x7e);
		else {
			add_range(0x20, 0x40);
			add_range(0x5b, 0x7e);
		}
		break;
	case 'A': /* All valid non-ASCII chars in codepage */
		switch (pers_opts.internal_enc) {
		case CP437:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP437);
			else
				add_string(CHARS_LOWER_CP437
				           CHARS_LOW_ONLY_CP437);
			add_string(CHARS_DIGITS_CP437
			           CHARS_PUNCTUATION_CP437
			           CHARS_SPECIALS_CP437
			           CHARS_WHITESPACE_CP437);
			break;
		case CP737:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP737);
			else
				add_string(CHARS_LOWER_CP737
				           CHARS_LOW_ONLY_CP737);
			add_string(CHARS_DIGITS_CP737
			           CHARS_PUNCTUATION_CP737
			           CHARS_SPECIALS_CP737
			           CHARS_WHITESPACE_CP737);
			break;
		case CP850:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP850);
			else
				add_string(CHARS_LOWER_CP850
				           CHARS_LOW_ONLY_CP850);
			add_string(CHARS_DIGITS_CP850
			           CHARS_PUNCTUATION_CP850
			           CHARS_SPECIALS_CP850
			           CHARS_WHITESPACE_CP850);
			break;
		case CP852:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP852);
			else
				add_string(CHARS_LOWER_CP852
				           CHARS_LOW_ONLY_CP852);
			add_string(CHARS_DIGITS_CP852
			           CHARS_PUNCTUATION_CP852
			           CHARS_SPECIALS_CP852
			           CHARS_WHITESPACE_CP852);
			break;
		case CP858:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP858);
			else
				add_string(CHARS_LOWER_CP858
				           CHARS_LOW_ONLY_CP858);
			add_string(CHARS_DIGITS_CP858
			           CHARS_PUNCTUATION_CP858
			           CHARS_SPECIALS_CP858
			           CHARS_WHITESPACE_CP858);
			break;
		case CP866:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP866);
			else
				add_string(CHARS_LOWER_CP866
				           CHARS_LOW_ONLY_CP866);
			add_string(CHARS_DIGITS_CP866
			           CHARS_PUNCTUATION_CP866
			           CHARS_SPECIALS_CP866
			           CHARS_WHITESPACE_CP866);
			break;
		case CP1250:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP1250);
			else
				add_string(CHARS_LOWER_CP1250
				           CHARS_LOW_ONLY_CP1250);
			add_string(CHARS_DIGITS_CP1250
			           CHARS_PUNCTUATION_CP1250
			           CHARS_SPECIALS_CP1250
			           CHARS_WHITESPACE_CP1250);
			break;
		case CP1251:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP1251);
			else
				add_string(CHARS_LOWER_CP1251
				           CHARS_LOW_ONLY_CP1251);
			add_string(CHARS_DIGITS_CP1251
			           CHARS_PUNCTUATION_CP1251
			           CHARS_SPECIALS_CP1251
			           CHARS_WHITESPACE_CP1251);
			break;
		case CP1252:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP1252);
			else
				add_string(CHARS_LOWER_CP1252
				           CHARS_LOW_ONLY_CP1252);
			add_string(CHARS_DIGITS_CP1252
			           CHARS_PUNCTUATION_CP1252
			           CHARS_SPECIALS_CP1252
			           CHARS_WHITESPACE_CP1252);
			break;
		case CP1253:
			if (fmt_case)
				add_string(CHARS_ALPHA_CP1253);
			else
				add_string(CHARS_LOWER_CP1253
				           CHARS_LOW_ONLY_CP1253);
			add_string(CHARS_DIGITS_CP1253
			           CHARS_PUNCTUATION_CP1253
			           CHARS_SPECIALS_CP1253
			           CHARS_WHITESPACE_CP1253);
			break;
		case ISO_8859_1:
			if (fmt_case)
				add_string(CHARS_ALPHA_ISO_8859_1);
			else
				add_string(CHARS_LOWER_ISO_8859_1
				           CHARS_LOW_ONLY_ISO_8859_1);
			add_string(CHARS_DIGITS_ISO_8859_1
			           CHARS_PUNCTUATION_ISO_8859_1
			           CHARS_SPECIALS_ISO_8859_1
			           CHARS_WHITESPACE_ISO_8859_1);
			break;
		case ISO_8859_2:
			if (fmt_case)
				add_string(CHARS_ALPHA_ISO_8859_2);
			else
				add_string(CHARS_LOWER_ISO_8859_2
				           CHARS_LOW_ONLY_ISO_8859_2);
			add_string(CHARS_DIGITS_ISO_8859_2
			           CHARS_PUNCTUATION_ISO_8859_2
			           CHARS_SPECIALS_ISO_8859_2
			           CHARS_WHITESPACE_ISO_8859_2);
			break;
		case ISO_8859_7:
			if (fmt_case)
				add_string(CHARS_ALPHA_ISO_8859_7);
			else
				add_string(CHARS_LOWER_ISO_8859_7
				           CHARS_LOW_ONLY_ISO_8859_7);
			add_string(CHARS_DIGITS_ISO_8859_7
			           CHARS_PUNCTUATION_ISO_8859_7
			           CHARS_SPECIALS_ISO_8859_7
			           CHARS_WHITESPACE_ISO_8859_7);
			break;
		case ISO_8859_15:
			if (fmt_case)
				add_string(CHARS_ALPHA_ISO_8859_15);
			else
				add_string(CHARS_LOWER_ISO_8859_15
				           CHARS_LOW_ONLY_ISO_8859_15);
			add_string(CHARS_DIGITS_ISO_8859_15
			           CHARS_PUNCTUATION_ISO_8859_15
			           CHARS_SPECIALS_ISO_8859_15
			           CHARS_WHITESPACE_ISO_8859_15);
			break;
		case KOI8_R:
			if (fmt_case)
				add_string(CHARS_ALPHA_KOI8_R);
			else
				add_string(CHARS_LOWER_KOI8_R
				           CHARS_LOW_ONLY_KOI8_R);
			add_string(CHARS_DIGITS_KOI8_R
			           CHARS_PUNCTUATION_KOI8_R
			           CHARS_SPECIALS_KOI8_R
			           CHARS_WHITESPACE_KOI8_R);
			break;
		default:
			add_range(0x80, 0xff);
		}
		break;
/*
 * Note: To add more cases, also append the symbol to string BUILT_IN_CHARSET.
 */
	default:
		if (john_main_process)
			fprintf(stderr, "Can't nest custom placeholder ?%c.\n",
			        p);
		error();
	}

	*o = '\0';
	return out;
}
#undef add_string

/*
 * Expands all non-custom placeholders in string and returns a new resulting
 * string. ?d is expanded to [0123456789] as opposed to [0-9]. If the outer
 * brackets are already given, as in [?d], output is still [0123456789]
 *
 * This function must pass any escaped characters on, as-is (still escaped).
 * It may also have to ADD escapes to ranges produced from eg. ?s.
 */
static char* expand_plhdr(char *string, int fmt_case)
{
	static char out[0x8000];
	unsigned char *s = (unsigned char*)string;
	char *d = out;
	int ab = 0;

	if (!string || !*string)
		return string;

	//fprintf(stderr, "%s(%s)\n", __FUNCTION__, string);
	if (*s != '[') {
		*d++ = '[';
		ab = 1;
	}
	while (*s && d < &out[sizeof(out) - 1]) {
		if (*s == '\\') {
			*d++ = *s++;
			*d++ = *s++;
		} else
		if (*s == '?' && strchr(BUILT_IN_CHARSET, s[1])) {
			char *ps = plhdr2string(s[1], fmt_case);
			while (*ps && d < &out[sizeof(out) - 2]) {
				if (strchr("\\[]?-", ARCH_INDEX(*ps)))
					*d++ = '\\';
				*d++ = *ps++;
			}
			s += 2;
		} else
			*d++ = *s++;
	}
	if (ab)
		*d++ = ']';
	*d = '\0';

#ifdef MASK_DEBUG
	fprintf(stderr, "%s(%s) return: %s\n", __FUNCTION__, string, out);
#endif
	return out;
}

/*
 * valid braces:
 * [abcd], [[[[[abcde], []]abcde]]], [[[ab]cdefr]]
 * invalid braces:
 * [[ab][c], parsed as two separate ranges [[ab] and [c]
 * [[ab][, error, sets parse_ok to 0.
 *
 * This function must pass any escaped characters on, as-is (still escaped).
 */
static void parse_braces(char *mask, parsed_ctx *parsed_mask)
{
	int i, j ,k;
	int cl_br_enc;

	for (i = 0; i < MAX_NUM_MASK_PLHDR; i++) {
		store_cl(i, -1);
		store_op(i, -1);
	}

	j = k = 0;
	while (j < strlen(mask)) {

		for (i = j; i < strlen(mask); i++) {
			if (mask[i] == '\\')
				i++;
			else
			if (mask[i] == '[')
				break;
		}
		if (i < strlen(mask))
		/* store first opening brace for kth placeholder */
			store_op(k, i);

		cl_br_enc = 0;
		for (i++; i < strlen(mask); i++) {
			if (mask[i] == '\\') {
				i++;
				continue;
			}
			if (mask[i] == ']') {
			/* store last closing brace for kth placeholder */
				store_cl(k, i);
				cl_br_enc = 1;
			}
			if (mask[i] == '[' && cl_br_enc)
				break;
		}

		j = i;
		k++;
	}

	parsed_mask->parse_ok = 1;
	for (i = 0; i < MAX_NUM_MASK_PLHDR; i++)
		if ((load_op(i) == -1) ^ (load_cl(i) == -1))
			parsed_mask->parse_ok = 0;
}

/*
 * Stores the valid ? placeholders in a stack_qtn
 * valid:
 * -if outside [] braces and
 * -if ? is immediately followed by the identifier such as
 * ?a for all printable ASCII.
 *
 * This function must pass any escaped characters on, as-is (still escaped).
 */
static void parse_qtn(char *mask, parsed_ctx *parsed_mask)
{
	int i, j, k;

	for (i = 0; i < MAX_NUM_MASK_PLHDR; i++)
		parsed_mask->stack_qtn[i] = -1;

	for (i = 0, k = 0; i < strlen(mask); i++) {
		if (mask[i] == '\\') {
			i++;
			continue;
		}
		else
		if (mask[i] == '?')
		if (i + 1 < strlen(mask))
		if (strchr(BUILT_IN_CHARSET, ARCH_INDEX(mask[i + 1]))) {
			j = 0;
			while (load_op(j) != -1 &&
			       load_cl(j) != -1) {
				if (i > load_op(j) &&
				    i < load_cl(j))
					goto cont;
				j++;
			}
			parsed_mask->stack_qtn[k++] = i;
		}
cont:
		;
	}
}

static int search_stack(parsed_ctx *parsed_mask, int loc)
{
	int t;

	for (t = 0; load_op(t) != -1; t++)
		if (load_op(t) <= loc && load_cl(t) >= loc)
			return load_cl(t);

	for (t = 0; load_qtn(t) != -1; t++)
		if (load_qtn(t) == loc)
			return loc + 1;
	return 0;
}

/*
 * Maps the position of a range in a mask to its actual postion in a key.
 * Offset for wordlist + mask is not taken into account.
 */
static int calc_pos_in_key(const char *mask, parsed_ctx *parsed_mask,
                           int mask_loc)
{
	int i, ret_pos;

	i = ret_pos = 0;
	while (i < mask_loc) {
		int t;
		if (mask[i] == '\\') {
			i++;
			if (i < mask_loc && mask[i] == '\\') {
				i++;
				ret_pos++;
			}
		        continue;
		}
		t = search_stack(parsed_mask, i);
		i = t ? t + 1: i + 1;
		ret_pos++;
	}

	return ret_pos;
}

/*
 * This function will finally remove any escape characters (after honoring
 * them of course, if they protected any of our specials)
 */
static void init_cpu_mask(const char *mask, parsed_ctx *parsed_mask,
                          cpu_mask_context *cpu_mask_ctx, struct db_main *db)
{
	int i, qtn_ctr, op_ctr, cl_ctr;
	char *p;
	int fmt_case = (db->format->params.flags & FMT_CASE);

#define count(i) cpu_mask_ctx->ranges[i].count
#define fill_range() 							\
	if (a > b) {							\
		for (x = a; x >= b; x--)				\
			if (!memchr((const char*)cpu_mask_ctx->		\
			   ranges[i].chars, x, count(i)))		\
				cpu_mask_ctx->ranges[i].		\
				chars[count(i)++] = x;			\
	} else {							\
		for (x = a; x <= b; x++) 				\
			if (!memchr((const char*)cpu_mask_ctx->		\
			    ranges[i].chars, x, count(i)))		\
				cpu_mask_ctx->ranges[i].		\
				chars[count(i)++] = x;			\
	}

/* Safe in non-bracketed if/for: The final ';' comes with the invocation */
#define add_string(string)						\
	for (p = (char*)string; *p; p++)				\
		cpu_mask_ctx->ranges[i].chars[count(i)++] = *p

#define set_range_start()						\
	for (j = 0; j < cpu_mask_ctx->ranges[i].count; j++)		\
			if (cpu_mask_ctx->ranges[i].chars[0] + j !=	\
			    cpu_mask_ctx->ranges[i].chars[j])		\
				break;					\
	if (j == cpu_mask_ctx->ranges[i].count)				\
		cpu_mask_ctx->ranges[i].start =				\
			cpu_mask_ctx->ranges[i].chars[0];

	for (i = 0; i < MAX_NUM_MASK_PLHDR; i++) {
		cpu_mask_ctx->ranges[i].start =
		cpu_mask_ctx->ranges[i].count =
		cpu_mask_ctx->ranges[i].pos =
		cpu_mask_ctx->ranges[i].iter =
		cpu_mask_ctx->active_positions[i] =
		cpu_mask_ctx->ranges[i].offset = 0;
		cpu_mask_ctx->ranges[i].next = MAX_NUM_MASK_PLHDR;
	}
	cpu_mask_ctx->count = cpu_mask_ctx->offset =
	cpu_mask_ctx->cpu_count = 0;
	cpu_mask_ctx->ps1 = MAX_NUM_MASK_PLHDR;

	qtn_ctr = op_ctr = cl_ctr = 0;
	for (i = 0; i < MAX_NUM_MASK_PLHDR; i++) {
		if ((unsigned int)load_op(op_ctr) <
		    (unsigned int)load_qtn(qtn_ctr)) {
#define check_n_insert 						\
	(!memchr((const char*)cpu_mask_ctx->ranges[i].chars,	\
		(int)mask[j], count(i)))			\
		cpu_mask_ctx->ranges[i].chars[count(i)++] = mask[j];

			int j;

			cpu_mask_ctx->
			ranges[i].pos = calc_pos_in_key(mask,
						        parsed_mask,
				                        load_op(op_ctr));

			for (j = load_op(op_ctr) + 1; j < load_cl(cl_ctr);) {
				int a , b;

				if (mask[j] == '\\') {
					j++;
					if (j >= load_cl(cl_ctr)) break;
					if check_n_insert
				}
				else if (mask[j] == '-' &&
				         j + 1 < load_cl(cl_ctr) &&
				         j - 1 > load_op(op_ctr) &&
					 mask[j + 1] != '\\') {
					int x;

/*
 * Remove the character mask[j-1] added in previous iteration, only if it
 * was added.
*/
					if (!memchr((const char*)cpu_mask_ctx->
					    ranges[i].chars, (int)mask[j - 1],
					            count(i)))
						count(i)--;

					a = mask[j - 1];
					b = mask[j + 1];

					fill_range();

					j++;
				}
				else if (mask[j] == '-' &&
				         j + 2 < load_cl(cl_ctr) &&
				         j - 1 > load_op(op_ctr) &&
					 mask[j + 1] == '\\') {
					 int x;

/*
 * Remove the character mask[j-1] added in previous iteration, only if it
 * was added.
*/
					if (!memchr((const char*)cpu_mask_ctx->
					    ranges[i].chars, (int)mask[j - 1],
					            count(i)))
						count(i)--;

					a = mask[j - 1];
					b = mask[j + 2];

					fill_range();

					j += 2;
				}
				else if check_n_insert

				j++;
			}

			set_range_start();

			op_ctr++;
			cl_ctr++;
			cpu_mask_ctx->count++;
#undef check_n_insert
		}
		else if ((unsigned int)load_op(op_ctr) >
		         (unsigned int)load_qtn(qtn_ctr))  {
			int j;

			cpu_mask_ctx->
			ranges[i].pos = calc_pos_in_key(mask,
							parsed_mask,
							load_qtn(qtn_ctr));

			add_string(plhdr2string(mask[load_qtn(qtn_ctr) + 1],
			                        fmt_case));
			set_range_start();

			qtn_ctr++;
			cpu_mask_ctx->count++;
		}
	}
#undef count
#undef swap
#undef fill_range
	for (i = 0; i < cpu_mask_ctx->count - 1; i++) {
		cpu_mask_ctx->ranges[i].next = i + 1;
		cpu_mask_ctx->active_positions[i] = 1;
	}
	cpu_mask_ctx->ranges[i].next = MAX_NUM_MASK_PLHDR;
	cpu_mask_ctx->active_positions[i] = 1;
}

static void save_restore(cpu_mask_context *cpu_mask_ctx, int range_idx, int ch)
{
	static int bckp_range_idx, bckp_next, toggle;

	/* save state */
	if (!ch) {
		bckp_range_idx = range_idx;
		bckp_next = cpu_mask_ctx->ranges[bckp_range_idx].next;
		toggle = 1;
	}
	/* restore state */
	else if (toggle){
		cpu_mask_ctx->ranges[bckp_range_idx].next = bckp_next;
		toggle = 0;
	}
}

/* truncates mask after range idx */
static void truncate_mask(cpu_mask_context *cpu_mask_ctx, int range_idx)
{
	int i;
	if (range_idx < mask_max_skip_loc && mask_max_skip_loc != -1) {
		fprintf(stderr, "Format internal ranges cannot be truncated!\n");
		fprintf(stderr, "Use a bigger key length or non-gpu format.\n");
		error();
	}

	cpu_mask_ctx->ranges[range_idx].next = MAX_NUM_MASK_PLHDR;

	mask_tot_cand = 1;
	cpu_mask_ctx->cpu_count = 0;
	cpu_mask_ctx->ps1 = MAX_NUM_MASK_PLHDR;
	for (i = 0; i <= range_idx; i++)
		if ((int)(cpu_mask_ctx->active_positions[i])) {
			if (!cpu_mask_ctx->cpu_count)
				cpu_mask_ctx->ps1 = i;
			cpu_mask_ctx->cpu_count++;
			mask_tot_cand *= cpu_mask_ctx->ranges[i].count;
		}
}

/*
 * Returns the template of the keys corresponding to the mask.
 */
static char* generate_template_key(char *mask, const char *key, int key_len,
				   parsed_ctx *parsed_mask,
				   cpu_mask_context *cpu_mask_ctx)
{
	int i, k, t, j, l, offset;
	i = 0, k = 0, j = 0, l = 0, offset = 0;

	while (template_key_offsets[l] != -1)
		template_key_offsets[l++] = -1;

	l = 0;
	while (i < strlen(mask)) {
		if ((t = search_stack(parsed_mask, i))) {
			template_key[k++] = '#';
			i = t + 1;
			cpu_mask_ctx->ranges[j++].offset = offset;
		}
		else if (mask[i] == '\\') {
			i++;
			if (i >= strlen(mask)) break;
			template_key[k++] = mask[i++];
		}
		else if (key != NULL && mask[i + 1] == 'w' && mask[i] == '?') {
			template_key_offsets[l++] = k;
			/* Subtract 2 to account for '?w' in mask.*/
			offset += (key_len - 2);
			k += key_len;
			i += 2;
		}
		else
			template_key[k++] = mask[i++];

		if (k >= (unsigned int)max_keylen) {
			save_restore(cpu_mask_ctx, j - 1, 0);
			truncate_mask(cpu_mask_ctx, j - 1);
			k = max_keylen;
			break;
		}
	}

	if (!mask_has_8bit && !(options.flags & FLG_MASK_STACKED)) {
		for (i = 0; i < strlen(template_key); i++)
			if (template_key[i] & 0x80) {
				mask_has_8bit = 1;
				break;
			}

		for (i = 0; !mask_has_8bit && i <= cpu_mask_ctx->count; i++)
		if (cpu_mask_ctx->ranges[i].pos < max_keylen) {
			for (j = 0; j < cpu_mask_ctx->ranges[i].count; j++) {
				if (cpu_mask_ctx->ranges[i].chars[j] & 0x80) {
					mask_has_8bit = 1;
					break;
				}
			}
		}
	}
#ifdef MASK_DEBUG
	fprintf(stderr, "Mask '%s' has%s 8-bit\n", template_key, mask_has_8bit ? "" : " no");
#endif

	template_key[k] = '\0';

	return template_key;
}

/* Handle internal encoding. */
static MAYBE_INLINE char* mask_cp_to_utf8(char *in)
{
	static char out[PLAINTEXT_BUFFER_SIZE + 1];

	if (mask_has_8bit &&
	    (pers_opts.internal_enc != UTF_8 && pers_opts.target_enc == UTF_8))
		return cp_to_utf8_r(in, out, fmt_maxlen);

	return in;
}

static int generate_keys(cpu_mask_context *cpu_mask_ctx,
			  unsigned long long *my_candidates)
{
	int ps1 = MAX_NUM_MASK_PLHDR, ps2 = MAX_NUM_MASK_PLHDR,
	    ps3 = MAX_NUM_MASK_PLHDR, ps4 = MAX_NUM_MASK_PLHDR, ps ;
	int start1, start2, start3, start4;

#define ranges(i) cpu_mask_ctx->ranges[i]

#define process_key(key)						\
	if (ext_filter(template_key))					\
		if ((crk_process_key(mask_cp_to_utf8(template_key))))   \
			return 1;
/*
 * Calculate next state of remaing placeholders, working
 * similar to counters.
 */
#define next_state(ps)							\
	while(1) {							\
		if (ps == MAX_NUM_MASK_PLHDR) goto done;		\
		if ((++(ranges(ps).iter)) == ranges(ps).count) {	\
			ranges(ps).iter = 0;				\
			template_key[ranges(ps).pos + ranges(ps).offset] = \
			ranges(ps).chars[ranges(ps).iter];		\
			ps = ranges(ps).next;				\
		}							\
		else {							\
			template_key[ranges(ps).pos + ranges(ps).offset] = \
			      ranges(ps).chars[ranges(ps).iter];	\
			break;						\
		}							\
	}

#define init_key(ps)							\
	while (ps != MAX_NUM_MASK_PLHDR) {				\
		template_key[ranges(ps).pos + ranges(ps).offset] =	\
		ranges(ps).chars[ranges(ps).iter];			\
		ps = ranges(ps).next;					\
	}

#define iterate_over(ps)						\
	;ranges(ps).iter < ranges(ps).count; ranges(ps).iter++

#define set_template_key(ps, start)					\
	template_key[ranges(ps).pos + ranges(ps).offset] =		\
		start ? start + ranges(ps).iter:			\
		ranges(ps).chars[ranges(ps).iter];

	ps1 = cpu_mask_ctx->ps1;
	ps2 = cpu_mask_ctx->ranges[ps1].next;
	ps3 = cpu_mask_ctx->ranges[ps2].next;
	ps4 = cpu_mask_ctx->ranges[ps3].next;

	if (cpu_mask_ctx->cpu_count < 4) {
		ps = ps1;

		/* Initialize the placeholders */
		init_key(ps);

		while (1) {
			if (options.node_count &&
			    !(options.flags & FLG_MASK_STACKED) &&
			    !(*my_candidates)--)
				goto done;

			process_key(template_key);
			ps = ps1;
			next_state(ps);
		}
	}

	else if (cpu_mask_ctx->cpu_count >= 4) {
		ps = ranges(ps4).next;

	/* Initialize the reaming placeholders other than the first four */
		init_key(ps);

		while (1) {
			start1 = ranges(ps1).start;
			start2 = ranges(ps2).start;
			start3 = ranges(ps3).start;
			start4 = ranges(ps4).start;
			/* Iterate over first three placeholders */
			for (iterate_over(ps4)) {
				set_template_key(ps4, start4);
				for (iterate_over(ps3)) {
					set_template_key(ps3, start3);
					for (iterate_over(ps2)) {
						set_template_key(ps2, start2);
						for (iterate_over(ps1)) {
							if (options.node_count &&
							    !(options.flags & FLG_MASK_STACKED) &&
							    !(*my_candidates)--)
								goto done;
							set_template_key(ps1, start1);
							process_key(template_key);
						}
					ranges(ps1).iter = 0;
					}
				ranges(ps2).iter = 0;
				}
			ranges(ps3).iter = 0;
			}
			ranges(ps4).iter = 0;
			ps = ranges(ps4).next;
			next_state(ps);
		}
	}
done:
	return 0;
#undef ranges
#undef process_key
#undef next_state
#undef init_key
#undef iterate_over
#undef set_template_key
}

/* Skips iteration for postions stored in arr */
static void skip_position(cpu_mask_context *cpu_mask_ctx, int *arr)
{
	int i;

	if (arr != NULL) {
		int k = 0;
		while (arr[k] >= 0 && arr[k] < cpu_mask_ctx->count) {
			int j, i, flag1 = 0, flag2 = 0;
			cpu_mask_ctx->active_positions[arr[k]] = 0;
			cpu_mask_ctx->ranges[arr[k]].next = MAX_NUM_MASK_PLHDR;

			for (j = arr[k] - 1; j >= 0; j--)
				if ((int)(cpu_mask_ctx->active_positions[j])) {
					flag1 = 1;
					break;
				}

			for (i = arr[k] + 1; i < cpu_mask_ctx->count; i++)
				if ((int)(cpu_mask_ctx->active_positions[i])) {
					flag2 = 1;
					break;
				}

			if (flag1)
				cpu_mask_ctx->ranges[j].next =
					flag2?i:MAX_NUM_MASK_PLHDR;
			k++;
		}
	}

	cpu_mask_ctx->cpu_count = 0;
	cpu_mask_ctx->ps1 = MAX_NUM_MASK_PLHDR;
	for (i = 0; i < cpu_mask_ctx->count; i++)
		if ((int)(cpu_mask_ctx->active_positions[i])) {
			if (!cpu_mask_ctx->cpu_count)
				cpu_mask_ctx->ps1 = i;
			cpu_mask_ctx->cpu_count++;
		}
}

static unsigned long long divide_work(cpu_mask_context *cpu_mask_ctx)
{
	unsigned long long offset, my_candidates, total_candidates, ctr;
	int ps;
	double fract;

	fract = (double)(options.node_max - options.node_min + 1) /
		options.node_count;

	offset = 1;
	ps = cpu_mask_ctx->ps1;
	while(ps != MAX_NUM_MASK_PLHDR) {
		if (cpu_mask_ctx->ranges[ps].pos < max_keylen)
			offset *= cpu_mask_ctx->ranges[ps].count;
		ps = cpu_mask_ctx->ranges[ps].next;
	}

	total_candidates = offset;
	offset *= fract;
	my_candidates = offset;
	offset = my_candidates * (options.node_min - 1);

	if (options.node_max == options.node_count)
		my_candidates = total_candidates - offset;

	if (!my_candidates) {
		if (john_main_process)
			fprintf(stderr, "Insufficient work. Cannot distribute "
			        "work among nodes!\n");
		error();
	}

	ctr = 1;
	ps = cpu_mask_ctx->ps1;
	while(ps != MAX_NUM_MASK_PLHDR) {
		cpu_mask_ctx->ranges[ps].iter = (offset / ctr) %
			cpu_mask_ctx->ranges[ps].count;
		ctr *= cpu_mask_ctx->ranges[ps].count;
		ps = cpu_mask_ctx->ranges[ps].next;
	}

	return my_candidates;
}

static double get_progress(void)
{
	double try;
	int num_nodes = options.node_count ? options.node_count : 1;

	emms();

	try = ((unsigned long long)status.cands.hi << 32) + status.cands.lo;

	if (!mask_tot_cand)
		return -1;

	if (cand_length)
		try -= cand_length;

	return 100.0 * try / (double)(mask_tot_cand / num_nodes);
}

void mask_save_state(FILE *file)
{
	int i;

	fprintf(file, "%llu\n", rec_cand + 1);
	fprintf(file, "%d\n", rec_ctx.count);
	fprintf(file, "%d\n", rec_ctx.offset);
	if (options.force_minlength >= 0) {
		fprintf(file, "%d\n", rec_len);
		fprintf(file, "%llu\n", cand_length);
	}
	for (i = 0; i < rec_ctx.count; i++)
		fprintf(file, "%hhu\n", rec_ctx.ranges[i].iter);
}

int mask_restore_state(FILE *file)
{
	int i, d;
	unsigned char uc;
	unsigned long long ull;
	int fail = !(options.flags & FLG_MASK_STACKED);

	if (fscanf(file, "%llu\n", &ull) == 1)
		cand = ull;
	else
		return fail;

	if (fscanf(file, "%d\n", &d) == 1)
		cpu_mask_ctx.count = d;
	else
		return fail;

	if (fscanf(file, "%d\n", &d) == 1)
		cpu_mask_ctx.offset = d;
	else
		return fail;

	if (options.force_minlength >= 0) {
		if (fscanf(file, "%d\n", &d) == 1)
			restored_len = d;
		else
			return fail;
		if (fscanf(file, "%llu\n", &ull) == 1)
			rec_cl = ull;
		/* FIXME: enable the below at 2015-01-01 or later */
		//else
		//	return fail;
	}

	for (i = 0; i < cpu_mask_ctx.count; i++)
	if (fscanf(file, "%hhu\n", &uc) == 1)
		cpu_mask_ctx.ranges[i].iter = uc;
	else
		return fail;
	restored = 0;
	return 0;
}

void mask_fix_state(void)
{
	int i;

	rec_cand = cand;
	rec_ctx.count = cpu_mask_ctx.count;
	rec_ctx.offset = cpu_mask_ctx.offset;
	rec_len = max_keylen;
	for (i = 0; i < rec_ctx.count; i++)
		rec_ctx.ranges[i].iter = cpu_mask_ctx.ranges[i].iter;
}

void remove_slash(char *mask)
{
	int i = 0;
	while (i < strlen(mask)) {
		if (mask[i] == '\\') {
		    int j = i;
		    while(j < strlen(mask)) {
			  mask[j] = mask[j + 1];
			  j++;
		    }
		}
		i++;
	}
}

char *stretch_mask(char *mask, parsed_ctx *parsed_mask)
{
	char *stretched_mask;
	int i, j, k;

	j = strlen(mask);
	stretched_mask = (char*)malloc((options.force_maxlength + 2) * j);

	strncpy(stretched_mask, mask, j);
	k = 0;
	while (k < options.force_maxlength) {
		i = strlen(mask) - 1;
		if (mask[i] == '\\' && i - 1 >= 0) {
			i--;
			if (!k) j--;
		}
		if (mask[i] == '\\') {
			if(!k) j++;
			strncpy(stretched_mask + j, mask + i, 2);
			j += 2;
		}
		else if(mask[i] != ']') {
		 	if (strchr(BUILT_IN_CHARSET, ARCH_INDEX(mask[i])) &&
			    i - 1 >= 0 && mask[i - 1] == '?') {
				strncpy(stretched_mask + j, mask + i - 1, 2);
				j += 2;
			}
			else {
			      stretched_mask[j] = mask[i];
			      j++;
			}
		}
		else if (mask[i] == ']') {
			int l = 0;
			while (parsed_mask->stack_op_br[l] != -1) l++;
			if (parsed_mask->stack_cl_br[l-1] == i) {
				i = parsed_mask->stack_op_br[l-1];
				strcpy(stretched_mask + j, mask + i);
				j += strlen(mask + i);
			}
			else {
			      stretched_mask[j] = mask[i];
			      j++;
			}
		}
		k++;
	}
	stretched_mask[j] = '\0';

	i = 0;
	while (parsed_mask->stack_cl_br[i] != -1) {
		parsed_mask->stack_cl_br[i] = -1;
		parsed_mask->stack_op_br[i++] = -1;
	}
	return stretched_mask;
}

/*
 * Notes about escapes, lists and ranges:
 *
 * Parsing chain:
 * mask -> utf8_to_cp() -> expand_plhdr() -> parse_hex()
 *                      -> parse_braces() -> parse_qtn()
 *
 * "\x41" means literal "A". Hex escaped characters must be passed as-is until
 * parse_hex(). All other escapes should be passed as-is past parse_qtn().
 * Note that de-hex comes after UTF-8 conversion so any 8-bit hex escaped
 * characters will be parsed as the *internal* encoding.
 *
 * Hex characters *can* compose ranges, eg. "\x80-\xff", but can not end up as
 * placeholders. Eg. "\x3fd" ("?d" after de-hex) must be parsed literally as
 * "?d" and not a digits range.
 *
 * Anything else escaped by "\" must be parsed as literal character,
 * including but not limited to:
 *    "\\" means literal "\" with no further meaning
 *    "\?" means literal "?" and must never be parsed as placeholder (but -"-)
 *    "\-" means literal "-" and must never be parsed as range
 *    "\[" means literal "[" and must never start a list range
 *    "\]" means literal "]" and must never end a list range
 *
 */
void mask_init(struct db_main *db, char *unprocessed_mask)
{
	int i;

	fmt_maxlen = db->format->params.plaintext_length;
	max_keylen = options.force_maxlength ?
		options.force_maxlength : fmt_maxlen;

	if ((options.flags & FLG_MASK_STACKED) && max_keylen < 2) {
		if (john_main_process)
			fprintf(stderr,
			        "Too short max-length for hybrid mask\n");
		error();
	}

#ifdef MASK_DEBUG
	fprintf(stderr, "%s(%s) maxlen %d\n", __FUNCTION__, unprocessed_mask,
	        max_keylen);
#endif
	log_event("Proceeding with mask mode");

	/* Load defaults from john.conf */
	if (options.flags & FLG_MASK_STACKED) {
		if (!unprocessed_mask && !(options.mask =
		   cfg_get_param("Mask", NULL, "DefaultHybridMask")))
			options.mask = "";
	} else
		if (!unprocessed_mask && !(options.mask =
		   cfg_get_param("Mask", NULL, "DefaultMask")))
			options.mask = "";

	/* Load defaults for custom placeholders ?1..?9 from john.conf */
	for (i = 0; i < MAX_NUM_CUST_PLHDR; i++) {
		char pl[2] = { '1' + i, 0 };

		if (!options.custom_mask[i] &&
		    !(options.custom_mask[i] = cfg_get_param("Mask", NULL, pl)))
			options.custom_mask[i] = "";
	}

	mask = options.mask;
	template_key = (char*)mem_alloc(0x400);

	/* Handle command-line (or john.conf) masks given in UTF-8 */
	if (pers_opts.input_enc == UTF_8 && pers_opts.internal_enc != UTF_8) {
		if (valid_utf8((UTF8*)mask) > 1)
			utf8_to_cp_r(mask, mask, strlen(mask));
		for (i = 0; i < MAX_NUM_CUST_PLHDR; i++)
		if (valid_utf8((UTF8*)options.custom_mask[i]) > 1)
			utf8_to_cp_r(options.custom_mask[i],
			             options.custom_mask[i],
			             strlen(options.custom_mask[i]));
	}

	/* Expand static placeholders within custom ones */
	for (i = 0; i < MAX_NUM_CUST_PLHDR; i++)
		options.custom_mask[i] =
			str_alloc_copy(expand_plhdr(options.custom_mask[i],
				db->format->params.flags & FMT_CASE));

	/* Finally expand custom placeholders ?1 .. ?9 */
	mask = expand_cplhdr(mask);

	/* De-hexify mask and custom placeholders */
	parse_hex(mask);
	for (i = 0; i < MAX_NUM_CUST_PLHDR; i++)
		parse_hex(options.custom_mask[i]);

#ifdef MASK_DEBUG
	fprintf(stderr, "Custom masks expanded (this is 'mask' when passed to "
	        "parse_braces()):\n%s\n", mask);
#endif

	/* Parse ranges */
	parse_braces(mask, &parsed_mask);

	if (parsed_mask.parse_ok) {
		if (!(options.flags & FLG_MASK_STACKED) &&
		      options.force_maxlength > 0) {
			mask = stretch_mask(mask, &parsed_mask);
			parse_braces(mask, &parsed_mask);
			if (!parsed_mask.parse_ok) {
				if (john_main_process)
					fprintf(stderr, "Parse unsuccessful,"
					 " missing closing"
					 " bracket\n");
				error();
			}

		}
		parse_qtn(mask, &parsed_mask);
	}
	else {
		if (john_main_process)
			fprintf(stderr, "Parsing unsuccessful, missing closing"
			        " bracket\n");
		error();
	}

	i = 0; mask_add_len = 0; mask_num_qw = 0;
	while (i < strlen(mask)) {
		int t;
		if ((t = search_stack(&parsed_mask, i))) {
			mask_add_len++;
			i = t + 1;
		}
		else if (mask[i] == '\\') {
			i+=2;
			mask_add_len++;
		}
		else if (i + 1 < strlen(mask) && mask[i] == '?' &&
		    mask[i + 1] == 'w') {
			mask_num_qw++;
			i += 2;
			if ((options.flags & FLG_MASK_STACKED) &&
			    mask_add_len >= (unsigned int)max_keylen &&
			    mask_num_qw == 1) {
				if (john_main_process)
				fprintf(stderr, "Hybrid mask must contain ?w"
				        " after truncation for max. length\n");
				error();
			}
		}
		else {
			i++;
			mask_add_len++;
		}
	}
	if (options.flags & FLG_MASK_STACKED) {
		mask_has_8bit = 1;
		if (mask_add_len > max_keylen - 1)
			mask_add_len = max_keylen - 1;

		if (mask_num_qw == 0) {
			if (john_main_process)
				fprintf(stderr,
				        "Hybrid mask must contain ?w\n");
			error();
		}
	} else {
		if (mask_add_len > max_keylen)
			mask_add_len = max_keylen;
		else
		if (options.force_maxlength && mask_add_len < max_keylen)
			if (john_main_process)
			fprintf(stderr, "Warning: mask is shorter than "
			        "-max-length parameter\n");

		if (mask_num_qw && john_main_process)
		fprintf(stderr, "Warning: ?w has no special meaning in pure "
		        "mask mode\n");
	}

#ifdef MASK_DEBUG
	fprintf(stderr, "qw %d minlen %d maxlen %d fmt_len %d mask_add_len %d\n", mask_num_qw, options.force_minlength, options.force_maxlength, fmt_maxlen, mask_add_len);
#endif
	/* We decrease these here instead of changing parent modes. */
	if (options.flags & FLG_MASK_STACKED) {
		if (options.force_minlength - mask_add_len >= 0)
			options.force_minlength -= mask_add_len;
		if (options.force_maxlength)
			options.force_maxlength -= mask_add_len;
		if (mask_num_qw) {
			options.force_minlength /= mask_num_qw;
			options.force_maxlength /= mask_num_qw;
		}
#ifdef MASK_DEBUG
		fprintf(stderr, "effective minlen %d maxlen %d fmt_len %d\n",
		        options.force_minlength, options.force_maxlength,
		        fmt_maxlen - mask_add_len);
#endif
	}

	template_key_offsets = (int*)mem_alloc((mask_num_qw + 1) * sizeof(int));

	for (i = 0; i < mask_num_qw + 1; i++)
		template_key_offsets[i] = -1;

#ifdef MASK_DEBUG
	fprintf(stderr, "Custom masks expanded (this is 'mask' when passed to "
	        "init_cpu_mask()):\n%s\n", mask);
#endif
	init_cpu_mask(mask, &parsed_mask, &cpu_mask_ctx, db);

	mask_calc_combination(&cpu_mask_ctx);

/*	fprintf(stderr, "MASK_FMT_INT_PLHDRs:");
	for (i = 0; i < MASK_FMT_INT_PLHDR && mask_skip_ranges; i++)
	fprintf(stderr, "%d ", mask_skip_ranges[i]);
	fprintf(stderr, "\n");*/

	/*
	 * Warning: NULL to be replaced by an array containing information
	 * regarding GPU portion of mask.
	 */
	skip_position(&cpu_mask_ctx, mask_skip_ranges);

	/* If running hybrid (stacked), we let the parent mode distribute */
	if (options.node_count && !(options.flags & FLG_MASK_STACKED))
		cand = divide_work(&cpu_mask_ctx);
	else {
		cand = 1;
		for (i = 0; i < cpu_mask_ctx.count; i++)
			if ((int)(cpu_mask_ctx.active_positions[i]))
			if ((options.flags & FLG_MASK_STACKED) ||
			    cpu_mask_ctx.ranges[i].pos < max_keylen)
				cand *= cpu_mask_ctx.ranges[i].count;
	}
	mask_tot_cand = cand;

	if (!(options.flags & FLG_MASK_STACKED)) {
		status_init(get_progress, 0);

		rec_restore_mode(mask_restore_state);
		rec_init(db, mask_save_state);

		crk_init(db, mask_fix_state, NULL);
	}
}

void mask_done()
{
	MEM_FREE(template_key);
	MEM_FREE(template_key_offsets);
	if (!mask_skip_ranges)
		MEM_FREE(mask_skip_ranges);
	if (!mask_int_cand.int_cand)
		MEM_FREE(mask_int_cand.int_cand);

	if (!(options.flags & FLG_MASK_STACKED)) {
		if (parsed_mask.parse_ok &&
		    options.force_maxlength > 0)
			MEM_FREE(mask);
		// For reporting DONE regardless of rounding errors
		if (!event_abort) {
			int num_nodes = options.node_count ?
				options.node_count : 1;
			mask_tot_cand = num_nodes *
				(((unsigned long long)status.cands.hi << 32) +
				 status.cands.lo);
			cand_length = 0;
		}
		crk_done();

		rec_done(event_abort);
	}
}

int do_mask_crack(const char *key)
{
	int key_len = key ? strlen(key) : 0;
	int i;

#ifdef MASK_DEBUG
	fprintf(stderr, "%s(%s)\n", __FUNCTION__, key);
#endif

	mask_parent_keys++;

	/* If --min-len is used, we iterate max_keylen */
	if (!(options.flags & FLG_MASK_STACKED) &&
	    options.force_minlength >= 0) {
		int template_key_len = -1;
		int max_len = max_keylen;

		mask_cur_len = restored_len ?
			restored_len : options.force_minlength;

		restored_len = 0;

		if (mask_cur_len == 0) {
			char nullstring[PLAINTEXT_BUFFER_SIZE] = { 0 };
			if (crk_process_key(nullstring))
				return 1;
			mask_cur_len++;
		}

		for (i = mask_cur_len; i <= max_len; i++) {
			int j = 0;

			mask_cur_len = max_keylen = i;
			cand_length = rec_cl ? rec_cl :
				((unsigned long long)status.cands.hi << 32) +
				status.cands.lo;
			rec_cl = 0;

			save_restore(&cpu_mask_ctx, 0, 1);
			generate_template_key(mask, key, key_len, &parsed_mask,
		                      &cpu_mask_ctx);

			if (options.node_count &&
			    !(options.flags & FLG_MASK_STACKED) && restored) {
				cand = divide_work(&cpu_mask_ctx);
				restored = 1;
			}

			if (template_key_len == strlen(template_key)) break;
			template_key_len = strlen(template_key);

			while(template_key_offsets[j] != -1) {
				int cpy_len = max_keylen -
					template_key_offsets[j];
				cpy_len = cpy_len > key_len ? key_len : cpy_len;
				memcpy(template_key + template_key_offsets[j++],
				       key, cpy_len);
			}

			if (generate_keys(&cpu_mask_ctx, &cand))
				return 1;
		}
	} else {
		static int old_keylen = -1;

		if (old_keylen != key_len) {
			save_restore(&cpu_mask_ctx, 0, 1);
			generate_template_key(mask, key, key_len, &parsed_mask,
		                      &cpu_mask_ctx);
			old_keylen = key_len;
		}

		i = 0;
		while(template_key_offsets[i] != -1) {
			int cpy_len = max_keylen - template_key_offsets[i];
			cpy_len = cpy_len > key_len ? key_len : cpy_len;
			memcpy(template_key + template_key_offsets[i++], key,
			       cpy_len);
		}

		if (generate_keys(&cpu_mask_ctx, &cand))
			return 1;
	}
	if (!event_abort && (options.flags & FLG_MASK_STACKED))
		crk_fix_state();

	return event_abort;
}
