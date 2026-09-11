// Compiler + VM self-check: one Doo object exercising the language subset.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include "compiler.h"
#include "obj.h"
#include "physics.h"
#include "save.h"
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

function arrays() {
    var a = []
    push(a, 3)
    push(a, 4)
    return len(a) + a[1] + math.max(2, 7) + math.min(2, 7)
}
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

// Inheritance: merged fields/functions, derived initializers win, virtual calls, super, errors in the parent's file.
static const SourceFile animalSrc = {"animal.doo", R"doo(object Animal
var sound = "..."
var legs = 4
function name() { return "animal" }
function speak() { return name() + " diz " + sound }
function describe() { return speak() + " (" + legs + " patas)" }
function boom() { var a = []
 return a[1] }
)doo"};
static const SourceFile birdSrc = {"bird.doo", R"doo(object Bird extends Animal
var sound = "piu"
var legs = 2
var wings = 2
function name() { return "pássaro" }
function describe() { return super.describe() + " e " + wings + " asas" }
)doo"};

static bool throws(const char* code, const VM& vm, const char* expectedPrefix) {
    try { compile(code, "x.doo", vm, false); }
    catch (const DooError& e) { return std::string(e.what()).rfind(expectedPrefix, 0) == 0; }
    return false;
}

static std::string readPrefab(const std::string& name) {  // the real SDK prefab sources
    std::ifstream f(std::string(DOODLE_ROOT "/sdk/prefabs/") + name + ".doo", std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static bool throwsAll(const std::vector<SourceFile>& files, const std::vector<SourceFile>& library, const VM& vm, const char* prefix) {
    try { compileAll(files, vm, false, library); }
    catch (const DooError& e) { return std::string(e.what()).rfind(prefix, 0) == 0; }
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
    vm.addNative("spawn", [&](Instance&, std::vector<Value>& a) {
        return Value(Ref{spawn(std::get<std::string>(a[0]), a.size() > 1 ? argVec(a, 1) : Vec3{})});
    });

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
    CHECK(std::get<double>(vm.call(*t, "arrays")) == 15);

    try { vm.call(*t, "explode"); CHECK(false); }
    catch (const DooError& e) { CHECK(std::string(e.what()).rfind("t.doo:32:", 0) == 0); }  // runtime errors carry file:line

    CHECK(throws("object X\nfunction f() {\n y = 1 }", vm, "x.doo:3:"));                  // undeclared variable
    CHECK(throws("object X\nfunction f() { g(1) }\nfunction g() {}", vm, "x.doo:2:"));    // wrong arity
    CHECK(throws("object X\nfunction f() { system.launch(\"a\") }", vm, "x.doo:2:"));     // firmware-only API
    CHECK(throws("object X\nuse Foo", vm, "x.doo:2:"));                                   // unknown component
    CHECK(compile("object X\nfunction f() { system.launch(\"a\") }", "fw.doo", vm, true)); // ...allowed when privileged

    auto birds = compileAll({birdSrc}, vm, false, {animalSrc});  // Animal comes from the library, like an SDK prefab
    CHECK(birds.size() == 2 && birds[0]->name == "Bird");
    auto bird = vm.instantiate(birds[0]);
    CHECK(std::get<std::string>(vm.call(*bird, "describe")) == "pássaro diz piu (2 patas) e 2 asas");
    std::vector<Value> isArgs = {Value(Ref{bird}), Value(std::string("Animal"))};
    CHECK(std::get<bool>(vm.natives[vm.nativeIndex.at("is")](*bird, isArgs)));
    try { vm.call(*bird, "boom"); CHECK(false); }
    catch (const DooError& e) { CHECK(std::string(e.what()).rfind("animal.doo:8:", 0) == 0); }  // inherited code: parent's file
    auto own = compileAll({{"meu.doo", "object Animal\nfunction name() { return \"meu\" }"}}, vm, false, {animalSrc});
    CHECK(own.size() == 1 && std::get<std::string>(vm.call(*vm.instantiate(own[0]), "name")) == "meu");  // program beats library
    SourceFile a = {"a.doo", "object A\nfunction name() { return 1 }"};
    CHECK(throwsAll({{"b.doo", "object B extends A\nfunction name(x) { return x }"}}, {a}, vm, "b.doo:2:"));  // override arity
    CHECK(throwsAll({{"c.doo", "object C extends Nada"}}, {}, vm, "c.doo:1:"));                                // unknown parent
    CHECK(throwsAll({{"d.doo", "object D extends A\nfunction f() { return super.f() }"}}, {a}, vm, "d.doo:2:"));  // nothing to super
    std::vector<Value> none;
    double r = std::get<double>(vm.natives[vm.nativeIndex.at("math.random")](*bird, none));
    CHECK(r >= 0 && r < 1);

    // SDK prefab ParticleSystem (the real file): a burst draws each live particle, then it removes itself
    int meshes = 0;
    vm.constants["Mesh.Cube"] = Value(0.0);
    vm.addNative("render.mesh", [&](Instance&, std::vector<Value>&) { meshes++; return Value(); });
    vm.addNative("time.delta", [](Instance&, std::vector<Value>&) { return Value(0.1); });
    auto fx = vm.instantiate(compile(readPrefab("ParticleSystem"), "ParticleSystem.doo", vm, false));
    vm.call(*fx, "create");
    vm.call(*fx, "burst", {Value(16.0)});
    vm.call(*fx, "update");
    vm.call(*fx, "draw");
    CHECK(meshes == 16 && fx->alive);
    for (int i = 0; i < 9; i++) vm.call(*fx, "update");  // past the 0.8 s lifetime
    CHECK(!fx->alive);                                   // auto_destroy

    // SDK UI prefabs (real files): the Canvas moves focus spatially, A clicks, left/right adjust a slider
    int pressed = -1;
    const char* buttons[] = {"Button.Up", "Button.Down", "Button.Left", "Button.Right", "Button.A"};
    for (int i = 0; i < 5; i++) vm.constants[buttons[i]] = Value(double(i));
    vm.addNative("input.just_pressed", [&](Instance&, std::vector<Value>& a) { return Value((int)argNum(a, 0) == pressed); });
    for (const char* n : {"render.text", "render.rect", "audio.play"}) vm.addNative(n, [](Instance&, std::vector<Value>&) { return Value(); });
    vm.addNative("render.text_width", [](Instance&, std::vector<Value>&) { return Value(0.0); });
    vm.addNative("render.image", [](Instance&, std::vector<Value>&) { return Value(false); });
    vm.addNative("time.unscaled_delta", [](Instance&, std::vector<Value>&) { return Value(0.016); });
    std::vector<SourceFile> ui;
    for (const char* n : {"UIElement", "Text", "Image", "Button", "Slider", "Canvas"}) ui.push_back({n, readPrefab(n)});
    const char* uiTest = "object UITest\nvar menu\nvar b1\nvar b2\nvar s\nfunction create() {\n menu = spawn(Canvas)\n"
                         " b1 = menu.button(\"Um\", 100, 100, 200, 40)\n b2 = menu.button(\"Dois\", 100, 160, 200, 40)\n"
                         " s = menu.slider(\"Vol\", 100, 220, 200, 0, 100, 50)\n}";
    for (auto& d : compileAll({{"ui.doo", uiTest}}, vm, false, ui)) defs[d->name] = d;
    auto ut = spawn("UITest", {});
    auto canvas = std::get<Ref>(*ut->field("menu")).p.lock();
    auto step = [&](int button) { pressed = button; vm.call(*canvas, "update"); };
    auto uiField = [&](const char* ref, const char* f) { return *std::get<Ref>(*ut->field(ref)).p.lock()->field(f); };
    step(-1);          // nothing pressed: focus goes to the first button
    CHECK(std::get<double>(*canvas->field("focus")) == 0);
    step(1);           // down, down: the slider
    step(1);
    CHECK(std::get<double>(*canvas->field("focus")) == 2);
    step(3);           // right on the slider: 50 -> 60
    CHECK(std::get<double>(uiField("s", "value")) == 60 && std::get<bool>(uiField("s", "changed")));
    step(0);           // up to "Dois", then A
    step(4);
    CHECK(std::get<bool>(uiField("b2", "clicked")) && !std::get<bool>(uiField("b1", "clicked")));

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

    // save data round-trip: exact numbers, strings with escapes, bools
    std::map<std::string, Value> save = {{"x", Value(0.1 + 0.2)}, {"nome", Value(std::string("a\\b\n\tc\""))}, {"ok", Value(true)}};
    auto back = decodeSave(encodeSave(save));
    CHECK(back.size() == 3 && std::get<double>(back["x"]) == 0.1 + 0.2);
    CHECK(std::get<std::string>(back["nome"]) == "a\\b\n\tc\"" && std::get<bool>(back["ok"]));

    puts("doo_test: ok");
    return 0;
}
