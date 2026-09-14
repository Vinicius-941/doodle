#include "vm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

// A live instance behind a ref, or nullptr if it was destroyed.
static std::shared_ptr<Instance> live(const Ref& r) {
    auto inst = r.p.lock();
    return inst && inst->alive ? inst : nullptr;
}

bool truthy(const Value& v) {
    if (auto b = std::get_if<bool>(&v)) return *b;
    if (auto n = std::get_if<double>(&v)) return *n != 0;
    if (auto r = std::get_if<Ref>(&v)) return live(*r) != nullptr;  // `if (enemy)`: still alive?
    return v.index() != 0;  // nil is false; strings, arrays and vec3 are true
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
    case 5: {
        const Vec3& p = std::get<Vec3>(v);
        return "vec3(" + toString(Value(p.x)) + ", " + toString(Value(p.y)) + ", " + toString(Value(p.z)) + ")";
    }
    case 6: {
        auto inst = live(std::get<Ref>(v));
        return inst ? "<" + inst->def->name + ">" : "<destruído>";
    }
    default: {
        std::string s;
        for (auto& [k, e] : std::get<std::shared_ptr<Struct>>(v)->m) s += (s.empty() ? "" : ", ") + k + ": " + toString(e);
        return "{" + s + "}";
    }
    }
}

static const char* typeName(const Value& v) {
    static const char* names[] = {"nil", "bool", "número", "string", "array", "vec3", "objeto", "struct"};
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
    if (va && vb && (op == OP_ADD || op == OP_SUB)) return Value(op == OP_ADD ? *va + *vb : *va - *vb);
    if ((va && op == OP_DIV) || (va && op == OP_MUL) || (vb && op == OP_MUL)) {
        double k = num(va ? b : a, name);
        return Value((va ? *va : *vb) * (op == OP_DIV ? 1 / k : k));
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

// s["chave"] num struct: ler chave que não existe é erro (como na GML); escrever cria.
static Value& entry(Struct& s, const std::string& k, bool escrever) {
    auto it = s.m.find(k);
    if (it != s.m.end()) return it->second;
    if (!escrever) throw std::runtime_error("o struct não tem '" + k + "' (struct_exists confere antes)");
    return s.m[k];
}

static Value& element(const Value& a, const Value& i, bool escrever = false) {
    if (auto st = std::get_if<std::shared_ptr<Struct>>(&a)) {
        auto k = std::get_if<std::string>(&i);
        if (!k) throw std::runtime_error("struct se indexa com texto: s[\"chave\"]");
        return entry(**st, *k, escrever);
    }
    auto arr = std::get_if<std::shared_ptr<Array>>(&a);
    if (!arr) throw std::runtime_error(std::string("não dá pra indexar ") + typeName(a));
    double n = num(i, "[]");
    if (n < 0 || n >= (*arr)->size() || n != std::floor(n))
        throw std::runtime_error("índice " + toString(i) + " fora do array (tamanho " + std::to_string((*arr)->size()) + ")");
    return (**arr)[(size_t)n];
}

static std::shared_ptr<Instance> instanceOf(const Value& v, const std::string& what) {
    auto r = std::get_if<Ref>(&v);
    if (!r) throw std::runtime_error("'" + what + "' não existe em " + typeName(v));
    auto inst = live(*r);
    if (!inst) throw std::runtime_error("'" + what + "' em um objeto que já foi destruído");
    return inst;
}

static double* component(Value& v, const std::string& name) {  // vec3 .x/.y/.z, or nullptr if v isn't a vec3
    auto p = std::get_if<Vec3>(&v);
    if (!p) return nullptr;
    if (name == "x") return &p->x;
    if (name == "y") return &p->y;
    if (name == "z") return &p->z;
    throw std::runtime_error("vec3 não tem '." + name + "' (só .x, .y, .z)");
}

static Value& field(const Value& v, const std::string& name) {  // obj.name on an instance ref
    auto inst = instanceOf(v, "." + name);
    Value* f = inst->field(name);
    if (!f) throw std::runtime_error(inst->def->name + " não tem '" + name + "'");
    return *f;  // the scene keeps the instance alive
}

// inimigo.x numa instância que não declarou x: é position.x, como o x solto (e como na GML)
static double* axis(const Value& v, const std::string& name) {
    if ((name != "x" && name != "y" && name != "z") || !std::holds_alternative<Ref>(v)) return nullptr;
    auto inst = instanceOf(v, "." + name);
    if (inst->field(name)) return nullptr;
    Value* pos = inst->field("position");
    auto p = pos ? std::get_if<Vec3>(pos) : nullptr;
    if (!p) return nullptr;
    return name == "x" ? &p->x : name == "y" ? &p->y : &p->z;
}

VM::VM() {
    addNative("show_debug_message", [](Instance&, std::vector<Value>& a) {
        std::string line;
        for (auto& v : a) line += (line.empty() ? "" : " ") + toString(v);
        printf("%s\n", line.c_str());
        fflush(stdout);
        return Value();
    });
    addNative("object_name", [](Instance&, std::vector<Value>& a) {  // nome do objeto: object_name(other) == Player
        if (a.size() != 1) throw std::runtime_error("object_name() recebe 1 argumento");
        auto r = std::get_if<Ref>(&a[0]);
        auto inst = r ? r->p.lock() : nullptr;
        return Value(inst ? inst->def->name : std::string(typeName(a[0])));
    });
    addNative("object_is", [](Instance&, std::vector<Value>& a) {  // object_is(obj, Tipo): e do tipo ou herda dele
        if (a.size() != 2) throw std::runtime_error("object_is() recebe 2 argumentos");
        auto r = std::get_if<Ref>(&a[0]);
        auto inst = r ? r->p.lock() : nullptr;
        if (!inst) return Value(false);
        auto& kinds = inst->def->kinds;
        return Value(std::find(kinds.begin(), kinds.end(), toString(a[1])) != kinds.end());
    });
    addNative("instance_destroy", [](Instance& self, std::vector<Value>&) {
        self.alive = false;
        return Value();
    });
    addNative("vec3", [](Instance&, std::vector<Value>& a) {
        return Value(a.empty() ? Vec3{} : Vec3{argNum(a, 0), argNum(a, 1), argNum(a, 2)});
    });

    // --- instâncias da cena (with e os instance_* da GML) ---
    auto kindOf = [](const Instance& i, const std::string& tipo) {
        return std::find(i.def->kinds.begin(), i.def->kinds.end(), tipo) != i.def->kinds.end();
    };
    auto doTipo = [this, kindOf](const std::string& tipo) {  // vivas, do tipo ou de um descendente
        std::vector<std::shared_ptr<Instance>> out;
        if (scene)
            for (auto& inst : *scene)
                if (inst->alive && kindOf(*inst, tipo)) out.push_back(inst);
        return out;
    };
    addNative("__with_targets", [doTipo](Instance&, std::vector<Value>& a) {  // with (x): tipo, ref ou array de refs
        auto out = std::make_shared<Array>();
        if (a.size() != 1) throw std::runtime_error("with espera um alvo");
        if (auto t = std::get_if<std::string>(&a[0]))
            for (auto& inst : doTipo(*t)) out->push_back(Value(Ref{inst}));
        else if (std::holds_alternative<Ref>(a[0]))
            out->push_back(a[0]);
        else if (auto arr = std::get_if<std::shared_ptr<Array>>(&a[0]))
            *out = **arr;  // cópia: o bloco pode mexer no array sem bagunçar a volta
        else if (!std::holds_alternative<std::monostate>(a[0]))
            throw std::runtime_error("with espera um objeto, uma referência ou um array de referências");
        return Value(out);
    });
    addNative("instance_exists", [doTipo](Instance&, std::vector<Value>& a) {  // (Tipo) ou (ref)
        if (a.size() != 1) throw std::runtime_error("instance_exists() recebe 1 argumento");
        if (auto t = std::get_if<std::string>(&a[0])) return Value(!doTipo(*t).empty());
        auto r = std::get_if<Ref>(&a[0]);
        auto inst = r ? r->p.lock() : nullptr;
        return Value(inst && inst->alive);
    });
    addNative("instance_number", [doTipo](Instance&, std::vector<Value>& a) {
        if (a.size() != 1 || !std::holds_alternative<std::string>(a[0])) throw std::runtime_error("instance_number(Tipo)");
        return Value((double)doTipo(std::get<std::string>(a[0])).size());
    });
    addNative("instance_find", [doTipo](Instance&, std::vector<Value>& a) {  // (Tipo, n) -> ref ou nil
        if (a.size() != 2 || !std::holds_alternative<std::string>(a[0])) throw std::runtime_error("instance_find(Tipo, n)");
        auto todas = doTipo(std::get<std::string>(a[0]));
        double n = argNum(a, 1);
        if (n < 0 || n >= todas.size()) return Value();
        return Value(Ref{todas[(size_t)n]});
    });
    addNative("instance_nearest", [doTipo](Instance&, std::vector<Value>& a) {  // (x, y, z, Tipo) -> ref ou nil
        if (a.size() != 4 || !std::holds_alternative<std::string>(a[3])) throw std::runtime_error("instance_nearest(x, y, z, Tipo)");
        Vec3 p{argNum(a, 0), argNum(a, 1), argNum(a, 2)};
        std::shared_ptr<Instance> melhor;
        double menor = 0;
        for (auto& inst : doTipo(std::get<std::string>(a[3]))) {
            auto pos = inst->field("position");
            auto v = pos ? std::get_if<Vec3>(pos) : nullptr;
            if (!v) continue;
            Vec3 d = *v - p;
            if (!melhor || d.dot(d) < menor) { melhor = inst; menor = d.dot(d); }
        }
        return melhor ? Value(Ref{melhor}) : Value();
    });
    registerStdlib(*this);
}

// alarm[0..7] conta quadros e chama alarm0()..alarm7() quando chega ao fim, como na GML.
void tickAlarms(VM& vm, std::vector<std::shared_ptr<Instance>>& scene) {
    for (size_t i = 0; i < scene.size(); i++) {
        if (!scene[i]->alive) continue;
        Value* v = scene[i]->field("alarm");
        auto arr = v ? std::get_if<std::shared_ptr<Array>>(v) : nullptr;
        if (!arr) continue;
        for (size_t k = 0; k < (*arr)->size() && k < 8; k++) {
            auto n = std::get_if<double>(&(**arr)[k]);
            if (!n || *n < 0) continue;  // -1 = desligado
            if ((*n -= 1) <= 0) {        // alarm[i] = 1 dispara no quadro seguinte, como na GML
                *n = -1;
                vm.call(*scene[i], "alarm" + std::to_string(k));
            }
        }
    }
}

void VM::addNative(const std::string& name, NativeFn fn) {
    nativeIndex[name] = (int)natives.size();
    natives.push_back(std::move(fn));
}

std::shared_ptr<Instance> VM::instantiate(std::shared_ptr<ObjectDef> def) {
    auto inst = std::make_shared<Instance>();
    inst->def = std::move(def);
    inst->fields.resize(inst->def->fields.size());
    run(*inst, inst->def->funcs[0], {});
    return inst;
}

Value VM::call(Instance& self, const std::string& fn, std::vector<Value> args) {
    auto it = self.def->funcIndex.find(fn);
    if (it == self.def->funcIndex.end()) return {};
    return run(self, self.def->funcs[it->second], std::move(args));
}

Value VM::run(Instance& self, const Function& f, std::vector<Value> locals) {
    // ponytail: one C++ frame per Doo call, capped at 200; move to an explicit frame stack if deep recursion matters
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{++depth};
    if (depth > 200) throw std::runtime_error("recursão profunda demais (stack overflow)");
    locals.resize(f.nlocals);
    // `me` é quem o código está rodando como: começa sendo o dono da função e troca dentro de with.
    // Campos por slot (OP_GET_FIELD) continuam sendo do dono; dentro de with o compilador acessa por nome.
    Instance* me = &self;
    std::vector<std::shared_ptr<Instance>> withStack, withHold;  // quem rodava antes de cada with / quem roda agora
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
    auto name = [&] { return std::get<std::string>(f.consts[f.code[pc++]]); };
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
                if (auto v = std::get_if<Vec3>(&st.back())) st.back() = Value(*v * -1);
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
                st.push_back(natives[idx](*me, a));
                break;
            }
            case OP_RET: return pop();
            case OP_ARRAY: st.push_back(Value(std::make_shared<Array>(args(f.code[pc++])))); break;
            case OP_INDEX: {
                Value i = pop(), a = pop();
                st.push_back(element(a, i));
                break;
            }
            case OP_SET_INDEX: {
                Value v = pop(), i = pop(), a = pop();
                element(a, i, true) = std::move(v);
                break;
            }
            case OP_GET_MEMBER: {
                std::string m = name();
                Value v = pop();
                if (double* c = component(v, m)) st.push_back(Value(*c));
                else if (auto s = std::get_if<std::shared_ptr<Struct>>(&v)) st.push_back(entry(**s, m, false));
                else if (double* e = axis(v, m)) st.push_back(Value(*e));
                else st.push_back(field(v, m));
                break;
            }
            case OP_SET_MEMBER: {  // vec3: changes the copy on the stack; ref: changes the instance itself
                std::string m = name();
                Value x = pop(), v = pop();
                if (double* c = component(v, m)) *c = num(x, "=");
                else if (auto s = std::get_if<std::shared_ptr<Struct>>(&v)) entry(**s, m, true) = std::move(x);
                else if (double* e = axis(v, m)) *e = num(x, "=");
                else field(v, m) = std::move(x);
                st.push_back(std::move(v));
                break;
            }
            case OP_INVOKE: {  // obj.method(args), dispatched by name on the target's object
                std::string m = name();
                auto a = args(f.code[pc++]);
                auto inst = instanceOf(pop(), m + "()");
                auto it = inst->def->funcIndex.find(m);
                if (it == inst->def->funcIndex.end()) throw std::runtime_error(inst->def->name + " não tem a função " + m + "()");
                const Function& g = inst->def->funcs[it->second];
                if (g.arity != (int)a.size())
                    throw std::runtime_error(m + "() recebe " + std::to_string(g.arity) + " argumento(s), veio " + std::to_string(a.size()));
                st.push_back(run(*inst, g, std::move(a)));
                break;
            }
            case OP_SELF: st.push_back(Value(Ref{me->weak_from_this()})); break;
            case OP_OTHER:
                if (withStack.empty()) throw std::runtime_error("other só existe dentro de with (ou como parâmetro de collision)");
                st.push_back(Value(Ref{withStack.back()}));
                break;
            case OP_WITH_SELF: {
                int skip = f.code[pc++];
                Value v = pop();
                auto r = std::get_if<Ref>(&v);
                auto inst = r ? r->p.lock() : nullptr;
                if (!inst || !inst->alive) { pc = skip; break; }  // destruída no meio do with: pula
                withStack.push_back(me->shared_from_this());
                me = inst.get();
                withHold.push_back(std::move(inst));  // não deixa a instância sumir enquanto o bloco roda
                break;
            }
            case OP_STRUCT: {
                auto s = std::make_shared<Struct>();
                auto kv = args(f.code[pc++] * 2);
                for (size_t k = 0; k < kv.size(); k += 2) s->m[std::get<std::string>(kv[k])] = std::move(kv[k + 1]);
                st.push_back(Value(std::move(s)));
                break;
            }
            case OP_WITH_RESTORE:
                me = withStack.back().get();
                withStack.pop_back();
                withHold.pop_back();
                break;
            default: throw std::runtime_error("bytecode inválido");
            }
        }
    } catch (const DooError&) {
        throw;  // already located by an inner frame
    } catch (const std::exception& e) {
        throw DooError(f.file + ":" + std::to_string(f.lines[pc - 1]) + ": " + e.what() + " (em " + f.name + ")");
    }
}
