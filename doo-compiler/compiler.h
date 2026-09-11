// Doo compiler: source -> tokens -> AST -> bytecode for the VM.
#pragma once
#include "vm.h"

struct SourceFile {
    std::string file, source;
};

// Compiles the .doo files of one program together (one object per file), so they can name each other:
// spawn(Enemy, pos), type(other) == Player. SDK names, constants and `use` components come from the VM.
// privileged = firmware: only it may call system.*. Throws DooError("file:line: ...").
std::vector<std::shared_ptr<ObjectDef>> compileAll(const std::vector<SourceFile>& files, const VM& vm, bool privileged);

// Single-file shortcut.
std::shared_ptr<ObjectDef> compile(const std::string& source, const std::string& file, const VM& vm, bool privileged);
