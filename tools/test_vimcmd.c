// Host check, no board involved:
//   gcc -std=c11 -O1 -Wall -Wextra -fsanitize=address,undefined -I main/text
//       tools/test_vimcmd.c main/ui/vimcmd.c -o /tmp/test-vimcmd && /tmp/test-vimcmd
//
// The editor cannot be run anywhere but the device; the command engine can, so
// every motion, operator and undo rule is settled here rather than by flashing.
#include "../main/ui/vimcmd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static vim_state_t V;
static vim_doc_t   D;
static char        BUF[8193];
static vim_action_t LAST;

static void load(const char *s) {
    size_t n=strlen(s);
    assert(n<sizeof(BUF));
    memcpy(BUF,s,n+1);
    vim_reset(&V);
    D=(vim_doc_t){.text=BUF,.len=n,.cap=sizeof(BUF)-1,.cursor=0,.changed=false};
    vim_clamp(&V,&D);
    LAST=VIM_ACT_NONE;
}

// One byte per key. 0x1b is Escape, '\r' is Enter and 0x7f is Backspace, which
// is exactly what the board and a host script send.
static void keys(const char *s) {
    for(const char *p=s;*p;p++) {
        vim_key_t k={VIM_KEY_TEXT,p,1};
        if(*p==0x1b)      k.kind=VIM_KEY_ESC;
        else if(*p=='\r') k.kind=VIM_KEY_ENTER;
        else if(*p==0x7f) k.kind=VIM_KEY_BACKSPACE;
        LAST=vim_feed(&V,&D,k);
    }
}

// A whole UTF-8 character as one keystroke, the way an IME commit arrives.
static void key_text(const char *s) {
    vim_key_t k={VIM_KEY_TEXT,s,strlen(s)};
    LAST=vim_feed(&V,&D,k);
}

static unsigned failures;
static void check(int ok, const char *what) {
    if(!ok) { printf("FAIL %s -- buf=[%s] cursor=%u\n",what,BUF,(unsigned)D.cursor);
              failures++; }
}
#define EQ(text,pos,what) do{ \
    check(D.len==strlen(text)&&!memcmp(BUF,text,D.len),what " (text)"); \
    check(D.cursor==(size_t)(pos),what " (cursor)"); }while(0)

int main(void) {
    // ---- motion ----------------------------------------------------------
    load("hello world\nsecond line\nthird");
    keys("ll");        EQ("hello world\nsecond line\nthird",2,"ll");
    keys("3l");        EQ("hello world\nsecond line\nthird",5,"3l");
    keys("$");         EQ("hello world\nsecond line\nthird",10,"$ stops on the last char");
    keys("l");         EQ("hello world\nsecond line\nthird",10,"l cannot leave the line");
    keys("0");         EQ("hello world\nsecond line\nthird",0,"0");
    keys("w");         EQ("hello world\nsecond line\nthird",6,"w");
    keys("w");         EQ("hello world\nsecond line\nthird",12,"w crosses the newline");
    keys("b");         EQ("hello world\nsecond line\nthird",6,"b");
    keys("e");         EQ("hello world\nsecond line\nthird",10,"e");
    keys("G");         EQ("hello world\nsecond line\nthird",24,"G");
    keys("gg");        EQ("hello world\nsecond line\nthird",0,"gg");
    keys("2G");        EQ("hello world\nsecond line\nthird",12,"2G");

    // j and k remember the column they started from, so a walk over a short
    // line comes back out where it went in.
    load("longer line\nab\nlonger line");
    keys("$");         EQ("longer line\nab\nlonger line",10,"$ before j");
    keys("j");         EQ("longer line\nab\nlonger line",13,"j clamps to a short line");
    keys("j");         EQ("longer line\nab\nlonger line",25,"j restores the column");
    keys("k");         EQ("longer line\nab\nlonger line",13,"k clamps again");

    load("  indented\nx");
    keys("$^");        EQ("  indented\nx",2,"^ finds the first non-blank");

    // ---- word classes ----------------------------------------------------
    load("foo.bar baz");
    keys("w");         EQ("foo.bar baz",3,"w stops at punctuation");
    keys("w");         EQ("foo.bar baz",4,"w leaves punctuation");
    load("foo.bar baz");
    keys("W");         EQ("foo.bar baz",8,"W ignores punctuation");
    load("one\n\ntwo");
    keys("w");         EQ("one\n\ntwo",4,"w stops on an empty line");
    keys("w");         EQ("one\n\ntwo",5,"w leaves an empty line");

    // ---- f t F T ; , -----------------------------------------------------
    load("a(b(c)d)e");
    keys("f(");        EQ("a(b(c)d)e",1,"f(");
    keys(";");         EQ("a(b(c)d)e",3,"; repeats f");
    keys(",");         EQ("a(b(c)d)e",1,", reverses f");
    load("a(b(c)d)e");
    keys("2f(");       EQ("a(b(c)d)e",3,"2f(");
    keys("t)");        EQ("a(b(c)d)e",4,"t stops before");
    keys("F(");        EQ("a(b(c)d)e",3,"F(");
    load("abc");
    keys("fz");        EQ("abc",0,"a find that misses does not move");

    // ---- operators, charwise ---------------------------------------------
    load("hello world");
    keys("dw");        EQ("world",0,"dw takes the trailing space");
    load("hello world");
    keys("wdw");       EQ("hello ",5,"dw at the end of a line");
    load("hello world");
    keys("d$");        EQ("",0,"d$ takes the last character too");
    load("one two three");
    keys("2dw");       EQ("three",0,"2dw");
    load("call(a, b)");
    keys("f(ldt)");    EQ("call()",5,"dt) leaves the paren");
    load("hello world");
    keys("x");         EQ("ello world",0,"x");
    keys("3x");        EQ("o world",0,"3x");
    keys("$X");        EQ("o word",5,"X deletes backwards");

    // `cw` on a word changes the word, not the space after it.
    load("hello world");
    keys("cwbye\x1b"); EQ("bye world",2,"cw behaves as ce");
    load("hello world");
    keys("wcwthere\x1b"); EQ("hello there",10,"cw on the last word");

    // ---- operators, linewise ---------------------------------------------
    load("one\ntwo\nthree");
    keys("jdd");       EQ("one\nthree",4,"dd takes the whole line");
    load("one\ntwo\nthree");
    keys("2dd");       EQ("three",0,"2dd");
    load("one\ntwo");
    keys("jdd");       EQ("one",0,"dd on the last line takes the newline before it");
    load("one\ntwo\nthree");
    keys("dj");        EQ("three",0,"dj is linewise");
    load("one\ntwo\nthree");
    keys("jdG");       EQ("one",0,"dG is linewise");
    load("  one\ntwo");
    keys("cc");        EQ("\ntwo",0,"cc empties the line and starts typing");
    check(vim_mode(&V)==VIM_INSERT,"cc enters insert");
    keys("new\x1b");   EQ("new\ntwo",2,"cc then typing");

    // ---- yank and put ----------------------------------------------------
    load("one\ntwo");
    keys("yyp");       EQ("one\none\ntwo",4,"yy then p puts a line below");
    load("one\ntwo");
    keys("yyP");       EQ("one\none\ntwo",0,"P puts a line above");
    load("one\ntwo");
    keys("jyyp");      EQ("one\ntwo\ntwo",8,"p after the last line makes its own break");
    load("abc");
    keys("ylp");       EQ("aabc",1,"charwise p goes after the cursor");
    load("abc");
    keys("yl3p");      EQ("aaaabc",3,"3p");
    load("one\ntwo\nthree");
    keys("2yyGp");     EQ("one\ntwo\nthree\none\ntwo",14,"2yy then p at the end");
    load("abc");
    keys("dd");        check(D.len==0,"dd on the only line");
    keys("p");         EQ("abc",0,"p restores it, adding no blank line");

    // ---- r, J, s, D, C, Y ------------------------------------------------
    load("abcd");
    keys("rZ");        EQ("Zbcd",0,"r");
    keys("3rx");       EQ("xxxd",2,"3r");
    keys("$3ry");      EQ("xxxd",3,"r refuses when the line is too short");
    load("one\n   two");
    keys("J");         EQ("one two",3,"J joins with one space");
    load("one\n)two");
    keys("J");         EQ("one)two",3,"J adds no space before a close paren");
    load("a\nb\nc");
    keys("3J");        EQ("a b c",3,"3J joins three lines");
    load("hello");
    keys("2sXY\x1b");  EQ("XYllo",1,"2s replaces two characters");
    load("hello world");
    keys("wD");        EQ("hello ",5,"D");
    load("hello world");
    keys("wCthere\x1b"); EQ("hello there",10,"C");
    load("one\ntwo");
    keys("YjP");       EQ("one\none\ntwo",4,"Y is yy");

    // ---- entering insert -------------------------------------------------
    load("bc");
    keys("iA\x1b");    EQ("Abc",0,"i");
    load("ac");
    keys("aB\x1b");    EQ("aBc",1,"a");
    load("  xy");
    keys("$IZ\x1b");   EQ("  Zxy",2,"I goes to the first non-blank");
    load("ab");
    keys("AC\x1b");    EQ("abC",2,"A goes past the last character");
    load("one\ntwo");
    keys("oX\x1b");    EQ("one\nX\ntwo",4,"o opens below");
    load("one\ntwo");
    keys("jOX\x1b");   EQ("one\nX\ntwo",4,"O opens above");
    load("");
    keys("iabc\x1b");  EQ("abc",2,"typing into an empty document");
    check(vim_mode(&V)==VIM_NORMAL,"Esc leaves insert");
    keys("\x1b");      check(LAST==VIM_ACT_QUIT_FORCE,"Esc from a clean normal leaves");
    keys("2d\x1b");    check(LAST==VIM_ACT_NONE,"Esc with a command pending only clears it");
    keys("x");         EQ("ab",1,"the cleared command did not run");

    // Backspace inside insert mode, including over a newline.
    load("ab");
    keys("A\x7f\x7fZ\x1b"); EQ("Z",0,"backspace in insert");
    load("a\nb");
    keys("jI\x7fX\x1b");    EQ("aXb",1,"backspace joins the line");

    // ---- undo ------------------------------------------------------------
    // A whole insert session is one change, which is the rule people rely on.
    load("hello");
    keys("Aworld\x1b"); EQ("helloworld",9,"typed a word");
    keys("u");          EQ("hello",4,"u takes back the whole insertion");
    keys("u");          check(!strcmp(vim_message(&V),"ALREADY AT OLDEST CHANGE"),
                              "nothing left to undo");
    load("one\ntwo\nthree");
    keys("dd");         EQ("two\nthree",0,"dd before undo");
    keys("dd");         EQ("three",0,"a second dd");
    keys("u");          EQ("two\nthree",0,"u restores one line");
    keys("u");          EQ("one\ntwo\nthree",0,"u restores the other");
    // Deleting more than the ring can hold clears the history rather than
    // half-restoring it.
    {
        char big[1200];
        memset(big,'x',sizeof(big)-1);
        big[sizeof(big)-1]=0;
        load(big);
        keys("d$");
        check(D.len==0,"a delete larger than the undo ring");
        keys("u");
        check(D.len==0 && !strcmp(vim_message(&V),"ALREADY AT OLDEST CHANGE"),
              "it is not undoable, and says so");
    }
    // The oldest changes fall off; the newest always come back.
    load("abcdefghij");
    for(int i=0;i<40;i++) keys("x");
    check(D.len==0,"forty deletes");
    keys("u");          check(D.len==1,"the newest delete still undoes");

    // ---- the ':' line ----------------------------------------------------
    load("a\nb");
    keys(":w\r");       check(LAST==VIM_ACT_SAVE,":w");
    check(vim_mode(&V)==VIM_NORMAL,":w returns to normal");
    keys(":q\r");       check(LAST==VIM_ACT_QUIT,":q");
    keys(":q!\r");      check(LAST==VIM_ACT_QUIT_FORCE,":q!");
    keys(":wq\r");      check(LAST==VIM_ACT_SAVE_QUIT,":wq");
    keys(":x\r");       check(LAST==VIM_ACT_SAVE_QUIT,":x");
    keys("ZZ");         check(LAST==VIM_ACT_SAVE_QUIT,"ZZ");
    keys(":zz\r");      check(LAST==VIM_ACT_NONE &&
                              !strcmp(vim_message(&V),"E492 NOT AN EDITOR COMMAND"),
                              "an unknown ex command");
    load("one\ntwo\nthree");
    keys(":3\r");       EQ("one\ntwo\nthree",8,":3 goes to line three");
    keys(":1\r");       EQ("one\ntwo\nthree",0,":1");
    keys(":w");         check(vim_mode(&V)==VIM_CMDLINE,"still typing the command");
    keys("\x1b");       check(vim_mode(&V)==VIM_NORMAL && LAST==VIM_ACT_NONE,
                              "Esc abandons the command line");
    keys(":w\x7f\x7f"); check(vim_mode(&V)==VIM_NORMAL,
                              "backspacing off the ':' leaves the line");

    // ---- search ----------------------------------------------------------
    load("alpha beta alpha gamma");
    keys("/beta\r");    EQ("alpha beta alpha gamma",6,"/beta");
    keys("/alpha\r");   EQ("alpha beta alpha gamma",11,"/alpha finds the next one");
    keys("n");          EQ("alpha beta alpha gamma",0,"n wraps");
    keys("N");          EQ("alpha beta alpha gamma",11,"N goes back");
    keys("?beta\r");    EQ("alpha beta alpha gamma",6,"?beta searches backwards");
    keys("/zzz\r");     check(!strcmp(vim_message(&V),"PATTERN NOT FOUND"),
                              "a search that finds nothing says so");

    // ---- UTF-8 -----------------------------------------------------------
    // The cursor must never land inside a character, and one commit from the
    // IME arrives as one keystroke of several bytes.
    load("あいう");
    keys("l");          EQ("あいう",3,"l steps a whole character");
    keys("x");          EQ("あう",3,"x deletes a whole character");
    keys("h");          EQ("あう",0,"h steps back a whole character");
    load("x");
    keys("i");
    key_text("漢字");
    keys("\x1b");       EQ("漢字x",3,"an IME commit inserts as one change");
    keys("u");          EQ("x",0,"and undoes as one");

    // A count that is refused should leave nothing half-done.
    load("abc");
    keys("99x");        EQ("",0,"a count past the end of the line stops there");

    if(failures) { printf("%u FAILED\n",failures); return 1; }
    printf("test_vimcmd: all checks passed\n");
    return 0;
}
