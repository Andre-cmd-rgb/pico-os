/*
 * Lexer: source text to tokens.
 */
#include <stdlib.h>
#include <string.h>

#include "lex.h"

static const struct {
	const char	*word;
	uint8_t		 kind;
} keywords[] = {
	{ "File", TK_FILE },
	{ "bool", TK_BOOL },
	{ "break", TK_BREAK },
	{ "case", TK_CASE },
	{ "const", TK_CONST },
	{ "continue", TK_CONTINUE },
	{ "default", TK_DEFAULT },
	{ "else", TK_ELSE },
	{ "enum", TK_ENUM },
	{ "false", TK_FALSE },
	{ "float", TK_FLOAT },
	{ "for", TK_FOR },
	{ "if", TK_IF },
	{ "int", TK_INT },
	{ "null", TK_NULL },
	{ "return", TK_RETURN },
	{ "str", TK_STR },
	{ "struct", TK_STRUCT },
	{ "switch", TK_SWITCH },
	{ "true", TK_TRUE },
	{ "void", TK_VOID },
	{ "while", TK_WHILE },
};

void lex_init(struct lexer *lx, const char *src, uint32_t len)
{
	lx->src = src;
	lx->len = len;
	lx->pos = 0;
	lx->line = 1;
	lx->line_start = 0;
}

static int peekc(const struct lexer *lx, uint32_t ahead)
{
	return lx->pos + ahead < lx->len ? (unsigned char)lx->src[lx->pos + ahead] : -1;
}

static bool is_ident_start(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(int c)
{
	return c >= '0' && c <= '9';
}

static int hex_value(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void newline(struct lexer *lx)
{
	lx->line++;
	lx->line_start = lx->pos;
}

/* Whitespace and comments. Returns an error message for an open comment. */
static const char *skip_space(struct lexer *lx)
{
	for (;;) {
		int c = peekc(lx, 0);

		if (c == '\n') {
			lx->pos++;
			newline(lx);
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
			lx->pos++;
		} else if (c == '/' && peekc(lx, 1) == '/') {
			while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
				lx->pos++;
		} else if (c == '/' && peekc(lx, 1) == '*') {
			lx->pos += 2;
			for (;;) {
				if (lx->pos >= lx->len)
					return "unterminated comment";
				if (lx->src[lx->pos] == '*' && peekc(lx, 1) == '/') {
					lx->pos += 2;
					break;
				}
				if (lx->src[lx->pos++] == '\n')
					newline(lx);
			}
		} else {
			return NULL;
		}
	}
}

/*
 * One escape sequence or character starting at src[*i]. Returns its value
 * (a code point for \u and for UTF-8 characters, else a byte), sets *utf8
 * when the value must be encoded, or returns -1 with *err set.
 */
static int32_t scan_char(const char *src, uint32_t end, uint32_t *i, bool *utf8, const char **err)
{
	int c = (unsigned char)src[*i];

	*utf8 = false;
	if (c != '\\') {
		(*i)++;
		return c;
	}
	if (*i + 1 >= end) {
		*err = "unfinished escape sequence";
		return -1;
	}
	c = (unsigned char)src[*i + 1];
	*i += 2;
	switch (c) {
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case '0': return 0;
	case 'e': return 0x1b;
	case '\\': return '\\';
	case '"': return '"';
	case '\'': return '\'';
	case 'x': {
		int h1 = *i < end ? hex_value((unsigned char)src[*i]) : -1;
		int h2 = *i + 1 < end ? hex_value((unsigned char)src[*i + 1]) : -1;
		if (h1 < 0 || h2 < 0) {
			*err = "\\x needs two hex digits";
			return -1;
		}
		*i += 2;
		return h1 << 4 | h2;
	}
	case 'u': {
		int32_t cp = 0;
		int digits = 0;
		if (*i >= end || src[*i] != '{') {
			*err = "expected \\u{hex digits}";
			return -1;
		}
		(*i)++;
		while (*i < end && hex_value((unsigned char)src[*i]) >= 0 && digits < 6) {
			cp = cp << 4 | hex_value((unsigned char)src[*i]);
			(*i)++;
			digits++;
		}
		if (!digits || *i >= end || src[*i] != '}' || cp > 0x10ffff) {
			*err = "bad \\u{...} escape";
			return -1;
		}
		(*i)++;
		*utf8 = true;
		return cp;
	}
	}
	*err = "unknown escape sequence";
	return -1;
}

static uint32_t utf8_encode(int32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = 0xc0 | cp >> 6;
		out[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = 0xe0 | cp >> 12;
		out[1] = 0x80 | ((cp >> 6) & 0x3f);
		out[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | cp >> 18;
	out[1] = 0x80 | ((cp >> 12) & 0x3f);
	out[2] = 0x80 | ((cp >> 6) & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	return 4;
}

/* Decode one UTF-8 sequence for a character literal; -1 if malformed. */
static int32_t utf8_decode(const char *s, uint32_t n, uint32_t *used)
{
	unsigned char c = s[0];
	int32_t cp;
	uint32_t len;

	if (c < 0x80) {
		*used = 1;
		return c;
	}
	if ((c & 0xe0) == 0xc0) {
		cp = c & 0x1f;
		len = 2;
	} else if ((c & 0xf0) == 0xe0) {
		cp = c & 0x0f;
		len = 3;
	} else if ((c & 0xf8) == 0xf0) {
		cp = c & 0x07;
		len = 4;
	} else {
		return -1;
	}
	if (len > n)
		return -1;
	for (uint32_t k = 1; k < len; k++) {
		if (((unsigned char)s[k] & 0xc0) != 0x80)
			return -1;
		cp = cp << 6 | ((unsigned char)s[k] & 0x3f);
	}
	*used = len;
	return cp;
}

static void lex_number(struct lexer *lx, struct token *t)
{
	const char *s = lx->src;
	uint32_t start = lx->pos;
	uint64_t v = 0;
	bool too_big = false;

	if (s[start] == '0' && (peekc(lx, 1) == 'x' || peekc(lx, 1) == 'X' ||
				peekc(lx, 1) == 'b' || peekc(lx, 1) == 'B')) {
		int base = (peekc(lx, 1) | 0x20) == 'x' ? 16 : 2;
		lx->pos += 2;
		uint32_t digits = 0;
		for (;;) {
			int d = hex_value(peekc(lx, 0));
			if (d < 0 || d >= base)
				break;
			v = v * base + d;
			too_big |= v > 0xffffffffu;
			lx->pos++;
			digits++;
		}
		if (!digits) {
			t->kind = TK_ERROR;
			t->v.msg = base == 16 ? "hex number needs digits" : "binary number needs digits";
		} else if (too_big) {
			t->kind = TK_ERROR;
			t->v.msg = "number does not fit in 32 bits";
		} else {
			t->kind = TK_INTLIT;
			t->v.i = (int32_t)(uint32_t)v;
		}
		if (is_ident_start(peekc(lx, 0)) || is_digit(peekc(lx, 0))) {
			t->kind = TK_ERROR;
			t->v.msg = "bad digit in number";
			while (is_ident_start(peekc(lx, 0)) || is_digit(peekc(lx, 0)))
				lx->pos++;
		}
		return;
	}

	bool is_float = false;
	while (is_digit(peekc(lx, 0))) {
		v = v * 10 + (s[lx->pos] - '0');
		too_big |= v > 0xffffffffu;
		lx->pos++;
	}
	if (peekc(lx, 0) == '.' && is_digit(peekc(lx, 1))) {
		is_float = true;
		lx->pos++;
		while (is_digit(peekc(lx, 0)))
			lx->pos++;
	}
	if (peekc(lx, 0) == 'e' || peekc(lx, 0) == 'E') {
		uint32_t k = 1;
		if (peekc(lx, k) == '+' || peekc(lx, k) == '-')
			k++;
		if (is_digit(peekc(lx, k))) {
			is_float = true;
			lx->pos += k;
			while (is_digit(peekc(lx, 0)))
				lx->pos++;
		}
	}
	if (is_ident_start(peekc(lx, 0))) {
		t->kind = TK_ERROR;
		t->v.msg = "bad character in number";
		while (is_ident_start(peekc(lx, 0)) || is_digit(peekc(lx, 0)))
			lx->pos++;
		return;
	}
	if (is_float) {
		char buf[64];
		uint32_t n = lx->pos - start;
		if (n >= sizeof(buf)) {
			t->kind = TK_ERROR;
			t->v.msg = "number too long";
			return;
		}
		memcpy(buf, s + start, n);
		buf[n] = '\0';
		t->kind = TK_FLOATLIT;
		t->v.f = strtof(buf, NULL);
		return;
	}
	if (too_big) {
		t->kind = TK_ERROR;
		t->v.msg = "number does not fit in 32 bits";
		return;
	}
	t->kind = TK_INTLIT;
	t->v.i = (int32_t)(uint32_t)v;
}

static void lex_quoted(struct lexer *lx, struct token *t, char quote)
{
	const char *s = lx->src;
	const char *err = NULL;
	int32_t value = 0;
	uint32_t chars = 0;

	lx->pos++;
	for (;;) {
		if (lx->pos >= lx->len || s[lx->pos] == '\n') {
			t->kind = TK_ERROR;
			t->v.msg = quote == '"' ? "unterminated string" : "unterminated character literal";
			return;
		}
		if (s[lx->pos] == quote)
			break;
		bool utf8;
		uint32_t i = lx->pos;
		int32_t c = scan_char(s, lx->len, &i, &utf8, &err);
		if (c < 0) {
			t->kind = TK_ERROR;
			t->v.msg = err;
			/* skip to the closing quote so one mistake gives one error */
			while (lx->pos < lx->len && s[lx->pos] != quote && s[lx->pos] != '\n')
				lx->pos++;
			if (lx->pos < lx->len && s[lx->pos] == quote)
				lx->pos++;
			return;
		}
		if (quote == '\'' && !utf8 && c >= 0x80 && s[lx->pos] != '\\') {
			uint32_t used;
			c = utf8_decode(s + lx->pos, lx->len - lx->pos, &used);
			if (c < 0) {
				t->kind = TK_ERROR;
				t->v.msg = "bad UTF-8 in character literal";
				lx->pos++;
				return;
			}
			i = lx->pos + used;
		}
		value = c;
		chars++;
		lx->pos = i;
	}
	lx->pos++;
	if (quote == '"') {
		t->kind = TK_STRLIT;
		return;
	}
	if (chars != 1) {
		t->kind = TK_ERROR;
		t->v.msg = chars ? "character literal holds more than one character" : "empty character literal";
		return;
	}
	t->kind = TK_CHARLIT;
	t->v.i = value;
}

void lex_next(struct lexer *lx, struct token *t)
{
	const char *err = skip_space(lx);
	const char *s = lx->src;

	t->pos = lx->pos;
	t->line = lx->line;
	t->col = lx->pos - lx->line_start + 1;
	t->len = 0;
	if (err) {
		t->kind = TK_ERROR;
		t->v.msg = err;
		lx->pos = lx->len;
		return;
	}
	if (lx->pos >= lx->len) {
		t->kind = TK_EOF;
		return;
	}

	int c = (unsigned char)s[lx->pos];
	if (is_ident_start(c)) {
		while (is_ident_start(peekc(lx, 0)) || is_digit(peekc(lx, 0)))
			lx->pos++;
		t->len = lx->pos - t->pos;
		t->kind = TK_IDENT;
		for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
			if (strlen(keywords[k].word) == t->len && !memcmp(keywords[k].word, s + t->pos, t->len)) {
				t->kind = keywords[k].kind;
				break;
			}
		}
		return;
	}
	if (is_digit(c)) {
		lex_number(lx, t);
		t->len = lx->pos - t->pos;
		return;
	}
	if (c == '"' || c == '\'') {
		lex_quoted(lx, t, c);
		t->len = lx->pos - t->pos;
		return;
	}

	int c1 = peekc(lx, 1), c2 = peekc(lx, 2);
	int kind, n = 1;

	switch (c) {
	case '(': kind = TK_LPAREN; break;
	case ')': kind = TK_RPAREN; break;
	case '{': kind = TK_LBRACE; break;
	case '}': kind = TK_RBRACE; break;
	case '[': kind = TK_LBRACKET; break;
	case ']': kind = TK_RBRACKET; break;
	case ',': kind = TK_COMMA; break;
	case ';': kind = TK_SEMI; break;
	case ':': kind = TK_COLON; break;
	case '.': kind = TK_DOT; break;
	case '?': kind = TK_QUESTION; break;
	case '~': kind = TK_TILDE; break;
	case '+':
		kind = c1 == '+' ? (n = 2, TK_INC) : c1 == '=' ? (n = 2, TK_PLUSEQ) : TK_PLUS;
		break;
	case '-':
		kind = c1 == '-' ? (n = 2, TK_DEC) : c1 == '=' ? (n = 2, TK_MINUSEQ) : TK_MINUS;
		break;
	case '*': kind = c1 == '=' ? (n = 2, TK_STAREQ) : TK_STAR; break;
	case '/': kind = c1 == '=' ? (n = 2, TK_SLASHEQ) : TK_SLASH; break;
	case '%': kind = c1 == '=' ? (n = 2, TK_PERCENTEQ) : TK_PERCENT; break;
	case '^': kind = c1 == '=' ? (n = 2, TK_CARETEQ) : TK_CARET; break;
	case '!': kind = c1 == '=' ? (n = 2, TK_NE) : TK_BANG; break;
	case '=': kind = c1 == '=' ? (n = 2, TK_EQ) : TK_ASSIGN; break;
	case '&':
		kind = c1 == '&' ? (n = 2, TK_ANDAND) : c1 == '=' ? (n = 2, TK_AMPEQ) : TK_AMP;
		break;
	case '|':
		kind = c1 == '|' ? (n = 2, TK_OROR) : c1 == '=' ? (n = 2, TK_PIPEEQ) : TK_PIPE;
		break;
	case '<':
		if (c1 == '<')
			kind = c2 == '=' ? (n = 3, TK_SHLEQ) : (n = 2, TK_SHL);
		else
			kind = c1 == '=' ? (n = 2, TK_LE) : TK_LT;
		break;
	case '>':
		if (c1 == '>')
			kind = c2 == '=' ? (n = 3, TK_SHREQ) : (n = 2, TK_SHR);
		else
			kind = c1 == '=' ? (n = 2, TK_GE) : TK_GT;
		break;
	default:
		kind = TK_ERROR;
		t->v.msg = c >= 0x80 ? "non-ASCII character outside a string" : "unexpected character";
		/* skip a whole UTF-8 sequence */
		while (lx->pos + n < lx->len && ((unsigned char)s[lx->pos + n] & 0xc0) == 0x80)
			n++;
		break;
	}
	lx->pos += n;
	t->kind = kind;
	t->len = n;
}

uint32_t lex_decode(const char *src, const struct token *t, char *out)
{
	uint32_t i = t->pos + 1, end = t->pos + t->len - 1, n = 0;
	const char *err;
	bool utf8;

	while (i < end) {
		int32_t c = scan_char(src, end, &i, &utf8, &err);
		if (c < 0)
			break;
		if (utf8)
			n += utf8_encode(c, out + n);
		else
			out[n++] = c;
	}
	return n;
}

const char *tok_name(int kind)
{
	static const char *const names[] = {
		[TK_EOF] = "end of file", [TK_ERROR] = "error", [TK_IDENT] = "name",
		[TK_INTLIT] = "number", [TK_FLOATLIT] = "number", [TK_STRLIT] = "string",
		[TK_CHARLIT] = "character", [TK_BOOL] = "'bool'", [TK_BREAK] = "'break'",
		[TK_CONST] = "'const'", [TK_CONTINUE] = "'continue'", [TK_ELSE] = "'else'",
		[TK_FALSE] = "'false'", [TK_FILE] = "'File'", [TK_FLOAT] = "'float'",
		[TK_FOR] = "'for'", [TK_IF] = "'if'", [TK_INT] = "'int'", [TK_NULL] = "'null'",
		[TK_RETURN] = "'return'", [TK_STR] = "'str'", [TK_STRUCT] = "'struct'",
		[TK_TRUE] = "'true'", [TK_VOID] = "'void'", [TK_WHILE] = "'while'",
		[TK_CASE] = "'case'", [TK_DEFAULT] = "'default'", [TK_ENUM] = "'enum'",
		[TK_SWITCH] = "'switch'",
		[TK_LPAREN] = "'('", [TK_RPAREN] = "')'", [TK_LBRACE] = "'{'",
		[TK_RBRACE] = "'}'", [TK_LBRACKET] = "'['", [TK_RBRACKET] = "']'",
		[TK_COMMA] = "','", [TK_SEMI] = "';'", [TK_COLON] = "':'", [TK_DOT] = "'.'",
		[TK_QUESTION] = "'?'", [TK_PLUS] = "'+'", [TK_MINUS] = "'-'", [TK_STAR] = "'*'",
		[TK_SLASH] = "'/'", [TK_PERCENT] = "'%'", [TK_AMP] = "'&'", [TK_PIPE] = "'|'",
		[TK_CARET] = "'^'", [TK_TILDE] = "'~'", [TK_BANG] = "'!'", [TK_LT] = "'<'",
		[TK_GT] = "'>'", [TK_LE] = "'<='", [TK_GE] = "'>='", [TK_EQ] = "'=='",
		[TK_NE] = "'!='", [TK_ANDAND] = "'&&'", [TK_OROR] = "'||'", [TK_SHL] = "'<<'",
		[TK_SHR] = "'>>'", [TK_ASSIGN] = "'='", [TK_PLUSEQ] = "'+='",
		[TK_MINUSEQ] = "'-='", [TK_STAREQ] = "'*='", [TK_SLASHEQ] = "'/='",
		[TK_PERCENTEQ] = "'%='", [TK_AMPEQ] = "'&='", [TK_PIPEEQ] = "'|='",
		[TK_CARETEQ] = "'^='", [TK_SHLEQ] = "'<<='", [TK_SHREQ] = "'>>='",
		[TK_INC] = "'++'", [TK_DEC] = "'--'",
	};

	return kind >= 0 && kind < (int)(sizeof(names) / sizeof(names[0])) && names[kind] ? names[kind] : "token";
}
