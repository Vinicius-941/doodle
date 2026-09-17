#include "assinatura.h"

#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>
#include <vector>

namespace {

struct Algoritmo {  // provedor do BCrypt, fechado na saída
    BCRYPT_ALG_HANDLE h = nullptr;
    explicit Algoritmo(LPCWSTR nome) {
        if (BCryptOpenAlgorithmProvider(&h, nome, nullptr, 0) < 0) throw std::runtime_error("criptografia indisponível");
    }
    ~Algoritmo() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct Chave {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~Chave() { if (h) BCryptDestroyKey(h); }
};

// SHA-256 dos dados: é o que se assina, não o arquivo inteiro.
std::string resumo(const std::string& dados) {
    Algoritmo sha(BCRYPT_SHA256_ALGORITHM);
    std::string saida(32, '\0');
    if (BCryptHash(sha.h, nullptr, 0, (PUCHAR)dados.data(), (ULONG)dados.size(), (PUCHAR)saida.data(), 32) < 0)
        throw std::runtime_error("não consegui resumir o arquivo");
    return saida;
}

}  // namespace

void assinatura::gerar(std::string& privada, std::string& publica) {
    Algoritmo ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);
    Chave par;
    if (BCryptGenerateKeyPair(ecdsa.h, &par.h, 256, 0) < 0 || BCryptFinalizeKeyPair(par.h, 0) < 0)
        throw std::runtime_error("não consegui gerar o par de chaves");
    auto exporta = [&](LPCWSTR tipo) {
        ULONG tam = 0;
        if (BCryptExportKey(par.h, nullptr, tipo, nullptr, 0, &tam, 0) < 0) throw std::runtime_error("não exportei a chave");
        std::string blob(tam, '\0');
        if (BCryptExportKey(par.h, nullptr, tipo, (PUCHAR)blob.data(), tam, &tam, 0) < 0)
            throw std::runtime_error("não exportei a chave");
        return blob;
    };
    privada = exporta(BCRYPT_ECCPRIVATE_BLOB);
    publica = exporta(BCRYPT_ECCPUBLIC_BLOB);
}

std::string assinatura::assinar(const std::string& dados, const std::string& privada) {
    Algoritmo ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);
    Chave chave;
    if (BCryptImportKeyPair(ecdsa.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &chave.h, (PUCHAR)privada.data(),
                            (ULONG)privada.size(), 0) < 0)
        throw std::runtime_error("chave privada inválida");
    std::string hash = resumo(dados);
    ULONG tam = 0;
    if (BCryptSignHash(chave.h, nullptr, (PUCHAR)hash.data(), (ULONG)hash.size(), nullptr, 0, &tam, 0) < 0)
        throw std::runtime_error("não consegui assinar");
    std::string sig(tam, '\0');
    if (BCryptSignHash(chave.h, nullptr, (PUCHAR)hash.data(), (ULONG)hash.size(), (PUCHAR)sig.data(), tam, &tam, 0) < 0)
        throw std::runtime_error("não consegui assinar");
    return sig;
}

bool assinatura::confere(const std::string& dados, const std::string& sig, const std::string& publica) {
    try {
        Algoritmo ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);
        Chave chave;
        if (BCryptImportKeyPair(ecdsa.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &chave.h, (PUCHAR)publica.data(),
                                (ULONG)publica.size(), 0) < 0)
            return false;
        std::string hash = resumo(dados);
        return BCryptVerifySignature(chave.h, nullptr, (PUCHAR)hash.data(), (ULONG)hash.size(), (PUCHAR)sig.data(),
                                     (ULONG)sig.size(), 0) == 0;
    } catch (const std::exception&) {
        return false;  // chave estragada, sem criptografia no sistema: não confere, e pronto
    }
}
