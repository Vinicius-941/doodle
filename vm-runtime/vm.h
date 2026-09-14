// Doo runtime: values, bytecode and the VM that executes it.
#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

struct Value;
struct Instance;
using Array = std::vector<Value>;

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

// ref<Object>: managed reference to an instance; it stops working once the instance is destroyed.
struct Ref {
    std::weak_ptr<Instance> p;
    bool operator==(const Ref& o) const { return !p.owner_before(o.p) && !o.p.owner_before(p); }
};

// nil | bool | number | string | array | vec3 | ref (arrays and refs are shared; vec3 is copied)
using ValueBase = std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, Vec3, Ref>;
struct Value : ValueBase { using ValueBase::ValueBase; };

// Error already tagged with "file:line:" — thrown by the compiler and the VM.
struct DooError : std::runtime_error { using runtime_error::runtime_error; };

enum Op : int {
    OP_CONST, OP_NIL, OP_POP, OP_DUP,
    OP_GET_LOCAL, OP_SET_LOCAL, OP_GET_FIELD, OP_SET_FIELD,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_NOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_JMP, OP_JF, OP_JT,   // absolute target; JF/JT pop the condition
    OP_CALL, OP_NATIVE,     // operands: index, argc
    OP_RET, OP_ARRAY, OP_INDEX, OP_SET_INDEX,
    OP_GET_MEMBER, OP_SET_MEMBER,  // operand: name const (vec3 x/y/z or instance field); SET pops [obj, value], pushes obj
    OP_INVOKE,                     // obj.method(args) — operands: name const, argc
    OP_SELF, OP_OTHER,             // ref para quem está rodando / para quem abriu o with
    OP_WITH_SELF,                  // pops ref e passa a rodar como ela; operand: para onde pular se ela já morreu
    OP_WITH_RESTORE,               // volta a rodar como quem estava antes do with
};

struct Function {
    std::string name, file;  // file: where the source is (an inherited function lives in the parent's file)
    int arity = 0, nlocals = 0;
    std::vector<int> code, lines;  // lines[i] = source line of code[i]
    std::vector<Value> consts;
};

// One .doo file = one object. With `extends`, the parent's fields, components and functions are
// merged in at compile time (single inheritance), so an instance is always one flat ObjectDef.
struct ObjectDef {
    std::string name, file;
    std::vector<std::string> kinds;   // this object, then its parent, grandparent... (for is())
    std::vector<std::string> fields;
    std::vector<std::string> uses;    // components: `use Rigidbody`
    std::vector<Function> funcs;      // funcs[0] = __init (field initializers)
    std::unordered_map<std::string, int> funcIndex;  // "f" = most derived version, "Parent.f" = each version
};

struct Instance : std::enable_shared_from_this<Instance> {  // always created by make_shared (VM::instantiate)
    std::shared_ptr<ObjectDef> def;
    std::vector<Value> fields;
    bool alive = true;

    Value* field(const std::string& name) {  // nullptr if the object has no such field
        for (size_t i = 0; i < def->fields.size(); i++) if (def->fields[i] == name) return &fields[i];
        return nullptr;
    }
};

using NativeFn = std::function<Value(Instance& self, std::vector<Value>& args)>;

class VM {
public:
    std::unordered_map<std::string, int> nativeIndex;  // "input.pressed" -> slot in natives
    std::vector<NativeFn> natives;
    std::unordered_map<std::string, Value> constants;  // "Button.A" -> 4, inlined at compile time
    // `use X` components -> fields they add (with defaults) when the object doesn't declare them
    std::unordered_map<std::string, std::vector<std::pair<std::string, Value>>> components;
    // cena do programa rodando agora: with (Objeto) e instance_* procuram nela (o host aponta)
    std::vector<std::shared_ptr<Instance>>* scene = nullptr;

    VM();  // registra as funções da linguagem: instance_*, object_*, vec3 e a biblioteca padrão (stdlib.cpp)
    void addNative(const std::string& name, NativeFn fn);
    std::shared_ptr<Instance> instantiate(std::shared_ptr<ObjectDef> def);  // runs field initializers (not create)
    Value call(Instance& self, const std::string& fn, std::vector<Value> args = {});  // no-op if fn is undefined

private:
    int depth = 0;
    Value run(Instance& self, const Function& f, std::vector<Value> locals);
};

bool truthy(const Value& v);
std::string toString(const Value& v);

// Argument checks for natives; throw a readable error on a missing/wrong argument.
double argNum(const std::vector<Value>& a, size_t i);

// Biblioteca padrao no estilo GML (stdlib.cpp): funcoes soltas de numero, sorteio, geometria, texto e array.
void registerStdlib(VM& vm);

// Conta os alarm[0..7] de cada instancia viva e dispara alarm0()..alarm7(); chame uma vez por quadro.
void tickAlarms(VM& vm, std::vector<std::shared_ptr<Instance>>& scene);
Vec3 argVec(const std::vector<Value>& a, size_t i);
