// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpointutils.h"
#include "debugger/variables.h"
#include "metadata/modules.h"
#include "utils/torelease.h"

namespace netcoredbg
{

namespace BreakpointUtils
{

HRESULT IsSameFunctionBreakpoint(ICorDebugFunctionBreakpoint *pBreakpoint1, ICorDebugFunctionBreakpoint *pBreakpoint2)
{
    HRESULT Status;

    if (!pBreakpoint1 || !pBreakpoint2)
        return E_FAIL;

    ULONG32 nOffset1;
    ULONG32 nOffset2;
    IfFailRet(pBreakpoint1->GetOffset(&nOffset1));
    IfFailRet(pBreakpoint2->GetOffset(&nOffset2));

    if (nOffset1 != nOffset2)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunction1;
    ToRelease<ICorDebugFunction> pFunction2;
    IfFailRet(pBreakpoint1->GetFunction(&pFunction1));
    IfFailRet(pBreakpoint2->GetFunction(&pFunction2));

    mdMethodDef methodDef1;
    mdMethodDef methodDef2;
    IfFailRet(pFunction1->GetToken(&methodDef1));
    IfFailRet(pFunction2->GetToken(&methodDef2));

    if (methodDef1 != methodDef2)
        return E_FAIL;

    ToRelease<ICorDebugModule> pModule1;
    ToRelease<ICorDebugModule> pModule2;
    IfFailRet(pFunction1->GetModule(&pModule1));
    IfFailRet(pFunction2->GetModule(&pModule2));

    CORDB_ADDRESS modAddress1;
    IfFailRet(pModule1->GetBaseAddress(&modAddress1));
    CORDB_ADDRESS modAddress2;
    IfFailRet(pModule2->GetBaseAddress(&modAddress2));

    if (modAddress1 != modAddress2)
        return E_FAIL;

    ToRelease<ICorDebugCode> pCode1;
    IfFailRet(pFunction1->GetILCode(&pCode1));
    ULONG32 methodVersion1;
    IfFailRet(pCode1->GetVersionNumber(&methodVersion1));
    ToRelease<ICorDebugCode> pCode2;
    IfFailRet(pFunction2->GetILCode(&pCode2));
    ULONG32 methodVersion2;
    IfFailRet(pCode2->GetVersionNumber(&methodVersion2));

    if (methodVersion1 != methodVersion2)
        return E_FAIL;

    return S_OK;
}

HRESULT IsEnableByCondition(const std::string &condition, Variables *pVariables, ICorDebugThread *pThread)
{
    HRESULT Status;

    if (!condition.empty())
    {
        DWORD threadId = 0;
        IfFailRet(pThread->GetID(&threadId));
        FrameId frameId(ThreadId{threadId}, FrameLevel{0});

        ToRelease<ICorDebugProcess> iCorProcess;
        IfFailRet(pThread->GetProcess(&iCorProcess));

        Variable variable;
        std::string output;
        IfFailRet(pVariables->Evaluate(iCorProcess, frameId, condition, variable, output));

        if (variable.type != "bool" || variable.value != "true")
            return E_FAIL;
    }

    return S_OK;
}

HRESULT SkipBreakpoint(ICorDebugModule *pModule, mdMethodDef methodToken, bool justMyCode)
{
    HRESULT Status;

    // Skip breakpoints outside of code with loaded PDB (see JMC setup during module load).
    ToRelease<ICorDebugFunction> iCorFunction;
    IfFailRet(pModule->GetFunctionFromToken(methodToken, &iCorFunction));
    ToRelease<ICorDebugFunction2> iCorFunction2;
    IfFailRet(iCorFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID*) &iCorFunction2));
    BOOL JMCStatus;
    IfFailRet(iCorFunction2->GetJMCStatus(&JMCStatus));
    if (JMCStatus == FALSE)
    {
        return S_OK; // need skip breakpoint
    }

    // Care about attributes for "JMC disabled" case.
    if (!justMyCode)
    {
        ToRelease<IUnknown> iUnknown;
        IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &iUnknown));
        ToRelease<IMetaDataImport> iMD;
        IfFailRet(iUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &iMD));

        if (HasAttribute(iMD, methodToken, DebuggerAttribute::Hidden))
            return S_OK; // need skip breakpoint
    }

    return S_FALSE; // don't skip breakpoint
}

} // namespace BreakpointUtils

} // namespace netcoredbg
