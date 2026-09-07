#include "vimcmd.h"
#include "utf8.h"
#include <string.h>
#include <stdio.h>

// ---- positions ------------------------------------------------------------

static size_t prev_boundary(const vim_doc_t *d, size_t i) {
    if(!i) return 0;
    i--;
    while(i && utf8_is_cont(d->text[i])) i--;
    return i;
}
static size_t next_boundary(const vim_doc_t *d, size_t i) {
    if(i>=d->len) return d->len;
    i++;
    while(i<d->len && utf8_is_cont(d->text[i])) i++;
    return i;
}
static size_t line_start(const vim_doc_t *d, size_t i) {
    while(i && d->text[i-1]!='\n') i--;
    return i;
}
static size_t line_end(const vim_doc_t *d, size_t i) {
    while(i<d->len && d->text[i]!='\n') i++;
    return i;
}
static size_t first_nonblank(const vim_doc_t *d, size_t i) {
    size_t s=line_start(d,i), e=line_end(d,s);
    while(s<e && (d->text[s]==' '||d->text[s]=='\t')) s++;
    return s;
}
static size_t next_line(const vim_doc_t *d, size_t i) {
    size_t e=line_end(d,i);
    return e<d->len ? e+1 : (size_t)-1;
}
static size_t prev_line(const vim_doc_t *d, size_t i) {
    size_t s=line_start(d,i);
    return s ? line_start(d,s-1) : (size_t)-1;
}

void vim_clamp(const vim_state_t *v, vim_doc_t *d) {
    if(d->cursor>d->len) d->cursor=d->len;
    // Insert mode is allowed to sit past the last character; normal mode is
    // always *on* one, which is what makes `x` and the block cursor mean
    // anything.
    if(v->mode!=VIM_NORMAL) return;
    size_t s=line_start(d,d->cursor), e=line_end(d,d->cursor);
    if(d->cursor>=e && e>s) d->cursor=prev_boundary(d,e);
    else if(e==s) d->cursor=s;
    while(d->cursor>0 && d->cursor<d->len && utf8_is_cont(d->text[d->cursor]))
        d->cursor--;
}

// ---- character classes ----------------------------------------------------

enum { CLS_SPACE = 0, CLS_WORD, CLS_PUNCT };

// Everything above U+007F counts as a word character. Classing kanji by script
// would make `w` stop at every glyph, which is not what anyone means by "next
// word" in a comment.
static int class_of(char c, bool big) {
    unsigned char u=(unsigned char)c;
    if(u==' '||u=='\t'||u=='\n'||u=='\r') return CLS_SPACE;
    if(big||u>=0x80) return CLS_WORD;
    if((u>='a'&&u<='z')||(u>='A'&&u<='Z')||(u>='0'&&u<='9')||u=='_') return CLS_WORD;
    return CLS_PUNCT;
}

// An empty line is a word, so `w` through a blank-separated block stops on each
// gap rather than skipping the shape of the file.
static bool blank_line(const vim_doc_t *d, size_t i) {
    return (i==0 || d->text[i-1]=='\n') && (i>=d->len || d->text[i]=='\n');
}

static size_t word_fwd(const vim_doc_t *d, size_t i, bool big) {
    if(i>=d->len) return d->len;
    int c=class_of(d->text[i],big);
    if(c!=CLS_SPACE)
        while(i<d->len && class_of(d->text[i],big)==c) i=next_boundary(d,i);
    else
        i=next_boundary(d,i);   // leave the blank stood on, or `w` sticks
    while(i<d->len && class_of(d->text[i],big)==CLS_SPACE && !blank_line(d,i))
        i=next_boundary(d,i);
    return i;
}
static size_t word_back(const vim_doc_t *d, size_t i, bool big) {
    if(!i) return 0;
    i=prev_boundary(d,i);
    while(i && class_of(d->text[i],big)==CLS_SPACE && !blank_line(d,i))
        i=prev_boundary(d,i);
    int c=class_of(d->text[i],big);
    if(c==CLS_SPACE) return i;
    while(i) {
        size_t p=prev_boundary(d,i);
        if(class_of(d->text[p],big)!=c) break;
        i=p;
    }
    return i;
}
static size_t word_end(const vim_doc_t *d, size_t i, bool big) {
    if(i>=d->len) return d->len;
    i=next_boundary(d,i);
    while(i<d->len && class_of(d->text[i],big)==CLS_SPACE) i=next_boundary(d,i);
    if(i>=d->len) return prev_boundary(d,d->len);
    int c=class_of(d->text[i],big);
    while(i<d->len) {
        size_t n=next_boundary(d,i);
        if(n>=d->len || class_of(d->text[n],big)!=c) break;
        i=n;
    }
    return i;
}

// ---- undo -----------------------------------------------------------------

static void undo_clear(vim_state_t *v) { v->nrec=0; v->ulen=0; }

// The payloads are stacked in record order, so forgetting the oldest record
// means sliding the ring down and shifting the offsets that survive it.
static void undo_drop_oldest(vim_state_t *v) {
    if(!v->nrec) return;
    uint16_t bytes = v->rec[0].kind==VIM_UNDO_DELETED ? v->rec[0].len : 0;
    if(bytes) {
        memmove(v->ubuf,v->ubuf+bytes,(size_t)(v->ulen-bytes));
        v->ulen=(uint16_t)(v->ulen-bytes);
    }
    memmove(v->rec,v->rec+1,(size_t)(v->nrec-1)*sizeof(v->rec[0]));
    v->nrec--;
    for(unsigned i=0;i<v->nrec;i++)
        if(v->rec[i].kind==VIM_UNDO_DELETED)
            v->rec[i].payload=(uint16_t)(v->rec[i].payload-bytes);
}

static void undo_push(vim_state_t *v, uint8_t kind, size_t pos,
                      const char *bytes, size_t n) {
    if(v->in_undo || !n) return;
    // One change bigger than the whole ring cannot be held at all. Forgetting
    // the history is the only honest answer: replaying half of a change would
    // hand back a document that never existed.
    if(kind==VIM_UNDO_DELETED && n>VIM_UNDO_BYTES) { undo_clear(v); return; }

    if(v->nrec) {
        vim_undo_t *t=&v->rec[v->nrec-1];
        // A run of insert-mode keystrokes is one change.
        if(kind==VIM_UNDO_INSERTED && t->kind==VIM_UNDO_INSERTED
           && t->seq==v->seq && (size_t)t->pos+t->len==pos) {
            t->len=(uint16_t)(t->len+n);
            return;
        }
        // Backspacing over what this same change just inserted only shortens
        // it — the bytes never belonged to the document being edited.
        if(kind==VIM_UNDO_DELETED && t->kind==VIM_UNDO_INSERTED
           && t->seq==v->seq && pos>=t->pos && pos+n==(size_t)t->pos+t->len) {
            t->len=(uint16_t)(t->len-n);
            if(!t->len) v->nrec--;
            return;
        }
    }

    while(v->nrec && (v->nrec>=VIM_UNDO_RECS ||
                      (kind==VIM_UNDO_DELETED && v->ulen+n>VIM_UNDO_BYTES)))
        undo_drop_oldest(v);

    vim_undo_t *r=&v->rec[v->nrec++];
    r->kind=kind; r->pos=(uint16_t)pos; r->len=(uint16_t)n;
    r->seq=v->seq; r->payload=v->ulen;
    if(kind==VIM_UNDO_DELETED) {
        memcpy(v->ubuf+v->ulen,bytes,n);
        v->ulen=(uint16_t)(v->ulen+n);
    }
}

// ---- mutation -------------------------------------------------------------

static void msg(vim_state_t *v, const char *s) {
    snprintf(v->msg,sizeof(v->msg),"%s",s);
}

static void doc_delete(vim_state_t *v, vim_doc_t *d, size_t at, size_t n) {
    if(at>=d->len || !n) return;
    if(at+n>d->len) n=d->len-at;
    undo_push(v,VIM_UNDO_DELETED,at,d->text+at,n);
    memmove(d->text+at,d->text+at+n,d->len-at-n);
    d->len-=n; d->text[d->len]=0; d->changed=true;
    if(d->cursor>=at+n) d->cursor-=n;
    else if(d->cursor>at) d->cursor=at;
}

static bool doc_insert(vim_state_t *v, vim_doc_t *d, size_t at,
                       const char *s, size_t n) {
    if(!n) return true;
    if(d->len+n>d->cap) { msg(v,"SOURCE FULL"); return false; }
    memmove(d->text+at+n,d->text+at,d->len-at);
    memcpy(d->text+at,s,n);
    d->len+=n; d->text[d->len]=0; d->changed=true;
    undo_push(v,VIM_UNDO_INSERTED,at,NULL,n);
    if(d->cursor>=at) d->cursor+=n;
    return true;
}

static void do_undo(vim_state_t *v, vim_doc_t *d) {
    if(!v->nrec) { msg(v,"ALREADY AT OLDEST CHANGE"); return; }
    v->in_undo=true;
    uint8_t seq=v->rec[v->nrec-1].seq;
    size_t where=d->cursor;
    while(v->nrec && v->rec[v->nrec-1].seq==seq) {
        vim_undo_t r=v->rec[--v->nrec];
        if(r.kind==VIM_UNDO_INSERTED) {
            memmove(d->text+r.pos,d->text+r.pos+r.len,d->len-r.pos-r.len);
            d->len-=r.len;
        } else {
            memmove(d->text+r.pos+r.len,d->text+r.pos,d->len-r.pos);
            memcpy(d->text+r.pos,v->ubuf+r.payload,r.len);
            d->len+=r.len;
            v->ulen=r.payload;
        }
        d->text[d->len]=0;
        where=r.pos;
    }
    d->changed=true;
    d->cursor=where>d->len?d->len:where;
    v->in_undo=false;
    vim_clamp(v,d);
}

// ---- the register ---------------------------------------------------------

static void yank_chars(vim_state_t *v, const vim_doc_t *d, size_t at, size_t n) {
    if(n>VIM_REG_MAX) { v->reg_len=0; msg(v,"YANK TOO BIG"); return; }
    memcpy(v->reg,d->text+at,n);
    v->reg_len=(uint16_t)n; v->reg_linewise=false;
}

// A linewise register always ends in a newline, whichever of the two newlines
// around the range the delete happened to consume. Without that rule `dd` on
// the last line pastes back as a line whose break is on the wrong side.
static void yank_lines(vim_state_t *v, const vim_doc_t *d, size_t from, size_t to) {
    size_t n=to-from;
    if(n+1>VIM_REG_MAX) { v->reg_len=0; msg(v,"YANK TOO BIG"); return; }
    memcpy(v->reg,d->text+from,n);
    v->reg[n]='\n';
    v->reg_len=(uint16_t)(n+1); v->reg_linewise=true;
}

static void do_put(vim_state_t *v, vim_doc_t *d, bool after, unsigned count) {
    if(!v->reg_len) { msg(v,"NOTHING TO PUT"); return; }
    v->seq++;
    if(v->reg_linewise) {
        size_t e=line_end(d,d->cursor);
        size_t at = after ? (e<d->len ? e+1 : d->len) : line_start(d,d->cursor);
        // The register always ends in a newline, but this document may not.
        // Landing at the very end therefore means making the break before the
        // put and dropping the one after it, so a paste never leaves the
        // source with a blank line it did not have.
        bool at_end = at>=d->len;
        if(at_end && d->len && d->text[d->len-1]!='\n') {
            if(!doc_insert(v,d,d->len,"\n",1)) return;
            at=d->len;
        }
        size_t landed=at;
        for(unsigned i=0;i<count;i++) {
            size_t n = (at_end && i+1==count) ? (size_t)v->reg_len-1 : v->reg_len;
            if(!doc_insert(v,d,at,v->reg,n)) break;
            at+=n;
        }
        d->cursor=first_nonblank(d,landed);
    } else {
        size_t at=d->cursor;
        if(after && line_end(d,at)>line_start(d,at)) at=next_boundary(d,at);
        size_t last=at;
        for(unsigned i=0;i<count;i++) {
            if(!doc_insert(v,d,at,v->reg,v->reg_len)) break;
            last=at; at+=v->reg_len;
        }
        d->cursor = v->reg_len ? prev_boundary(d,last+v->reg_len) : last;
    }
    vim_clamp(v,d);
}

// ---- search ---------------------------------------------------------------

static bool find_pattern(const vim_doc_t *d, const char *pat, size_t plen,
                         int dir, size_t from, size_t *out) {
    if(!plen || plen>d->len) return false;
    size_t span=d->len-plen;
    for(size_t step=0;step<=span;step++) {
        size_t i = dir>0 ? (from+step)%(span+1)
                         : (from + (span+1) - step%(span+1))%(span+1);
        if(!memcmp(d->text+i,pat,plen)) { *out=i; return true; }
    }
    return false;
}

static void do_search(vim_state_t *v, vim_doc_t *d, int dir) {
    if(!v->pat_len) { msg(v,"NO PREVIOUS PATTERN"); return; }
    size_t span = d->len>=v->pat_len ? d->len-v->pat_len : 0;
    size_t from = dir>0 ? (d->cursor+1>span?0:d->cursor+1)
                        : (d->cursor?d->cursor-1:span);
    size_t at;
    if(find_pattern(d,v->pattern,v->pat_len,dir,from,&at)) {
        d->cursor=at; vim_clamp(v,d);
    } else {
        msg(v,"PATTERN NOT FOUND");
    }
}

// ---- motions --------------------------------------------------------------

typedef struct {
    size_t at;
    bool   ok, linewise, inclusive;
} motion_t;

static motion_t none_motion(void) { motion_t m={0}; return m; }

// The count-th occurrence of the pending f/F/t/T character on this line.
static motion_t find_char(vim_state_t *v, const vim_doc_t *d, char kind,
                          unsigned count) {
    motion_t m=none_motion();
    bool fwd = kind=='f'||kind=='t';
    size_t s=line_start(d,d->cursor), e=line_end(d,d->cursor), i=d->cursor;
    for(unsigned n=0;n<count;n++) {
        bool hit=false;
        if(fwd) {
            for(size_t j=next_boundary(d,i);j<e;j=next_boundary(d,j))
                if(j+v->find_len<=e && !memcmp(d->text+j,v->find_ch,v->find_len)) {
                    i=j; hit=true; break;
                }
        } else {
            for(size_t j=i;j>s;) {
                j=prev_boundary(d,j);
                if(j+v->find_len<=e && !memcmp(d->text+j,v->find_ch,v->find_len)) {
                    i=j; hit=true; break;
                }
            }
        }
        // Vim fails the whole motion rather than stopping at the count it did
        // reach, so `3fx` on two x's leaves the cursor where it was.
        if(!hit) return m;
    }
    if(kind=='t')      i=prev_boundary(d,i);
    else if(kind=='T') i=next_boundary(d,i);
    m.at=i; m.ok=true;
    // A backward motion is exclusive of where the cursor already is, so only
    // the forward pair extends the operator's range over the target.
    m.inclusive=fwd;
    return m;
}

static motion_t vertical(vim_state_t *v, const vim_doc_t *d, int dir,
                         unsigned count) {
    motion_t m=none_motion();
    size_t at=d->cursor;
    if(v->want_col==(uint16_t)-1)
        v->want_col=(uint16_t)(d->cursor-line_start(d,d->cursor));
    for(unsigned n=0;n<count;n++) {
        size_t next = dir>0 ? next_line(d,at) : prev_line(d,at);
        if(next==(size_t)-1) { if(!n) return m; break; }
        at=next;
    }
    size_t e=line_end(d,at);
    at += v->want_col;
    if(at>e) at=e;
    while(at>0 && at<d->len && utf8_is_cont(d->text[at])) at--;
    m.at=at; m.ok=true; m.linewise=true;
    return m;
}

// `c` is the motion key; `count` is already the product of both counts.
// `for_op` is set when an operator is waiting, which is the one case where
// `cw` has to behave like `ce`.
static motion_t motion_for(vim_state_t *v, const vim_doc_t *d, char c,
                           unsigned count, char op) {
    motion_t m=none_motion();
    size_t at=d->cursor;
    switch(c) {
        case 'h':
            for(unsigned n=0;n<count && at>line_start(d,at);n++) at=prev_boundary(d,at);
            m.at=at; m.ok=true; return m;
        case 'l': {
            size_t e=line_end(d,at);
            // An operator may take the last character of the line; a bare `l`
            // may not step past it.
            size_t lim = op ? e : (e>line_start(d,at) ? prev_boundary(d,e) : e);
            for(unsigned n=0;n<count && at<lim;n++) at=next_boundary(d,at);
            m.at=at; m.ok=true; return m;
        }
        case 'j': return vertical(v,d,1,count);
        case 'k': return vertical(v,d,-1,count);
        case 'w': case 'W': {
            bool big=c=='W';
            // `cw` on a word changes the word, not up to the next one: vim's
            // one deliberate inconsistency, and the one people rely on.
            if(op=='c' && at<d->len && class_of(d->text[at],big)!=CLS_SPACE) {
                for(unsigned n=0;n<count;n++) at=word_end(d,at,big);
                m.at=at; m.ok=true; m.inclusive=true; return m;
            }
            for(unsigned n=0;n<count;n++) at=word_fwd(d,at,big);
            // `dw` on the last word of a line stops at the newline rather than
            // eating it, or `dw` would join two lines.
            if(op) {
                size_t e=line_end(d,d->cursor);
                if(at>e && d->cursor<=e) at=e;
            }
            m.at=at; m.ok=true; return m;
        }
        case 'b': case 'B':
            for(unsigned n=0;n<count;n++) at=word_back(d,at,c=='B');
            m.at=at; m.ok=true; return m;
        case 'e': case 'E':
            for(unsigned n=0;n<count;n++) at=word_end(d,at,c=='E');
            m.at=at; m.ok=true; m.inclusive=true; return m;
        case '0': m.at=line_start(d,at); m.ok=true; return m;
        case '^': m.at=first_nonblank(d,at); m.ok=true; return m;
        case '$': {
            for(unsigned n=1;n<count;n++) {
                size_t nl=next_line(d,at);
                if(nl==(size_t)-1) break;
                at=nl;
            }
            m.at=line_end(d,at); m.ok=true; return m;   // exclusive: [cursor,eol)
        }
        case 'G': {
            size_t i=0;
            if(v->count1||v->count2) {
                for(unsigned n=1;n<count;n++) {
                    size_t nl=next_line(d,i);
                    if(nl==(size_t)-1) break;
                    i=nl;
                }
            } else {
                i=line_start(d,d->len);
            }
            m.at=first_nonblank(d,i); m.ok=true; m.linewise=true; return m;
        }
        case 'g': {   // gg, already confirmed by the caller
            size_t i=0;
            for(unsigned n=1;n<count;n++) {
                size_t nl=next_line(d,i);
                if(nl==(size_t)-1) break;
                i=nl;
            }
            m.at=first_nonblank(d,i); m.ok=true; m.linewise=true; return m;
        }
    }
    return m;
}

// ---- operators ------------------------------------------------------------

static void enter_insert(vim_state_t *v) { v->seq++; v->mode=VIM_INSERT; }

static void clear_pending(vim_state_t *v) {
    v->count1=v->count2=0; v->op=0; v->pending=0;
}

static void apply_operator(vim_state_t *v, vim_doc_t *d, char op, motion_t m) {
    size_t a=d->cursor, b=m.at;
    if(a>b) { size_t t=a; a=b; b=t; }
    v->seq++;
    if(m.linewise) {
        size_t ls=line_start(d,a), le=line_end(d,b);
        yank_lines(v,d,ls,le);
        if(op=='y') {
            // A yank leaves the cursor where it is unless the
            // range began above it.
            if(d->cursor>ls) d->cursor=first_nonblank(d,ls);
            vim_clamp(v,d);
            return;
        }
        a=ls; b=le<d->len?le+1:le;
        // The last line of a document has no newline of its own to take, so it
        // takes the one that ends the line before it.
        if(b==le && ls>0) a=ls-1;
    } else {
        if(m.inclusive) b=next_boundary(d,b);
        yank_chars(v,d,a,b-a);
        if(op=='y') { d->cursor=a; vim_clamp(v,d); return; }
    }
    doc_delete(v,d,a,b-a);
    if(op=='c') {
        if(m.linewise) {
            // `cc` empties the line rather than removing it.
            doc_insert(v,d,a,"\n",1);
            d->cursor=a;
        } else {
            d->cursor=a;
        }
        enter_insert(v);
        return;
    }
    d->cursor = m.linewise ? first_nonblank(d,a>d->len?d->len:a) : a;
    vim_clamp(v,d);
}

// `dd`, `cc`, `yy`: the same operator over `count` whole lines.
static void line_operator(vim_state_t *v, vim_doc_t *d, char op, unsigned count) {
    size_t last=d->cursor;
    for(unsigned n=1;n<count;n++) {
        size_t nl=next_line(d,last);
        if(nl==(size_t)-1) break;
        last=nl;
    }
    motion_t m={.at=last,.ok=true,.linewise=true};
    apply_operator(v,d,op,m);
}

static void join_lines(vim_state_t *v, vim_doc_t *d, unsigned count) {
    if(count<2) count=2;
    v->seq++;
    for(unsigned n=1;n<count;n++) {
        size_t e=line_end(d,d->cursor);
        if(e>=d->len) break;
        size_t j=e+1;
        while(j<d->len && (d->text[j]==' '||d->text[j]=='\t')) j++;
        // Vim's rule: one space at the seam, unless the line already ends in
        // one or the next line starts with a close paren.
        bool space = e>line_start(d,e) && d->text[e-1]!=' ' && d->text[e-1]!='\t'
                     && j<d->len && d->text[j]!=')' && d->text[j]!='\n';
        d->cursor=e;
        doc_delete(v,d,e,j-e);
        if(space) doc_insert(v,d,e," ",1);
        d->cursor=e;
    }
    vim_clamp(v,d);
}

// ---- the ':' and '/' line -------------------------------------------------

static void goto_line(vim_state_t *v, vim_doc_t *d, unsigned n) {
    size_t i=0;
    for(unsigned k=1;k<n;k++) {
        size_t nl=next_line(d,i);
        if(nl==(size_t)-1) break;
        i=nl;
    }
    d->cursor=first_nonblank(d,i);
    vim_clamp(v,d);
}

static vim_action_t run_ex(vim_state_t *v, vim_doc_t *d) {
    const char *s=v->cmdline; size_t n=v->cmd_len;
    while(n && *s==' ') { s++; n--; }
    while(n && s[n-1]==' ') n--;
    if(!n) return VIM_ACT_NONE;
    if(n==1 && s[0]=='w')                 return VIM_ACT_SAVE;
    if(n==1 && s[0]=='q')                 return VIM_ACT_QUIT;
    if(n==2 && !memcmp(s,"q!",2))         return VIM_ACT_QUIT_FORCE;
    if(n==2 && !memcmp(s,"wq",2))         return VIM_ACT_SAVE_QUIT;
    if(n==3 && !memcmp(s,"wq!",3))        return VIM_ACT_SAVE_QUIT;
    if(n==1 && s[0]=='x')                 return VIM_ACT_SAVE_QUIT;
    unsigned line=0, digits=0;
    for(size_t i=0;i<n;i++) {
        if(s[i]<'0'||s[i]>'9') { digits=0; break; }
        if(line<100000u) line=line*10+(unsigned)(s[i]-'0');
        digits++;
    }
    if(digits) { goto_line(v,d,line?line:1); return VIM_ACT_NONE; }
    msg(v,"E492 NOT AN EDITOR COMMAND");
    return VIM_ACT_NONE;
}

static vim_action_t cmdline_key(vim_state_t *v, vim_doc_t *d, vim_key_t k) {
    switch(k.kind) {
        case VIM_KEY_ESC:
            v->cmd_len=0; v->mode=VIM_NORMAL;
            return VIM_ACT_NONE;
        case VIM_KEY_BACKSPACE: {
            if(!v->cmd_len) { v->mode=VIM_NORMAL; return VIM_ACT_NONE; }
            size_t i=v->cmd_len-1;
            while(i && utf8_is_cont(v->cmdline[i])) i--;
            v->cmd_len=(uint8_t)i;
            if(!v->cmd_len) v->mode=VIM_NORMAL;
            return VIM_ACT_NONE;
        }
        case VIM_KEY_ENTER: break;
        case VIM_KEY_TEXT:
            if(k.len && k.text[0]=='\n') break;
            if(k.len && k.text[0]=='\b') {
                vim_key_t bs={VIM_KEY_BACKSPACE,NULL,0};
                return cmdline_key(v,d,bs);
            }
            if(k.len && (unsigned char)k.text[0]>=0x20
               && v->cmd_len+k.len<VIM_CMD_MAX) {
                memcpy(v->cmdline+v->cmd_len,k.text,k.len);
                v->cmd_len=(uint8_t)(v->cmd_len+k.len);
            }
            return VIM_ACT_NONE;
        default:
            return VIM_ACT_NONE;
    }

    v->mode=VIM_NORMAL;
    if(v->cmd_kind==':') return run_ex(v,d);
    // A search: remember it so `n` and `N` have something to repeat.
    v->pat_len = v->cmd_len<VIM_PAT_MAX ? v->cmd_len : VIM_PAT_MAX;
    memcpy(v->pattern,v->cmdline,v->pat_len);
    v->search_dir = v->cmd_kind=='?' ? -1 : 1;
    do_search(v,d,v->search_dir);
    return VIM_ACT_NONE;
}

// ---- insert mode ----------------------------------------------------------

static void leave_insert(vim_state_t *v, vim_doc_t *d) {
    v->mode=VIM_NORMAL;
    if(d->cursor>line_start(d,d->cursor)) d->cursor=prev_boundary(d,d->cursor);
    v->want_col=(uint16_t)-1;
    vim_clamp(v,d);
}

static vim_action_t insert_key(vim_state_t *v, vim_doc_t *d, vim_key_t k) {
    switch(k.kind) {
        case VIM_KEY_ESC:   leave_insert(v,d); return VIM_ACT_NONE;
        case VIM_KEY_LEFT:  d->cursor=prev_boundary(d,d->cursor); return VIM_ACT_NONE;
        case VIM_KEY_RIGHT: d->cursor=next_boundary(d,d->cursor); return VIM_ACT_NONE;
        case VIM_KEY_UP:    { motion_t m=vertical(v,d,-1,1); if(m.ok) d->cursor=m.at;
                              return VIM_ACT_NONE; }
        case VIM_KEY_DOWN:  { motion_t m=vertical(v,d,1,1);  if(m.ok) d->cursor=m.at;
                              return VIM_ACT_NONE; }
        case VIM_KEY_ENTER: doc_insert(v,d,d->cursor,"\n",1); return VIM_ACT_NONE;
        case VIM_KEY_BACKSPACE: {
            size_t s=prev_boundary(d,d->cursor);
            if(s<d->cursor) doc_delete(v,d,s,d->cursor-s);
            return VIM_ACT_NONE;
        }
        case VIM_KEY_TEXT: break;
    }
    if(!k.len) return VIM_ACT_NONE;
    unsigned char c=(unsigned char)k.text[0];
    v->want_col=(uint16_t)-1;
    if(c=='\n')      doc_insert(v,d,d->cursor,"\n",1);
    else if(c=='\t') doc_insert(v,d,d->cursor,"  ",2);
    else if(c=='\b') { size_t s=prev_boundary(d,d->cursor);
                       if(s<d->cursor) doc_delete(v,d,s,d->cursor-s); }
    else if(c>=0x20) doc_insert(v,d,d->cursor,k.text,k.len);
    // Anything else is a control byte the editor did not claim; dropping it
    // keeps a stray 0x07 out of the source.
    return VIM_ACT_NONE;
}

// ---- normal mode ----------------------------------------------------------

static unsigned eff_count(const vim_state_t *v) {
    unsigned a=v->count1?v->count1:1, b=v->count2?v->count2:1;
    unsigned n=a*b;
    // Bounded so a mistyped count cannot walk the buffer thousands of times.
    return n?(n>9999?9999:n):1;
}

static vim_action_t normal_key(vim_state_t *v, vim_doc_t *d, vim_key_t k) {
    char c;
    switch(k.kind) {
        case VIM_KEY_ESC:
            if(v->op||v->pending||v->count1||v->count2) {
                clear_pending(v);
                return VIM_ACT_NONE;
            }
            return VIM_ACT_QUIT_FORCE;
        case VIM_KEY_LEFT:      c='h'; break;
        case VIM_KEY_RIGHT:     c='l'; break;
        case VIM_KEY_UP:        c='k'; break;
        case VIM_KEY_DOWN:      c='j'; break;
        case VIM_KEY_ENTER:     c='j'; break;
        case VIM_KEY_BACKSPACE: c='h'; break;
        default:
            if(!k.len) return VIM_ACT_NONE;
            c=k.text[0];
    }

    // An argument the previous key is still waiting for. These come first
    // because every one of them takes an arbitrary character, digits included.
    if(v->pending) {
        char p=v->pending;
        v->pending=0;
        if(p=='g') {
            if(c=='g') {
                motion_t m=motion_for(v,d,'g',eff_count(v),v->op);
                if(v->op) apply_operator(v,d,v->op,m);
                else { d->cursor=m.at; v->want_col=(uint16_t)-1; vim_clamp(v,d); }
            }
            clear_pending(v);
            return VIM_ACT_NONE;
        }
        if(p=='Z') {
            clear_pending(v);
            return c=='Z' ? VIM_ACT_SAVE_QUIT : VIM_ACT_NONE;
        }
        if(p=='r') {
            unsigned n=eff_count(v);
            clear_pending(v);
            if((unsigned char)c<0x20 || k.kind!=VIM_KEY_TEXT) return VIM_ACT_NONE;
            // Vim refuses when the line is too short, rather than replacing
            // what is there and swallowing the newline.
            size_t at=d->cursor, e=line_end(d,at);
            for(unsigned i=0;i<n;i++) {
                if(at>=e) { msg(v,"NOT ENOUGH CHARACTERS"); return VIM_ACT_NONE; }
                at=next_boundary(d,at);
            }
            v->seq++;
            size_t from=d->cursor;
            doc_delete(v,d,from,at-from);
            for(unsigned i=0;i<n;i++) doc_insert(v,d,from,k.text,k.len);
            d->cursor=from;
            for(unsigned i=1;i<n;i++) d->cursor=next_boundary(d,d->cursor);
            vim_clamp(v,d);
            return VIM_ACT_NONE;
        }
        if(p=='f'||p=='F'||p=='t'||p=='T') {
            if(k.kind!=VIM_KEY_TEXT || !k.len || (unsigned char)c<0x20) {
                clear_pending(v);
                return VIM_ACT_NONE;
            }
            v->find_kind=p;
            v->find_len=(uint8_t)(k.len<VIM_FIND_MAX?k.len:VIM_FIND_MAX);
            memcpy(v->find_ch,k.text,v->find_len);
            motion_t m=find_char(v,d,p,eff_count(v));
            if(m.ok) {
                if(v->op) apply_operator(v,d,v->op,m);
                else { d->cursor=m.at; v->want_col=(uint16_t)-1; vim_clamp(v,d); }
            }
            clear_pending(v);
            return VIM_ACT_NONE;
        }
    }

    // Counts. A leading `0` is the motion, a later one is a digit.
    if(c>='1'&&c<='9') {
        uint32_t *slot = v->op ? &v->count2 : &v->count1;
        if(*slot<100000u) *slot=*slot*10+(uint32_t)(c-'0');
        return VIM_ACT_NONE;
    }
    if(c=='0') {
        uint32_t *slot = v->op ? &v->count2 : &v->count1;
        if(*slot) { if(*slot<100000u) *slot*=10; return VIM_ACT_NONE; }
    }

    if(c!='j' && c!='k' && k.kind!=VIM_KEY_UP && k.kind!=VIM_KEY_DOWN)
        v->want_col=(uint16_t)-1;

    unsigned count=eff_count(v);

    // Keys that take an argument.
    if(c=='g'||c=='Z'||c=='r'||c=='f'||c=='F'||c=='t'||c=='T') {
        // `r` and `Z` are not motions, so an operator waiting on one is a typo.
        if(v->op && (c=='Z'||c=='r')) { clear_pending(v); return VIM_ACT_NONE; }
        v->pending=c;
        return VIM_ACT_NONE;
    }

    // `;` and `,` replay the last f/F/t/T; `,` in the other direction.
    if((c==';'||c==',') && v->find_kind) {
        char kind=v->find_kind;
        if(c==',') kind = kind=='f'?'F':kind=='F'?'f':kind=='t'?'T':'t';
        motion_t m=find_char(v,d,kind,count);
        if(m.ok) {
            if(v->op) apply_operator(v,d,v->op,m);
            else { d->cursor=m.at; vim_clamp(v,d); }
        }
        clear_pending(v);
        return VIM_ACT_NONE;
    }

    // A plain motion, which an operator consumes if one is waiting.
    if(c && strchr("hjklwWbBeE0^$G",c)) {
        motion_t m=motion_for(v,d,c,count,v->op);
        if(m.ok) {
            if(v->op) apply_operator(v,d,v->op,m);
            else { d->cursor=m.at; vim_clamp(v,d); }
        }
        clear_pending(v);
        return VIM_ACT_NONE;
    }

    if(c=='d'||c=='c'||c=='y') {
        if(v->op==c) { line_operator(v,d,c,count); clear_pending(v); }
        else if(v->op) clear_pending(v);       // `dy`: a typo, not a command
        else v->op=c;
        return VIM_ACT_NONE;
    }
    // `D`, `C` and `S` are the operator to the end of the line or of the line
    // itself; `Y` is `yy`, which is what vim does however much it is regretted.
    if(c=='D'||c=='C') {
        char op = c=='D'?'d':'c';
        motion_t m=motion_for(v,d,'$',count,op);
        if(v->op) { clear_pending(v); return VIM_ACT_NONE; }
        apply_operator(v,d,op,m);
        clear_pending(v);
        return VIM_ACT_NONE;
    }
    if(c=='Y'||c=='S') {
        if(v->op) { clear_pending(v); return VIM_ACT_NONE; }
        line_operator(v,d,c=='Y'?'y':'c',count);
        clear_pending(v);
        return VIM_ACT_NONE;
    }

    if(v->op) {   // anything below is not a motion, so the operator was a typo
        clear_pending(v);
        return VIM_ACT_NONE;
    }

    switch(c) {
        case 'x': case 's': {
            size_t e=line_end(d,d->cursor), at=d->cursor;
            for(unsigned i=0;i<count && at<e;i++) at=next_boundary(d,at);
            if(at>d->cursor) {
                v->seq++;
                yank_chars(v,d,d->cursor,at-d->cursor);
                doc_delete(v,d,d->cursor,at-d->cursor);
            } else if(c=='x') { clear_pending(v); return VIM_ACT_NONE; }
            if(c=='s') enter_insert(v); else vim_clamp(v,d);
            break;
        }
        case 'X': {
            size_t s=line_start(d,d->cursor), at=d->cursor;
            for(unsigned i=0;i<count && at>s;i++) at=prev_boundary(d,at);
            if(at<d->cursor) {
                v->seq++;
                yank_chars(v,d,at,d->cursor-at);
                doc_delete(v,d,at,d->cursor-at);
            }
            vim_clamp(v,d);
            break;
        }
        case 'p': do_put(v,d,true,count);  break;
        case 'P': do_put(v,d,false,count); break;
        case 'J': join_lines(v,d,count); break;
        case 'u': do_undo(v,d); break;
        case 'i': enter_insert(v); break;
        case 'a': if(line_end(d,d->cursor)>line_start(d,d->cursor))
                      d->cursor=next_boundary(d,d->cursor);
                  enter_insert(v); break;
        case 'I': d->cursor=first_nonblank(d,d->cursor); enter_insert(v); break;
        case 'A': d->cursor=line_end(d,d->cursor); enter_insert(v); break;
        case 'o': {
            size_t e=line_end(d,d->cursor);
            v->seq++;
            d->cursor=e;
            if(doc_insert(v,d,e,"\n",1)) { v->mode=VIM_INSERT; }
            break;
        }
        case 'O': {
            size_t s=line_start(d,d->cursor);
            v->seq++;
            d->cursor=s;
            if(doc_insert(v,d,s,"\n",1)) { d->cursor=s; v->mode=VIM_INSERT; }
            break;
        }
        case 'n': do_search(v,d,v->search_dir?v->search_dir:1); break;
        case 'N': do_search(v,d,v->search_dir?-v->search_dir:-1); break;
        case ':': case '/': case '?':
            v->mode=VIM_CMDLINE; v->cmd_kind=c; v->cmd_len=0;
            break;
        default: break;
    }
    clear_pending(v);
    return VIM_ACT_NONE;
}

// ---- the surface ----------------------------------------------------------

void vim_reset(vim_state_t *v) {
    v->mode=VIM_NORMAL; v->in_undo=false; v->seq=0;
    clear_pending(v);
    v->cmd_len=0; v->cmd_kind=0;
    v->want_col=(uint16_t)-1;
    v->msg[0]=0; v->echo[0]=0;
    undo_clear(v);
}

vim_mode_t vim_mode(const vim_state_t *v) { return (vim_mode_t)v->mode; }

void vim_begin_insert(vim_state_t *v) {
    clear_pending(v);
    v->seq++;
    v->mode=VIM_INSERT;
}

const char *vim_message(const vim_state_t *v) { return v->msg; }

const char *vim_cmdline(const vim_state_t *v, size_t *len) {
    if(v->mode!=VIM_CMDLINE) return NULL;
    *len=v->cmd_len;
    return v->cmdline;
}

const char *vim_pending(vim_state_t *v) {
    char *p=v->echo; size_t room=sizeof(v->echo);
    int n=0;
    if(v->count1) n=snprintf(p,room,"%u",(unsigned)v->count1);
    if(n<0) n=0;
    if(v->op && (size_t)n+1<room) p[n++]=v->op;
    if(v->count2 && (size_t)n<room) n+=snprintf(p+n,room-(size_t)n,"%u",(unsigned)v->count2);
    if(v->pending && (size_t)n+1<room) p[n++]=v->pending;
    if((size_t)n<room) p[n]=0; else p[room-1]=0;
    return v->echo;
}

vim_action_t vim_feed(vim_state_t *v, vim_doc_t *d, vim_key_t k) {
    v->msg[0]=0;
    switch(v->mode) {
        case VIM_INSERT:  return insert_key(v,d,k);
        case VIM_CMDLINE: return cmdline_key(v,d,k);
        default:          return normal_key(v,d,k);
    }
}
