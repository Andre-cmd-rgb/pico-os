/*
 * Tokens. The lexer works on the source in memory and never allocates.
 */
#pragma once

#include <stdint.h>

enum tok {
	TK_EOF,
	TK_ERROR,
	TK_IDENT,
	TK_INTLIT,
	TK_FLOATLIT,
	TK_STRLIT,
	TK_CHARLIT,

	/* keywords */
	TK_BOOL,
	TK_BREAK,
	TK_CONST,
	TK_CONTINUE,
	TK_ELSE,
	TK_FALSE,
	TK_FILE,
	TK_FLOAT,
	TK_FOR,
	TK_IF,
	TK_INT,
	TK_NULL,
	TK_RETURN,
	TK_STR,
	TK_STRUCT,
	TK_TRUE,
	TK_VOID,
	TK_WHILE,
	TK_CASE,
	TK_DEFAULT,
	TK_ENUM,
	TK_SWITCH,

	/* punctuation */
	TK_LPAREN,
	TK_RPAREN,
	TK_LBRACE,
	TK_RBRACE,
	TK_LBRACKET,
	TK_RBRACKET,
	TK_COMMA,
	TK_SEMI,
	TK_COLON,
	TK_DOT,
	TK_QUESTION,
	TK_PLUS,
	TK_MINUS,
	TK_STAR,
	TK_SLASH,
	TK_PERCENT,
	TK_AMP,
	TK_PIPE,
	TK_CARET,
	TK_TILDE,
	TK_BANG,
	TK_LT,
	TK_GT,
	TK_LE,
	TK_GE,
	TK_EQ,
	TK_NE,
	TK_ANDAND,
	TK_OROR,
	TK_SHL,
	TK_SHR,
	TK_ASSIGN,
	TK_PLUSEQ,
	TK_MINUSEQ,
	TK_STAREQ,
	TK_SLASHEQ,
	TK_PERCENTEQ,
	TK_AMPEQ,
	TK_PIPEEQ,
	TK_CARETEQ,
	TK_SHLEQ,
	TK_SHREQ,
	TK_INC,
	TK_DEC,
};

struct token {
	uint8_t		kind;
	uint32_t	pos;
	uint32_t	len;
	uint32_t	line;
	uint32_t	col;
	union {
		int32_t		 i;
		float		 f;
		const char	*msg;	/* TK_ERROR */
	} v;
};

struct lexer {
	const char	*src;
	uint32_t	 len;
	uint32_t	 pos;
	uint32_t	 line;
	uint32_t	 line_start;
};

void	lex_init(struct lexer *lx, const char *src, uint32_t len);
void	lex_next(struct lexer *lx, struct token *t);
const char *tok_name(int kind);

/*
 * Decode a string or character literal already accepted by the lexer.
 * Writes at most t->len bytes to out; returns the length.
 */
uint32_t lex_decode(const char *src, const struct token *t, char *out);
