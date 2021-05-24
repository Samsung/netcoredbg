// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <memory>
#include <mutex>
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;

class EntryBreakpoint
{
public:

    EntryBreakpoint(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules),
        m_stopAtEntry(false)
    {}

    void SetStopAtEntry(bool enable) { m_stopAtEntry = enable; }
    void Delete();

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pProcess->Continue(0);
    // Good:
    //     IfFailRet(pProcess->Continue(0));
    //     return S_OK;
    HRESULT ManagedCallbackBreakpoint(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);
    HRESULT ManagedCallbackLoadModule(ICorDebugModule *pModule);

private:

    std::mutex m_entryMutex;
    std::shared_ptr<Modules> m_sharedModules;
    ToRelease<ICorDebugFunctionBreakpoint> m_iCorFuncBreakpoint;
    bool m_stopAtEntry;
};

} // namespace netcoredbg
