#include "texto.h"

#include <sstream>

// Quebra em linhas que cabem na largura, sem cortar palavra. Palavra sozinha maior que a largura fica na
// linha dela e vaza — igual à GML, e melhor do que picar a palavra no meio.
std::vector<std::string> quebraLinhas(const std::string& texto, double largura, const MedeTexto& mede) {
    std::vector<std::string> linhas;
    std::istringstream in(texto);
    std::string palavra, linha;
    while (in >> palavra) {
        std::string tentativa = linha.empty() ? palavra : linha + " " + palavra;
        if (!linha.empty() && mede(tentativa) > largura) {
            linhas.push_back(linha);
            linha = palavra;
        } else {
            linha = tentativa;
        }
    }
    if (!linha.empty()) linhas.push_back(linha);
    return linhas;
}

std::string encurtaTexto(const std::string& texto, double largura, const MedeTexto& mede) {
    if (mede(texto) <= largura) return texto;
    std::string t = texto;
    while (!t.empty() && mede(t + "...") > largura) {
        while (!t.empty()) {  // tira um caractere UTF-8 inteiro, não um byte: "ção" não vira meio "ç"
            unsigned char c = (unsigned char)t.back();
            t.pop_back();
            if ((c & 0xC0) != 0x80) break;  // 10xxxxxx é continuação: ainda está no meio do caractere
        }
    }
    return t + "...";
}
