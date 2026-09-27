// RegExp compiler+matcher coverage: libregexp.c shares the DynBuf that the
// bytecode compiler had the OOM bugs in (docs/vm/oom-parse-safety.md), but
// has never been swept. Deterministic output only.

// Character classes, quantifiers, alternation.
print("cc1", /[a-zA-Z0-9_]+/.test("var_1"));
print("cc2", /[^abc]+/.exec("xxabcyy")[0]);
print("cc3", "a1 b22 c333".match(/[a-z]\d+/g).join(","));
print("alt1", /cat|dog|bird/.test("I have a dog"));
print("q1", /a{2,4}/.exec("aaaaa")[0]);
print("q2", /a{3,}/.exec("aaaaa")[0]);
print("lazy1", /<.+?>/.exec("<a><b>")[0]);
print("greedy1", /<.+>/.exec("<a><b>")[0]);
print("star", /go*gle/.test("gggle"));
print("plus", /go+gle/.test("gogle"));
print("opt", /colou?r/.test("color") && /colou?r/.test("colour"));

// Anchors, boundaries, dot-all.
print("anchor1", /^abc$/.test("abc"));
print("wb1", /\bcat\b/.test("the cat sat"));
print("dotall", /a.b/s.test("a\nb"));
print("multi", "a\nb".match(/^./gm).join(","));

// Groups: capturing, non-capturing, named, backreferences.
print("grp1", /(\d+)-(\d+)/.exec("12-34").slice(1).join(","));
print("grp2", /(?:foo)(bar)/.exec("foobar")[1]);
print("named1", /(?<y>\d{4})-(?<m>\d{2})/.exec("2026-09").groups.y);
print("backref1", /(\w)\1/.test("hello"));
print("named-backref", /(?<c>.)\k<c>/.test("aa"));

// Lookahead / lookbehind.
print("la1", /foo(?=bar)/.test("foobar"));
print("nla1", /foo(?!bar)/.test("foobaz"));
print("lb1", /(?<=\$)\d+/.exec("$100")[0]);
print("nlb1", /(?<!\$)\d+/.test("100"));

// Case-insensitive, sticky, global, unicode.
print("ci1", /HELLO/i.test("hello"));
print("sticky1", (() => { const r = /foo/y; r.lastIndex = 3; return r.test("xxxfoo"); })());
print("global1", "aXbXcX".split(/X/g).join(","));
print("u1", /\u{1F600}/u.test("\u{1F600}"));
print("u2", /\p{Letter}+/u.test("hello"));
try { print("v1", new RegExp("[\\p{Letter}--[a-z]]", "v").test("A")); } catch (e) { print("v1-unsupported", e.constructor.name); }

// String.prototype methods with regexps.
print("replace1", "2026-09-23".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"));
print("replaceAll1", "a-b-c".replaceAll(/-/g, "_"));
print("replFn", "abc".replace(/b/, (m) => m.toUpperCase()));
print("matchAll1", [..."a1b2c3".matchAll(/[a-z](\d)/g)].map((m) => m[1]).join(","));
print("split1", "one, two,  three".split(/,\s*/).join("|"));
print("search1", "hello world".search(/wor/));

// Large/complex patterns: exercise bigger bytecode buffers, more DynBuf growth.
const bigAlt = new RegExp(Array.from({ length: 60 }, (_, i) => "w" + i).join("|"));
print("bigalt", bigAlt.test("w42"));
const bigClass = new RegExp("[" + Array.from({ length: 40 }, (_, i) => String.fromCharCode(97 + (i % 26))).join("") + "]{5,20}");
print("bigclass", bigClass.test("abcdefghij"));
const nestedGroups = /((((a)(b))(c))(d))/;
print("nested", nestedGroups.exec("abcd").slice(1).join(","));
const manyGroups = new RegExp(Array.from({ length: 20 }, (_, i) => "(g" + i + ")?").join(""));
print("manygroups", manyGroups.test("g0g1g2"));

// Compile-error paths: must fail cleanly, not just under OOM.
try { new RegExp("("); } catch (e) { print("badsyntax", e.constructor.name); }
try { new RegExp("a{2,1}"); } catch (e) { print("badquant", e.constructor.name); }
try { new RegExp("[z-a]"); } catch (e) { print("badrange", e.constructor.name); }

// toString/source/flags round trip and exec state (lastIndex on global).
const gRe = /\d+/g;
let cnt = 0, mm;
while ((mm = gRe.exec("1 22 333")) !== null) { cnt++; if (cnt > 10) break; }
print("execloop", cnt, gRe.lastIndex);
print("props", /abc/gimsuy.source, /abc/gimsuy.flags);
