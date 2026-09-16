#include "lessons.h"

// The counting chapter changes one piece of text again and again, and the scene
// that holds it is five lines the editor has no room for once the frame
// function is there. It is given already made, run before the learner's lines
// and never shown, so the lines they see are only the ones they change. The
// drawing is apps/hello/main.js's, cut down to one piece of text.
static const char PRELUDE_T[] =
    "let t\n"
    "pocket.kasane.replace(tx => {\n"
    "  tx.background(0x2050a0ff)\n"
    "  t = tx.text({bounds: [40, 60, 240, 78], color: 0xffffffff,\n"
    "    text: 'COUNT 0', capacity: 32})\n"
    "})\n";

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
    .body={"replace は画面を描き直",
           "す係、background は背",
           "景の色。最後の行は見せ",
           "続ける係。0x000000ff",
           "を 0x2050a0ff に変えて実行"},
    .hint="Escで戻る。色は 0xRRGGBBff",
    .code="pocket.kasane.replace(tx => {\n"
          "  tx.background(0x000000ff)\n"
          "})\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=NULL,
    .check=CHECK_CHANGED, .want="0x000000ff",
},
{
    .title="文字を置く",
    .body={"tx.text は文字を置く係。",
           "bounds は置く場所、color",
           "は色、text は中身です。",
           "まず実行、次に HELLO を",
           "好きな英字に変えて実行。"},
    .hint="日本語は9章で。まず英字で",
    .code="pocket.kasane.replace(tx => {\n"
          "  tx.background(0x2050a0ff)\n"
          "  tx.text({bounds: [40, 60, 240, 78],\n"
          "    color: 0xffffffff, text: 'HELLO'})\n"
          "})\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=NULL,
    .check=CHECK_CHANGED, .want="'HELLO'",
},
{
    .title="色と場所を変える",
    .body={"bounds は [左,上,右,下]",
           "のドット位置。画面は横",
           "240 縦135。color は文字",
           "の色。数字を変えて、文字",
           "を右下の赤にしてみよう。"},
    .hint="赤は 0xff4040ff。下は135まで",
    .code="pocket.kasane.replace(tx => {\n"
          "  tx.background(0x2050a0ff)\n"
          "  tx.text({bounds: [40, 60, 240, 78],\n"
          "    color: 0xffffffff, text: 'HELLO'})\n"
          "})\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=NULL,
    .check=CHECK_CHANGED, .want="0xffffffff",
},
{
    .title="ボタンで数える",
    .body={"frame の係は画面が出てい",
           "る間ずっと呼ばれ、b には",
           "押されたキー。b & 0x4000",
           "は Enter のこと。patch は",
           "t の文字だけを書き換える。"},
    .hint="Enter で数が増えれば合格",
    .code="let n = 0\n"
          "globalThis.frame = (b) => {\n"
          "  if (b & 0x4000) {\n"
          "    n = n + 1\n"
          "    pocket.kasane.patch(tx => t.setText(tx, 'COUNT ' + n))\n"
          "    print('ENTER ' + n) } }\n",
    .preload=true, .prelude=PRELUDE_T,
    .check=CHECK_PRINTS_PREFIX, .want="ENTER ",
},
{
    .title="おまけ 日本語",
    .body={"日本語もそのまま書けます。",
           "font は字体で 'body' が",
           "日本語向き。Ctrl+J で",
           "かな入力（もう一度で戻る）",
           "'こんにちは' を名前に。"},
    .hint="そのまま実行でも合格",
    .code="pocket.kasane.replace(tx => {\n"
          "  tx.background(0x2050a0ff)\n"
          "  tx.text({bounds: [40, 60, 240, 78], font: 'body',\n"
          "    color: 0xffffffff, text: 'こんにちは'})\n"
          "})\n"
          "globalThis.frame = () => {}\n",
    .preload=true, .prelude=NULL,
    .check=CHECK_JAPANESE, .want="'body'",
},
};

#define LESSON_N (sizeof(LESSONS)/sizeof(LESSONS[0]))

unsigned lesson_count(void) { return LESSON_N; }

const lesson_t *lesson_at(unsigned index) {
    return index<LESSON_N ? &LESSONS[index] : &LESSONS[0];
}

// The calls and numbers stay opaque however carefully a chapter explains them,
// so they are also gathered where Tab can reach them from any chapter.
static const char *const REFERENCE[LESSON_REF_ROWS] = {
    "pocket.kasane の書き方",
    "replace(tx=>{}) 描き直す",
    "patch(tx=>{}) 一部だけ",
    "tx.background(色) 背景",
    "tx.text({...}) 文字",
    "bounds [左,上,右,下]",
    "色 0xRRGGBBff 画面240x135",
};

const char *lesson_reference(unsigned row) {
    return row<LESSON_REF_ROWS ? REFERENCE[row] : "";
}
