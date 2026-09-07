#include "lessons.h"

// Building a text node takes eight lines, and the editor shows six. Chapters
// that work on one are given it already made, prepended at run time and never
// shown, so the lines the learner sees are only the ones they change. The
// property numbers match apps/hello/main.js.
static const char PRELUDE_T[] =
    "ui.setProp(1, 64, 0x2050a0ff);\n"
    "const t = ui.createNode(1);\n"
    "ui.setProp(t, 24, 1);\n"
    "ui.setProp(t, 28, 40); ui.setProp(t, 25, 60);\n"
    "ui.setProp(t, 1, 200); ui.setProp(t, 2, 18);\n"
    "ui.setProp(t, 96, 0xffffffff); ui.setProp(t, 97, 1);\n"
    "ui.setText(t, '');\n"
    "ui.insertBefore(1, t, 0);\n";

static const lesson_t LESSONS[] = {
{
    .title="しゃべらせてみる",
    .body={"まず hello と出します。",
           "下の1行をそのまま打ち、",
           "Ctrl+R を押してください。",
           "print は「出せ」の合図、",
           "' 'の中身がそのまま出ます。"},
    .hint="赤い字が出たら綴りを確認",
    .code="print('hello')\n",
    .preload=false, .prelude=NULL,
    .check=CHECK_PRINTS, .want="hello",
},
{
    .title="間違いを読む",
    .body={"わざと間違えます。",
           "print を prnt に変えて",
           "Ctrl+R を押すと赤い字で",
           "prnt is not defined",
           "と出ます。直せば消えます。"},
    .hint="意味は「prnt は知らない」",
    .code="prnt('hello')\n",
    .preload=false, .prelude=NULL,
    .check=CHECK_ERROR_THEN_PRINTS, .want="is not defined", .want2="hello",
},
{
    .title="計算させる",
    .body={"数を出すときは ' '",
           "で囲みません。",
           "print(1 + 2) で 3、",
           "print('1 + 2') だと",
           "そのまま 1 + 2 と出ます。"},
    .hint="下の欄に 3 が出れば合格",
    .code="print(1 + 2)\n",
    .preload=false, .prelude=NULL,
    .check=CHECK_PRINTS, .want="3",
},
{
    .title="箱に入れる",
    .body={"同じ数を何度も使うなら",
           "箱に入れます。",
           "let a = 5 は「a という",
           "箱を用意して 5 を入れる」",
           "a * 2 で中身を2倍。"},
    .hint="10 と 6 が出れば合格",
    .code="let a = 5\nprint(a * 2)\nprint(a + 1)\n",
    .preload=false, .prelude=NULL,
    .check=CHECK_PRINTS_BOTH, .want="10", .want2="6",
},
{
    .title="画面を塗る",
    .body={"最後の行は「画面を見せ",
           "続ける係」。無いと絵は出",
           "ません。1は画面、64は背",
           "景の色。0x000000ff を",
           "0x2050a0ff に変えて実行。"},
    .hint="Escで戻る。色は 0xRRGGBBff",
    .code="ui.setProp(1, 64, 0x000000ff)\nglobalThis.frame = () => {}\n",
    .preload=true, .prelude=NULL,
    .check=CHECK_CHANGED, .want="0x000000ff",
},
{
    .title="文字を置く",
    .body={"t は画面に貼った文字の札",
           "です。setText は「札の",
           "文字を書き換える」。まず",
           "実行、次に HELLO を好きな",
           "英字に変えて実行。"},
    .hint="日本語は9章で。まず英字で",
    .code="ui.setText(t, 'HELLO')\nglobalThis.frame = () => {}\n",
    .preload=true, .prelude=PRELUDE_T,
    .check=CHECK_CHANGED, .want="'HELLO'",
},
{
    .title="色と場所を変える",
    .body={"28 は左から、25 は上から",
           "何ドット目か。画面は横240",
           "縦135。96 は文字の色。",
           "数字を変えて、文字を",
           "右下の赤にしてみよう。"},
    .hint="赤は 0xff4040ff",
    .code="ui.setProp(t, 28, 40)\n"
          "ui.setProp(t, 25, 60)\n"
          "ui.setProp(t, 96, 0xffffffff)\n"
          "ui.setText(t, 'HELLO')\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=PRELUDE_T,
    .check=CHECK_CHANGED, .want="0xffffffff",
},
{
    .title="ボタンで数える",
    .body={"最後の行の係は、画面が出て",
           "いる間ずっと呼ばれ、b に",
           "押されたキーが入ります。",
           "if (b & 0x4000) は「もし",
           "Enter が押されたら」。"},
    .hint="Enter で数が増えれば合格",
    .code="let n = 0\n"
          "globalThis.frame = (b) => {\n"
          "  if (b & 0x4000) {\n"
          "    n = n + 1\n"
          "    ui.setText(t, 'COUNT ' + n)\n"
          "    print('ENTER ' + n) } }\n",
    .preload=true, .prelude=PRELUDE_T,
    .check=CHECK_PRINTS_PREFIX, .want="ENTER ",
},
{
    .title="おまけ 日本語",
    .body={"97 は字体の番号。2 に",
           "すると日本語が使えます。",
           "Ctrl+J でかな入力に切替",
           "（もう一度押すと戻る）。",
           "'こんにちは' を名前に。"},
    .hint="そのまま実行でも合格",
    .code="ui.setProp(t, 97, 2)\n"
          "ui.setText(t, 'こんにちは')\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=PRELUDE_T,
    .check=CHECK_JAPANESE, .want="97, 2)",
},
};

#define LESSON_N (sizeof(LESSONS)/sizeof(LESSONS[0]))

unsigned lesson_count(void) { return LESSON_N; }

const lesson_t *lesson_at(unsigned index) {
    return index<LESSON_N ? &LESSONS[index] : &LESSONS[0];
}

// Numbers stay opaque however carefully a chapter explains them, so they are
// also gathered where Tab can reach them from any chapter.
static const char *const REFERENCE[LESSON_REF_ROWS] = {
    "番号の意味 (ui.setProp)",
    " 1  画面そのもの",
    "64  背景の色",
    "28  左から何ドット",
    "25  上から何ドット",
    "96  文字の色",
    "97  字体 0:小 1:大 2:日本語",
};

const char *lesson_reference(unsigned row) {
    return row<LESSON_REF_ROWS ? REFERENCE[row] : "";
}
