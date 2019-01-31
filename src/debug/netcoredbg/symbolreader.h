// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD
#pragma once

#include <cor.h>
#include <cordebug.h>

#include <string>
#include <vector>
#include <functional>


/// FIXME: Definition of `TADDR`
#include "torelease.h"


typedef  int (*ReadMemoryDelegate)(ULONG64, char *, int);
typedef  PVOID (*LoadSymbolsForModuleDelegate)(const WCHAR*, BOOL, ULONG64, int, ULONG64, int, ReadMemoryDelegate);
typedef  void (*DisposeDelegate)(PVOID);
typedef  BOOL (*ResolveSequencePointDelegate)(PVOID, const WCHAR*, unsigned int, unsigned int*, unsigned int*);
typedef  BOOL (*GetLocalVariableNameAndScope)(PVOID, int, int, BSTR*, unsigned int*, unsigned int*);
typedef  BOOL (*GetSequencePointByILOffsetDelegate)(PVOID, mdMethodDef, ULONG64, PVOID);
typedef  BOOL (*GetStepRangesFromIPDelegate)(PVOID, int, mdMethodDef, unsigned int*, unsigned int*);
typedef  BOOL (*GetSequencePointsDelegate)(PVOID, mdMethodDef, PVOID*, int*);
typedef  BOOL (*ParseExpressionDelegate)(const WCHAR*, const WCHAR*, PVOID*, int *, BSTR*);
typedef  BOOL (*EvalExpressionDelegate)(const WCHAR*, PVOID, BSTR*, int*, int*, PVOID*);
typedef  BOOL (*GetChildDelegate)(PVOID, PVOID, const WCHAR*, int *, PVOID*);
typedef  BOOL (*RegisterGetChildDelegate)(GetChildDelegate);

typedef BSTR (*SysAllocStringLen_t)(const OLECHAR*, UINT);
typedef void (*SysFreeString_t)(BSTR);
typedef UINT (*SysStringLen_t)(BSTR);
typedef LPVOID (*CoTaskMemAlloc_t)(size_t);
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
    static GetSequencePointByILOffsetDelegate getSequencePointByILOffsetDelegate;
    static GetStepRangesFromIPDelegate getStepRangesFromIPDelegate;
    static GetSequencePointsDelegate getSequencePointsDelegate;
    static ParseExpressionDelegate parseExpressionDelegate;
    static EvalExpressionDelegate evalExpressionDelegate;
    static RegisterGetChildDelegate registerGetChildDelegate;

    static SysAllocStringLen_t sysAllocStringLen;
    static SysFreeString_t sysFreeString;
    static SysStringLen_t sysStringLen;
    static CoTaskMemAlloc_t coTaskMemAlloc;
    static CoTaskMemFree_t coTaskMemFree;

    static HRESULT PrepareSymbolReader();

    HRESULT LoadSymbolsForPortablePDB(
        const std::string &modulePath,
        BOOL isInMemory,
        BOOL isFileLayout,
        ULONG64 peAddress,
        ULONG64 peSize,
        ULONG64 inMemoryPdbAddress,
        ULONG64 inMemoryPdbSize);

public:
    static const int HiddenLine;

    PACK_BEGIN struct SequencePoint {
        int32_t startLine;
        int32_t startColumn;
        int32_t endLine;
        int32_t endColumn;
        int32_t offset;
        BSTR document;
        SequencePoint() :
            startLine(0), startColumn(0),
            endLine(0), endColumn(0),
            offset(0),
            document(nullptr)
        {}
        ~SequencePoint() { sysFreeString(document); }
    } PACK_END;

    // Keep in sync with string[] basicTypes in SymbolReader.cs
    enum BasicTypes {
        TypeCorValue = -1,
        TypeObject = 0, //     "System.Object",
        TypeBoolean, //        "System.Boolean",
        TypeByte,    //        "System.Byte",
        TypeSByte,   //        "System.SByte",
        TypeChar,    //        "System.Char",
        TypeDouble,  //        "System.Double",
        TypeSingle,  //        "System.Single",
        TypeInt32,   //        "System.Int32",
        TypeUInt32,  //        "System.UInt32",
        TypeInt64,   //        "System.Int64",
        TypeUInt64,  //        "System.UInt64",
        TypeInt16,   //        "System.Int16",
        TypeUInt16,  //        "System.UInt16",
        TypeIntPtr,  //        "System.IntPtr",
        TypeUIntPtr, //        "System.UIntPtr",
        TypeDecimal, //        "System.Decimal",
        TypeString,  //        "System.String"
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

    typedef std::function<bool(PVOID, const std::string&, int *, PVOID*)> GetChildCallback;

    HRESULT LoadSymbols(IMetaDataImport* pMD, ICorDebugModule* pModule);
    HRESULT GetSequencePointByILOffset(mdMethodDef MethodToken, ULONG64 IlOffset, SequencePoint *sequencePoint);
    HRESULT GetNamedLocalVariableAndScope(ICorDebugILFrame * pILFrame, mdMethodDef methodToken, ULONG localIndex, WCHAR* paramName, ULONG paramNameLen, ICorDebugValue **ppValue, ULONG32* pIlStart, ULONG32* pIlEnd);
    HRESULT ResolveSequencePoint(const char *filename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset);
    HRESULT GetStepRangesFromIP(ULONG32 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset);
    HRESULT GetSequencePoints(mdMethodDef methodToken, std::vector<SequencePoint> &points);
    static HRESULT ParseExpression(const std::string &expr, const std::string &typeName, std::string &data, std::string &errorText);
    static HRESULT EvalExpression(const std::string &expr, std::string &result, int *typeId, ICorDebugValue **ppValue, GetChildCallback cb);
    static PVOID AllocBytes(size_t size);
    static PVOID AllocString(const std::string &str);
};
