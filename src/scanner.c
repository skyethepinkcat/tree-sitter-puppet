#include "tree_sitter/parser.h"
#include <string.h>
#include <stdlib.h>

typedef enum {
  HEREDOC_START,
  HEREDOC_BODY,
  HEREDOC_END,
} TokenType;

typedef struct {
  char delimiter[256];
  uint32_t delimiter_len;
  bool in_heredoc;
} Scanner;

static void scanner_init(Scanner *s) {
  s->delimiter_len = 0;
  s->delimiter[0] = '\0';
  s->in_heredoc = false;
}

static bool is_delimiter_char(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// Advance through optional whitespace, optional |, optional -, then try to
// match the stored delimiter followed by EOL. Returns true on match (lexer
// position is past the delimiter + EOL); false if no match (lexer advanced
// some chars but caller should discard by consuming the rest of the line).
static bool try_match_end_tag(Scanner *s, TSLexer *lexer) {
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, false);
  }
  if (lexer->lookahead == '|') {
    lexer->advance(lexer, false);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      lexer->advance(lexer, false);
    }
  }
  if (lexer->lookahead == '-') {
    lexer->advance(lexer, false);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      lexer->advance(lexer, false);
    }
  }
  for (uint32_t i = 0; i < s->delimiter_len; i++) {
    if ((char)lexer->lookahead != s->delimiter[i]) return false;
    lexer->advance(lexer, false);
  }
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, false);
  }
  return lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->lookahead == '\0';
}

static bool scan_heredoc_start(Scanner *s, TSLexer *lexer) {
  // Skip whitespace — extras are not pre-skipped before external scanner is called
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\n' || lexer->lookahead == '\r') {
    lexer->advance(lexer, true);
  }

  if (lexer->lookahead != '@') return false;
  lexer->advance(lexer, false);
  if (lexer->lookahead != '(') return false;
  lexer->advance(lexer, false);

  s->delimiter_len = 0;

  if (lexer->lookahead == '"') {
    // Double-quoted: @("TAG"/flags?)
    lexer->advance(lexer, false);
    while (lexer->lookahead != '\0' && lexer->lookahead != '"') {
      if (lexer->lookahead == '\n' || lexer->lookahead == '\r') return false;
      if (s->delimiter_len >= 255) return false;
      s->delimiter[s->delimiter_len++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
    }
    if (lexer->lookahead != '"') return false;
    lexer->advance(lexer, false);
  } else if (is_delimiter_char(lexer->lookahead)) {
    // Unquoted: @(TAG/flags?) — literal, no interpolation
    while (is_delimiter_char(lexer->lookahead)) {
      if (s->delimiter_len >= 255) return false;
      s->delimiter[s->delimiter_len++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
    }
  } else {
    return false;
  }

  s->delimiter[s->delimiter_len] = '\0';

  // Optional /flags
  if (lexer->lookahead == '/') {
    lexer->advance(lexer, false);
    while ((lexer->lookahead >= 'a' && lexer->lookahead <= 'z') ||
           (lexer->lookahead >= 'A' && lexer->lookahead <= 'Z')) {
      lexer->advance(lexer, false);
    }
  }

  if (lexer->lookahead != ')') return false;
  lexer->advance(lexer, false);

  s->in_heredoc = true;
  lexer->result_symbol = HEREDOC_START;
  return true;
}

// HEREDOC_BODY is non-optional: it always consumes the newline that ends the
// @(...) opening line, plus zero or more content lines before the end tag.
// mark_end is placed just before the end-tag line so HEREDOC_END starts there.
//
// Grammar invariant: HEREDOC_BODY is only valid after HEREDOC_START, and
// HEREDOC_END is only valid after HEREDOC_BODY. This means each scanner call
// has exactly one external token in valid_symbols, avoiding position conflicts
// between sub-scanner calls within a single outer scan invocation.
static bool scan_heredoc_body(Scanner *s, TSLexer *lexer) {
  if (!s->in_heredoc) return false;

  // Consume the newline that ends the @(...) line.
  if (lexer->lookahead == '\r') lexer->advance(lexer, false);
  if (lexer->lookahead == '\n') {
    lexer->advance(lexer, false);
  } else {
    return false;
  }

  // Process lines: for each line, try to match the end tag.
  // If end tag found: set mark_end before it, return body.
  // If not: consume the line and continue.
  while (true) {
    // Set mark_end here — if current line is the end tag, body ends before it.
    lexer->mark_end(lexer);

    // Try to match end tag on the current line.
    // try_match_end_tag advances into the line regardless of outcome.
    if (try_match_end_tag(s, lexer)) {
      // End tag found. mark_end was set before this line.
      lexer->result_symbol = HEREDOC_BODY;
      return true;
    }

    // Not an end tag. Consume the rest of this line.
    while (lexer->lookahead != '\n' && lexer->lookahead != '\r' && lexer->lookahead != '\0') {
      lexer->advance(lexer, false);
    }
    if (lexer->lookahead == '\r') lexer->advance(lexer, false);
    if (lexer->lookahead == '\n') lexer->advance(lexer, false);

    if (lexer->lookahead == '\0') {
      // EOF without end tag — consume everything as body
      lexer->mark_end(lexer);
      lexer->result_symbol = HEREDOC_BODY;
      return true;
    }
  }
}

// HEREDOC_END is called after HEREDOC_BODY. Body set mark_end just before the
// end-tag line, so the lexer starts directly at the delimiter (no leading \n).
static bool scan_heredoc_end(Scanner *s, TSLexer *lexer) {
  if (!s->in_heredoc) return false;

  // Skip leading whitespace on the delimiter line
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, false);
  }
  // Optional | + surrounding whitespace
  if (lexer->lookahead == '|') {
    lexer->advance(lexer, false);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      lexer->advance(lexer, false);
    }
  }
  // Optional - + surrounding whitespace
  if (lexer->lookahead == '-') {
    lexer->advance(lexer, false);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      lexer->advance(lexer, false);
    }
  }
  // Match delimiter
  for (uint32_t i = 0; i < s->delimiter_len; i++) {
    if ((char)lexer->lookahead != s->delimiter[i]) return false;
    lexer->advance(lexer, false);
  }
  // Consume trailing whitespace and newline
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, false);
  }
  if (lexer->lookahead == '\r') lexer->advance(lexer, false);
  if (lexer->lookahead == '\n') lexer->advance(lexer, false);

  s->in_heredoc = false;
  s->delimiter_len = 0;
  s->delimiter[0] = '\0';
  lexer->result_symbol = HEREDOC_END;
  return true;
}

void *tree_sitter_puppet_external_scanner_create(void) {
  Scanner *s = malloc(sizeof(Scanner));
  scanner_init(s);
  return s;
}

void tree_sitter_puppet_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_puppet_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = payload;
  unsigned len = 0;
  buffer[len++] = s->in_heredoc ? 1 : 0;
  buffer[len++] = (char)(s->delimiter_len & 0xFF);
  buffer[len++] = (char)((s->delimiter_len >> 8) & 0xFF);
  if (s->delimiter_len > 0 && s->delimiter_len <= 253) {
    memcpy(buffer + len, s->delimiter, s->delimiter_len);
    len += s->delimiter_len;
  }
  return len;
}

void tree_sitter_puppet_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = payload;
  scanner_init(s);
  if (length < 3) return;
  unsigned len = 0;
  s->in_heredoc = buffer[len++] != 0;
  s->delimiter_len = (uint8_t)buffer[len] | ((uint8_t)buffer[len + 1] << 8);
  len += 2;
  if (s->delimiter_len > 0 && s->delimiter_len <= 255 && len + s->delimiter_len <= length) {
    memcpy(s->delimiter, buffer + len, s->delimiter_len);
    s->delimiter[s->delimiter_len] = '\0';
  } else {
    s->delimiter_len = 0;
    s->delimiter[0] = '\0';
  }
}

bool tree_sitter_puppet_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *s = payload;

  if (valid_symbols[HEREDOC_START] && !s->in_heredoc) {
    if (scan_heredoc_start(s, lexer)) return true;
  }

  if (valid_symbols[HEREDOC_BODY]) {
    if (scan_heredoc_body(s, lexer)) return true;
  }

  if (valid_symbols[HEREDOC_END]) {
    if (scan_heredoc_end(s, lexer)) return true;
  }

  return false;
}
