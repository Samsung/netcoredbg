// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

namespace Modules
{

struct SequencePoint {
    int32_t startLine;
    int32_t startColumn;
    int32_t endLine;
    int32_t endColumn;
    int32_t offset;
    std::string document;
};

HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id);

HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule);

std::string GetModuleFileName(ICorDebugModule *pModule);

HRESULT GetFrameLocation(ICorDebugFrame *pFrame,
                         ULONG32 &ilOffset,
                         Modules::SequencePoint &sequencePoint);

HRESULT GetLocationInModule(ICorDebugModule *pModule,
                            std::string filename,
                            ULONG linenum,
                            ULONG32 &ilOffset,
                            mdMethodDef &methodToken,
                            std::string &fullname);

HRESULT GetLocationInAny(std::string filename,
                         ULONG linenum,
                         ULONG32 &ilOffset,
                         mdMethodDef &methodToken,
                         std::string &fullname,
                         ICorDebugModule **ppModule);

HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread,
                                  COR_DEBUG_STEP_RANGE *range);

HRESULT TryLoadModuleSymbols(ICorDebugModule *pModule,
                             std::string &id,
                             std::string &name,
                             bool &symbolsLoaded,
                             CORDB_ADDRESS &baseAddress,
                             ULONG32 &size);

void CleanupAllModules();

HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd);

HRESULT ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb);
}
