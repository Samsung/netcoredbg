// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>
#include <list>
#include <mutex>
#include <memory>
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;
class EvalWaiter;

HRESULT FindFunction(ICorDebugModule *pModule, const WCHAR *typeName, const WCHAR *methodName, ICorDebugFunction **ppFunction);
// Return `false` only for non generic type without static constructor (its static members could be read without type object).
bool TypeObjectNeeded(ICorDebugType *pType);

class EvalHelpers
{
public:

    EvalHelpers(std::shared_ptr<Modules> &sharedModules,
                std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
        m_sharedModules(sharedModules),
        m_sharedEvalWaiter(sharedEvalWaiter)
    {}

    // Create type object (run type static constructor) before type static members access.
    // Return S_FALSE without ICorDebugValue creation in case type don't have static members (DetectStaticMembers is true).
    // Return S_OK without ICorDebugValue creation (*ppTypeObjectResult is not changed) in case type object not needed
    // (see TypeObjectNeeded()) and ForceCreation is false, static members of this type should be read directly.
    // Note, static field value getter (see Evaluator::WalkMembers()) return S_FALSE in case value is default value of static
    // field without storage (class not loaded or storage not allocated yet), this value can't be changed. In order to change
    // field, type object must be created with ForceCreation and value must be received again (for thread static field of
    // thread, that never accessed it, storage is still not allocated after this).
    HRESULT CreatTypeObjectStaticConstructor(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugValue **ppTypeObjectResult = nullptr,
        bool DetectStaticMembers = true,
        bool ForceCreation = false);

    HRESULT EvalFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType **ppArgsType,
        ULONG32 ArgsTypeCount,
        ICorDebugValue **ppArgsValue,
        ULONG32 ArgsValueCount,
        ICorDebugValue **ppEvalResult,
        int evalFlags);

    HRESULT EvalGenericFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType **ppArgsType,
        ULONG32 ArgsTypeCount,
        ICorDebugValue **ppArgsValue,
        ULONG32 ArgsValueCount,
        ICorDebugValue **ppEvalResult,
        int evalFlags);

    HRESULT GetLiteralValue(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugModule *pModule,
        PCCOR_SIGNATURE pSignatureBlob,
        ULONG sigBlobLength,
        UVCP_CONSTANT pRawValue,
        ULONG rawValueLength,
        ICorDebugValue **ppLiteralValue);

    // Create default value of field type without evaluation, E_NOTIMPL in case this is not possible.
    HRESULT CreateDefaultValue(
        ICorDebugThread *pThread,
        ICorDebugModule *pModule,
        PCCOR_SIGNATURE pSignatureBlob,
        ICorDebugValue **ppDefaultValue);

    HRESULT CreateString(ICorDebugThread *pThread, const std::string &value, ICorDebugValue **ppNewString);

    HRESULT FindMethodInModule(const std::string &moduleName, const WCHAR className[], const WCHAR methodName[], ICorDebugFunction **ppFunction);

    void Cleanup();

private:

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalWaiter> m_sharedEvalWaiter;

    std::mutex m_pSuppressFinalizeMutex;
    ToRelease<ICorDebugFunction> m_pSuppressFinalize;

    struct type_object_t
    {
        COR_TYPEID id;
        ToRelease<ICorDebugHandleValue> typeObject;
    };

    std::mutex m_typeObjectCacheMutex;
    // Because handles affect the performance of the garbage collector, the debugger should limit itself to a relatively
    // small number of handles (about 256) that are active at a time.
    // https://learn.microsoft.com/en-us/dotnet/core/unmanaged-api/debugging/icordebug/icordebugheapvalue2-createhandle-method
    // Note, we also use handles (results of eval) in var refs during brake (cleared at 'Continue').
    // Warning! Since we use `std::prev(m_typeObjectCache.end())` without any check in code, make sure cache size is `2` or bigger.
    static const size_t m_typeObjectCacheSize = 100;
    // The idea of cache is not hold all type objects, but prevent numerous times same type objects creation during eval.
    // At access, element moved to front of list, new element also add to front. In this way, not used elements displaced from cache.
    std::list<type_object_t> m_typeObjectCache;

    HRESULT TryReuseTypeObjectFromCache(ICorDebugType *pType, ICorDebugValue **ppTypeObjectResult);
    HRESULT AddTypeObjectToCache(ICorDebugType *pType, ICorDebugValue *pTypeObject);

};

} // namespace netcoredbg
