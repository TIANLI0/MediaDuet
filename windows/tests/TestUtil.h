#pragma once

#include "md/util/Bytes.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace md::test {

inline nlohmann::json loadVectors() {
    std::ifstream in(std::string(MD_VECTORS_DIR) + "/crypto-v1.json", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

inline Bytes hex(const nlohmann::json& j) { return *hexDecode(j.get<std::string>()); }
inline Bytes hex(const char* s) { return *hexDecode(s); }

} // namespace md::test
