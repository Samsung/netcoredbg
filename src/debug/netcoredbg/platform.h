// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>

unsigned long OSPageSize();
void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList);
std::string GetExeAbsPath();
std::string GetFileName(const std::string &path);
bool SetWorkDir(const std::string &path);
void USleep(uint32_t duration);
void *DLOpen(const std::string &path);
void *DLSym(void *handle, const std::string &name);
