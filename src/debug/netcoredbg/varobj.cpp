// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <iomanip>

#include "debugger.h"
#include "typeprinter.h"
#include "modules.h"
#include "valuewalk.h"
#include "valueprint.h"
#include "varobj.h"
#include "expr.h"
#include "frames.h"


HRESULT GetNumChild(ICorDebugValue *pValue,
                    unsigned int &numchild,
                    bool static_members = false)
{
    HRESULT Status = S_OK;
    numchild = 0;

    ULONG numstatic = 0;
    ULONG numinstance = 0;

    IfFailRet(WalkMembers(pValue, nullptr, nullptr, [&numstatic, &numinstance](
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *,
        bool is_static,
        const std::string &)
    {
        if (is_static)
            numstatic++;
        else
            numinstance++;
        return S_OK;
    }));

    if (static_members)
    {
        numchild = numstatic;
    }
    else
    {
        numchild = (numstatic > 0) ? numinstance + 1 : numinstance;
    }
    return S_OK;
}

static ToRelease<ICorDebugFunction> g_pRunClassConstructor;
static ToRelease<ICorDebugFunction> g_pGetTypeHandle;

// TODO: Move this into debugger
void CleanupVars()
{
    if (g_pRunClassConstructor)
        g_pRunClassConstructor->Release();
    if (g_pGetTypeHandle)
        g_pGetTypeHandle->Release();
}

static HRESULT GetMethodToken(IMetaDataImport *pMD, mdTypeDef cl, const WCHAR *methodName)
{
    ULONG numMethods = 0;
    HCORENUM mEnum = NULL;
    mdMethodDef methodDef = mdTypeDefNil;
    pMD->EnumMethodsWithName(&mEnum, cl, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(mEnum);
    return methodDef;
}

static HRESULT FindFunction(ICorDebugModule *pModule,
                            const WCHAR *typeName,
                            const WCHAR *methodName,
                            ICorDebugFunction **ppFunction)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;

    IfFailRet(pMD->FindTypeDefByName(typeName, mdTypeDefNil, &typeDef));

    mdMethodDef methodDef = GetMethodToken(pMD, typeDef, methodName);

    if (methodDef == mdMethodDefNil)
        return E_FAIL;

    return pModule->GetFunctionFromToken(methodDef, ppFunction);
}

HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue)
{
    HRESULT Status;

    if (!g_pRunClassConstructor && !g_pGetTypeHandle)
    {
        ToRelease<ICorDebugModule> pModule;
        IfFailRet(Modules::GetModuleWithName("System.Private.CoreLib.dll", &pModule));

        static const WCHAR helpersName[] = W("System.Runtime.CompilerServices.RuntimeHelpers");
        static const WCHAR runCCTorMethodName[] = W("RunClassConstructor");
        static const WCHAR typeName[] = W("System.Type");
        static const WCHAR getTypeHandleMethodName[] = W("GetTypeHandle");
        IfFailRet(FindFunction(pModule, helpersName, runCCTorMethodName, &g_pRunClassConstructor));
        IfFailRet(FindFunction(pModule, typeName, getTypeHandleMethodName, &g_pGetTypeHandle));
    }

    ToRelease<ICorDebugValue> pNewValue;

    ToRelease<ICorDebugValue> pUnboxedValue;
    BOOL isNull = FALSE;
    IfFailRet(DereferenceAndUnboxValue(pValue, &pUnboxedValue, &isNull));

    CorElementType et;
    IfFailRet(pUnboxedValue->GetType(&et));

    if (et != ELEMENT_TYPE_CLASS)
        return S_OK;

    if (isNull)
    {
        ToRelease<ICorDebugValue2> pValue2;
        ToRelease<ICorDebugType> pType;

        IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
        IfFailRet(pValue2->GetExactType(&pType));

        EvalObjectNoConstructor(pThread, pType, &pNewValue);
    }

    ToRelease<ICorDebugValue> pRuntimeHandleValue;
    IfFailRet(EvalFunction(pThread, g_pGetTypeHandle, nullptr, pNewValue ? pNewValue.GetPtr() : pValue, &pRuntimeHandleValue));

    ToRelease<ICorDebugValue> pResultValue;
    IfFailRet(EvalFunction(pThread, g_pRunClassConstructor, nullptr, pRuntimeHandleValue, &pResultValue));

    return S_OK;
}

struct Member
{
    std::string name;
    std::string ownerType;
    ToRelease<ICorDebugValue> value;
    Member(const std::string &name, const std::string ownerType, ToRelease<ICorDebugValue> value) :
        name(name),
        ownerType(ownerType),
        value(std::move(value))
    {}
    Member(Member &&that) = default;
private:
    Member(const Member &that) = delete;
};

static HRESULT FetchFieldsAndProperties(ICorDebugValue *pInputValue,
                                        ICorDebugThread *pThread,
                                        ICorDebugILFrame *pILFrame,
                                        std::vector<Member> &members,
                                        bool fetchOnlyStatic,
                                        bool &hasStaticMembers,
                                        int childStart,
                                        int childEnd)
{
    hasStaticMembers = false;
    HRESULT Status;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));

    int currentIndex = -1;

    IfFailRet(WalkMembers(pInputValue, pThread, pILFrame, [&](
        mdMethodDef mdGetter,
        ICorDebugModule *pModule,
        ICorDebugType *pType,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &name)
    {
        if (is_static)
            hasStaticMembers = true;

        bool addMember = fetchOnlyStatic ? is_static : !is_static;
        if (!addMember)
            return S_OK;

        ++currentIndex;
        if (currentIndex < childStart)
            return S_OK;
        if (currentIndex >= childEnd)
            return S_OK;

        std::string className;
        if (pType)
            TypePrinter::GetTypeOfValue(pType, className);

        ToRelease<ICorDebugValue> pResultValue;

        if (mdGetter != mdMethodDefNil)
        {
            ToRelease<ICorDebugFunction> pFunc;
            if (SUCCEEDED(pModule->GetFunctionFromToken(mdGetter, &pFunc)))
                EvalFunction(pThread, pFunc, pType, is_static ? nullptr : pInputValue, &pResultValue);
        }
        else
        {
            if (pValue)
                pValue->AddRef();
            pResultValue = pValue;
        }

        members.emplace_back(name, className, std::move(pResultValue));
        return S_OK;
    }));

    return S_OK;
}

int Debugger::GetNamedVariables(uint32_t variablesReference)
{
    auto it = m_variables.find(variablesReference);
    if (it == m_variables.end())
        return 0;
    return it->second.namedVariables;
}

HRESULT Debugger::GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables)
{
    auto it = m_variables.find(variablesReference);
    if (it == m_variables.end())
        return E_FAIL;

    VariableReference &ref = it->second;

    HRESULT Status;

    StackFrame stackFrame(ref.frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, stackFrame.GetLevel(), &pFrame));

    // Named and Indexed variables are in the same index (internally), Named variables go first
    if (filter == VariablesNamed && (start + count > ref.namedVariables || count == 0))
        count = ref.namedVariables - start;
    if (filter == VariablesIndexed)
        start += ref.namedVariables;

    if (ref.IsScope())
    {
        IfFailRet(GetStackVariables(ref.frameId, pThread, pFrame, start, count, variables));
    } else {
        IfFailRet(GetChildren(ref, pThread, pFrame, start, count, variables));
    }
    return S_OK;
}

void Debugger::AddVariableReference(Variable &variable, uint64_t frameId, ICorDebugValue *value, VariableReference::ValueKind valueKind)
{
    HRESULT Status;
    unsigned int numChild = 0;
    GetNumChild(value, numChild, valueKind == VariableReference::ValueIsClass);
    if (numChild == 0)
        return;

    variable.namedVariables = numChild;
    variable.variablesReference = m_nextVariableReference++;
    value->AddRef();
    VariableReference variableReference(variable, frameId, value, valueKind);
    m_variables.emplace(std::make_pair(variable.variablesReference, std::move(variableReference)));
}

HRESULT Debugger::GetStackVariables(uint64_t frameId, ICorDebugThread *pThread, ICorDebugFrame *pFrame, int start, int count, std::vector<Variable> &variables)
{
    HRESULT Status;

    int currentIndex = -1;

    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
    {
        ++currentIndex;
        bool outOfRange = currentIndex < start || (count != 0 && currentIndex >= start + count);
        if (!outOfRange)
        {
            Variable var;
            var.name = "$exception";
            var.evaluateName = var.name;
            bool escape = true;
            PrintValue(pExceptionValue, var.value, escape);
            TypePrinter::GetTypeOfValue(pExceptionValue, var.type);
            AddVariableReference(var, frameId, pExceptionValue, VariableReference::ValueIsVariable);
            variables.push_back(var);
        }
    }

    IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
    {
        ++currentIndex;
        if (currentIndex < start || (count != 0 && currentIndex >= start + count))
            return S_OK;
        Variable var;
        var.name = name;
        var.evaluateName = var.name;
        bool escape = true;
        PrintValue(pValue, var.value, escape);
        TypePrinter::GetTypeOfValue(pValue, var.type);
        AddVariableReference(var, frameId, pValue, VariableReference::ValueIsVariable);
        variables.push_back(var);
        return S_OK;
    }));

    return S_OK;
}

HRESULT Debugger::GetScopes(uint64_t frameId, std::vector<Scope> &scopes)
{
    HRESULT Status;

    StackFrame stackFrame(frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, stackFrame.GetLevel(), &pFrame));

    int namedVariables = 0;
    uint32_t variablesReference = 0;

    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
        namedVariables++;

    IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
    {
        namedVariables++;
        return S_OK;
    }));

    if (namedVariables > 0)
    {
        variablesReference = m_nextVariableReference++;
        VariableReference scopeReference(variablesReference, frameId, namedVariables);
        m_variables.emplace(std::make_pair(variablesReference, std::move(scopeReference)));
    }

    scopes.emplace_back(variablesReference, "Locals", frameId);

    return S_OK;
}

static void FixupInheritedFieldNames(std::vector<Member> &members)
{
    std::unordered_set<std::string> names;
    for (auto &it : members)
    {
        auto r = names.insert(it.name);
        if (!r.second)
        {
            it.name += " (" + it.ownerType + ")";
        }
    }
}

HRESULT Debugger::GetChildren(VariableReference &ref,
                              ICorDebugThread *pThread,
                              ICorDebugFrame *pFrame,
                              int start,
                              int count,
                              std::vector<Variable> &variables)
{
    if (ref.IsScope())
        return E_INVALIDARG;

    HRESULT Status;

    ToRelease<ICorDebugILFrame> pILFrame;
    if (pFrame)
        IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    std::vector<Member> members;

    bool hasStaticMembers = false;

    if (!ref.value)
        return S_OK;

    IfFailRet(FetchFieldsAndProperties(ref.value,
                                       pThread,
                                       pILFrame,
                                       members,
                                       ref.valueKind == VariableReference::ValueIsClass,
                                       hasStaticMembers,
                                       start,
                                       count == 0 ? INT_MAX : start + count));

    FixupInheritedFieldNames(members);

    for (auto &it : members)
    {
        Variable var;
        var.name = it.name;
        bool isIndex = !it.name.empty() && it.name.at(0) == '[';
        if (var.name.find('(') == std::string::npos) // expression evaluator does not support typecasts
            var.evaluateName = ref.evaluateName + (isIndex ? "" : ".") + var.name;
        bool escape = true;
        PrintValue(it.value, var.value, escape);
        TypePrinter::GetTypeOfValue(it.value, var.type);
        AddVariableReference(var, ref.frameId, it.value, VariableReference::ValueIsVariable);
        variables.push_back(var);
    }

    if (ref.valueKind == VariableReference::ValueIsVariable && hasStaticMembers)
    {
        bool staticsInRange = start < ref.namedVariables && (count == 0 || start + count >= ref.namedVariables);
        if (staticsInRange)
        {
            RunClassConstructor(pThread, ref.value);

            Variable var;
            var.name = "Static members";
            TypePrinter::GetTypeOfValue(ref.value, var.evaluateName); // do not expose type for this fake variable
            AddVariableReference(var, ref.frameId, ref.value, VariableReference::ValueIsClass);
            variables.push_back(var);
        }
    }

    return S_OK;
}

HRESULT Debugger::Evaluate(uint64_t frameId, const std::string &expression, Variable &variable)
{
    HRESULT Status;

    StackFrame stackFrame(frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, stackFrame.GetLevel(), &pFrame));

    ToRelease<ICorDebugValue> pResultValue;
    IfFailRet(EvalExpr(pThread, pFrame, expression, &pResultValue));

    variable.evaluateName = expression;

    bool escape = true;
    PrintValue(pResultValue, variable.value, escape);
    TypePrinter::GetTypeOfValue(pResultValue, variable.type);
    AddVariableReference(variable, frameId, pResultValue, VariableReference::ValueIsVariable);

    return S_OK;
}
