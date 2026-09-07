#include "jslex.h"
#include <stdbool.h>
#include <string.h>

// The reserved words, as quickjs.c lists them between TOK_NULL and TOK_AWAIT,
// plus the contextual ones worth colouring. In rodata, so the table costs no
// RAM. `value` says the word ends an expression, which is what decides whether
// a following '/' divides or opens a regexp: `this / 2` divides, `return /x/`
// does not. The contextual words (as, from, get, set, of, static, async) are
// ordinary identifiers most of the time, so they count as values too.
static const struct { const char *word; bool value; } KEYWORDS[] = {
    {"as",1},{"async",1},{"await",0},{"break",0},{"case",0},{"catch",0},
    {"class",0},{"const",0},{"continue",0},{"debugger",0},{"default",0},
    {"delete",0},{"do",0},{"else",0},{"enum",0},{"export",0},{"extends",0},
    {"false",1},{"finally",0},{"for",0},{"from",1},{"function",0},{"get",1},
    {"if",0},{"import",0},{"in",0},{"instanceof",0},{"let",0},{"new",0},
    {"null",1},{"of",1},{"return",0},{"set",1},{"static",1},{"super",1},
    {"switch",0},{"this",1},{"throw",0},{"true",1},{"try",0},{"typeof",0},
    {"undefined",1},{"var",0},{"void",0},{"while",0},{"with",0},{"yield",0},
};
#define KEYWORD_N (sizeof(KEYWORDS)/sizeof(KEYWORDS[0]))

static bool is_ident_first(unsigned char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'||c>=0x80;
}
static bool is_ident(unsigned char c) {
    return is_ident_first(c)||(c>='0'&&c<='9');
}
static bool is_digit(unsigned char c) { return c>='0'&&c<='9'; }

// Returns the table index, or -1. `n` is a byte count, not NUL-terminated.
static int keyword_index(const char *s, size_t n) {
    if(n<2||n>10) return -1;
    for(unsigned i=0;i<KEYWORD_N;i++)
        if(!strncmp(KEYWORDS[i].word,s,n) && KEYWORDS[i].word[n]=='\0') return (int)i;
    return -1;
}

// State the caller has to carry between runs of the main loop.
typedef struct {
    const char   *text;
    size_t        len;
    unsigned      from_line, to_line;
    jslex_emit_fn emit;
    void         *user_data;
    unsigned      line;
    // What the last run that carries a value was, which is the whole basis for
    // telling a regexp literal from a division. quickjs.c does the same thing
    // in js_parse_skip_parens_token, where its own lexer runs without the
    // parser to lean on; it is a heuristic there too.
    bool          value_before;
} lexer_t;

// Reports one run, splitting it at newlines and counting lines as it goes.
// Every byte the scanner consumes passes through here, so the line number
// stays right without the main loop counting anything itself.
static void run(lexer_t *s, size_t start, size_t end, uint8_t kind) {
    size_t i=start;
    while(i<end) {
        size_t stop=i;
        while(stop<end && s->text[stop]!='\n') stop++;
        if(stop>i && s->line>=s->from_line && s->line<=s->to_line)
            s->emit(s->user_data,s->line,i,stop-i,kind);
        if(stop<end) { s->line++; stop++; }   // the newline itself is not drawn
        i=stop;
    }
}

void jslex_scan(const char *text, size_t len,
                unsigned from_line, unsigned to_line,
                jslex_emit_fn emit, void *user_data) {
    lexer_t s={.text=text,.len=len,.from_line=from_line,.to_line=to_line,
               .emit=emit,.user_data=user_data,.line=0,.value_before=false};
    size_t i=0;
    while(i<len) {
        // Everything past the window has no colour anyone will read.
        if(s.line>to_line) return;
        unsigned char c=(unsigned char)text[i];

        if(c=='/' && i+1<len && text[i+1]=='*') {
            size_t j=i+2;
            while(j+1<len && !(text[j]=='*'&&text[j+1]=='/')) j++;
            j = (j+1<len) ? j+2 : len;
            run(&s,i,j,JSLEX_COMMENT); i=j;
            s.value_before=false;
            continue;
        }
        if(c=='/' && i+1<len && text[i+1]=='/') {
            size_t j=i;
            while(j<len && text[j]!='\n') j++;
            run(&s,i,j,JSLEX_COMMENT); i=j;
            s.value_before=false;
            continue;
        }
        if(c=='/' && !s.value_before) {
            // A regexp literal. It cannot span a line, so an unterminated one
            // ends there rather than colouring the rest of the file.
            size_t j=i+1; bool in_class=false, closed=false;
            while(j<len && text[j]!='\n') {
                char d=text[j];
                if(d=='\\') { j+=2; continue; }
                if(d=='[') in_class=true;
                else if(d==']') in_class=false;
                else if(d=='/' && !in_class) { j++; closed=true; break; }
                j++;
            }
            if(j>len) j=len;
            while(closed && j<len && is_ident((unsigned char)text[j])) j++;  // flags
            run(&s,i,j,JSLEX_STRING); i=j;
            s.value_before=true;
            continue;
        }
        if(c=='\''||c=='"') {
            size_t j=i+1;
            while(j<len && text[j]!='\n') {
                if(text[j]=='\\') { j+=2; continue; }
                if((unsigned char)text[j]==c) { j++; break; }
                j++;
            }
            if(j>len) j=len;
            run(&s,i,j,JSLEX_STRING); i=j;
            s.value_before=true;
            continue;
        }
        if(c=='`') {
            // A template does span lines. Substitutions are coloured as part
            // of the literal: telling them apart needs a brace stack, and at
            // 20 columns the distinction buys nothing.
            size_t j=i+1;
            while(j<len) {
                if(text[j]=='\\') { j+=2; continue; }
                if(text[j]=='`') { j++; break; }
                j++;
            }
            if(j>len) j=len;
            run(&s,i,j,JSLEX_STRING); i=j;
            s.value_before=true;
            continue;
        }
        if(is_digit(c) || (c=='.' && i+1<len && is_digit((unsigned char)text[i+1]))) {
            size_t j=i;
            // Hex, binary and octal prefixes fall out of this: the letters are
            // ident characters and the digits are digits.
            while(j<len && (is_ident((unsigned char)text[j]) || text[j]=='.')) j++;
            run(&s,i,j,JSLEX_NUMBER); i=j;
            s.value_before=true;
            continue;
        }
        if(is_ident_first(c)) {
            size_t j=i;
            while(j<len && is_ident((unsigned char)text[j])) j++;
            int kw=keyword_index(text+i,j-i);
            run(&s,i,j,kw>=0?JSLEX_KEYWORD:JSLEX_PLAIN); i=j;
            s.value_before = kw<0 || KEYWORDS[kw].value;
            continue;
        }

        // Runs of whitespace and punctuation, batched so short operators do
        // not each cost a callback.
        size_t j=i;
        while(j<len) {
            unsigned char d=(unsigned char)text[j];
            if(is_ident_first(d)||is_digit(d)||d=='\''||d=='"'||d=='`'||d=='/') break;
            j++;
        }
        if(j==i) j++;
        for(size_t k=i;k<j;k++) {
            char d=text[k];
            if(d==')'||d==']'||d=='}') s.value_before=true;
            else if(d!=' '&&d!='\t'&&d!='\r'&&d!='\n') s.value_before=false;
        }
        run(&s,i,j,JSLEX_PLAIN); i=j;
    }
}
