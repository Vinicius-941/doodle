#include "vm.h"
#include <cmath>
#include <cstdio>

bool truthy(const Value& v) {
    if (auto b = std::get_if<bool>(&v)) return *b;
    if (auto n = std::get_if<double>(&v)) return *n != 0;
    return v.index() != 0;  // nil is false; strings and arrays are true
}

std::string toString(const Value& v) {
    switch (v.index()) {
    case 0: return "nil";
    case 1: return std::get<bool>(v) ? "true" : "false";
    case 2: {
        double n = std::get<double>(v);
        char buf[32];
        if (n == std::floor(n) && std::fabs(n) < 1e15) snprintf(buf, sizeof buf, "%.0f", n);
        else snprintf(buf, sizeof buf, "%g", n);
        return buf;
    }
    case 3: return std::get<std::string>(v);
    default: {
        std::string s;
        for (auto& e : *std::get<std::shared_ptr<Array>>(v)) s += (s.empty() ? "" : ", ") + toString(e);
        return "[" + s + "]";
    }
    }
}

static const char* typeName(const Value& v) {
    static const char* names[] = {"nil", "bool", "número", "string", "array"};
    return names[v.index()];
}

static double num(const Value& v, const char* op) {
    if (auto n = std::get_if<double>(&v)) return *n;
    throw std::runtime_error(std::string("'") + op + "' espera número, recebeu " + typeName(v));
}

static Value& element(const Value& a, const Value& i) {
    auto arr = std::get_if<std::shared_ptr<Array>>(&a);
    if (!arr) throw std::runtime_error(std::string("não dá pra indexar ") + typeName(a));
    double n = num(i, "[]");
    if (n < 0 || n >= (*arr)->size() || n != std::floor(n))
        throw std::runtime_error("índice " + toString(i) + " fora do array (tamanho " + std::to_string((*arr)->size()) + ")");
    return (**arr)[(size_t)n];
}

VM::VM() {
    addNative("len", [](Instance&, std::vector<Value>& a) {
        if (a.size() == 1) {
            if (auto arr = std::get_if<std::shared_ptr<Array>>(&a[0])) return Value(double((*arr)->size()));
            if (auto s = std::get_if<std::string>(&a[0])) return Value(double(s->size()));
        }
        throw std::runtime_error("len() espera um array ou string");
    });
    addNative("print", [](Instance&, std::vector<Value>& a) {
        std::string line;
        for (auto& v : a) line += (line.empty() ? "" : " ") + toString(v);
        printf("%s\n", line.c_str());
        fflush(stdout);
        return Value();
    });
    addNative("destroy_self", [](Instance& self, std::vector<Value>&) {
        self.alive = false;
        return Value();
    });
}

void VM::addNative(const std::string& name, NativeFn fn) {
    nativeIndex[name] = (int)natives.size();
    natives.push_back(std::move(fn));
}

std::unique_ptr<Instance> VM::instantiate(std::shared_ptr<ObjectDef> def) {
    auto inst = std::make_unique<Instance>();
    inst->def = std::move(def);
    inst->fields.resize(inst->def->fields.size());
    depth = 0;
    run(*inst, inst->def->funcs[0], {});
    call(*inst, "create");
    return inst;
}

Value VM::call(Instance& self, const std::string& fn, std::vector<Value> args) {
    auto it = self.def->funcIndex.find(fn);
    if (it == self.def->funcIndex.end()) return {};
    depth = 0;
    return run(self, self.def->funcs[it->second], std::move(args));
}

Value VM::run(Instance& self, const Function& f, std::vector<Value> locals) {
    // ponytail: one C++ frame per Doo call, capped at 200; move to an explicit frame stack if deep recursion matters
    if (++depth > 200) throw std::runtime_error("recursão profunda demais (stack overflow)");
    locals.resize(f.nlocals);
    std::vector<Value> st;
    size_t pc = 0;
    auto pop = [&] { Value v = std::move(st.back()); st.pop_back(); return v; };
    auto args = [&](int argc) {
        std::vector<Value> a(std::make_move_iterator(st.end() - argc), std::make_move_iterator(st.end()));
        st.erase(st.end() - argc, st.end());
        return a;
    };
    auto arith = [&](const char* op, auto fn) {
        double b = num(pop(), op), a = num(pop(), op);
        st.push_back(Value(fn(a, b)));
    };
    auto compare = [&](auto fn) {
        Value b = pop(), a = pop();
        auto sa = std::get_if<std::string>(&a), sb = std::get_if<std::string>(&b);
        if (sa && sb) st.push_back(Value(fn(sa->compare(*sb), 0)));
        else st.push_back(Value(fn(num(a, "comparação"), num(b, "comparação"))));
    };
    try {
        for (;;) {
            switch (f.code[pc++]) {
            case OP_CONST: st.push_back(f.consts[f.code[pc++]]); break;
            case OP_NIL: st.emplace_back(); break;
            case OP_POP: st.pop_back(); break;
            case OP_DUP: st.push_back(st.back()); break;
            case OP_GET_LOCAL: st.push_back(locals[f.code[pc++]]); break;
            case OP_SET_LOCAL: locals[f.code[pc++]] = pop(); break;
            case OP_GET_FIELD: st.push_back(self.fields[f.code[pc++]]); break;
            case OP_SET_FIELD: self.fields[f.code[pc++]] = pop(); break;
            case OP_ADD: {
                Value b = pop(), a = pop();
                if (a.index() == 3 || b.index() == 3) st.push_back(Value(toString(a) + toString(b)));
                else st.push_back(Value(num(a, "+") + num(b, "+")));
                break;
            }
            case OP_SUB: arith("-", [](double a, double b) { return a - b; }); break;
            case OP_MUL: arith("*", [](double a, double b) { return a * b; }); break;
            case OP_DIV: arith("/", [](double a, double b) { return a / b; }); break;
            case OP_MOD: arith("%", [](double a, double b) { return std::fmod(a, b); }); break;
            case OP_NEG: st.back() = Value(-num(st.back(), "-")); break;
            case OP_NOT: st.back() = Value(!truthy(st.back())); break;
            case OP_EQ: case OP_NE: {
                Value b = pop(), a = pop();
                bool eq = static_cast<const ValueBase&>(a) == static_cast<const ValueBase&>(b);
                st.push_back(Value(f.code[pc - 1] == OP_EQ ? eq : !eq));
                break;
            }
            case OP_LT: compare([](auto a, auto b) { return a < b; }); break;
            case OP_LE: compare([](auto a, auto b) { return a <= b; }); break;
            case OP_GT: compare([](auto a, auto b) { return a > b; }); break;
            case OP_GE: compare([](auto a, auto b) { return a >= b; }); break;
            case OP_JMP: pc = f.code[pc]; break;
            case OP_JF: case OP_JT: {
                bool jumpIf = f.code[pc - 1] == OP_JT;
                int target = f.code[pc++];
                if (truthy(pop()) == jumpIf) pc = target;
                break;
            }
            case OP_CALL: {
                const Function& g = self.def->funcs[f.code[pc++]];
                auto a = args(f.code[pc++]);
                st.push_back(run(self, g, std::move(a)));
                break;
            }
            case OP_NATIVE: {
                int idx = f.code[pc++];
                auto a = args(f.code[pc++]);
                st.push_back(natives[idx](self, a));
                break;
            }
            case OP_RET: --depth; return pop();
            case OP_ARRAY: st.push_back(Value(std::make_shared<Array>(args(f.code[pc++])))); break;
            case OP_INDEX: {
                Value i = pop(), a = pop();
                st.push_back(element(a, i));
                break;
            }
            case OP_SET_INDEX: {
                Value v = pop(), i = pop(), a = pop();
                element(a, i) = std::move(v);
                break;
            }
            default: throw std::runtime_error("bytecode inválido");
            }
        }
    } catch (const DooError&) {
        throw;  // already located by an inner frame
    } catch (const std::exception& e) {
        throw DooError(self.def->file + ":" + std::to_string(f.lines[pc - 1]) + ": " + e.what() + " (em " + f.name + ")");
    }
}
