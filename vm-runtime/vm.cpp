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
    case 4: {
        std::string s;
        for (auto& e : *std::get<std::shared_ptr<Array>>(v)) s += (s.empty() ? "" : ", ") + toString(e);
        return "[" + s + "]";
    }
    default: {
        const Vec3& p = std::get<Vec3>(v);
        return "vec3(" + toString(Value(p.x)) + ", " + toString(Value(p.y)) + ", " + toString(Value(p.z)) + ")";
    }
    }
}

static const char* typeName(const Value& v) {
    static const char* names[] = {"nil", "bool", "número", "string", "array", "vec3"};
    return names[v.index()];
}

static double num(const Value& v, const char* op) {
    if (auto n = std::get_if<double>(&v)) return *n;
    throw std::runtime_error(std::string("'") + op + "' espera número, recebeu " + typeName(v));
}

double argNum(const std::vector<Value>& a, size_t i) {
    if (i < a.size() && std::holds_alternative<double>(a[i])) return std::get<double>(a[i]);
    throw std::runtime_error("argumento " + std::to_string(i + 1) + " deve ser um número");
}

Vec3 argVec(const std::vector<Value>& a, size_t i) {
    if (i < a.size() && std::holds_alternative<Vec3>(a[i])) return std::get<Vec3>(a[i]);
    throw std::runtime_error("argumento " + std::to_string(i + 1) + " deve ser um vec3");
}

// Numbers: + - * / %.  vec3: vec3 +- vec3, vec3 */ number, number * vec3.
static Value arith(int op, const Value& a, const Value& b) {
    static const char* names[] = {"+", "-", "*", "/", "%"};
    const char* name = names[op - OP_ADD];
    auto va = std::get_if<Vec3>(&a), vb = std::get_if<Vec3>(&b);
    if (va && vb && (op == OP_ADD || op == OP_SUB)) {
        double s = op == OP_ADD ? 1 : -1;
        return Value(Vec3{va->x + s * vb->x, va->y + s * vb->y, va->z + s * vb->z});
    }
    if ((va && op == OP_DIV) || (va && op == OP_MUL) || (vb && op == OP_MUL)) {
        const Vec3& v = va ? *va : *vb;
        double k = num(va ? b : a, name);
        if (op == OP_DIV) k = 1 / k;
        return Value(Vec3{v.x * k, v.y * k, v.z * k});
    }
    double x = num(a, name), y = num(b, name);
    switch (op) {
    case OP_ADD: return Value(x + y);
    case OP_SUB: return Value(x - y);
    case OP_MUL: return Value(x * y);
    case OP_DIV: return Value(x / y);
    default: return Value(std::fmod(x, y));
    }
}

static Value& element(const Value& a, const Value& i) {
    auto arr = std::get_if<std::shared_ptr<Array>>(&a);
    if (!arr) throw std::runtime_error(std::string("não dá pra indexar ") + typeName(a));
    double n = num(i, "[]");
    if (n < 0 || n >= (*arr)->size() || n != std::floor(n))
        throw std::runtime_error("índice " + toString(i) + " fora do array (tamanho " + std::to_string((*arr)->size()) + ")");
    return (**arr)[(size_t)n];
}

static double& component(Value& v, int k) {  // v.x / v.y / v.z
    auto p = std::get_if<Vec3>(&v);
    if (!p) throw std::runtime_error(std::string("só vec3 tem .x/.y/.z, isto é ") + typeName(v));
    return k == 0 ? p->x : k == 1 ? p->y : p->z;
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
    addNative("vec3", [](Instance&, std::vector<Value>& a) {
        return Value(a.empty() ? Vec3{} : Vec3{argNum(a, 0), argNum(a, 1), argNum(a, 2)});
    });
    // ponytail: minimal math set; the full Doo stdlib is still a pending decision (doc §9)
    static const std::pair<const char*, double (*)(double)> math[] = {
        {"math.sin", std::sin}, {"math.cos", std::cos}, {"math.sqrt", std::sqrt}, {"math.abs", std::fabs}, {"math.floor", std::floor}};
    for (auto& m : math)
        addNative(m.first, [fn = m.second](Instance&, std::vector<Value>& a) { return Value(fn(argNum(a, 0))); });
    constants["math.pi"] = Value(3.14159265358979323846);
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
    auto compare = [&](auto fn) {
        Value b = pop(), a = pop();
        auto sa = std::get_if<std::string>(&a), sb = std::get_if<std::string>(&b);
        if (sa && sb) st.push_back(Value(fn(sa->compare(*sb), 0)));
        else st.push_back(Value(fn(num(a, "comparação"), num(b, "comparação"))));
    };
    try {
        for (;;) {
            int op = f.code[pc++];
            switch (op) {
            case OP_CONST: st.push_back(f.consts[f.code[pc++]]); break;
            case OP_NIL: st.emplace_back(); break;
            case OP_POP: st.pop_back(); break;
            case OP_DUP: st.push_back(st.back()); break;
            case OP_GET_LOCAL: st.push_back(locals[f.code[pc++]]); break;
            case OP_SET_LOCAL: locals[f.code[pc++]] = pop(); break;
            case OP_GET_FIELD: st.push_back(self.fields[f.code[pc++]]); break;
            case OP_SET_FIELD: self.fields[f.code[pc++]] = pop(); break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
                Value b = pop(), a = pop();
                bool concat = op == OP_ADD && (a.index() == 3 || b.index() == 3);
                st.push_back(concat ? Value(toString(a) + toString(b)) : arith(op, a, b));
                break;
            }
            case OP_NEG:
                if (auto v = std::get_if<Vec3>(&st.back())) st.back() = Value(Vec3{-v->x, -v->y, -v->z});
                else st.back() = Value(-num(st.back(), "-"));
                break;
            case OP_NOT: st.back() = Value(!truthy(st.back())); break;
            case OP_EQ: case OP_NE: {
                Value b = pop(), a = pop();
                bool eq = static_cast<const ValueBase&>(a) == static_cast<const ValueBase&>(b);
                st.push_back(Value(op == OP_EQ ? eq : !eq));
                break;
            }
            case OP_LT: compare([](auto a, auto b) { return a < b; }); break;
            case OP_LE: compare([](auto a, auto b) { return a <= b; }); break;
            case OP_GT: compare([](auto a, auto b) { return a > b; }); break;
            case OP_GE: compare([](auto a, auto b) { return a >= b; }); break;
            case OP_JMP: pc = f.code[pc]; break;
            case OP_JF: case OP_JT: {
                bool jumpIf = op == OP_JT;
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
            case OP_GET_MEMBER: {
                Value v = pop();
                st.push_back(Value(component(v, f.code[pc++])));
                break;
            }
            case OP_SET_MEMBER: {
                Value x = pop(), v = pop();
                component(v, f.code[pc++]) = num(x, "=");
                st.push_back(std::move(v));
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
