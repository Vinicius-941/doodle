// Save data format for store.save/store.load. No platform code.
#pragma once
#include <map>
#include "vm.h"

// One "key<TAB>value" line per entry. Values: number (exact), bool, or string (starts with ", escapes \\ \n \t).
std::string encodeSave(const std::map<std::string, Value>& kv);  // throws on other value types
std::map<std::string, Value> decodeSave(const std::string& text);
