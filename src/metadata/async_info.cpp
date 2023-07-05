// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/async_info.h"
#include "metadata/modules.h"
#include "managed/interop.h"


namespace netcoredbg
{

// Caller must care about m_asyncMethodSteppingInfoMutex.
HRESULT AsyncInfo::GetAsyncMethodSteppingInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion)
{
    if (!asyncMethodSteppingInfo.awaits.empty())
        asyncMethodSteppingInfo.awaits.clear();

    return m_sharedModules->GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        HRESULT Status;
        std::vector<Interop::AsyncAwaitInfoBlock> AsyncAwaitInfo;
        IfFailRet(Interop::GetAsyncMethodSteppingInfo(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, AsyncAwaitInfo, &asyncMethodSteppingInfo.lastIlOffset));

        for (const auto &entry : AsyncAwaitInfo)
        {
            asyncMethodSteppingInfo.awaits.emplace_back(entry.yield_offset, entry.resume_offset);
        }

        asyncMethodSteppingInfo.modAddress = modAddress;
        asyncMethodSteppingInfo.methodToken = methodToken;
        asyncMethodSteppingInfo.methodVersion = methodVersion;

        return S_OK;
    });
}

// Check if method have await block. In this way we detect async method with awaits.
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
bool AsyncInfo::IsMethodHaveAwait(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodSteppingInfoMutex);

    return SUCCEEDED(GetAsyncMethodSteppingInfo(modAddress, methodToken, methodVersion));
}

// Find await block after IL offset in particular async method and return await info, if present.
// In case of async stepping, we need await info from PDB in order to setup breakpoints in proper places (yield and resume offsets).
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
// [in] ipOffset - IL offset;
// [out] awaitInfo - result, next await info.
bool AsyncInfo::FindNextAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 ipOffset, AwaitInfo **awaitInfo)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodSteppingInfoMutex);

    if (FAILED(GetAsyncMethodSteppingInfo(modAddress, methodToken, methodVersion)))
        return false;

    for (auto &await : asyncMethodSteppingInfo.awaits)
    {
        if (ipOffset <= await.yield_offset)
        {
            if (awaitInfo)
                *awaitInfo = &await;
            return true;
        }
        // Stop search, if IP inside 'await' routine.
        else if (ipOffset < await.resume_offset)
        {
            break;
        }
    }

    return false;
}

// Find last IL offset for user code in async method, if present.
// In case of step-in and step-over we must detect last user code line in order to "emulate"
// step-out (DebuggerOfWaitCompletion magic) instead.
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
// [out] lastIlOffset - result, IL offset for last user code line in async method.
bool AsyncInfo::FindLastIlOffsetAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 &lastIlOffset)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodSteppingInfoMutex);

    if (FAILED(GetAsyncMethodSteppingInfo(modAddress, methodToken, methodVersion)))
        return false;

    lastIlOffset = asyncMethodSteppingInfo.lastIlOffset;
    return true;
}

} // namespace netcoredbg
