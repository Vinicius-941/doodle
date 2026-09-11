// Compiler + VM self-check: one Doo object exercising the language subset.
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "compiler.h"
#include "obj.h"
#include "physics.h"
#include "wav.h"

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

// Objects of one program talking through refs: spawn, method calls, fields, type().
static const SourceFile levelSrc = {"level.doo", R"(
object Level
var e
function create() {
    e = spawn(Enemy, vec3(1, 2, 3))
    e.take_damage(30)
    e.hp -= 5
}
function report() { return type(e) + " " + e.hp + " " + e.position.y }
)"};
static const SourceFile enemySrc = {"enemy.doo", R"(
object Enemy
use SphereCollider
var hp = 100
function take_damage(n) { hp -= n }
)"};

// Physics: a ball dropped on a floor comes to rest on top of it and reports the contact.
static const SourceFile ballSrc = {"ball.doo", R"(
object Ball
use Rigidbody
use SphereCollider
var hits = 0
function on_collision(other) { if (type(other) == Floor) { hits += 1 } }
)"};
static const SourceFile floorSrc = {"floor.doo", "object Floor\nuse BoxCollider\n"};

static bool throws(const char* code, const VM& vm, const char* expectedPrefix) {
    try { compile(code, "x.doo", vm, false); }
    catch (const DooError& e) { return std::string(e.what()).rfind(expectedPrefix, 0) == 0; }
    return false;
}

int main() {
    VM vm;
    registerPhysics(vm);
    vm.addNative("system.launch", [](Instance&, std::vector<Value>&) { return Value(); });
    std::unordered_map<std::string, std::shared_ptr<ObjectDef>> defs;
    std::vector<std::shared_ptr<Instance>> scene;
    auto spawn = [&](const std::string& name, Vec3 pos) {  // same contract as the simulator's spawn()
        auto inst = vm.instantiate(defs.at(name));
        if (Value* p = inst->field("position")) *p = Value(pos);
        scene.push_back(inst);
        vm.call(*inst, "create");
        return inst;
    };
    vm.addNative("spawn", [&](Instance&, std::vector<Value>& a) { return Value(Ref{spawn(std::get<std::string>(a[0]), argVec(a, 1))}); });

    auto t = vm.instantiate(compile(src, "t.doo", vm, false));
    vm.call(*t, "create");
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
    CHECK(throws("object X\nuse Foo", vm, "x.doo:2:"));                                   // unknown component
    CHECK(compile("object X\nfunction f() { system.launch(\"a\") }", "fw.doo", vm, true)); // ...allowed when privileged

    for (auto& d : compileAll({levelSrc, enemySrc, ballSrc, floorSrc}, vm, false)) defs[d->name] = d;
    auto level = spawn("Level", {});
    CHECK(std::get<std::string>(vm.call(*level, "report")) == "Enemy 65 2");

    auto ball = spawn("Ball", {0, 3, 0});
    auto ground = spawn("Floor", {0, -0.5, 0});
    *ground->field("size") = Value(Vec3{10, 1, 10});
    for (int i = 0; i < 120; i++) physicsStep(vm, scene, 1.0 / 60);
    Vec3 p = std::get<Vec3>(*ball->field("position"));
    CHECK(std::fabs(p.y - 0.5) < 0.01 && p.x == 0);  // resting on the floor (radius 0.5)
    CHECK(std::get<bool>(*ball->field("grounded")));
    CHECK(std::get<double>(*ball->field("hits")) > 0);

    auto roller = spawn("Ball", {3, 0.5, 0});  // rolling into a wall stops at its face
    *roller->field("velocity") = Value(Vec3{5, 0, 0});
    auto wall = spawn("Floor", {4.5, 2, 0});
    *wall->field("size") = Value(Vec3{1, 4, 10});
    for (int i = 0; i < 60; i++) physicsStep(vm, scene, 1.0 / 60);
    CHECK(std::fabs(std::get<Vec3>(*roller->field("position")).x - 3.5) < 0.01);

    // .obj: a quad (with a relative -1 index) fanned into 2 triangles, flat normal, material from the .mtl
    auto parts = parseObj("mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 1\nusemtl tijolo\nf 1/1 2/1 3/2 -1/2\n",
                          [](const std::string& f) { return f == "m.mtl" ? "newmtl tijolo\nKd 1 0.5 0\nmap_Kd tijolo.png\n" : ""; });
    CHECK(parts.size() == 1 && parts[0].tris.size() == 6);
    CHECK(parts[0].texture == "tijolo.png" && parts[0].color.y == 0.5);
    CHECK((parts[0].tris[0].normal == Vec3{0, 0, 1}));
    CHECK((parts[0].tris[5].pos == Vec3{0, 1, 0}) && parts[0].tris[5].u == 1);

    // .wav: a 16-byte PCM fmt, then an odd-sized chunk (padded to even) before the data
    auto le32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    Wav w = parseWav("RIFF" + le32(0) + "WAVE" + "fmt " + le32(16) + std::string(16, '\x01') + "LIST" + le32(3) + "abc" +
                     '\0' + "data" + le32(4) + "wxyz");
    CHECK(w.format.size() == 18 && w.format[17] == 0 && w.format[0] == 1);
    CHECK(std::string(w.data.begin(), w.data.end()) == "wxyz");

    puts("doo_test: ok");
    return 0;
}
