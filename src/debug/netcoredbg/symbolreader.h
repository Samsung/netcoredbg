// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

static const char *SymbolReaderDllName = "SymbolReader";
static const char *SymbolReaderClassName = "SOS.SymbolReader";

typedef  int (*ReadMemoryDelegate)(ULONG64, char *, int);
typedef  PVOID (*LoadSymbolsForModuleDelegate)(const char*, BOOL, ULONG64, int, ULONG64, int, ReadMemoryDelegate);
typedef  void (*DisposeDelegate)(PVOID);
typedef  BOOL (*ResolveSequencePointDelegate)(PVOID, const char*, unsigned int, unsigned int*, unsigned int*);
typedef  BOOL (*GetLocalVariableNameAndScope)(PVOID, int, int, BSTR*, unsigned int*, unsigned int*);
typedef  BOOL (*GetLineByILOffsetDelegate)(PVOID, mdMethodDef, ULONG64, ULONG *, BSTR*);
typedef  BOOL (*GetStepRangesFromIPDelegate)(PVOID, int, mdMethodDef, unsigned int*, unsigned int*);
typedef  BOOL (*GetSequencePointsDelegate)(PVOID, mdMethodDef, PVOID*, int*);

typedef BSTR (*SysAllocStringLen_t)(const OLECHAR*, UINT);
typedef void (*SysFreeString_t)(BSTR);
typedef UINT (*SysStringLen_t)(BSTR);
typedef void (*CoTaskMemFree_t)(LPVOID);

BOOL SafeReadMemory (TADDR offset, PVOID lpBuffer, ULONG cb,
                     PULONG lpcbBytesRead);

class SymbolReader
{
private:
    PVOID m_symbolReaderHandle;

    static std::string coreClrPath;
    static LoadSymbolsForModuleDelegate loadSymbolsForModuleDelegate;
    static DisposeDelegate disposeDelegate;
    static ResolveSequencePointDelegate resolveSequencePointDelegate;
    static GetLocalVariableNameAndScope getLocalVariableNameAndScopeDelegate;
    static GetLineByILOffsetDelegate getLineByILOffsetDelegate;
    static GetStepRangesFromIPDelegate getStepRangesFromIPDelegate;
    static GetSequencePointsDelegate getSequencePointsDelegate;

    static SysAllocStringLen_t sysAllocStringLen;
    static SysFreeString_t sysFreeString;
    static SysStringLen_t sysStringLen;
    static CoTaskMemFree_t coTaskMemFree;

    static HRESULT PrepareSymbolReader();

    HRESULT LoadSymbolsForPortablePDB(
        WCHAR* pModuleName,
        BOOL isInMemory,
        BOOL isFileLayout,
        ULONG64 peAddress,
        ULONG64 peSize,
        ULONG64 inMemoryPdbAddress,
        ULONG64 inMemoryPdbSize);

public:
    static const int HiddenLine;
    struct __attribute__((packed)) SequencePoint {
        int32_t startLine;
        int32_t startColumn;
        int32_t endLine;
        int32_t endColumn;
        int32_t offset;
    };

    SymbolReader()
    {
        m_symbolReaderHandle = 0;
    }

    ~SymbolReader()
    {
        if (m_symbolReaderHandle != 0)
        {
            disposeDelegate(m_symbolReaderHandle);
            m_symbolReaderHandle = 0;
        }
    }

    bool SymbolsLoaded() const { return m_symbolReaderHandle != 0; }

    static void SetCoreCLRPath(const std::string &path) { coreClrPath = path; }

    HRESULT LoadSymbols(IMetaDataImport* pMD, ICorDebugModule* pModule);
    HRESULT GetLineByILOffset(mdMethodDef MethodToken, ULONG64 IlOffset, ULONG *pLinenum, WCHAR* pwszFileName, ULONG cchFileName);
    HRESULT GetNamedLocalVariableAndScope(ICorDebugILFrame * pILFrame, mdMethodDef methodToken, ULONG localIndex, WCHAR* paramName, ULONG paramNameLen, ICorDebugValue **ppValue, ULONG32* pIlStart, ULONG32* pIlEnd);
    HRESULT ResolveSequencePoint(const char *filename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset);
    HRESULT GetStepRangesFromIP(ULONG64 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset);
    HRESULT GetSequencePoints(mdMethodDef methodToken, std::vector<SequencePoint> &points);
};
