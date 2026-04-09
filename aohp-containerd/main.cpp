/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define LOG_TAG "aohp-containerd"

#include "container_manager.h"
#include "protocol.h"
#include "session.h"

#include <cerrno>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>

#include <android-base/logging.h>
#include <cutils/sockets.h>

using namespace aohp;

static ContainerManager gMgr;

static std::string toJson(const ExecResult& r) {
    // Minimal JSON encoding: escape quotes and backslashes.
    auto escape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    };
    return "{\"exitCode\":" + std::to_string(r.exitCode) +
           ",\"stdout\":\"" + escape(r.stdoutStr) +
           "\",\"stderr\":\"" + escape(r.stderrStr) + "\"}";
}

static void handleClient(int clientFd) {
    FILE* fp = fdopen(dup(clientFd), "r");
    if (!fp) {
        close(clientFd);
        return;
    }

    char lineBuf[8192];
    while (fgets(lineBuf, sizeof(lineBuf), fp)) {
        std::string line(lineBuf);
        // Strip trailing newline.
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        auto tokens = splitLine(line);
        if (tokens.empty()) continue;

        std::string response;
        const std::string& cmd = tokens[0];

        if (cmd == CMD_LIST) {
            auto list = gMgr.listContainers();
            std::string joined;
            for (size_t i = 0; i < list.size(); ++i) {
                if (i > 0) joined += ",";
                joined += list[i];
            }
            response = std::string(RESP_OK) + " " + joined + "\n";

        } else if (cmd == CMD_CREATE && tokens.size() >= 3) {
            bool ok = gMgr.createContainer(tokens[1], tokens[2]);
            if (ok) {
                response = std::string(RESP_OK) + " created\n";
            } else {
                std::string detail = gMgr.getLastError();
                if (detail.empty()) detail = "create failed";
                for (char& c : detail) {
                    if (c == '\n' || c == '\r') c = ' ';
                }
                response = std::string(RESP_ERR) + " " + detail + "\n";
            }

        } else if (cmd == CMD_DESTROY && tokens.size() >= 2) {
            bool ok = gMgr.destroyContainer(tokens[1]);
            response = ok ? std::string(RESP_OK) + " destroyed\n"
                          : std::string(RESP_ERR) + " destroy failed\n";

        } else if (cmd == CMD_RESET && tokens.size() >= 2) {
            bool ok = gMgr.resetContainer(tokens[1]);
            response = ok ? std::string(RESP_OK) + " reset\n"
                          : std::string(RESP_ERR) + " reset failed\n";

        } else if (cmd == CMD_EXEC && tokens.size() >= 4) {
            std::string name = tokens[1];
            int timeout = atoi(tokens[2].c_str());
            std::string command = restOfLine(line, 3);
            auto result = gMgr.execSync(name, command, timeout);
            response = std::string(RESP_OK) + " " + toJson(result) + "\n";

        } else if (cmd == CMD_SHELL && tokens.size() >= 2) {
            std::string name = tokens[1];
            int masterFd = gMgr.openShell(name);
            if (masterFd < 0) {
                response = std::string(RESP_ERR) + " shell failed\n";
            } else {
                // Send OK, then switch to PTY relay mode.
                std::string ok = std::string(RESP_OK) + " shell\n";
                write(clientFd, ok.c_str(), ok.size());
                fclose(fp);
                relayPtyToSocket(masterFd, clientFd);
                close(clientFd);
                return;
            }

        } else {
            response = std::string(RESP_ERR) + " unknown command\n";
        }

        write(clientFd, response.c_str(), response.size());
    }

    fclose(fp);
    close(clientFd);
}

int main(int argc, char** argv) {
    // LogdLogger: KernelLogger writes to /dev/kmsg which this domain may not access.
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    LOG(INFO) << "aohp-containerd starting";

    // Ensure base directories exist.
    mkdir("/data/aohp", 0770);
    mkdir(CONTAINER_BASE_DIR, 0770);

    int serverFd = android_get_control_socket("aohp_container");
    if (serverFd < 0) {
        PLOG(FATAL) << "Failed to get control socket 'aohp_container'";
        return 1;
    }

    if (listen(serverFd, 8) < 0) {
        PLOG(FATAL) << "listen";
        return 1;
    }

    LOG(INFO) << "aohp-containerd listening on control socket";

    while (true) {
        struct sockaddr_un addr;
        socklen_t addrLen = sizeof(addr);
        int clientFd = accept(serverFd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            PLOG(ERROR) << "accept";
            continue;
        }

        std::thread(handleClient, clientFd).detach();
    }

    return 0;
}
