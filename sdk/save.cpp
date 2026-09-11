#include "save.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

std::string encodeSave(const std::map<std::string, Value>& kv) {
    std::string out;
    for (auto& [key, v] : kv) {
        if (key.empty() || key.find_first_of("\t\r\n") != std::string::npos) throw std::runtime_error("chave de save inválida");
        out += key + '\t';
        if (auto s = std::get_if<std::string>(&v)) {
            out += '"';
            for (char c : *s) {
                if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\t') out += "\\t";
                else out += c;
            }
        } else if (auto n = std::get_if<double>(&v)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.17g", *n);  // round-trips exactly
            out += buf;
        } else if (auto b = std::get_if<bool>(&v)) {
            out += *b ? "true" : "false";
        } else {
            throw std::runtime_error("só dá pra salvar número, texto ou bool (em '" + key + "')");
        }
        out += '\n';
    }
    return out;
}

std::map<std::string, Value> decodeSave(const std::string& text) {
    std::map<std::string, Value> kv;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // edited on Windows
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = line.substr(0, tab), v = line.substr(tab + 1);
        if (!v.empty() && v[0] == '"') {
            std::string s;
            for (size_t i = 1; i < v.size(); i++) {
                char c = v[i];
                if (c == '\\' && i + 1 < v.size()) {
                    c = v[++i];
                    c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
                }
                s += c;
            }
            kv[key] = Value(s);
        } else if (v == "true" || v == "false") {
            kv[key] = Value(v == "true");
        } else {
            kv[key] = Value(strtod(v.c_str(), nullptr));
        }
    }
    return kv;
}
