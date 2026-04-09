/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aohp {

struct ExecResult {
    int exitCode;
    std::string stdoutStr;
    std::string stderrStr;
};

class ContainerManager {
public:
    ContainerManager();

    std::vector<std::string> listContainers();
    bool createContainer(const std::string& name, const std::string& templateName);
    bool destroyContainer(const std::string& name);
    bool resetContainer(const std::string& name);
    ExecResult execSync(const std::string& name, const std::string& command, int timeoutMs);

    // Returns master PTY fd for interactive shell, -1 on error.
    int openShell(const std::string& name);

    /** Non-empty after createContainer() returns false (diagnostics for clients). */
    const std::string& getLastError() const { return mLastError_; }

private:
    std::string mLastError_;
    std::mutex mWorkDirMutex_;
    std::map<std::string, std::string> mWorkDir_;

    std::string rootfsPath(const std::string& name);
    std::string envPath(const std::string& name);
    std::string templatePath(const std::string& templateName);

    bool extractTemplate(const std::string& templateTarGz, const std::string& destDir);
    bool setupBindMounts(const std::string& rootfs);
    bool teardownBindMounts(const std::string& rootfs);
    void killContainerProcesses(const std::string& rootfs);
    int forkIntoContainer(const std::string& rootfs, const char* const argv[], bool usePty);
};

}  // namespace aohp
