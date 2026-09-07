#include "textfield.h"
#include "utf8.h"
#include <string.h>

void tf_init(textfield_t *t, char *buf, size_t cap, bool multiline) {
    memset(t,0,sizeof(*t));
    t->buf=buf; t->cap=cap; t->multiline=multiline;
    t->generation=1;
    if(buf) buf[0]='\0';
}

bool tf_set(textfield_t *t, const char *s, size_t len) {
    if(!t->buf) return false;
    if(len>t->cap) return false;
    if(len && !utf8_valid(s,len)) return false;
    if(len) memcpy(t->buf,s,len);
    t->len=len; t->cursor=len; t->buf[len]='\0';
    return true;
}

bool tf_insert(textfield_t *t, const char *s, size_t len) {
    if(!t->buf || !len) return false;
    if(t->len+len>t->cap) { t->refused=true; return false; }
    // The tail moves up; the cursor is on a boundary and the insert is whole
    // characters, so the result cannot be a split sequence.
    memmove(t->buf+t->cursor+len,t->buf+t->cursor,t->len-t->cursor);
    memcpy(t->buf+t->cursor,s,len);
    t->len+=len; t->cursor+=len; t->buf[t->len]='\0';
    t->refused=false;
    return true;
}

bool tf_commit(textfield_t *t, uint32_t generation, const char *s, size_t len) {
    if(generation!=t->generation) return false;
    return tf_insert(t,s,len);
}

void tf_refocus(textfield_t *t) { t->generation++; }

bool tf_backspace(textfield_t *t) {
    if(!t->buf || !t->cursor) return false;
    size_t i=t->cursor-1;
    while(i>0 && utf8_is_cont(t->buf[i])) i--;
    size_t gone=t->cursor-i;
    memmove(t->buf+i,t->buf+t->cursor,t->len-t->cursor);
    t->len-=gone; t->cursor=i; t->buf[t->len]='\0';
    return true;
}

bool tf_left(textfield_t *t) {
    if(!t->cursor) return false;
    size_t i=t->cursor-1;
    while(i>0 && utf8_is_cont(t->buf[i])) i--;
    t->cursor=i;
    return true;
}

bool tf_right(textfield_t *t) {
    if(t->cursor>=t->len) return false;
    size_t adv=0;
    utf8_decode(t->buf,t->len,t->cursor,&adv);
    t->cursor+=adv;
    if(t->cursor>t->len) t->cursor=t->len;
    return true;
}

static bool token_is(const char *key, size_t len, const char *name) {
    size_t n=strlen(name);
    return len==n+1 && !memcmp(key+1,name,n);
}

tf_result_t tf_key(textfield_t *t, const char *key, size_t len, bool ime_took) {
    // The engine had its say first (ime_core's ordering rule), and what it ate
    // is not the field's to act on. This is what keeps a conversion-committing
    // Enter from also being a submit, and a conversion-cancelling Escape from
    // also being an onCancel: in both cases the IME returned IME_TAKEN or
    // IME_TEXT and the field never sees the key at all.
    if(ime_took || !len) return TF_NONE;

    if(key[0]=='\0') {
        if(token_is(key,len,"esc"))   return TF_CANCEL;
        // Fn+Del on this keyboard, and 0x04 over USB. It deletes backwards
        // because that is what every other text screen here makes it do.
        if(token_is(key,len,"del"))   return tf_backspace(t)?TF_EDIT:TF_NONE;
        if(token_is(key,len,"left"))  { tf_left(t);  return TF_NONE; }
        if(token_is(key,len,"right")) { tf_right(t); return TF_NONE; }
        // up/down have nothing to mean in a field this small, and an unknown
        // token is not an edit. Both are swallowed rather than inserted: a
        // "\0name" token reaching tf_insert would put a NUL in the text.
        return TF_NONE;
    }

    unsigned char c=(unsigned char)key[0];
    if(c=='\b' || c==0x7f) return tf_backspace(t)?TF_EDIT:TF_NONE;
    if(c=='\n' || c=='\r') {
        // Section 6, exactly: 単行の非変換EnterのみonSubmit、複数行は改行.
        if(!t->multiline) return TF_SUBMIT;
        return tf_insert(t,"\n",1)?TF_EDIT:TF_NONE;
    }
    // Below 0x20 and not one of the three above: a control byte the shell uses
    // for something else (C-p is the host capture, C-k the IME toggle). Not
    // text, so not inserted.
    if(c<0x20) return TF_NONE;
    return tf_insert(t,key,len)?TF_EDIT:TF_NONE;
}
