/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define LOG_TAG "aohp-containerd"

#include "container_manager.h"
#include "protocol.h"
#include "tar_gz_extract.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <dirent.h>
#include <fcntl.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aohp {

namespace {

// Create every directory component of an absolute path (excluding the final leaf if it is a file path).
bool mkdirParentsAbsolute(const std::string& path, mode_t mode) {
    if (path.empty() || path[0] != '/') return false;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string partial = path.substr(0, pos);
        if (partial.empty()) continue;
        if (mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) {
            PLOG(ERROR) << "mkdirParentsAbsolute " << partial;
            return false;
        }
    }
    return true;
}

bool ensureDir(const std::string& path, mode_t mode) {
    if (!mkdirParentsAbsolute(path, mode)) return false;
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "ensureDir mkdir " << path;
        return false;
    }
    struct stat sb;
    if (stat(path.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        LOG(ERROR) << "ensureDir not a directory: " << path;
        return false;
    }
    return true;
}

// CLONE_NEWPID|CLONE_NEWNS often returns EINVAL on Android (threads / policy). Mount namespace alone
// is enough to avoid littering the global mount table when bind-mounting before chroot.
void tryUnshareMountNs() {
    if (unshare(CLONE_NEWNS) != 0) {
        PLOG(WARNING) << "unshare(CLONE_NEWNS) failed, using host mount namespace";
    }
}

}  // namespace

ContainerManager::ContainerManager() {
    mkdir(CONTAINER_BASE_DIR, 0770);
}

std::string ContainerManager::envPath(const std::string& name) {
    return std::string(CONTAINER_BASE_DIR) + "/" + name;
}

std::string ContainerManager::rootfsPath(const std::string& name) {
    return envPath(name) + "/rootfs";
}

std::string ContainerManager::templatePath(const std::string& templateName) {
    return std::string(TEMPLATE_DIR) + "/" + templateName + ".tar.gz";
}

std::vector<std::string> ContainerManager::listContainers() {
    std::vector<std::string> result;
    DIR* dir = opendir(CONTAINER_BASE_DIR);
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            std::string rootfs = rootfsPath(entry->d_name);
            struct stat st;
            if (stat(rootfs.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                result.emplace_back(entry->d_name);
            }
        }
    }
    closedir(dir);
    return result;
}

bool ContainerManager::extractTemplate(const std::string& templateTarGz,
                                       const std::string& destDir) {
    std::string err;
    if (!extractTarGz(templateTarGz, destDir, &err)) {
        mLastError_ = err.empty() ? "template extract failed" : err;
        LOG(ERROR) << mLastError_;
        return false;
    }
    return true;
}

bool ContainerManager::createContainer(const std::string& name,
                                       const std::string& templateName) {
    mLastError_.clear();
    std::string env = envPath(name);
    std::string rootfs = rootfsPath(name);

    struct stat st;
    if (stat(rootfs.c_str(), &st) == 0) {
        mLastError_ = "container already exists: " + name;
        LOG(WARNING) << mLastError_;
        return false;
    }

    if (mkdir(env.c_str(), 0755) != 0 && errno != EEXIST) {
        mLastError_ = std::string("mkdir ") + env + ": " + strerror(errno);
        PLOG(ERROR) << mLastError_;
        return false;
    }

    std::string tpl = templatePath(templateName);
    if (stat(tpl.c_str(), &st) != 0) {
        mLastError_ = "template not found: " + tpl;
        LOG(ERROR) << mLastError_;
        return false;
    }

    if (!extractTemplate(tpl, rootfs)) {
        return false;
    }

    // Ensure mount points exist inside rootfs.
    mkdir((rootfs + "/proc").c_str(), 0555);
    mkdir((rootfs + "/dev").c_str(), 0755);
    mkdir((rootfs + "/dev/pts").c_str(), 0755);
    mkdir((rootfs + "/sys").c_str(), 0555);
    mkdir((rootfs + "/sdcard").c_str(), 0755);
    mkdir((rootfs + "/tmp").c_str(), 01777);

    // Create /etc/resolv.conf so DNS works inside the container.
    std::string resolv = rootfs + "/etc/resolv.conf";
    int fd = open(resolv.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char* dns = "nameserver 8.8.8.8\nnameserver 8.8.4.4\n";
        write(fd, dns, strlen(dns));
        close(fd);
    }

    LOG(INFO) << "Container " << name << " created from template " << templateName;
    return true;
}

static int removePathRecursive(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return -1;
        while (struct dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (removePathRecursive(path + "/" + e->d_name) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        if (rmdir(path.c_str()) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    if (unlink(path.c_str()) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int recursiveRemove(const std::string& path) { return removePathRecursive(path); }

void ContainerManager::killContainerProcesses(const std::string& rootfs) {
    // Best-effort: scan /proc for processes whose root link points into our rootfs.
    DIR* proc = opendir("/proc");
    if (!proc) return;
    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        char* endp;
        long pid = strtol(entry->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 1) continue;

        char link[PATH_MAX];
        std::string rootLink = std::string("/proc/") + entry->d_name + "/root";
        ssize_t len = readlink(rootLink.c_str(), link, sizeof(link) - 1);
        if (len <= 0) continue;
        link[len] = '\0';
        if (std::string(link).find(rootfs) == 0) {
            kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
    closedir(proc);
    usleep(100000);  // 100ms grace
}

bool ContainerManager::teardownBindMounts(const std::string& rootfs) {
    const char* mounts[] = {"/proc", "/dev/pts", "/dev", "/sys", "/sdcard"};
    bool ok = true;
    for (int i = sizeof(mounts) / sizeof(mounts[0]) - 1; i >= 0; --i) {
        std::string target = rootfs + mounts[i];
        if (umount2(target.c_str(), MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
            PLOG(WARNING) << "umount " << target;
            ok = false;
        }
    }
    return ok;
}

bool ContainerManager::destroyContainer(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        mWorkDir_.erase(name);
    }
    std::string rootfs = rootfsPath(name);
    std::string env = envPath(name);

    killContainerProcesses(rootfs);
    teardownBindMounts(rootfs);

    if (recursiveRemove(env) != 0) {
        LOG(ERROR) << "Failed to remove " << env;
        return false;
    }
    LOG(INFO) << "Container " << name << " destroyed";
    return true;
}

bool ContainerManager::resetContainer(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        mWorkDir_.erase(name);
    }
    std::string rootfs = rootfsPath(name);
    std::string env = envPath(name);

    killContainerProcesses(rootfs);
    teardownBindMounts(rootfs);
    recursiveRemove(rootfs);

    // Re-discover template: look for first .tar.gz in TEMPLATE_DIR.
    std::string tpl;
    DIR* dir = opendir(TEMPLATE_DIR);
    if (dir) {
        struct dirent* e;
        while ((e = readdir(dir)) != nullptr) {
            std::string fn(e->d_name);
            if (fn.size() > 7 && fn.substr(fn.size() - 7) == ".tar.gz") {
                tpl = std::string(TEMPLATE_DIR) + "/" + fn;
                break;
            }
        }
        closedir(dir);
    }
    if (tpl.empty()) {
        LOG(ERROR) << "No template found for reset";
        return false;
    }

    if (!extractTemplate(tpl, rootfs)) {
        return false;
    }

    mkdir((rootfs + "/proc").c_str(), 0555);
    mkdir((rootfs + "/dev").c_str(), 0755);
    mkdir((rootfs + "/dev/pts").c_str(), 0755);
    mkdir((rootfs + "/sys").c_str(), 0555);
    mkdir((rootfs + "/sdcard").c_str(), 0755);
    mkdir((rootfs + "/tmp").c_str(), 01777);

    std::string resolv = rootfs + "/etc/resolv.conf";
    int fd = open(resolv.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char* dns = "nameserver 8.8.8.8\nnameserver 8.8.4.4\n";
        write(fd, dns, strlen(dns));
        close(fd);
    }

    LOG(INFO) << "Container " << name << " reset";
    return true;
}

bool ContainerManager::setupBindMounts(const std::string& rootfs) {
    auto doMount = [&](const char* src, const char* rel, unsigned long flags) -> bool {
        std::string target = rootfs + rel;
        if (!ensureDir(target, 0755)) return false;
        if (mount(src, target.c_str(), nullptr, MS_BIND | flags, nullptr) != 0) {
            PLOG(ERROR) << "bind mount " << src << " -> " << target;
            return false;
        }
        return true;
    };

    if (!doMount("/proc", "/proc", 0)) return false;
    if (!doMount("/dev", "/dev", 0)) return false;
    if (!doMount("/sys", "/sys", MS_RDONLY)) return false;

    // Mount devpts for PTY support inside the container.
    std::string devpts = rootfs + "/dev/pts";
    if (!ensureDir(devpts, 0755)) return false;
    if (mount("devpts", devpts.c_str(), "devpts", 0, "newinstance,ptmxmode=0666") != 0) {
        PLOG(WARNING) << "devpts mount failed (PTY in container may not work)";
    }

    // Bind /sdcard (FUSE-backed on modern Android).
    struct stat st;
    if (stat("/sdcard", &st) == 0) {
        doMount("/sdcard", "/sdcard", 0);
    } else if (stat("/storage/emulated/0", &st) == 0) {
        doMount("/storage/emulated/0", "/sdcard", 0);
    }
    return true;
}

int ContainerManager::forkIntoContainer(const std::string& rootfs,
                                        const char* const argv[],
                                        bool usePty) {
    int masterFd = -1;
    int pipeFds[2] = {-1, -1};

    if (usePty) {
        // Nothing here; forkpty below handles it.
    } else {
        if (pipe(pipeFds) < 0) {
            PLOG(ERROR) << "pipe";
            return -1;
        }
    }

    pid_t pid;
    if (usePty) {
        pid = forkpty(&masterFd, nullptr, nullptr, nullptr);
    } else {
        pid = fork();
    }

    if (pid < 0) {
        PLOG(ERROR) << "fork/forkpty";
        return -1;
    }

    if (pid == 0) {
        tryUnshareMountNs();
        if (!setupBindMounts(rootfs)) {
            _exit(125);
        }

        if (chroot(rootfs.c_str()) != 0) {
            PLOG(ERROR) << "chroot " << rootfs;
            _exit(126);
        }
        chdir("/");

        setenv("HOME", "/root", 1);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "C.UTF-8", 1);

        if (!usePty) {
            close(pipeFds[0]);
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);
        }

        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }

    // Parent.
    if (usePty) {
        return masterFd;
    } else {
        close(pipeFds[1]);
        return pipeFds[0];  // Caller reads from this fd.
    }
}

ExecResult ContainerManager::execSync(const std::string& name,
                                      const std::string& command,
                                      int timeoutMs) {
    ExecResult result{-1, "", ""};
    std::string rootfs = rootfsPath(name);

    struct stat st;
    if (stat(rootfs.c_str(), &st) != 0) {
        result.stderrStr = "Container not found: " + name;
        return result;
    }

    std::string workDir = "/";
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        auto it = mWorkDir_.find(name);
        if (it != mWorkDir_.end() && !it->second.empty() && it->second[0] == '/') {
            workDir = it->second;
        }
    }

    int stdoutPipe[2], stderrPipe[2];
    if (pipe(stdoutPipe) < 0 || pipe(stderrPipe) < 0) {
        result.stderrStr = "pipe() failed";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stderrStr = "fork() failed";
        return result;
    }

    if (pid == 0) {
        close(stdoutPipe[0]);
        close(stderrPipe[0]);

        tryUnshareMountNs();
        if (!setupBindMounts(rootfs)) {
            const char* msg = "aohp-containerd: setupBindMounts failed\n";
            write(stderrPipe[1], msg, strlen(msg));
            _exit(125);
        }

        if (chroot(rootfs.c_str()) != 0) {
            _exit(126);
        }
        chdir("/");

        setenv("HOME", "/root", 1);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "C.UTF-8", 1);

        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        // Persist cwd across execSync invocations (each command is a new sh; without this,
        // "cd sdcard" appears to do nothing).
        setenv("AOHP_WD", workDir.c_str(), 1);
        setenv("AOHP_CMD", command.c_str(), 1);
        const char* argv[] = {
                "/bin/sh",
                "-c",
                "cd \"${AOHP_WD}\" 2>/dev/null || cd /; "
                "eval \"${AOHP_CMD}\"; ec=$?; "
                "pwd > /tmp/.aohp_lastpwd; "
                "exit $ec",
                nullptr,
        };
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }

    close(stdoutPipe[1]);
    close(stderrPipe[1]);

    auto readAll = [](int fd) -> std::string {
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            out.append(buf, n);
        }
        close(fd);
        return out;
    };

    result.stdoutStr = readAll(stdoutPipe[0]);
    result.stderrStr = readAll(stderrPipe[0]);

    int status = 0;
    if (timeoutMs > 0) {
        // Simple alarm-based timeout.
        alarm(timeoutMs / 1000 + 1);
    }
    waitpid(pid, &status, 0);
    alarm(0);

    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    std::string pwdFile = rootfs + "/tmp/.aohp_lastpwd";
    std::ifstream in(pwdFile.c_str());
    std::string line;
    if (std::getline(in, line)) {
        std::string p = android::base::Trim(line);
        if (!p.empty() && p[0] == '/') {
            std::lock_guard<std::mutex> lock(mWorkDirMutex_);
            mWorkDir_[name] = std::move(p);
        }
    }

    return result;
}

int ContainerManager::openShell(const std::string& name) {
    std::string rootfs = rootfsPath(name);
    struct stat st;
    if (stat(rootfs.c_str(), &st) != 0) {
        LOG(ERROR) << "Container not found: " << name;
        return -1;
    }

    const char* argv[] = {"/bin/sh", "-l", nullptr};
    return forkIntoContainer(rootfs, argv, true);
}

}  // namespace aohp
