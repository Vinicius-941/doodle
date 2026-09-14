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
    static const char* ops[] = {"++", "--", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "(", ")", "{",
                                "}", "[", "]", ",", ".", ";", "+", "-", "*", "/", "%", "<", ">", "=", "!", "?", ":"};
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

enum class K { Lit, Name, Member, Index, Call, Array, Unary, Binary, Var, Assign, If, While, For, Return, Block, Expr, With, Break, Continue, Ternary, Struct };

// Generic AST node. kids layout per kind:
//   Member: [obj] text=member   Index: [arr, i]   Call: [callee, args...]   Unary/Binary: operands, text=op
//   Var: [init?] text=name   Assign: [target, value] text=op   If: [cond, then, else?]   While: [cond, body]
//   For: [init|null, cond|null, step|null, body]   Return: [value?]   Block: stmts   Expr: [expr]   With: [alvo, body]   Ternary: [cond, sim, nao]   Struct: [chave, valor, chave, valor...]
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
    int line = 0;
};

struct FieldDecl {
    std::string name;
    P init;  // null: starts as nil
};

// One parsed .doo file, before its parent (extends) is merged in.
struct Parsed {
    std::string name, parent, file;
    int line = 1;  // of the `object` declaration
    std::vector<std::string> uses;
    std::vector<FieldDecl> fields;
    std::vector<FuncDecl> decls;
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
        if (accept("break")) return node(K::Break);
        if (accept("continue")) return node(K::Continue);
        if (accept("with")) {  // with (Inimigo) { hp -= 1 }: o bloco roda como cada um deles, como na GML
            auto n = node(K::With);
            expect("(");
            n->kids.push_back(expr());
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
        if (accept("++") || accept("--")) return increment(expr(), t[p - 1].text);  // ++i
        P e = expr();
        if (accept("++") || accept("--")) return increment(std::move(e), t[p - 1].text);  // i++
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

    // i++ é i += 1 (só como comando: não vale dentro de uma expressão, como a[i++])
    P increment(P alvo, const std::string& op) {
        if (alvo->kind != K::Name && alvo->kind != K::Index && alvo->kind != K::Member)
            throw err("só dá pra usar " + op + " em variável, elemento de array ou membro");
        auto n = node(K::Assign, op == "++" ? "+=" : "-=");
        auto um = node(K::Lit);
        um->value = Value(1.0);
        n->kids.push_back(std::move(alvo));
        n->kids.push_back(std::move(um));
        return n;
    }

    P expr() {  // cond ? sim : nao, com a menor precedência (e da direita para a esquerda)
        P c = binary(0);
        if (!accept("?")) return c;
        auto n = node(K::Ternary);
        n->kids.push_back(std::move(c));
        n->kids.push_back(expr());
        expect(":");
        n->kids.push_back(expr());
        return n;
    }

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
        if (accept("{")) {  // { hp: 10, "nome completo": "Ana" }: só aparece onde cabe uma expressão
            auto n = node(K::Struct);
            if (!accept("}")) {
                do {
                    auto chave = node(K::Lit);
                    if (t[p].kind != Token::Ident && t[p].kind != Token::Str) throw err("chave do struct: nome ou texto");
                    chave->value = Value(t[p++].text);
                    n->kids.push_back(std::move(chave));
                    expect(":");
                    n->kids.push_back(expr());
                } while (accept(","));
                expect("}");
            }
            return n;
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
    const std::vector<const Parsed*>& chain;          // base first; the last one is `obj`
    size_t owner = 0;                                 // whose code is being compiled (for super and errors)
    Function* f = nullptr;
    std::vector<std::pair<std::string, int>> scope;  // visible locals -> slot
    int line = 0;
    int withDepth = 0;  // > 0: dentro de with, onde campos e funções são de quem o bloco está rodando como
    struct Laco {
        std::vector<int> saidas, voltas;  // saltos de break e de continue, ajustados quando o laço termina
        bool with = false;                // sair de um with precisa devolver quem estava rodando
    };
    std::vector<Laco> lacos;

    void apontar(const std::vector<int>& saltos, int destino) {
        for (int slot : saltos) f->code[slot] = destino;
    }

    DooError err(const std::string& m) const { return DooError(chain[owner]->file + ":" + std::to_string(line) + ": " + m); }
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

    void function(Function& fn, const std::vector<std::string>& params, const Node* body) {
        f = &fn;
        scope.clear();
        for (auto& p : params) declare(p);
        stmt(body);
        emit(OP_NIL);
        emit(OP_RET);
    }

    void stmt(const Node* n) {
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
        case K::With: with(n); break;
        case K::Break:
        case K::Continue: {
            if (lacos.empty()) throw err(std::string(n->kind == K::Break ? "break" : "continue") + " fora de um laço");
            if (lacos.back().with) emit(OP_WITH_RESTORE);
            int salto = jump(OP_JMP);
            (n->kind == K::Break ? lacos.back().saidas : lacos.back().voltas).push_back(salto);
            break;
        }
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
            lacos.emplace_back();
            stmt(n->kids[1].get());
            Laco l = std::move(lacos.back());
            lacos.pop_back();
            apontar(l.voltas, top);
            emit(OP_JMP, top);
            patch(jf);
            apontar(l.saidas, here());
            break;
        }
        case K::For: {
            size_t mark = scope.size();
            if (n->kids[0]) stmt(n->kids[0].get());
            int top = here(), jf = -1;
            if (n->kids[1]) { expr(n->kids[1].get()); jf = jump(OP_JF); }
            lacos.emplace_back();
            stmt(n->kids[3].get());
            Laco l = std::move(lacos.back());
            lacos.pop_back();
            apontar(l.voltas, here());  // continue ainda roda o passo (i += 1)
            if (n->kids[2]) stmt(n->kids[2].get());
            emit(OP_JMP, top);
            if (jf >= 0) patch(jf);
            apontar(l.saidas, here());
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

    // with (alvo) corpo: vira a lista de instâncias (uma foto, tirada na entrada) e um laço que roda o corpo
    // como cada uma. Os locais da função continuam visíveis lá dentro, como na GML.
    void with(const Node* n) {
        expr(n->kids[0].get());
        native("__with_targets", 1);
        size_t mark = scope.size();
        int lista = declare(" with.lista"), i = declare(" with.i");  // com espaço: nenhum código escreve esse nome
        emit(OP_SET_LOCAL, lista);
        emit(OP_CONST, constant(Value(0.0)));
        emit(OP_SET_LOCAL, i);
        int topo = here();
        emit(OP_GET_LOCAL, i);
        emit(OP_GET_LOCAL, lista);
        native("array_length", 1);
        emit(OP_LT);
        int fim = jump(OP_JF);
        emit(OP_GET_LOCAL, lista);
        emit(OP_GET_LOCAL, i);
        emit(OP_INDEX);
        int pula = jump(OP_WITH_SELF);
        lacos.push_back({{}, {}, true});
        withDepth++;
        stmt(n->kids[1].get());
        withDepth--;
        Laco l = std::move(lacos.back());
        lacos.pop_back();
        line = n->line;
        emit(OP_WITH_RESTORE);
        patch(pula);
        apontar(l.voltas, here());  // continue: próximo alvo
        emit(OP_GET_LOCAL, i);
        emit(OP_CONST, constant(Value(1.0)));
        emit(OP_ADD);
        emit(OP_SET_LOCAL, i);
        emit(OP_JMP, topo);
        patch(fim);
        apontar(l.saidas, here());
        scope.resize(mark);
    }


    // Dentro de with o tipo do alvo não é conhecido na compilação, então campo é por nome em quem roda (x/y/z
    // viram position na VM). Membro vazio = o próprio campo (hp -= 1); senão, membro dele (velocity.y = 9).
    void assignSelf(const std::string& base, const std::string& membro, const std::string& op, const Node* valor) {
        int kb = constant(Value(base));
        emit(OP_SELF);
        if (membro.empty()) {
            if (!op.empty()) {
                emit(OP_SELF);
                emit(OP_GET_MEMBER, kb);
            }
        } else {
            emit(OP_SELF);
            emit(OP_GET_MEMBER, kb);
            if (!op.empty()) {
                emit(OP_DUP);
                emit(OP_GET_MEMBER, constant(Value(membro)));
            }
        }
        expr(valor);
        if (!op.empty()) emit(binop(op));
        if (!membro.empty()) emit(OP_SET_MEMBER, constant(Value(membro)));
        emit(OP_SET_MEMBER, kb);
        emit(OP_POP);
    }

    // position.x/y/z quando o objeto não declarou x/y/z: devolve o slot de position, senão -1
    int xyz(const std::string& name) const {
        if (name != "x" && name != "y" && name != "z") return -1;
        return field("position");
    }

    void assign(const Node* n) {
        Node* target = n->kids[0].get();
        std::string op = n->text.substr(0, n->text.size() - 1);  // "+=" -> "+", "=" -> ""
        const Node* base = target->kind == K::Member ? target->kids[0].get() : target;
        bool baseLivre = base->kind == K::Name && local(base->text) < 0;
        bool refDireta = baseLivre && target->kind == K::Member && (base->text == "self" || base->text == "other");
        if (refDireta) {  // other.pontos += 1: é uma ref, altera a instância direto
            int km = constant(Value(target->text));
            expr(base);
            if (!op.empty()) {
                emit(OP_DUP);
                emit(OP_GET_MEMBER, km);
            }
            expr(n->kids[1].get());
            line = n->line;
            if (!op.empty()) emit(binop(op));
            emit(OP_SET_MEMBER, km);
            return emit(OP_POP);
        }
        if (withDepth > 0 && baseLivre) {
            if (target->kind == K::Name) return assignSelf(target->text, "", op, n->kids[1].get());
            if (target->kind == K::Member) return assignSelf(base->text, target->text, op, n->kids[1].get());
        }
        if (target->kind == K::Name && local(target->text) < 0 && field(target->text) < 0) {
            if (int p = xyz(target->text); p >= 0) {  // x += 5 mexe em position.x
                int k = constant(Value(target->text));
                emit(OP_GET_FIELD, p);
                if (!op.empty()) {
                    emit(OP_GET_FIELD, p);
                    emit(OP_GET_MEMBER, k);
                }
                expr(n->kids[1].get());
                line = n->line;
                if (!op.empty()) emit(binop(op));
                emit(OP_SET_MEMBER, k);
                return emit(OP_SET_FIELD, p);
            }
        }
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
    std::string qualified(const Node* m) const {
        if (withDepth > 0) return {};
        Node* base = m->kids[0].get();
        if (base->kind != K::Name || local(base->text) >= 0 || field(base->text) >= 0) return {};
        return base->text + "." + m->text;
    }

    void native(const std::string& name, int argc) {
        auto it = vm.nativeIndex.find(name);
        if (it == vm.nativeIndex.end()) throw err("função desconhecida '" + name + "'");
        if (!privileged && nativaDoFirmware(name)) throw err("'" + name + "' é exclusiva do firmware");  // jogo não instala jogo
        emit(OP_NATIVE, it->second);
        emit(argc);
    }

    // super.f(args): the version of f from the nearest ancestor of the code's owner
    bool superCall(const Node* n) {
        Node* callee = n->kids[0].get();
        if (callee->kind != K::Member || callee->kids[0]->kind != K::Name || callee->kids[0]->text != "super") return false;
        if (local("super") >= 0 || field("super") >= 0) return false;
        std::string target;
        for (size_t k = owner; k-- > 0 && target.empty();)
            for (auto& d : chain[k]->decls) if (d.name == callee->text) target = chain[k]->name + "." + d.name;
        if (target.empty()) throw err("nenhum objeto pai de " + chain[owner]->name + " define " + callee->text + "()");
        int idx = obj.funcIndex.at(target), argc = (int)n->kids.size() - 1;
        if (obj.funcs[idx].arity != argc)
            throw err(callee->text + "() recebe " + std::to_string(obj.funcs[idx].arity) + " argumento(s), veio " + std::to_string(argc));
        for (size_t i = 1; i < n->kids.size(); i++) expr(n->kids[i].get());
        line = n->line;
        emit(OP_CALL, idx);
        emit(argc);
        return true;
    }

    void call(const Node* n) {
        Node* callee = n->kids[0].get();
        bool chamaSuper = callee->kind == K::Member && callee->kids[0]->kind == K::Name && callee->kids[0]->text == "super";
        if (withDepth > 0 && chamaSuper) throw err("super não funciona dentro de with");
        if (superCall(n)) return;
        int argc = (int)n->kids.size() - 1;
        bool method = callee->kind == K::Member && qualified(callee).empty();  // enemy.take_damage(10)
        bool deQuemRoda = withDepth > 0 && callee->kind == K::Name && !vm.nativeIndex.count(callee->text);
        if (method) expr(callee->kids[0].get());
        if (deQuemRoda) emit(OP_SELF);
        for (size_t i = 1; i < n->kids.size(); i++) expr(n->kids[i].get());
        line = n->line;
        if (method || deQuemRoda) {
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

    void expr(const Node* n) {
        line = n->line;
        switch (n->kind) {
        case K::Lit:
            if (n->value.index() == 0) emit(OP_NIL); else emit(OP_CONST, constant(n->value));
            break;
        case K::Name: {
            if (n->text == "other" && withDepth > 0) { emit(OP_OTHER); break; }  // quem abriu o with
            int s = local(n->text);
            if (s >= 0) { emit(OP_GET_LOCAL, s); break; }
            if (n->text == "self") { emit(OP_SELF); break; }
            if (withDepth > 0) {  // o que é global primeiro; o resto é campo de quem o bloco roda como
                if (objects.count(n->text)) { emit(OP_CONST, constant(Value(n->text))); break; }
                if (auto c = vm.constants.find(n->text); c != vm.constants.end()) { emit(OP_CONST, constant(c->second)); break; }
                if (vm.nativeIndex.count(n->text)) { native(n->text, 0); break; }
                emit(OP_SELF);
                emit(OP_GET_MEMBER, constant(Value(n->text)));
                break;
            }
            s = field(n->text);
            if (s >= 0) { emit(OP_GET_FIELD, s); break; }
            if (objects.count(n->text)) { emit(OP_CONST, constant(Value(n->text))); break; }  // object type = its name
            if (auto c = vm.constants.find(n->text); c != vm.constants.end()) {  // pi, btn_a, mesh_cube...
                emit(OP_CONST, constant(c->second));
                break;
            }
            if (vm.nativeIndex.count(n->text)) { native(n->text, 0); break; }  // delta_time e outras sem parênteses
            if (int p = xyz(n->text); p >= 0) {  // x/y/z soltos são position.x/y/z, como na GML
                emit(OP_GET_FIELD, p);
                emit(OP_GET_MEMBER, constant(Value(n->text)));
                break;
            }
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
        case K::Struct:
            for (auto& k : n->kids) expr(k.get());
            emit(OP_STRUCT, (int)n->kids.size() / 2);
            break;
        case K::Ternary: {
            expr(n->kids[0].get());
            int nao = jump(OP_JF);
            expr(n->kids[1].get());
            int fim = jump(OP_JMP);
            patch(nao);
            expr(n->kids[2].get());
            patch(fim);
            break;
        }
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

static Parsed parse(const SourceFile& src, const VM& vm) {
    Parser ps{lex(src.source, src.file), 0, src.file};
    Parsed out;
    out.file = src.file;
    ps.expect("object");
    out.name = ps.ident();
    out.line = ps.t[ps.p - 1].line;
    if (ps.accept("extends")) out.parent = ps.ident();

    while (ps.peek().kind != Token::End) {
        if (ps.accept(";")) continue;
        if (ps.accept("use")) {
            std::string c = ps.ident();
            if (!vm.components.count(c)) throw ps.err("componente desconhecido '" + c + "'");
            out.uses.push_back(c);
        } else if (ps.accept("var")) {
            std::string name = ps.ident();
            for (auto& f : out.fields) if (f.name == name) throw ps.err("'" + name + "' declarada duas vezes");
            P init = ps.accept("=") ? ps.expr() : nullptr;
            out.fields.push_back({name, std::move(init)});
        } else if (ps.accept("function")) {
            FuncDecl d;
            d.line = ps.t[ps.p - 1].line;
            d.name = ps.ident();
            for (auto& o : out.decls) if (o.name == d.name) throw ps.err("função '" + d.name + "' declarada duas vezes");
            ps.expect("(");
            if (!ps.accept(")")) {
                do d.params.push_back(ps.ident()); while (ps.accept(","));
                ps.expect(")");
            }
            d.body = ps.block();
            out.decls.push_back(std::move(d));
        } else {
            throw ps.err("esperado 'use', 'var' ou 'function', encontrado '" + ps.peek().text + "'");
        }
    }
    return out;
}

using ParsedByName = std::unordered_map<std::string, const Parsed*>;

static DooError errorAt(const Parsed& p, int line, const std::string& m) {
    return DooError(p.file + ":" + std::to_string(line) + ": " + m);
}

static std::vector<const Parsed*> chainOf(const Parsed& x, const ParsedByName& all) {  // x and its ancestors, base first
    std::vector<const Parsed*> chain{&x};
    while (!chain.back()->parent.empty()) {
        const Parsed& p = *chain.back();
        auto it = all.find(p.parent);
        if (it == all.end()) throw errorAt(p, p.line, "o objeto pai '" + p.parent + "' não existe");
        if (std::count(chain.begin(), chain.end(), it->second)) throw errorAt(x, x.line, "herança em círculo passando por '" + x.name + "'");
        chain.push_back(it->second);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// The flat ObjectDef of x: fields, components and functions of its whole chain (base first).
// Each object in the chain gets "Name.__init" (its own initializers, run base first so derived values win)
// and "Name.f" per function; plain "f" is the most derived version, so every call is virtual.
static std::shared_ptr<ObjectDef> generate(const Parsed& x, const ParsedByName& all, const VM& vm, bool privileged,
                                           const std::unordered_set<std::string>& objects) {
    auto chain = chainOf(x, all);
    auto obj = std::make_shared<ObjectDef>();
    obj->name = x.name;
    obj->file = x.file;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) obj->kinds.push_back((*it)->name);

    auto slot = [&](const std::string& n) {
        auto it = std::find(obj->fields.begin(), obj->fields.end(), n);
        if (it != obj->fields.end()) return int(it - obj->fields.begin());
        obj->fields.push_back(n);
        return int(obj->fields.size()) - 1;
    };
    std::vector<std::vector<std::pair<int, const Node*>>> inits(chain.size());  // per object: field slot <- initializer
    std::vector<P> defaults;                                                       // component defaults as literals
    for (size_t k = 0; k < chain.size(); k++) {
        const Parsed& a = *chain[k];
        for (auto& f : a.fields) {
            int s = slot(f.name);
            if (f.init) inits[k].push_back({s, f.init.get()});
        }
        for (auto& c : a.uses) {  // `use Rigidbody` adds velocity..., unless declared already (here or by an ancestor)
            if (!std::count(obj->uses.begin(), obj->uses.end(), c)) obj->uses.push_back(c);
            for (auto& [name, value] : vm.components.at(c)) {
                if (std::count(obj->fields.begin(), obj->fields.end(), name)) continue;
                auto lit = std::make_unique<Node>();
                lit->kind = K::Lit;
                lit->line = a.line;
                lit->value = value;
                inits[k].push_back({slot(name), lit.get()});
                defaults.push_back(std::move(lit));
            }
        }
    }

    if (!std::count(obj->fields.begin(), obj->fields.end(), "alarm")) {  // toda instância tem alarm[0..7], -1 = desligado
        auto arr = std::make_unique<Node>();
        arr->kind = K::Array;
        arr->line = 1;
        for (int i = 0; i < 8; i++) {
            auto lit = std::make_unique<Node>();
            lit->kind = K::Lit;
            lit->line = 1;
            lit->value = Value(-1.0);
            arr->kids.push_back(std::move(lit));
        }
        inits[0].push_back({slot("alarm"), arr.get()});
        defaults.push_back(std::move(arr));
    }

    // Every function slot first (bodies may call functions declared further down), then the code.
    obj->funcs.resize(1);
    obj->funcs[0].name = "__init";
    obj->funcs[0].file = x.file;
    auto add = [&](const std::string& key, const std::string& name, int arity, const std::string& file) {
        obj->funcIndex[key] = (int)obj->funcs.size();
        obj->funcs.emplace_back();
        obj->funcs.back().name = name;
        obj->funcs.back().arity = arity;
        obj->funcs.back().file = file;
        return (int)obj->funcs.size() - 1;
    };
    for (auto a : chain) add(a->name + ".__init", "__init", 0, a->file);
    for (auto a : chain) {
        for (auto& d : a->decls) {
            int arity = (int)d.params.size();
            auto prev = obj->funcIndex.find(d.name);
            if (prev != obj->funcIndex.end() && obj->funcs[prev->second].arity != arity)
                throw errorAt(*a, d.line, d.name + "() substitui a versão do pai, mas com " + std::to_string(arity) +
                                              " parâmetro(s) em vez de " + std::to_string(obj->funcs[prev->second].arity));
            int idx = add(a->name + "." + d.name, d.name, arity, a->file);
            obj->funcIndex[d.name] = idx;
        }
    }

    Codegen g{*obj, vm, privileged, objects, chain};
    g.owner = chain.size() - 1;
    g.f = &obj->funcs[0];
    for (auto a : chain) {
        g.emit(OP_CALL, obj->funcIndex.at(a->name + ".__init"));
        g.emit(0);
        g.emit(OP_POP);
    }
    g.emit(OP_NIL);
    g.emit(OP_RET);
    for (size_t k = 0; k < chain.size(); k++) {
        g.owner = k;
        g.f = &obj->funcs[obj->funcIndex.at(chain[k]->name + ".__init")];
        g.scope.clear();
        for (auto& [s, init] : inits[k]) {
            g.expr(init);
            g.emit(OP_SET_FIELD, s);
        }
        g.emit(OP_NIL);
        g.emit(OP_RET);
        for (auto& d : chain[k]->decls)
            g.function(obj->funcs[obj->funcIndex.at(chain[k]->name + "." + d.name)], d.params, d.body.get());
    }
    return obj;
}

std::vector<std::shared_ptr<ObjectDef>> compileAll(const std::vector<SourceFile>& files, const VM& vm, bool privileged,
                                                   const std::vector<SourceFile>& library) {
    std::vector<Parsed> parsed;
    for (auto& f : files) parsed.push_back(parse(f, vm));
    for (auto& f : library) parsed.push_back(parse(f, vm));
    ParsedByName all;  // pointers into `parsed`, which no longer grows
    std::unordered_set<std::string> names;
    for (size_t i = 0; i < parsed.size(); i++) {
        Parsed& p = parsed[i];
        if (all.count(p.name)) {
            if (i < files.size()) throw errorAt(p, p.line, "o objeto '" + p.name + "' já existe em outro arquivo");
            continue;  // the program's own object wins over an SDK prefab with the same name
        }
        all[p.name] = &p;
        names.insert(p.name);
    }
    std::vector<std::shared_ptr<ObjectDef>> out;
    for (auto& p : parsed)
        if (all.at(p.name) == &p) out.push_back(generate(p, all, vm, privileged, names));
    return out;
}

std::shared_ptr<ObjectDef> compile(const std::string& source, const std::string& file, const VM& vm, bool privileged) {
    return compileAll({{file, source}}, vm, privileged)[0];
}
