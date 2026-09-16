// Default initializers call before rest is materialized, including resumable floors.
function inner() { return 7; }
function args(a = inner(), ...rest) {
    return [a, arguments.length, rest.join("/"), this.id].join(":");
}
const receiver = {id: "receiver", args};
print("default-rest", receiver.args(undefined, 2, 3));
print("missing-rest", receiver.args());
function* gen(a = inner(), ...rest) {
    yield [a, arguments.length, rest.join("/"), this.id].join(":");
    inner();
    return this.id;
}
let g = gen.call(receiver, undefined, 4, 5);
print("generator", g.next().value, g.next().value);
async function af(a = inner(), ...rest) {
    await Promise.resolve();
    inner();
    return [a, arguments.length, rest.join("/"), this.id].join(":");
}
af.call(receiver, undefined, 6, 8).then(x => print("async", x));
class Base {
    constructor(a = inner(), ...rest) {
        this.value = [a, rest.join("/"), new.target.name].join(":");
    }
}
class Child extends Base {
    constructor(a = inner(), ...rest) {
        inner();
        super(a, ...rest);
        inner();
        this.kind = new.target.name;
    }
}
let child = new Child(undefined, 9, 10);
print("constructor", child.value, child.kind);
class Implicit extends Base {}
print("implicit", new Implicit(undefined, 11).value);
function mapped(a, b) {
    inner();
    arguments[0] = 12;
    inner();
    return [a, b, arguments.length].join(":");
}
print("mapped", mapped(1, 2));
function makeTail() {
    "use strict";
    return function tail(n, a = inner(), ...rest) {
        // Fixed arity permits the optional TCO path to reuse this frame.
        if (n) return tail(n - 1, undefined, rest[0], rest[1]);
        return [a, rest.join("/")].join(":");
    };
}
const tail = makeTail();
print("tail", tail(20, undefined, 13, 14));
