// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>
#include "debugger/breakpoint_entry.h"
#include "debugger/breakpointutils.h"
#include "metadata/modules.h"
#include "utils/utf.h"

namespace netcoredbg
{

static mdMethodDef GetEntryPointTokenFromFile(const std::string &path)
{
    class scope_guard
    {
    private:
        FILE **ppFile_;

    public:
        scope_guard(FILE **ppFile) : ppFile_(ppFile) {}
        ~scope_guard() {if (*ppFile_) fclose(*ppFile_);}
    };

    FILE *pFile = nullptr;
    scope_guard file(&pFile);

#ifdef WIN32
    if (_wfopen_s(&pFile, to_utf16(path).c_str(), L"rb") != 0)
        return mdMethodDefNil;
#else
    pFile = fopen(path.c_str(), "rb");
#endif // WIN32

    if (!pFile)
        return mdMethodDefNil;

    IMAGE_DOS_HEADER dosHeader;
    IMAGE_NT_HEADERS32 ntHeaders;

    if (fread(&dosHeader, sizeof(dosHeader), 1, pFile) != 1) return mdMethodDefNil;
    if (fseek(pFile, VAL32(dosHeader.e_lfanew), SEEK_SET) != 0) return mdMethodDefNil;
    if (fread(&ntHeaders, sizeof(ntHeaders), 1, pFile) != 1) return mdMethodDefNil;

    ULONG corRVA = 0;
    if (ntHeaders.OptionalHeader.Magic == VAL16(IMAGE_NT_OPTIONAL_HDR32_MAGIC))
    {
        corRVA = VAL32(ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }
    else
    {
        IMAGE_NT_HEADERS64 ntHeaders64;
        if (fseek(pFile, VAL32(dosHeader.e_lfanew), SEEK_SET) != 0) return mdMethodDefNil;
        if (fread(&ntHeaders64, sizeof(ntHeaders64), 1, pFile) != 1) return mdMethodDefNil;
        corRVA = VAL32(ntHeaders64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }

    constexpr DWORD DWORD_MAX = 4294967295;
    DWORD pos = VAL32(dosHeader.e_lfanew);
    if (pos > DWORD_MAX - sizeof(ntHeaders.Signature) - sizeof(ntHeaders.FileHeader) - VAL16(ntHeaders.FileHeader.SizeOfOptionalHeader))
        return mdMethodDefNil;
    pos += sizeof(ntHeaders.Signature) + sizeof(ntHeaders.FileHeader) + VAL16(ntHeaders.FileHeader.SizeOfOptionalHeader);

    if (fseek(pFile, pos, SEEK_SET) != 0) return mdMethodDefNil;

    for (int i = 0; i < VAL16(ntHeaders.FileHeader.NumberOfSections); i++)
    {
        IMAGE_SECTION_HEADER sectionHeader;

        if (fread(&sectionHeader, sizeof(sectionHeader), 1, pFile) != 1) return mdMethodDefNil;

        if (corRVA >= VAL32(sectionHeader.VirtualAddress) &&
            corRVA < VAL32(sectionHeader.VirtualAddress) + VAL32(sectionHeader.SizeOfRawData))
        {
            ULONG offset = (corRVA - VAL32(sectionHeader.VirtualAddress)) + VAL32(sectionHeader.PointerToRawData);

            IMAGE_COR20_HEADER corHeader;
            if (fseek(pFile, offset, SEEK_SET) != 0) return mdMethodDefNil;
            if (fread(&corHeader, sizeof(corHeader), 1, pFile) != 1) return mdMethodDefNil;

            if (VAL32(corHeader.Flags) & COMIMAGE_FLAGS_NATIVE_ENTRYPOINT)
                return mdMethodDefNil;

            return VAL32(corHeader.EntryPointToken);
        }
    }

    return mdMethodDefNil;
}

// Try to setup proper entry breakpoint method token and IL offset for async Main method.
// [in] pModule - module with async Main method;
// [in] pMD - metadata interface for pModule;
// [in] pModules - all loaded modules debug related data;
// [in] mdMainClass - class token with Main method in module pModule;
// [out] entryPointToken - corrected method token;
// [out] entryPointOffset - corrected IL offset on first user code line.
static HRESULT TrySetupAsyncEntryBreakpoint(ICorDebugModule *pModule, IMetaDataImport *pMD, Modules *pModules,
                                            mdTypeDef mdMainClass, mdMethodDef &entryPointToken, ULONG32 &entryPointOffset)
{
    // In case of async method, compiler use `Namespace.ClassName.<Main>()` as entry method, that call
    // `Namespace.ClassName.Main()`, that create `Namespace.ClassName.<Main>d__0` and start state machine routine.
    // In this case, "real entry method" with user code from initial `Main()` method will be in:
    // Namespace.ClassName.<Main>d__0.MoveNext()
    // Note, number in "<Main>d__0" class name could be different.
    // Note, `Namespace.ClassName` could be different (see `-main` compiler option).
    // Note, `Namespace.ClassName.<Main>d__0` type have enclosing class as method `Namespace.ClassName.<Main>()` class.
    HRESULT Status;
     ULONG numTypedefs = 0;
    HCORENUM hEnum = NULL;
    mdTypeDef typeDef;
    mdMethodDef resultToken = mdMethodDefNil;
    while(SUCCEEDED(pMD->EnumTypeDefs(&hEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0 && resultToken == mdMethodDefNil)
    {
        mdTypeDef mdEnclosingClass;
        if (FAILED(pMD->GetNestedClassProps(typeDef, &mdEnclosingClass) ||
            mdEnclosingClass != mdMainClass))
            continue;

        DWORD flags;
        WCHAR className[mdNameLen];
        ULONG classNameLen;
        IfFailRet(pMD->GetTypeDefProps(typeDef, className, _countof(className), &classNameLen, &flags, NULL));
        if (!starts_with(className, W("<Main>d__")))
            continue;

        ULONG numMethods = 0;
        HCORENUM fEnum = NULL;
        mdMethodDef methodDef;
        while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
        {
            mdTypeDef memTypeDef;
            WCHAR funcName[mdNameLen];
            ULONG funcNameLen;
            if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef, funcName, _countof(funcName), &funcNameLen,
                                            nullptr, nullptr, nullptr, nullptr, nullptr)))
            {
                continue;
            }

            if (str_equal(funcName, W("MoveNext")))
            {
                resultToken = methodDef;
                break;
            }
        }
        pMD->CloseEnum(fEnum);
    }
    pMD->CloseEnum(hEnum);

    if (resultToken == mdMethodDefNil)
        return E_FAIL;

    // Note, in case of async `MoveNext` method, user code don't start from 0 IL offset.
    ULONG32 ilCloseOffset;
    const ULONG32 currentVersion = 1; // In case entry breakpoint, this can be only base PDB, not delta PDB for sure.
    IfFailRet(pModules->GetNextSequencePointInMethod(pModule, resultToken, currentVersion, 0, ilCloseOffset));

    entryPointToken = resultToken;
    entryPointOffset = ilCloseOffset;
    return S_OK;
}

HRESULT EntryBreakpoint::ManagedCallbackLoadModule(ICorDebugModule *pModule)
{
    std::lock_guard<std::mutex> lock(m_entryMutex);

    if (!m_stopAtEntry || m_iCorFuncBreakpoint)
        return S_FALSE;

    HRESULT Status;
    mdMethodDef entryPointToken = GetEntryPointTokenFromFile(GetModuleFileName(pModule));
    // Note, by some reason, in CoreCLR 6.0 System.Private.CoreLib.dll have Token "0" as entry point RVA.
    if (entryPointToken == mdMethodDefNil ||
        TypeFromToken(entryPointToken) != mdtMethodDef)
        return S_FALSE;

    ULONG32 entryPointOffset = 0;
    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    mdTypeDef mdMainClass;
    WCHAR funcName[mdNameLen];
    ULONG funcNameLen;
    // If we can't setup entry point correctly for async method, leave it "as is".
    if (SUCCEEDED(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown)) &&
        SUCCEEDED(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD)) &&
        SUCCEEDED(pMD->GetMethodProps(entryPointToken, &mdMainClass, funcName, _countof(funcName), &funcNameLen,
                                      nullptr, nullptr, nullptr, nullptr, nullptr)) &&
        // The `Main` method is the entry point of a C# application. (Libraries and services do not require a Main method as an entry point.)
        // https://docs.microsoft.com/en-us/dotnet/csharp/programming-guide/main-and-command-args/
        // In case of async method as entry method, GetEntryPointTokenFromFile() should return compiler's generated method `<Main>`, plus,
        // this should be method without user code.
        str_equal(funcName, W("<Main>")))
    {
        TrySetupAsyncEntryBreakpoint(pModule, pMD, m_sharedModules.get(), mdMainClass, entryPointToken, entryPointOffset);
    }

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pModule->GetFunctionFromToken(entryPointToken, &pFunction));
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunction->GetILCode(&pCode));
    ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(entryPointOffset, &iCorFuncBreakpoint));

    m_iCorFuncBreakpoint = iCorFuncBreakpoint.Detach();

    return S_OK;
}

HRESULT EntryBreakpoint::CheckBreakpointHit(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    std::lock_guard<std::mutex> lock(m_entryMutex);

    if (!m_stopAtEntry || !m_iCorFuncBreakpoint)
        return S_FALSE; // S_FALSE - no error, but not affect on callback

    HRESULT Status;
    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    IfFailRet(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID*) &pFunctionBreakpoint));
    IfFailRet(BreakpointUtils::IsSameFunctionBreakpoint(pFunctionBreakpoint, m_iCorFuncBreakpoint));

    m_iCorFuncBreakpoint->Activate(FALSE);
    m_iCorFuncBreakpoint.Free();
    return S_OK;
}

void EntryBreakpoint::Delete()
{
    std::lock_guard<std::mutex> lock(m_entryMutex);

    if (!m_iCorFuncBreakpoint)
        return;

    m_iCorFuncBreakpoint->Activate(FALSE);
    m_iCorFuncBreakpoint.Free();
}

} // namespace netcoredbg
