// Bytecode em arquivo (.doobc): um jogo compilado, para distribuir sem o código-fonte.
//
// O arquivo guarda os objetos já achatados pela herança, com as cópias dos prefabs do SDK que o jogo usa
// (então um jogo não quebra se o SDK mudar depois). Nativas vão por nome e são ligadas na carga, porque a
// ordem delas muda entre versões do console.
//
// O arquivo vem de fora (da loja), então a carga valida tudo antes de a VM tocar: cada instrução, cada
// índice, cada salto, o fim de cada função e as funções exclusivas do firmware. Um .doobc forjado é
// recusado com erro, nunca derruba a VM.
#pragma once
#include "vm.h"

// Escreve os objetos de um programa (o primeiro é o objeto principal, como no compileAll).
std::string saveBytecode(const std::vector<std::shared_ptr<ObjectDef>>& defs, const VM& vm);

// Lê e valida. privileged = firmware (só ele pode chamar system_* e as funções da loja que instalam).
// Joga DooError com o motivo se o arquivo for inválido, de outra versão ou pedir nativa que não existe.
std::vector<std::shared_ptr<ObjectDef>> loadBytecode(const std::string& data, const VM& vm, bool privileged,
                                                     const std::string& nome);
