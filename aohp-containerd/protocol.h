/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <string>
#include <vector>
#include <sstream>

namespace aohp {

// Command opcodes sent from Java AohpContainerService over LocalSocket.
// Wire format: "<OPCODE> <arg1> <arg2> ...\n"
// Response:    "OK <json-or-text>\n"  or  "ERR <message>\n"
// For openShell, after "OK", the socket becomes a bidirectional PTY relay.

constexpr const char* CMD_LIST       = "LIST";
constexpr const char* CMD_CREATE     = "CREATE";     // CREATE <name> <template>
constexpr const char* CMD_DESTROY    = "DESTROY";    // DESTROY <name>
constexpr const char* CMD_RESET      = "RESET";      // RESET <name>
constexpr const char* CMD_EXEC       = "EXEC";       // EXEC <name> <timeout_ms> <command...>
constexpr const char* CMD_SHELL      = "SHELL";      // SHELL <name>

constexpr const char* RESP_OK  = "OK";
constexpr const char* RESP_ERR = "ERR";

constexpr const char* CONTAINER_BASE_DIR = "/data/aohp/envs";
constexpr const char* TEMPLATE_DIR       = "/system/etc/aohp/rootfs-templates";

inline std::vector<std::string> splitLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

inline std::string restOfLine(const std::string& line, size_t skipTokens) {
    size_t pos = 0;
    for (size_t i = 0; i < skipTokens && pos < line.size(); ++i) {
        while (pos < line.size() && line[pos] == ' ') ++pos;
        while (pos < line.size() && line[pos] != ' ') ++pos;
    }
    while (pos < line.size() && line[pos] == ' ') ++pos;
    return line.substr(pos);
}

}  // namespace aohp
