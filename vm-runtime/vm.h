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
using Array = std::vector<Value>;
// nil | bool | number | string | array (arrays are shared, managed references)
using ValueBase = std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>>;
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
};

struct Function {
    std::string name;
    int arity = 0, nlocals = 0;
    std::vector<int> code, lines;  // lines[i] = source line of code[i]
    std::vector<Value> consts;
};

// One .doo file = one object.
struct ObjectDef {
    std::string name, file;
    std::vector<std::string> fields;
    std::vector<Function> funcs;  // funcs[0] = __init (field initializers)
    std::unordered_map<std::string, int> funcIndex;
};

struct Instance {
    std::shared_ptr<ObjectDef> def;
    std::vector<Value> fields;
    bool alive = true;
};

using NativeFn = std::function<Value(Instance& self, std::vector<Value>& args)>;

class VM {
public:
    std::unordered_map<std::string, int> nativeIndex;  // "input.pressed" -> slot in natives
    std::vector<NativeFn> natives;
    std::unordered_map<std::string, Value> constants;  // "Button.A" -> 4, inlined at compile time

    VM();  // registers the language built-ins: len, print, destroy_self
    void addNative(const std::string& name, NativeFn fn);
    std::unique_ptr<Instance> instantiate(std::shared_ptr<ObjectDef> def);  // runs field inits + create()
    Value call(Instance& self, const std::string& fn, std::vector<Value> args = {});  // no-op if fn is undefined

private:
    int depth = 0;
    Value run(Instance& self, const Function& f, std::vector<Value> locals);
};

bool truthy(const Value& v);
std::string toString(const Value& v);
