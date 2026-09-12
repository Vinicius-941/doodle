// Biblioteca padrão da Doo, no formato da GML: funções soltas, sem namespace.
// Nomes e semântica seguem a GML de propósito (posições de texto começam em 1, arrays em 0),
// para quem vem do GameMaker acertar de primeira.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <stdexcept>

#include "vm.h"

namespace {

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

std::string& argStr(std::vector<Value>& a, size_t i, const char* fn) {
    if (i >= a.size()) throw std::runtime_error(std::string(fn) + ": faltou o argumento " + std::to_string(i + 1));
    if (auto s = std::get_if<std::string>(&a[i])) return *s;
    throw std::runtime_error(std::string(fn) + ": esperava um texto");
}

std::shared_ptr<Array>& argArray(std::vector<Value>& a, size_t i, const char* fn) {
    if (i >= a.size()) throw std::runtime_error(std::string(fn) + ": faltou o argumento " + std::to_string(i + 1));
    if (auto p = std::get_if<std::shared_ptr<Array>>(&a[i])) return *p;
    throw std::runtime_error(std::string(fn) + ": esperava um array");
}

// A GML conta caracteres a partir do 1; devolve o índice de C++ já cortado no tamanho do texto.
size_t at1(const std::string& s, double pos) { return (size_t)std::clamp(pos - 1, 0.0, (double)s.size()); }

const double DEG = 3.14159265358979323846 / 180;

}  // namespace

void registerStdlib(VM& vm) {
    auto one = [&vm](const char* name, double (*fn)(double)) {
        vm.addNative(name, [fn](Instance&, std::vector<Value>& a) { return Value(fn(argNum(a, 0))); });
    };
    vm.constants["pi"] = Value(3.14159265358979323846);

    // --- números ---
    one("abs", std::fabs);
    one("sqrt", std::sqrt);
    one("floor", std::floor);
    one("ceil", std::ceil);
    one("exp", std::exp);
    one("ln", std::log);
    one("log2", std::log2);
    one("log10", std::log10);
    one("sin", std::sin);
    one("cos", std::cos);
    one("tan", std::tan);
    one("arcsin", std::asin);
    one("arccos", std::acos);
    one("arctan", std::atan);
    one("round", [](double x) { return std::nearbyint(x); });  // meio para o par, como na GML
    one("frac", [](double x) { return x - std::trunc(x); });
    one("sqr", [](double x) { return x * x; });
    one("sign", [](double x) { return double(x > 0) - double(x < 0); });
    one("dsin", [](double d) { return std::sin(d * DEG); });
    one("dcos", [](double d) { return std::cos(d * DEG); });
    one("dtan", [](double d) { return std::tan(d * DEG); });
    one("degtorad", [](double d) { return d * DEG; });
    one("radtodeg", [](double r) { return r / DEG; });
    vm.addNative("power", [](Instance&, std::vector<Value>& a) { return Value(std::pow(argNum(a, 0), argNum(a, 1))); });
    vm.addNative("arctan2", [](Instance&, std::vector<Value>& a) { return Value(std::atan2(argNum(a, 0), argNum(a, 1))); });
    vm.addNative("min", [](Instance&, std::vector<Value>& a) {
        double m = argNum(a, 0);
        for (size_t i = 1; i < a.size(); i++) m = std::fmin(m, argNum(a, i));
        return Value(m);
    });
    vm.addNative("max", [](Instance&, std::vector<Value>& a) {
        double m = argNum(a, 0);
        for (size_t i = 1; i < a.size(); i++) m = std::fmax(m, argNum(a, i));
        return Value(m);
    });
    vm.addNative("mean", [](Instance&, std::vector<Value>& a) {
        double s = 0;
        for (size_t i = 0; i < a.size(); i++) s += argNum(a, i);
        return Value(a.empty() ? 0 : s / a.size());
    });
    vm.addNative("clamp", [](Instance&, std::vector<Value>& a) {
        return Value(std::fmin(std::fmax(argNum(a, 0), argNum(a, 1)), argNum(a, 2)));
    });
    vm.addNative("lerp", [](Instance&, std::vector<Value>& a) {
        double x = argNum(a, 0), y = argNum(a, 1);
        return Value(x + (y - x) * argNum(a, 2));
    });

    // --- sorteio ---
    vm.addNative("random", [](Instance&, std::vector<Value>& a) {  // 0 <= x < n (n = 1 sem argumento)
        double n = a.empty() ? 1 : argNum(a, 0);
        return Value(std::uniform_real_distribution<double>(0, n)(rng()));
    });
    vm.addNative("random_range", [](Instance&, std::vector<Value>& a) {
        return Value(std::uniform_real_distribution<double>(argNum(a, 0), argNum(a, 1))(rng()));
    });
    vm.addNative("irandom", [](Instance&, std::vector<Value>& a) {  // inteiro de 0 a n, o n incluído (como na GML)
        return Value((double)std::uniform_int_distribution<long long>(0, (long long)argNum(a, 0))(rng()));
    });
    vm.addNative("irandom_range", [](Instance&, std::vector<Value>& a) {
        return Value((double)std::uniform_int_distribution<long long>((long long)argNum(a, 0), (long long)argNum(a, 1))(rng()));
    });
    vm.addNative("choose", [](Instance&, std::vector<Value>& a) {
        if (a.empty()) throw std::runtime_error("choose() precisa de pelo menos um valor");
        return a[std::uniform_int_distribution<size_t>(0, a.size() - 1)(rng())];
    });
    vm.addNative("random_set_seed", [](Instance&, std::vector<Value>& a) {
        rng().seed((unsigned)argNum(a, 0));
        return Value();
    });

    // --- geometria: graus, 0 = direita, crescendo anti-horário (a convenção da GML) ---
    vm.addNative("point_distance", [](Instance&, std::vector<Value>& a) {
        return Value(std::hypot(argNum(a, 2) - argNum(a, 0), argNum(a, 3) - argNum(a, 1)));
    });
    vm.addNative("point_distance_3d", [](Instance&, std::vector<Value>& a) {
        double dx = argNum(a, 3) - argNum(a, 0), dy = argNum(a, 4) - argNum(a, 1), dz = argNum(a, 5) - argNum(a, 2);
        return Value(std::sqrt(dx * dx + dy * dy + dz * dz));
    });
    vm.addNative("point_direction", [](Instance&, std::vector<Value>& a) {
        double d = std::atan2(argNum(a, 1) - argNum(a, 3), argNum(a, 2) - argNum(a, 0)) / DEG;
        return Value(d < 0 ? d + 360 : d);
    });
    vm.addNative("lengthdir_x", [](Instance&, std::vector<Value>& a) {
        return Value(argNum(a, 0) * std::cos(argNum(a, 1) * DEG));
    });
    vm.addNative("lengthdir_y", [](Instance&, std::vector<Value>& a) {
        return Value(-argNum(a, 0) * std::sin(argNum(a, 1) * DEG));
    });
    vm.addNative("angle_difference", [](Instance&, std::vector<Value>& a) {  // -180..180, o caminho mais curto
        double d = std::fmod(argNum(a, 0) - argNum(a, 1), 360.0);
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return Value(d);
    });

    // --- texto: posições começam em 1, como na GML ---
    vm.addNative("string", [](Instance&, std::vector<Value>& a) {
        return Value(a.empty() ? std::string() : toString(a[0]));
    });
    vm.addNative("real", [](Instance&, std::vector<Value>& a) {
        std::string& s = argStr(a, 0, "real");
        try {
            return Value(std::stod(s));
        } catch (const std::exception&) {
            throw std::runtime_error("real(): nao e um numero: " + s);
        }
    });
    vm.addNative("string_length", [](Instance&, std::vector<Value>& a) {
        return Value((double)argStr(a, 0, "string_length").size());
    });
    vm.addNative("string_upper", [](Instance&, std::vector<Value>& a) {
        std::string s = argStr(a, 0, "string_upper");
        for (char& c : s) c = (char)toupper((unsigned char)c);
        return Value(s);
    });
    vm.addNative("string_lower", [](Instance&, std::vector<Value>& a) {
        std::string s = argStr(a, 0, "string_lower");
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return Value(s);
    });
    vm.addNative("string_char_at", [](Instance&, std::vector<Value>& a) {
        std::string& s = argStr(a, 0, "string_char_at");
        size_t i = at1(s, argNum(a, 1));
        return Value(i < s.size() ? s.substr(i, 1) : std::string());
    });
    vm.addNative("string_copy", [](Instance&, std::vector<Value>& a) {  // (texto, comeco, quantidade)
        std::string& s = argStr(a, 0, "string_copy");
        return Value(s.substr(at1(s, argNum(a, 1)), (size_t)std::max(0.0, argNum(a, 2))));
    });
    vm.addNative("string_delete", [](Instance&, std::vector<Value>& a) {
        std::string s = argStr(a, 0, "string_delete");
        s.erase(at1(s, argNum(a, 1)), (size_t)std::max(0.0, argNum(a, 2)));
        return Value(s);
    });
    vm.addNative("string_insert", [](Instance&, std::vector<Value>& a) {  // (novo, texto, posicao)
        std::string s = argStr(a, 1, "string_insert");
        s.insert(at1(s, argNum(a, 2)), argStr(a, 0, "string_insert"));
        return Value(s);
    });
    vm.addNative("string_pos", [](Instance&, std::vector<Value>& a) {  // (pedaco, texto) -> posicao ou 0
        size_t p = argStr(a, 1, "string_pos").find(argStr(a, 0, "string_pos"));
        return Value(p == std::string::npos ? 0.0 : double(p + 1));
    });
    vm.addNative("string_repeat", [](Instance&, std::vector<Value>& a) {
        std::string s, um = argStr(a, 0, "string_repeat");
        for (int i = 0, n = (int)argNum(a, 1); i < n; i++) s += um;
        return Value(s);
    });
    vm.addNative("string_replace_all", [](Instance&, std::vector<Value>& a) {  // (texto, pedaco, novo)
        std::string s = argStr(a, 0, "string_replace_all"), de = argStr(a, 1, "string_replace_all"),
                    para = argStr(a, 2, "string_replace_all");
        if (de.empty()) return Value(s);
        for (size_t p = s.find(de); p != std::string::npos; p = s.find(de, p + para.size())) s.replace(p, de.size(), para);
        return Value(s);
    });

    // --- arrays: começam em 0, como na GML ---
    vm.addNative("array_length", [](Instance&, std::vector<Value>& a) {
        return Value((double)argArray(a, 0, "array_length")->size());
    });
    vm.addNative("array_push", [](Instance&, std::vector<Value>& a) {
        if (a.size() < 2) throw std::runtime_error("array_push() espera (array, valor)");
        argArray(a, 0, "array_push")->push_back(a[1]);
        return Value();
    });
    vm.addNative("array_pop", [](Instance&, std::vector<Value>& a) {
        auto& arr = argArray(a, 0, "array_pop");
        if (arr->empty()) return Value();
        Value ultimo = arr->back();
        arr->pop_back();
        return ultimo;
    });
    vm.addNative("array_insert", [](Instance&, std::vector<Value>& a) {  // (array, posicao, valor)
        auto& arr = argArray(a, 0, "array_insert");
        size_t i = (size_t)std::clamp(argNum(a, 1), 0.0, (double)arr->size());
        arr->insert(arr->begin() + i, a.size() > 2 ? a[2] : Value());
        return Value();
    });
    vm.addNative("array_delete", [](Instance&, std::vector<Value>& a) {  // (array, posicao, quantidade)
        auto& arr = argArray(a, 0, "array_delete");
        size_t i = (size_t)std::clamp(argNum(a, 1), 0.0, (double)arr->size());
        size_t n = (size_t)std::clamp(a.size() > 2 ? argNum(a, 2) : 1, 0.0, (double)(arr->size() - i));
        arr->erase(arr->begin() + i, arr->begin() + i + n);
        return Value();
    });
    vm.addNative("array_create", [](Instance&, std::vector<Value>& a) {  // (quantidade, valor = 0)
        return Value(std::make_shared<Array>((size_t)std::max(0.0, argNum(a, 0)), a.size() > 1 ? a[1] : Value(0.0)));
    });
}
