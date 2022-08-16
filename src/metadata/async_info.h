// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <memory>

#include "metadata/modules.h"

namespace netcoredbg
{

class Modules;

class AsyncInfo
{
public:

    AsyncInfo(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules)
    {}

    struct AwaitInfo
    {
        uint32_t yield_offset;
        uint32_t resume_offset;

        AwaitInfo() :
            yield_offset(0), resume_offset(0)
        {};
        AwaitInfo(uint32_t offset1, uint32_t offset2) :
            yield_offset(offset1), resume_offset(offset2)
        {};
    };

    bool IsMethodHaveAwait(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion);
    bool FindNextAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 ipOffset, AwaitInfo **awaitInfo);
    bool FindLastIlOffsetAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 &lastIlOffset);

private:

    std::shared_ptr<Modules> m_sharedModules;

    struct AsyncMethodInfo
    {
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 methodVersion;

        std::vector<AwaitInfo> awaits;
        // Part of NotifyDebuggerOfWaitCompletion magic, see ManagedDebugger::SetupAsyncStep().
        ULONG32 lastIlOffset;

        AsyncMethodInfo() :
            modAddress(0), methodToken(mdMethodDefNil), methodVersion(0), awaits(), lastIlOffset(0)
        {};
    };

    AsyncMethodInfo asyncMethodSteppingInfo;
    std::mutex m_asyncMethodSteppingInfoMutex;
    // Note, result stored into asyncMethodSteppingInfo.
    HRESULT GetAsyncMethodSteppingInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion);

};

} // namespace netcoredbg
