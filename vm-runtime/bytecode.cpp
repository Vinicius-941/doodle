#include "bytecode.h"

#include <cstring>
#include <map>
#include <unordered_map>

namespace {

const char MAGICO[6] = {'D', 'O', 'O', 'B', 'C', 0};
const uint16_t VERSAO = 1;  // suba quando mudar opcode ou formato: arquivo de outra versão é recusado

// Quantos operandos cada opcode tem (na ordem do enum Op).
int operandos(int op) {
    switch (op) {
    case OP_CONST: case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_GET_FIELD: case OP_SET_FIELD:
    case OP_JMP: case OP_JF: case OP_JT: case OP_ARRAY: case OP_GET_MEMBER: case OP_SET_MEMBER:
    case OP_WITH_SELF: case OP_STRUCT:
        return 1;
    case OP_CALL: case OP_NATIVE: case OP_INVOKE:
        return 2;
    default:
        return 0;
    }
}

// ---------- escrita ----------

struct Escritor {
    std::string out;
    void u8(uint8_t v) { out.push_back(char(v)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f64(double d) {
        uint64_t v;
        memcpy(&v, &d, 8);
        for (int i = 0; i < 8; i++) u8(uint8_t(v >> (8 * i)));
    }
    void str(const std::string& s) {
        u32(uint32_t(s.size()));
        out += s;
    }
    void strs(const std::vector<std::string>& v) {
        u32(uint32_t(v.size()));
        for (auto& s : v) str(s);
    }
};

// ---------- leitura, sempre conferindo o tamanho ----------

struct Leitor {
    const std::string& in;
    size_t p = 0;
    std::string nome;
    [[noreturn]] void falha(const std::string& m) const { throw DooError(nome + ": " + m); }
    void precisa(size_t n) const {
        if (n > in.size() - p) falha("arquivo cortado ou corrompido");
    }
    uint8_t u8() {
        precisa(1);
        return uint8_t(in[p++]);
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= uint32_t(u8()) << (8 * i);
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    double f64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= uint64_t(u8()) << (8 * i);
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
    uint32_t contagem() {  // quantidade de itens: cada um ocupa pelo menos 1 byte, então não pode passar do resto
        uint32_t n = u32();
        if (n > in.size() - p) falha("arquivo corrompido (quantidade impossível)");
        return n;
    }
    std::string str() {
        uint32_t n = u32();
        precisa(n);
        std::string s = in.substr(p, n);
        p += n;
        return s;
    }
    std::vector<std::string> strs() {
        std::vector<std::string> v(contagem());
        for (auto& s : v) s = str();
        return v;
    }
};

}  // namespace

std::string saveBytecode(const std::vector<std::shared_ptr<ObjectDef>>& defs, const VM& vm) {
    std::unordered_map<int, std::string> nomeDaNativa;
    for (auto& [n, i] : vm.nativeIndex) nomeDaNativa[i] = n;

    // tabela das nativas usadas: o código guarda a posição nela, não o índice da VM
    std::vector<std::string> tabela;
    std::unordered_map<int, int> posicao;
    std::vector<std::vector<std::vector<int>>> codigos;  // cópia do código com o operando das nativas trocado
    for (auto& d : defs) {
        codigos.emplace_back();
        for (auto& f : d->funcs) {
            std::vector<int> c = f.code;
            for (size_t pc = 0; pc < c.size(); pc += 1 + operandos(c[pc])) {
                if (c[pc] != OP_NATIVE) continue;
                int idx = c[pc + 1];
                if (!posicao.count(idx)) {
                    posicao[idx] = (int)tabela.size();
                    tabela.push_back(nomeDaNativa.at(idx));
                }
                c[pc + 1] = posicao[idx];
            }
            codigos.back().push_back(std::move(c));
        }
    }

    Escritor w;
    w.out.append(MAGICO, 6);
    w.u8(uint8_t(VERSAO));
    w.u8(uint8_t(VERSAO >> 8));
    w.strs(tabela);
    w.u32(uint32_t(defs.size()));
    for (size_t o = 0; o < defs.size(); o++) {
        const ObjectDef& d = *defs[o];
        w.str(d.name);
        w.str(d.file);
        w.strs(d.kinds);
        w.strs(d.fields);
        w.strs(d.uses);
        w.u32(uint32_t(d.funcs.size()));
        for (size_t fi = 0; fi < d.funcs.size(); fi++) {
            const Function& f = d.funcs[fi];
            w.str(f.name);
            w.str(f.file);
            w.i32(f.arity);
            w.i32(f.nlocals);
            w.u32(uint32_t(codigos[o][fi].size()));
            for (int x : codigos[o][fi]) w.i32(x);
            w.u32(uint32_t(f.lines.size()));
            for (int x : f.lines) w.i32(x);
            w.u32(uint32_t(f.consts.size()));
            for (auto& k : f.consts) {
                if (std::holds_alternative<std::monostate>(k)) {
                    w.u8(0);
                } else if (auto b = std::get_if<bool>(&k)) {
                    w.u8(1);
                    w.u8(*b);
                } else if (auto n = std::get_if<double>(&k)) {
                    w.u8(2);
                    w.f64(*n);
                } else if (auto s = std::get_if<std::string>(&k)) {
                    w.u8(3);
                    w.str(*s);
                } else if (auto v = std::get_if<Vec3>(&k)) {  // padrões de componente: position = vec3()
                    w.u8(4);
                    w.f64(v->x);
                    w.f64(v->y);
                    w.f64(v->z);
                } else {
                    throw DooError(d.file + ": constante que não cabe no bytecode (" + toString(k) + ")");
                }
            }
        }
        w.u32(uint32_t(d.funcIndex.size()));
        for (auto& [k, i] : std::map<std::string, int>(d.funcIndex.begin(), d.funcIndex.end())) {  // em ordem: mesmos bytes sempre
            w.str(k);
            w.i32(i);
        }
    }
    return w.out;
}

std::vector<std::shared_ptr<ObjectDef>> loadBytecode(const std::string& data, const VM& vm, bool privileged,
                                                     const std::string& nome) {
    Leitor r{data, 0, nome};
    r.precisa(8);
    if (data.compare(0, 6, std::string(MAGICO, 6)) != 0) r.falha("não é um arquivo de bytecode do Doodle");
    r.p = 6;
    unsigned versao = r.u8() | (unsigned(r.u8()) << 8);
    if (versao != VERSAO)
        r.falha("compilado para a versão " + std::to_string(versao) + " do bytecode; este console lê a " +
                std::to_string(VERSAO) + ". Compile o jogo de novo");

    // nativas por nome -> índice desta VM, recusando o que não existe e o que é do firmware
    std::vector<int> nativa;
    for (auto& n : r.strs()) {
        auto it = vm.nativeIndex.find(n);
        if (it == vm.nativeIndex.end()) r.falha("o jogo pede a função '" + n + "', que este console não tem");
        if (!privileged && nativaDoFirmware(n)) r.falha("'" + n + "' é exclusiva do firmware");
        nativa.push_back(it->second);
    }

    std::vector<std::shared_ptr<ObjectDef>> defs(r.contagem());
    if (defs.empty()) r.falha("nenhum objeto no arquivo");
    for (auto& dp : defs) {
        dp = std::make_shared<ObjectDef>();
        ObjectDef& d = *dp;
        d.name = r.str();
        d.file = r.str();
        d.kinds = r.strs();
        d.fields = r.strs();
        d.uses = r.strs();
        d.funcs.resize(r.contagem());
        if (d.funcs.empty()) r.falha(d.name + ": objeto sem __init");
        for (auto& f : d.funcs) {
            f.name = r.str();
            f.file = r.str();
            f.arity = r.i32();
            f.nlocals = r.i32();
            f.code.resize(r.contagem());
            for (int& x : f.code) x = r.i32();
            f.lines.resize(r.contagem());
            for (int& x : f.lines) x = r.i32();
            f.consts.resize(r.contagem());
            for (auto& k : f.consts) {
                switch (r.u8()) {
                case 0: k = Value(); break;
                case 1: k = Value(r.u8() != 0); break;
                case 2: k = Value(r.f64()); break;
                case 3: k = Value(r.str()); break;
                case 4: {
                    double x = r.f64(), y = r.f64(), z = r.f64();
                    k = Value(Vec3{x, y, z});
                    break;
                }
                default: r.falha("constante de tipo desconhecido");
                }
            }
        }
        uint32_t n = r.contagem();
        for (uint32_t i = 0; i < n; i++) {
            std::string k = r.str();
            int idx = r.i32();
            if (idx < 0 || size_t(idx) >= d.funcs.size()) r.falha(d.name + ": índice de função inválido");
            d.funcIndex[k] = idx;
        }

        // cada função: instruções inteiras, operandos dentro dos limites, saltos para o começo de uma
        // instrução, e termina num RET ou JMP (senão a VM andaria para fora do código)
        for (auto& f : d.funcs) {
            std::string onde = d.name + "." + f.name + ": ";
            if (f.arity < 0 || f.nlocals < f.arity) r.falha(onde + "locais inválidos");
            if (f.lines.size() != f.code.size()) r.falha(onde + "tabela de linhas não bate com o código");
            if (f.code.empty()) r.falha(onde + "função vazia");
            std::vector<bool> inicio(f.code.size(), false);
            size_t ultimo = 0;
            for (size_t pc = 0; pc < f.code.size(); pc += 1 + operandos(f.code[pc])) {
                if (f.code[pc] < 0 || f.code[pc] > OP_STRUCT) r.falha(onde + "instrução desconhecida");
                if (pc + operandos(f.code[pc]) >= f.code.size()) r.falha(onde + "instrução cortada");
                inicio[pc] = true;
                ultimo = pc;
            }
            if (f.code[ultimo] != OP_RET && f.code[ultimo] != OP_JMP) r.falha(onde + "termina sem return");
            auto dentro = [&](int v, size_t limite) { return v >= 0 && size_t(v) < limite; };
            auto texto = [&](int v) { return dentro(v, f.consts.size()) && std::holds_alternative<std::string>(f.consts[v]); };
            for (size_t pc = 0; pc < f.code.size(); pc += 1 + operandos(f.code[pc])) {
                int op = f.code[pc], a = operandos(op) > 0 ? f.code[pc + 1] : 0, b = operandos(op) > 1 ? f.code[pc + 2] : 0;
                bool ok = true;
                switch (op) {
                case OP_CONST: ok = dentro(a, f.consts.size()); break;
                case OP_GET_LOCAL: case OP_SET_LOCAL: ok = dentro(a, size_t(f.nlocals)); break;
                case OP_GET_FIELD: case OP_SET_FIELD: ok = dentro(a, d.fields.size()); break;
                case OP_JMP: case OP_JF: case OP_JT: case OP_WITH_SELF: ok = dentro(a, f.code.size()) && inicio[a]; break;
                case OP_CALL: ok = dentro(a, d.funcs.size()) && b >= 0; break;
                case OP_NATIVE:
                    ok = dentro(a, nativa.size()) && b >= 0;
                    if (ok) f.code[pc + 1] = nativa[a];  // posição na tabela do arquivo -> índice desta VM
                    break;
                case OP_ARRAY: case OP_STRUCT: ok = a >= 0; break;
                case OP_GET_MEMBER: case OP_SET_MEMBER: ok = texto(a); break;
                case OP_INVOKE: ok = texto(a) && b >= 0; break;
                }
                if (!ok) r.falha(onde + "operando inválido na posição " + std::to_string(pc));
            }
        }
    }
    if (r.p != data.size()) r.falha("sobrou coisa depois do fim do arquivo");
    return defs;
}
