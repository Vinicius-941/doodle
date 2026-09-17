// Assinatura dos pacotes da loja: ECDSA P-256 com o BCrypt, que já vem no Windows (sem dependência nova).
//
// A loja assina cada jogo.doobc com a chave privada; o console guarda só a chave pública e recusa instalar
// o que não bater. Sem chave pública configurada, o console instala sem conferir (modo caseiro).
#pragma once
#include <string>

namespace assinatura {

// Gera um par de chaves novo. Devolve os dois blobs (guarde a privada fora do controle de versão).
void gerar(std::string& privada, std::string& publica);

// Assina os dados com a chave privada (o blob gerado acima).
std::string assinar(const std::string& dados, const std::string& privada);

// Confere a assinatura com a chave pública. Nunca joga: qualquer problema é false.
bool confere(const std::string& dados, const std::string& assinatura, const std::string& publica);

}  // namespace assinatura
