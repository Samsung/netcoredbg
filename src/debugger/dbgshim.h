// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"

// For `HRESULT` definition
#ifdef FEATURE_PAL
#include <pal_mstypes.h>
#else
#include <windows.h>
#include "palclr.h"
#endif

#include "filesystem.h"
#include "dynlibs.h"
#include <string>


namespace netcoredbg
{

// Based on coreclr/src/dlls/dbgshim/dbgshim.h
struct dbgshim_t
{
    typedef VOID (*PSTARTUP_CALLBACK)(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT (*CreateProcessForLaunch)(LPWSTR lpCommandLine, BOOL bSuspendProcess, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, PDWORD pProcessId, HANDLE *pResumeHandle);
    HRESULT (*ResumeProcess)(HANDLE hResumeHandle);
    HRESULT (*CloseResumeHandle)(HANDLE hResumeHandle);
    HRESULT (*RegisterForRuntimeStartup)(DWORD dwProcessId, PSTARTUP_CALLBACK pfnCallback, PVOID parameter, PVOID *ppUnregisterToken);
    HRESULT (*UnregisterForRuntimeStartup)(PVOID pUnregisterToken);
    HRESULT (*EnumerateCLRs)(DWORD debuggeePID, HANDLE** ppHandleArrayOut, LPWSTR** ppStringArrayOut, DWORD* pdwArrayLengthOut);
    HRESULT (*CloseCLREnumeration)(HANDLE* pHandleArray, LPWSTR* pStringArray, DWORD dwArrayLength);
    HRESULT (*CreateVersionStringFromModule)(DWORD pidDebuggee, LPCWSTR szModuleName, LPWSTR pBuffer, DWORD cchBuffer, DWORD* pdwLength);
    HRESULT (*CreateDebuggingInterfaceFromVersionEx)(int iDebuggerVersion, LPCWSTR szDebuggeeVersion, IUnknown ** ppCordb);

    dbgshim_t() :
        CreateProcessForLaunch(nullptr),
        ResumeProcess(nullptr),
        CloseResumeHandle(nullptr),
        RegisterForRuntimeStartup(nullptr),
        UnregisterForRuntimeStartup(nullptr),
        EnumerateCLRs(nullptr),
        CloseCLREnumeration(nullptr),
        CreateVersionStringFromModule(nullptr),
        CreateDebuggingInterfaceFromVersionEx(nullptr),
        m_module(nullptr)
    {
#ifdef DBGSHIM_RUNTIME_DIR
        std::string libName(DBGSHIM_RUNTIME_DIR);
        libName += DIRECTORY_SEPARATOR_STR_A;
#else
        std::string exe = GetExeAbsPath();
        if (exe.empty())
            throw std::runtime_error("Unable to detect exe path");

        std::size_t dirSepIndex = exe.rfind(DIRECTORY_SEPARATOR_STR_A);
        if (dirSepIndex == std::string::npos)
            return;
        std::string libName = exe.substr(0, dirSepIndex + 1);
#endif

#ifdef WIN32
        libName += "dbgshim.dll";
#elif defined(__APPLE__)
        libName += "libdbgshim.dylib";
#else
        libName += "libdbgshim.so";
#endif

        m_module = DLOpen(libName);
        if (!m_module)
            throw std::invalid_argument("Unable to load " + libName);

        *((void**)&CreateProcessForLaunch) = DLSym(m_module, "CreateProcessForLaunch");
        *((void**)&ResumeProcess) = DLSym(m_module, "ResumeProcess");
        *((void**)&CloseResumeHandle) = DLSym(m_module, "CloseResumeHandle");
        *((void**)&RegisterForRuntimeStartup) = DLSym(m_module, "RegisterForRuntimeStartup");
        *((void**)&UnregisterForRuntimeStartup) = DLSym(m_module, "UnregisterForRuntimeStartup");
        *((void**)&EnumerateCLRs) = DLSym(m_module, "EnumerateCLRs");
        *((void**)&CloseCLREnumeration) = DLSym(m_module, "CloseCLREnumeration");
        *((void**)&CreateVersionStringFromModule) = DLSym(m_module, "CreateVersionStringFromModule");
        *((void**)&CreateDebuggingInterfaceFromVersionEx) = DLSym(m_module, "CreateDebuggingInterfaceFromVersionEx");

        bool dlsym_ok = CreateProcessForLaunch &&
                        ResumeProcess &&
                        CloseResumeHandle &&
                        RegisterForRuntimeStartup &&
                        UnregisterForRuntimeStartup &&
                        EnumerateCLRs &&
                        CloseCLREnumeration &&
                        CreateVersionStringFromModule &&
                        CreateDebuggingInterfaceFromVersionEx;

        if (!dlsym_ok)
            throw std::invalid_argument("Unable to dlsym for dbgshim module");
    }

    ~dbgshim_t()
    {
        if (m_module)
            DLClose(m_module);
    }

private:
    DLHandle m_module;
};

} // namespace netcoredbg
