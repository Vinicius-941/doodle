#include "compiler.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

// ---------- lexer ----------

struct Token {
    enum Kind { Ident, Num, Str, Punct, End } kind;
    std::string text;
    double num;
    int line;
};

static std::vector<Token> lex(const std::string& s, const std::string& file) {
    static const char* ops[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "(", ")", "{", "}",
                                "[", "]", ",", ".", ";", "+", "-", "*", "/", "%", "<", ">", "=", "!"};
    std::vector<Token> out;
    int line = 1;
    size_t i = s.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;  // skip UTF-8 BOM
    auto fail = [&](const std::string& m) { return DooError(file + ":" + std::to_string(line) + ": " + m); };
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') { line++; i++; }
        else if (isspace((unsigned char)c)) i++;
        else if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') i++; }
        else if (isdigit((unsigned char)c)) {  // strtod also reads hex colors like 0xFF8800
            char* end;
            double v = strtod(s.c_str() + i, &end);
            size_t n = end - (s.c_str() + i);
            out.push_back({Token::Num, s.substr(i, n), v, line});
            i += n;
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            out.push_back({Token::Ident, s.substr(i, j - i), 0, line});
            i = j;
        } else if (c == '"') {
            std::string str;
            for (i++; i < s.size() && s[i] != '"' && s[i] != '\n'; i++) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char e = s[++i];
                    str += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else str += s[i];
            }
            if (i >= s.size() || s[i] != '"') throw fail("string sem aspas de fechamento");
            i++;
            out.push_back({Token::Str, str, 0, line});
        } else {
            const char* op = nullptr;
            for (const char* o : ops) if (s.compare(i, strlen(o), o) == 0) { op = o; break; }
            if (!op) throw fail(std::string("caractere inesperado '") + c + "'");
            out.push_back({Token::Punct, op, 0, line});
            i += strlen(op);
        }
    }
    out.push_back({Token::End, "fim do arquivo", 0, line});
    return out;
}

// ---------- parser -> AST ----------

enum class K { Lit, Name, Member, Index, Call, Array, Unary, Binary, Var, Assign, If, While, For, Return, Block, Expr };

// Generic AST node. kids layout per kind:
//   Member: [obj] text=member   Index: [arr, i]   Call: [callee, args...]   Unary/Binary: operands, text=op
//   Var: [init?] text=name   Assign: [target, value] text=op   If: [cond, then, else?]   While: [cond, body]
//   For: [init|null, cond|null, step|null, body]   Return: [value?]   Block: stmts   Expr: [expr]
struct Node {
    K kind;
    int line;
    std::string text;
    Value value;  // Lit
    std::vector<std::unique_ptr<Node>> kids;
};
using P = std::unique_ptr<Node>;

struct FuncDecl {
    std::string name;
    std::vector<std::string> params;
    P body;
};

struct Parser {
    std::vector<Token> t;
    size_t p;
    std::string file;

    const Token& peek() const { return t[p]; }
    bool is(const char* s) const { return (t[p].kind == Token::Punct || t[p].kind == Token::Ident) && t[p].text == s; }
    bool accept(const char* s) { return is(s) ? (p++, true) : false; }
    DooError err(const std::string& m) const { return DooError(file + ":" + std::to_string(t[p].line) + ": " + m); }
    void expect(const char* s) {
        if (!accept(s)) throw err(std::string("esperado '") + s + "', encontrado '" + t[p].text + "'");
    }
    std::string ident() {
        if (t[p].kind != Token::Ident) throw err("esperado um nome, encontrado '" + t[p].text + "'");
        return t[p++].text;
    }
    // Nodes are created right after consuming their first token, so the previous token holds their line.
    P node(K k, std::string text = {}) {
        auto n = std::make_unique<Node>();
        n->kind = k;
        n->line = t[p - 1].line;
        n->text = std::move(text);
        return n;
    }

    P block() {
        expect("{");
        auto n = node(K::Block);
        while (!accept("}")) {
            if (peek().kind == Token::End) throw err("faltou fechar '}'");
            if (!accept(";")) n->kids.push_back(statement());
        }
        return n;
    }

    P statement() {
        if (is("{")) return block();
        if (accept("if")) {
            auto n = node(K::If);
            expect("(");
            n->kids.push_back(expr());
            expect(")");
            n->kids.push_back(statement());
            if (accept("else")) n->kids.push_back(statement());
            return n;
        }
        if (accept("while")) {
            auto n = node(K::While);
            expect("(");
            n->kids.push_back(expr());
            expect(")");
            n->kids.push_back(statement());
            return n;
        }
        if (accept("for")) {
            auto n = node(K::For);
            expect("(");
            n->kids.push_back(is(";") ? nullptr : simple());
            expect(";");
            n->kids.push_back(is(";") ? nullptr : expr());
            expect(";");
            n->kids.push_back(is(")") ? nullptr : simple());
            expect(")");
            n->kids.push_back(statement());
            return n;
        }
        if (accept("return")) {
            auto n = node(K::Return);
            if (!is("}") && !is(";")) n->kids.push_back(expr());
            return n;
        }
        return simple();
    }

    // var declaration, assignment, or expression statement (also used in for headers)
    P simple() {
        if (accept("var")) {
            auto n = node(K::Var, ident());
            if (accept("=")) n->kids.push_back(expr());
            return n;
        }
        P e = expr();
        for (const char* op : {"=", "+=", "-=", "*=", "/="}) {
            if (!accept(op)) continue;
            if (e->kind != K::Name && e->kind != K::Index && e->kind != K::Member)
                throw err("só dá pra atribuir a variável, elemento de array ou v.x/.y/.z");
            auto n = node(K::Assign, op);
            n->kids.push_back(std::move(e));
            n->kids.push_back(expr());
            return n;
        }
        auto n = node(K::Expr);
        n->kids.push_back(std::move(e));
        return n;
    }

    P expr() { return binary(0); }

    P binary(size_t level) {
        static const std::vector<std::vector<const char*>> levels = {
            {"||"}, {"&&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"+", "-"}, {"*", "/", "%"}};
        if (level == levels.size()) return unary();
        P left = binary(level + 1);
        for (;;) {
            auto op = std::find_if(levels[level].begin(), levels[level].end(), [&](const char* o) { return is(o); });
            if (op == levels[level].end()) return left;
            p++;
            auto n = node(K::Binary, *op);
            n->kids.push_back(std::move(left));
            n->kids.push_back(binary(level + 1));
            left = std::move(n);
        }
    }

    P unary() {
        if (accept("-") || accept("!")) {
            auto n = node(K::Unary, t[p - 1].text);
            n->kids.push_back(unary());
            return n;
        }
        return postfix();
    }

    P postfix() {
        P e = primary();
        for (;;) {
            // '(' and '[' only continue an expression on the same line (no semicolons in Doo)
            bool sameLine = t[p].line == t[p - 1].line;
            if (sameLine && accept("(")) {
                auto n = node(K::Call);
                n->kids.push_back(std::move(e));
                if (!accept(")")) {
                    do n->kids.push_back(expr()); while (accept(","));
                    expect(")");
                }
                e = std::move(n);
            } else if (sameLine && accept("[")) {
                auto n = node(K::Index);
                n->kids.push_back(std::move(e));
                n->kids.push_back(expr());
                expect("]");
                e = std::move(n);
            } else if (accept(".")) {
                auto n = node(K::Member);
                n->kids.push_back(std::move(e));
                n->text = ident();
                e = std::move(n);
            } else {
                return e;
            }
        }
    }

    P primary() {
        const Token& tk = t[p];
        auto lit = [&](Value v) { p++; auto n = node(K::Lit); n->value = std::move(v); return n; };
        if (tk.kind == Token::Num) return lit(Value(tk.num));
        if (tk.kind == Token::Str) return lit(Value(tk.text));
        if (is("true")) return lit(Value(true));
        if (is("false")) return lit(Value(false));
        if (is("nil")) return lit(Value());
        if (accept("(")) {
            P e = expr();
            expect(")");
            return e;
        }
        if (accept("[")) {
            auto n = node(K::Array);
            if (!accept("]")) {
                do n->kids.push_back(expr()); while (accept(","));
                expect("]");
            }
            return n;
        }
        if (tk.kind == Token::Ident) {
            p++;
            return node(K::Name, tk.text);
        }
        throw err("inesperado: '" + tk.text + "'");
    }
};

// ---------- AST -> bytecode ----------

static int binop(const std::string& op) {
    static const std::unordered_map<std::string, int> ops = {
        {"+", OP_ADD}, {"-", OP_SUB}, {"*", OP_MUL}, {"/", OP_DIV}, {"%", OP_MOD}, {"==", OP_EQ},
        {"!=", OP_NE}, {"<", OP_LT}, {"<=", OP_LE}, {">", OP_GT}, {">=", OP_GE}};
    return ops.at(op);
}

struct Codegen {
    ObjectDef& obj;
    const VM& vm;
    bool privileged;
    const std::unordered_set<std::string>& objects;  // object names of the program: spawn(Enemy, ...)
    Function* f = nullptr;
    std::vector<std::pair<std::string, int>> scope;  // visible locals -> slot
    int line = 0;

    DooError err(const std::string& m) const { return DooError(obj.file + ":" + std::to_string(line) + ": " + m); }
    void emit(int x) { f->code.push_back(x); f->lines.push_back(line); }
    void emit(int op, int a) { emit(op); emit(a); }
    int here() const { return (int)f->code.size(); }
    int jump(int op) { emit(op, -1); return here() - 1; }  // returns the operand to patch
    void patch(int slot) { f->code[slot] = here(); }
    int constant(const Value& v) { f->consts.push_back(v); return (int)f->consts.size() - 1; }

    int local(const std::string& n) const {
        for (auto it = scope.rbegin(); it != scope.rend(); ++it) if (it->first == n) return it->second;
        return -1;
    }
    int field(const std::string& n) const {
        auto it = std::find(obj.fields.begin(), obj.fields.end(), n);
        return it == obj.fields.end() ? -1 : (int)(it - obj.fields.begin());
    }
    int declare(const std::string& n) { scope.push_back({n, f->nlocals}); return f->nlocals++; }

    void function(Function& fn, const std::vector<std::string>& params, Node* body) {
        f = &fn;
        scope.clear();
        for (auto& p : params) declare(p);
        stmt(body);
        emit(OP_NIL);
        emit(OP_RET);
    }

    void stmt(Node* n) {
        line = n->line;
        switch (n->kind) {
        case K::Block: {
            size_t mark = scope.size();
            for (auto& k : n->kids) stmt(k.get());
            scope.resize(mark);
            break;
        }
        case K::Var: {
            if (n->kids.empty()) emit(OP_NIL); else expr(n->kids[0].get());
            emit(OP_SET_LOCAL, declare(n->text));  // declared after the initializer: `var x = x` reads the outer x
            break;
        }
        case K::Assign: assign(n); break;
        case K::Expr: expr(n->kids[0].get()); emit(OP_POP); break;
        case K::If: {
            expr(n->kids[0].get());
            int jf = jump(OP_JF);
            stmt(n->kids[1].get());
            if (n->kids.size() > 2) {
                int end = jump(OP_JMP);
                patch(jf);
                stmt(n->kids[2].get());
                patch(end);
            } else {
                patch(jf);
            }
            break;
        }
        case K::While: {
            int top = here();
            expr(n->kids[0].get());
            int jf = jump(OP_JF);
            stmt(n->kids[1].get());
            emit(OP_JMP, top);
            patch(jf);
            break;
        }
        case K::For: {
            size_t mark = scope.size();
            if (n->kids[0]) stmt(n->kids[0].get());
            int top = here(), jf = -1;
            if (n->kids[1]) { expr(n->kids[1].get()); jf = jump(OP_JF); }
            stmt(n->kids[3].get());
            if (n->kids[2]) stmt(n->kids[2].get());
            emit(OP_JMP, top);
            if (jf >= 0) patch(jf);
            scope.resize(mark);
            break;
        }
        case K::Return:
            if (n->kids.empty()) emit(OP_NIL); else expr(n->kids[0].get());
            emit(OP_RET);
            break;
        default: throw err("isso não é um comando");
        }
    }

    void assign(Node* n) {
        Node* target = n->kids[0].get();
        std::string op = n->text.substr(0, n->text.size() - 1);  // "+=" -> "+", "=" -> ""
        if (target->kind == K::Member) {  // v.x = e / enemy.hp = e: set the member, then store v back (vec3 is a copy)
            if (target->kids[0]->kind != K::Name) throw err("só dá pra alterar membro de uma variável (ex.: v.x, inimigo.vida)");
            expr(target->kids[0].get());
        }
        if (target->kind == K::Index) { expr(target->kids[0].get()); expr(target->kids[1].get()); }
        if (!op.empty()) expr(target);  // current value (a[i] is evaluated again: fine for plain expressions)
        expr(n->kids[1].get());
        line = n->line;
        if (!op.empty()) emit(binop(op));
        if (target->kind == K::Index) return emit(OP_SET_INDEX);
        if (target->kind == K::Member) {
            emit(OP_SET_MEMBER, constant(Value(target->text)));
            target = target->kids[0].get();
        }
        int s = local(target->text);
        if (s >= 0) return emit(OP_SET_LOCAL, s);
        s = field(target->text);
        if (s >= 0) return emit(OP_SET_FIELD, s);
        throw err("'" + target->text + "' não foi declarada (use 'var')");
    }

    // `ns.name` where ns is not a variable -> "ns.name" (SDK namespace or constant), else ""
    std::string qualified(Node* m) const {
        Node* base = m->kids[0].get();
        if (base->kind != K::Name || local(base->text) >= 0 || field(base->text) >= 0) return {};
        return base->text + "." + m->text;
    }

    void native(const std::string& name, int argc) {
        auto it = vm.nativeIndex.find(name);
        if (it == vm.nativeIndex.end()) throw err("função desconhecida '" + name + "'");
        if (!privileged && name.rfind("system.", 0) == 0) throw err("'" + name + "' é exclusiva do firmware");
        emit(OP_NATIVE, it->second);
        emit(argc);
    }

    void call(Node* n) {
        Node* callee = n->kids[0].get();
        int argc = (int)n->kids.size() - 1;
        bool method = callee->kind == K::Member && qualified(callee).empty();  // enemy.take_damage(10)
        if (method) expr(callee->kids[0].get());
        for (size_t i = 1; i < n->kids.size(); i++) expr(n->kids[i].get());
        line = n->line;
        if (method) {
            emit(OP_INVOKE, constant(Value(callee->text)));
            return emit(argc);
        }
        std::string name = callee->kind == K::Name ? callee->text : callee->kind == K::Member ? qualified(callee) : "";
        if (name.empty()) throw err("isso não é uma função");
        auto it = obj.funcIndex.find(name);
        if (it == obj.funcIndex.end()) return native(name, argc);
        int arity = obj.funcs[it->second].arity;
        if (arity != argc) throw err(name + "() recebe " + std::to_string(arity) + " argumento(s), veio " + std::to_string(argc));
        emit(OP_CALL, it->second);
        emit(argc);
    }

    void expr(Node* n) {
        line = n->line;
        switch (n->kind) {
        case K::Lit:
            if (n->value.index() == 0) emit(OP_NIL); else emit(OP_CONST, constant(n->value));
            break;
        case K::Name: {
            int s = local(n->text);
            if (s >= 0) { emit(OP_GET_LOCAL, s); break; }
            s = field(n->text);
            if (s >= 0) { emit(OP_GET_FIELD, s); break; }
            if (objects.count(n->text)) { emit(OP_CONST, constant(Value(n->text))); break; }  // object type = its name
            throw err("'" + n->text + "' não foi declarada");
        }
        case K::Member: {
            std::string q = qualified(n);
            if (q.empty()) {  // v.x on a vec3, enemy.hp on an instance
                expr(n->kids[0].get());
                emit(OP_GET_MEMBER, constant(Value(n->text)));
                break;
            }
            auto c = vm.constants.find(q);  // Button.A (constant) or time.delta (SDK property = zero-arg native)
            if (c != vm.constants.end()) emit(OP_CONST, constant(c->second));
            else if (vm.nativeIndex.count(q)) native(q, 0);
            else throw err("'" + q + "' não existe");
            break;
        }
        case K::Index: expr(n->kids[0].get()); expr(n->kids[1].get()); emit(OP_INDEX); break;
        case K::Array:
            for (auto& k : n->kids) expr(k.get());
            emit(OP_ARRAY, (int)n->kids.size());
            break;
        case K::Unary: expr(n->kids[0].get()); emit(n->text == "-" ? OP_NEG : OP_NOT); break;
        case K::Binary:
            expr(n->kids[0].get());
            if (n->text == "&&" || n->text == "||") {  // short-circuit: the deciding operand is the result
                emit(OP_DUP);
                int j = jump(n->text == "&&" ? OP_JF : OP_JT);
                emit(OP_POP);
                expr(n->kids[1].get());
                patch(j);
            } else {
                expr(n->kids[1].get());
                line = n->line;
                emit(binop(n->text));
            }
            break;
        case K::Call: call(n); break;
        default: throw err("isso não é uma expressão");
        }
    }
};

struct Parsed {
    std::shared_ptr<ObjectDef> obj;
    std::vector<P> inits;  // field initializers, parallel to obj->fields
    std::vector<FuncDecl> decls;
};

static Parsed parse(const SourceFile& src, const VM& vm) {
    Parser ps{lex(src.source, src.file), 0, src.file};
    Parsed out;
    out.obj = std::make_shared<ObjectDef>();
    ObjectDef* obj = out.obj.get();
    std::vector<P>& inits = out.inits;
    std::vector<FuncDecl>& decls = out.decls;
    obj->file = src.file;
    ps.expect("object");
    obj->name = ps.ident();

    while (ps.peek().kind != Token::End) {
        if (ps.accept(";")) continue;
        if (ps.accept("use")) {
            std::string c = ps.ident();
            if (!vm.components.count(c)) throw ps.err("componente desconhecido '" + c + "'");
            obj->uses.push_back(c);
        } else if (ps.accept("var")) {
            std::string name = ps.ident();
            if (std::count(obj->fields.begin(), obj->fields.end(), name)) throw ps.err("'" + name + "' declarada duas vezes");
            obj->fields.push_back(name);
            inits.push_back(ps.accept("=") ? ps.expr() : nullptr);
        } else if (ps.accept("function")) {
            FuncDecl d;
            d.name = ps.ident();
            if (obj->funcIndex.count(d.name)) throw ps.err("função '" + d.name + "' declarada duas vezes");
            obj->funcIndex[d.name] = (int)decls.size() + 1;
            ps.expect("(");
            if (!ps.accept(")")) {
                do d.params.push_back(ps.ident()); while (ps.accept(","));
                ps.expect(")");
            }
            d.body = ps.block();
            decls.push_back(std::move(d));
        } else {
            throw ps.err("esperado 'use', 'var' ou 'function', encontrado '" + ps.peek().text + "'");
        }
    }

    // Fields the components bring (`use Rigidbody` -> velocity...), unless the object declared them itself.
    for (auto& c : obj->uses) {
        for (auto& [name, value] : vm.components.at(c)) {
            if (std::count(obj->fields.begin(), obj->fields.end(), name)) continue;
            obj->fields.push_back(name);
            auto lit = std::make_unique<Node>();
            lit->kind = K::Lit;
            lit->line = 1;
            lit->value = value;
            inits.push_back(std::move(lit));
        }
    }
    return out;
}

static void generate(Parsed& p, const VM& vm, bool privileged, const std::unordered_set<std::string>& objects) {
    ObjectDef& obj = *p.obj;
    // All signatures first, so bodies can call functions declared further down.
    obj.funcs.resize(p.decls.size() + 1);
    obj.funcs[0].name = "__init";
    for (size_t i = 0; i < p.decls.size(); i++) {
        obj.funcs[i + 1].name = p.decls[i].name;
        obj.funcs[i + 1].arity = (int)p.decls[i].params.size();
    }
    Codegen g{obj, vm, privileged, objects};
    g.f = &obj.funcs[0];
    for (size_t i = 0; i < p.inits.size(); i++) {
        if (!p.inits[i]) continue;
        g.expr(p.inits[i].get());
        g.emit(OP_SET_FIELD, (int)i);
    }
    g.emit(OP_NIL);
    g.emit(OP_RET);
    for (size_t i = 0; i < p.decls.size(); i++) g.function(obj.funcs[i + 1], p.decls[i].params, p.decls[i].body.get());
}

std::vector<std::shared_ptr<ObjectDef>> compileAll(const std::vector<SourceFile>& files, const VM& vm, bool privileged) {
    std::vector<Parsed> parsed;
    std::unordered_set<std::string> names;
    for (auto& f : files) {
        parsed.push_back(parse(f, vm));
        if (!names.insert(parsed.back().obj->name).second)
            throw DooError(f.file + ":1: o objeto '" + parsed.back().obj->name + "' já existe em outro arquivo");
    }
    std::vector<std::shared_ptr<ObjectDef>> out;
    for (auto& p : parsed) {
        generate(p, vm, privileged, names);
        out.push_back(p.obj);
    }
    return out;
}

std::shared_ptr<ObjectDef> compile(const std::string& source, const std::string& file, const VM& vm, bool privileged) {
    return compileAll({{file, source}}, vm, privileged)[0];
}
