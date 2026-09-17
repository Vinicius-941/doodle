#pragma once
#include <functional>
#include <string>
#include <vector>

// Quebrar linha e encurtar com "..." dependem da largura do texto na tela, que só a fonte sabe medir.
// A conta em si não depende de plataforma nenhuma, então ela mora aqui e recebe o medidor de fora.
using MedeTexto = std::function<double(const std::string&)>;

std::vector<std::string> quebraLinhas(const std::string& texto, double largura, const MedeTexto& mede);
std::string encurtaTexto(const std::string& texto, double largura, const MedeTexto& mede);
