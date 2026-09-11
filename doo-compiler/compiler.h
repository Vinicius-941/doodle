// Doo compiler: source -> tokens -> AST -> bytecode for the VM.
#pragma once
#include "vm.h"

// Compiles one .doo file (one object). SDK names/constants are resolved against the VM.
// privileged = firmware: only it may call system.*. Throws DooError("file:line: ...").
std::shared_ptr<ObjectDef> compile(const std::string& source, const std::string& file, const VM& vm, bool privileged);
