
/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "od_macro.h"
#include "od_list.h"
#include "od_lex.h"

void od_lexinit(od_lex_t *lex)
{
	memset(lex, 0, sizeof(*lex));
	od_listinit(&lex->stack);
	od_listinit(&lex->list);
}

void od_lexopen(od_lex_t *lex, od_keyword_t *list, char *buf, int size)
{
	lex->buf      = buf;
	lex->size     = size;
	lex->keywords = list;
}

void od_lexfree(od_lex_t *lex)
{
	od_list_t *i, *n;
	od_listforeach_safe(&lex->list, i, n) {
		od_token_t *tk = od_container_of(i, od_token_t, link_alloc);
		if (tk->id == OD_LSTRING ||
		    tk->id == OD_LID) {
			free(tk->v.string);
		}
		free(tk);
	}
	if (lex->buf)
		free(lex->buf);
	if (lex->error)
		free(lex->error);
}

char *od_lexname_of(od_lex_t *lex, int id)
{
	switch (id) {
	case OD_LEOF:    return "eof";
	case OD_LERROR:  return "error";
	case OD_LNUMBER: return "number";
	case OD_LSTRING: return "string";
	case OD_LID:     return "name";
	case OD_LPUNCT:  return "punctuation";
	}
	int i;
	for (i = 0 ; lex->keywords[i].name ; i++)
		if (lex->keywords[i].id == id)
			return lex->keywords[i].name;
	return NULL;
}

void od_lexpush(od_lex_t *lex, od_token_t *tk)
{
	od_listpush(&lex->stack, &tk->link);
	lex->count++;
}

static int
od_lexerror(od_lex_t *lex, const char *fmt, ...)
{
	if (fmt == NULL)
		return OD_LEOF;
	if (lex->error)
		free(lex->error);
	char msg[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	lex->error = strdup(msg);
	return OD_LERROR;
}

static inline od_token_t*
od_lexalloc(od_lex_t *lex, int id, int line)
{
	od_token_t *tk = malloc(sizeof(od_token_t));
	if (tk == NULL)
		return NULL;
	memset(tk, 0, sizeof(*tk));
	tk->id   = id;
	tk->line = line;
	od_listinit(&tk->link);
	od_listinit(&tk->link_alloc);
	od_listappend(&lex->list, &tk->link_alloc);
	return tk;
}

static inline int
od_lexnext(od_lex_t *lex) {
	if (lex->pos == lex->size)
		return 0;
	lex->pos++;
	return 1;
}

static inline uint8_t
od_lexchar(od_lex_t *lex) {
	return *(lex->buf + lex->pos);
}

static inline od_token_t*
od_lexpop_stack(od_lex_t *lex)
{
	if (lex->count == 0)
		return NULL;
	od_token_t *tk = od_container_of(lex->stack.next, od_token_t, link);
	od_listunlink(&tk->link);
	lex->count--;
	return tk;
}

int od_lexpop(od_lex_t *lex, od_token_t **result)
{
	/* stack first */
	if (lex->count) {
		*result = od_lexpop_stack(lex);
		if ((*result)->id == OD_LPUNCT)
			return (*result)->v.num;
		return (*result)->id;
	}

	/* skip white-spaces and comments */
	unsigned char ch;
	while (1) {
		if (lex->pos == lex->size) {
			*result = od_lexalloc(lex, OD_LEOF, lex->line);
			if (*result == NULL)
				return od_lexerror(lex, "memory allocation error");
			return OD_LEOF;
		}
		ch = od_lexchar(lex);
		if (isspace(ch)) {
			if (ch == '\n') {
				if (((lex->pos + 1) != lex->size))
					lex->line++;
			}
			od_lexnext(lex);
			continue;
		}
		if (ch == '#') {
			while (1) {
				if (lex->pos == lex->size) {
					*result = od_lexalloc(lex, OD_LEOF, lex->line);
					if (*result == NULL)
						return od_lexerror(lex, "memory allocation error");
					return OD_LEOF;
				}
				od_lexnext(lex);
				ch = od_lexchar(lex);
				if (ch == '\n') {
					if (((lex->pos + 1) != lex->size))
						lex->line++;
					od_lexnext(lex);
					break;
				}
			}
			continue;
		}
		break;
	}

	/* token position */
	int line  = lex->line;
	int start = lex->pos;
	int size  = 0;

	/* string */
	ch = od_lexchar(lex);
	if (ch == '\"') {
		start++;
		while (1) {
			if (od_lexnext(lex) == 0)
				return od_lexerror(lex, "bad string definition");
			ch = od_lexchar(lex);
			if (ch == '\"')
				break;
			if (ch == '\n')
				return od_lexerror(lex, "bad string definition");
		}
		size = lex->pos - start;
		od_lexnext(lex);
		*result = od_lexalloc(lex, OD_LSTRING, line);
		if (*result == NULL)
			return od_lexerror(lex, "memory allocation error");
		od_token_t *tk = *result;
		tk->v.string = malloc(size + 1);
		if (tk->v.string == NULL)
			return od_lexerror(lex, "memory allocation error");
		memcpy(tk->v.string, lex->buf + start, size);
		tk->v.string[size] = 0;
		return OD_LSTRING;
	}

	/* punctuation */
	if (ispunct(ch) && ch != '_') {
		od_lexnext(lex);
		*result = od_lexalloc(lex, OD_LPUNCT, line);
		if (*result == NULL)
			return od_lexerror(lex, "memory allocation error");
		(*result)->v.num = ch;
		return ch;
	}

	/* numeric value */
	if (isdigit(ch)) {
		int64_t num = 0;
		while (1) {
			ch = od_lexchar(lex);
			if (isdigit(ch))
				num = (num * 10) + ch - '0';
			else
				break;
			if (od_lexnext(lex) == 0)
				break;
		}
		*result = od_lexalloc(lex, OD_LNUMBER, line);
		if (*result == NULL)
			return od_lexerror(lex, "memory allocation error");
		(*result)->v.num = num;
		return OD_LNUMBER;
	}

	/* skip to the end of lexem */
	while (1) {
		ch = od_lexchar(lex);
		if (isspace(ch) || (ispunct(ch) && ch != '_'))
			break;
		if (od_lexnext(lex) == 0)
			break;
	}
	size = lex->pos - start;

	/* match keyword */
	int i = 0;
	for ( ; lex->keywords[i].name ; i++) {
		if (lex->keywords[i].size != size)
			continue;
		if (strncasecmp(lex->keywords[i].name, lex->buf + start, size) == 0) {
			*result = od_lexalloc(lex, lex->keywords[i].id, line);
			if (*result == NULL)
				return od_lexerror(lex, "memory allocation error");
			return lex->keywords[i].id;
		}
	}

	/* identification */
	*result = od_lexalloc(lex, OD_LID, line);
	if (*result == NULL)
		return od_lexerror(lex, "memory allocation error");
	od_token_t *tk = *result;
	tk->v.string = malloc(size + 1);
	if (tk->v.string == NULL)
		return od_lexerror(lex, "memory allocation error");
	memcpy(tk->v.string, lex->buf + start, size);
	tk->v.string[size] = 0;
	return OD_LID;
}
