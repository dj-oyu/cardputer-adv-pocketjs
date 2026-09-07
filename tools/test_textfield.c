// Host check, no board involved:
//   gcc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
//     -I main/text tools/test_textfield.c main/text/textfield.c
//     -o /tmp/test-textfield && /tmp/test-textfield
//
// pocket.input.text cannot be run anywhere but the device; every decision the
// section 6 paragraph actually makes -- Enter, Escape, maxBytes on UTF-8, and
// the stale-commit rule -- is a function of one keystroke and is settled here.
#include "../main/text/textfield.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
static void check(int ok, const char *what) {
    if(!ok) { printf("FAIL %s\n",what); failures++; }
}
#define EQ(t,text,what) check((t).len==strlen(text) && \
    !memcmp((t).buf,(text),(t).len) && (t).buf[(t).len]=='\0', what)

// One ASCII key, the way keymap.c produces it.
static tf_result_t k(textfield_t *t, char c) {
    char key[1]={c};
    return tf_key(t,key,1,false);
}
// A "\0name" token. The leading NUL is load-bearing, so this cannot use strlen.
static tf_result_t tok(textfield_t *t, const char *name) {
    char key[16]={0};
    size_t n=strlen(name);
    memcpy(key+1,name,n);
    return tf_key(t,key,n+1,false);
}
static void type(textfield_t *t, const char *s) {
    for(const char *p=s;*p;p++) k(t,*p);
}

int main(void) {
    char buf[64];
    textfield_t T;

    // ---- plain typing ----------------------------------------------------
    tf_init(&T,buf,32,false);
    check(k(&T,'h')==TF_EDIT,"a printable key is an edit");
    type(&T,"ello");
    EQ(T,"hello","typing appends");
    check(T.cursor==5,"the cursor follows the text");

    // ---- Enter -----------------------------------------------------------
    check(k(&T,'\n')==TF_SUBMIT,"single-line Enter submits");
    EQ(T,"hello","a submitting Enter is not also a character");
    check(k(&T,'\r')==TF_SUBMIT,"CR submits too (a host script sends it)");

    tf_init(&T,buf,32,true);
    type(&T,"ab");
    check(k(&T,'\n')==TF_EDIT,"multiline Enter is a newline");
    EQ(T,"ab\n","...and the newline is in the text");

    // A conversion-committing Enter is one the IME ate. It must not ALSO
    // submit: that is the double-delivery the spec names.
    tf_init(&T,buf,32,false);
    type(&T,"x");
    { char key[1]={'\n'};
      check(tf_key(&T,key,1,true)==TF_NONE,"an Enter the IME took does not submit"); }
    EQ(T,"x","...and does not reach the buffer either");

    // ---- Escape ----------------------------------------------------------
    tf_init(&T,buf,32,false);
    type(&T,"hi");
    check(tok(&T,"esc")==TF_CANCEL,"a non-converting Escape cancels the session");
    { char key[4]={0,'e','s','c'};
      check(tf_key(&T,key,4,true)==TF_NONE,
            "an Escape the IME took cancels the conversion, not the session"); }
    EQ(T,"hi","neither Escape touches the text");

    // ---- backspace and delete -------------------------------------------
    tf_init(&T,buf,32,false);
    type(&T,"abc");
    check(k(&T,'\b')==TF_EDIT,"backspace edits");
    EQ(T,"ab","backspace removes one character");
    check(tok(&T,"del")==TF_EDIT,"Fn+Del deletes backwards, as elsewhere here");
    EQ(T,"a","...one character of it");
    k(&T,'\b');
    check(k(&T,'\b')==TF_NONE,"backspace on an empty field changes nothing");
    EQ(T,"","...and leaves it empty");

    // ---- UTF-8: never half a character ----------------------------------
    tf_init(&T,buf,32,false);
    check(tf_insert(&T,"日本語",9),"a three-character commit fits");
    EQ(T,"日本語","...and lands whole");
    check(T.cursor==9,"the cursor is at the end in bytes, not characters");
    check(tf_backspace(&T),"backspace over a 3-byte character");
    EQ(T,"日本","...removes all three of its bytes");

    // maxBytes is a BYTE count. 8 bytes holds two of these and refuses the
    // third whole rather than cutting it into a broken sequence.
    tf_init(&T,buf,8,false);
    check(tf_insert(&T,"あ",3),"1st fits in 8 bytes");
    check(tf_insert(&T,"あ",3),"2nd fits");
    check(!tf_insert(&T,"あ",3),"3rd is refused: 9 > 8");
    check(T.refused,"...and the field says it refused");
    EQ(T,"ああ","a refused insert leaves the text exactly as it was");
    check(T.len==6,"no partial sequence was written");

    // ---- cursor motion ---------------------------------------------------
    tf_init(&T,buf,32,false);
    tf_insert(&T,"aあb",5);
    check(tok(&T,"left")==TF_NONE,"a cursor move is not an edit");
    check(T.cursor==4,"left steps over the 1-byte b");
    tok(&T,"left");
    check(T.cursor==1,"left steps over the whole 3-byte character");
    check(k(&T,'X')==TF_EDIT,"typing inserts at the cursor");
    EQ(T,"aXあb","...in the middle, not at the end");
    tok(&T,"right");
    check(T.cursor==5,"right steps over the 3-byte character");
    tok(&T,"right"); tok(&T,"right");
    check(T.cursor==T.len,"right stops at the end");
    tok(&T,"left");tok(&T,"left");tok(&T,"left");tok(&T,"left");tok(&T,"left");
    check(T.cursor==0,"left stops at the start");

    // ---- tokens are never text ------------------------------------------
    tf_init(&T,buf,32,false);
    check(tok(&T,"up")==TF_NONE,"an unhandled token does nothing");
    check(tok(&T,"nosuchkey")==TF_NONE,"...and neither does an unknown one");
    EQ(T,"","no token ever reaches the buffer");
    check(tf_key(&T,"",0,false)==TF_NONE,"a zero-length key is not a key");

    // ---- the stale commit ------------------------------------------------
    //
    // The one rule that will not show up in casual testing: an IME commit is
    // computed for the field that had focus when the key was fed, and focus can
    // move before it is applied -- an onEdit that closes the session and opens
    // another is the way to reach it from JS.
    tf_init(&T,buf,32,false);
    uint32_t gen=T.generation;
    check(tf_commit(&T,gen,"ねこ",6),"a commit for the current generation lands");
    EQ(T,"ねこ","...as ordinary text");
    tf_refocus(&T);
    check(!tf_commit(&T,gen,"いぬ",6),"a commit tagged with the old generation is refused");
    EQ(T,"ねこ","...and writes nothing");
    check(tf_commit(&T,T.generation,"いぬ",6),"the new generation still works");
    EQ(T,"ねこいぬ","...normally");
    check(T.generation!=0,"generation 0 is never handed out");
    check(!tf_commit(&T,0,"x",1),"so a caller that forgot to record one is refused");

    // ---- initial ---------------------------------------------------------
    tf_init(&T,buf,8,false);
    check(tf_set(&T,"hello",5),"initial fits");
    EQ(T,"hello","...and is the text");
    check(T.cursor==5,"the cursor starts at the end of initial");
    check(!tf_set(&T,"123456789",9),"initial longer than maxBytes is refused");
    EQ(T,"hello","...leaving the field as it was");
    check(!tf_set(&T,"\xc3",1),"truncated UTF-8 is refused");
    check(!tf_set(&T,"\xed\xa0\x80",3),"a lone surrogate is refused");
    check(tf_set(&T,"",0),"an empty initial is fine");
    EQ(T,"","...and empties the field");

    if(failures) { printf("%u FAILED\n",failures); return 1; }
    printf("textfield: all checks passed\n");
    return 0;
}
