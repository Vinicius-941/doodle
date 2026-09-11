// Compiler + VM self-check: one Doo object exercising the language subset.
#include <cstdio>
#include "compiler.h"

#define CHECK(c) if (!(c)) { printf("FAIL linha %d: %s\n", __LINE__, #c); return 1; }

static const char* src = R"(
object T

var n = 0
var hex = 0xFF

function create() {
    n = fib(10)   // calls a function declared below
}

function fib(k) {
    if (k < 2) { return k }
    return fib(k - 1) + fib(k - 2)
}

function loops() {
    var s = 0
    for (var i = 0; i < 5; i += 1) { s += i }
    var j = 0
    while (j < 3) { j += 1 }
    return s * 10 + j
}

function logic() {
    var a = [1, 2, 3]
    a[1] += 5
    return (a[1] == 7 && !false) || explode()   // short-circuit: explode() never runs
}

function explode() {
    var a = []
    return a[5]
}

function strings() { return "n=" + n + " " + len([1, 2]) + " " + (2 % 3 - 0.5) }

var pos = vec3(1, 2, 3)

function vectors() {
    pos.y += 10                        // component write-back into a field
    var v = pos + vec3(1, 1, 1) * 2
    v.x = -v.x / 3
    return v
}

function mathy() { return math.floor(math.sqrt(16.5)) + math.abs(-1) }
)";

static bool throws(const char* code, const VM& vm, const char* expectedPrefix) {
    try { compile(code, "x.doo", vm, false); }
    catch (const DooError& e) { return std::string(e.what()).rfind(expectedPrefix, 0) == 0; }
    return false;
}

int main() {
    VM vm;
    vm.addNative("system.launch", [](Instance&, std::vector<Value>&) { return Value(); });

    auto t = vm.instantiate(compile(src, "t.doo", vm, false));
    CHECK(std::get<double>(t->fields[0]) == 55);
    CHECK(std::get<double>(t->fields[1]) == 255);
    CHECK(std::get<double>(vm.call(*t, "loops")) == 103);
    CHECK(std::get<bool>(vm.call(*t, "logic")) == true);
    CHECK(std::get<std::string>(vm.call(*t, "strings")) == "n=55 2 1.5");
    CHECK((std::get<Vec3>(vm.call(*t, "vectors")) == Vec3{-1, 14, 5}));
    CHECK((std::get<Vec3>(t->fields[2]) == Vec3{1, 12, 3}));
    CHECK(std::get<double>(vm.call(*t, "mathy")) == 5);

    try { vm.call(*t, "explode"); CHECK(false); }
    catch (const DooError& e) { CHECK(std::string(e.what()).rfind("t.doo:32:", 0) == 0); }  // runtime errors carry file:line

    CHECK(throws("object X\nfunction f() {\n y = 1 }", vm, "x.doo:3:"));                  // undeclared variable
    CHECK(throws("object X\nfunction f() { g(1) }\nfunction g() {}", vm, "x.doo:2:"));    // wrong arity
    CHECK(throws("object X\nfunction f() { system.launch(\"a\") }", vm, "x.doo:2:"));     // firmware-only API
    CHECK(throws("object X\nfunction f() { var v = vec3()\n return v.w }", vm, "x.doo:3:")); // vec3 has only x/y/z
    CHECK(compile("object X\nfunction f() { system.launch(\"a\") }", "fw.doo", vm, true)); // ...allowed when privileged

    puts("doo_test: ok");
    return 0;
}
