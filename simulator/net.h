#pragma once
#include <string>

// Cliente HTTP mínimo do simulador (WinHTTP, que já vem no Windows).
// A loja é um servidor estático: o console só faz GET de arquivos.
namespace net {
// GET síncrono — use sempre fora da thread do quadro. Joga std::runtime_error em erro de rede,
// status != 200 ou resposta maior que maxBytes.
std::string get(const std::string& url, size_t maxBytes);
}
