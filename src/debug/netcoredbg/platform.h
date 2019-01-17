// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>
#include <iostream>
#include <functional>

#include <palclr.h>

unsigned long OSPageSize();
void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList);
std::string GetExeAbsPath();
std::string GetFileName(const std::string &path);
bool SetWorkDir(const std::string &path);
void USleep(uint32_t duration);
void *DLOpen(const std::string &path);
void *DLSym(void *handle, const std::string &name);
void UnsetCoreCLREnv();
std::string GetTempFolder();
std::string GetBasename(const std::string &path);
bool IsFullPath(const std::string &path);

class IORedirectServerHandles;

class IORedirectServer
{
    std::streambuf *m_in;
    std::streambuf *m_out;
    std::streambuf *m_err;
    std::streambuf *m_prevIn;
    std::streambuf *m_prevOut;
    std::streambuf *m_prevErr;
    IORedirectServerHandles *m_handles;

public:
    IORedirectServer(
        uint16_t port,
        std::function<void(std::string)> onStdOut,
        std::function<void(std::string)> onStdErr);
    ~IORedirectServer();
    operator bool() const;
};
