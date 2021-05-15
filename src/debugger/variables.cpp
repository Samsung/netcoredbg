// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <regex>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <algorithm>
#include <sstream>

#include "metadata/typeprinter.h"
#include "valueprint.h"
#include "debugger/variables.h"
#include "debugger/evalhelpers.h"
#include "debugger/evaluator.h"
#include "debugger/frames.h"
#include "managed/interop.h"
#include "utils/logger.h"
#include "utils/utf.h"
#include "protocols/protocol.h"

using std::string;
using std::vector;

namespace netcoredbg
{

void Variables::GetNumChild(
    ICorDebugValue *pValue,
    int &numChild,
    bool static_members)
{
    numChild = 0;

    if (pValue == nullptr)
        return;

    int numStatic = 0;
    int numInstance = 0;
    // No thread and FrameLevel{0} here, since we need only count childs.
    if (FAILED(m_sharedEvaluator->WalkMembers(pValue, nullptr, FrameLevel{0}, [&numStatic, &numInstance](
        ICorDebugType *,
        bool is_static,
        const std::string &,
        Evaluator::GetValueCallback,
        Evaluator::SetValueCallback)
    {
        if (is_static)
            numStatic++;
        else
            numInstance++;
        return S_OK;
    })))
    {
        return;
    }

    if (static_members)
    {
        numChild = numStatic;
    }
    else
    {
        // Note, "+1", since all static members will be "packed" into "Static members" entry
        numChild = (numStatic > 0) ? numInstance + 1 : numInstance;
    }
}

struct Variables::Member
{
    std::string name;
    std::string ownerType;
    ToRelease<ICorDebugValue> value;
    Member(const std::string &name, const std::string& ownerType, ICorDebugValue *pValue) :
        name(name),
        ownerType(ownerType),
        value(pValue)
    {}
    Member(Member &&that) = default;
    Member(const Member &that) = delete;
};

void Variables::FillValueAndType(Member &member, Variable &var, bool escape)
{
    if (member.value == nullptr)
    {
        var.value = "<error>";
        return;
    }
    PrintValue(member.value, var.value, escape);
    TypePrinter::GetTypeOfValue(member.value, var.type);
}

HRESULT Variables::FetchFieldsAndProperties(
    ICorDebugValue *pInputValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    std::vector<Member> &members,
    bool fetchOnlyStatic,
    bool &hasStaticMembers,
    int childStart,
    int childEnd,
    int evalFlags)
{
    hasStaticMembers = false;
    HRESULT Status;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));

    int currentIndex = -1;

    IfFailRet(m_sharedEvaluator->WalkMembers(pInputValue, pThread, frameLevel, [&](
        ICorDebugType *pType,
        bool is_static,
        const std::string &name,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetValueCallback)
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

        ToRelease<ICorDebugValue> iCorResultValue;
        getValue(&iCorResultValue, evalFlags); // no result check here, since error is result too

        string className;
        if (pType)
            TypePrinter::GetTypeOfValue(pType, className);

        members.emplace_back(name, className, iCorResultValue.Detach());
        return S_OK;
    }));

    return S_OK;
}

int Variables::GetNamedVariables(uint32_t variablesReference)
{
    std::lock_guard<std::recursive_mutex> lock(m_referencesMutex);

    auto it = m_references.find(variablesReference);
    if (it == m_references.end())
        return 0;
    return it->second.namedVariables;
}

HRESULT Variables::GetVariables(
    ICorDebugProcess *pProcess,
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    if (pProcess == nullptr)
        return E_FAIL;

    std::lock_guard<std::recursive_mutex> lock(m_referencesMutex);

    auto it = m_references.find(variablesReference);
    if (it == m_references.end())
        return E_FAIL;

    VariableReference &ref = it->second;

    HRESULT Status;

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(ref.frameId.getThread()), &pThread));

    // Named and Indexed variables are in the same index (internally), Named variables go first
    if (filter == VariablesNamed && (start + count > ref.namedVariables || count == 0))
        count = ref.namedVariables - start;
    if (filter == VariablesIndexed)
        start += ref.namedVariables;

    if (ref.IsScope())
    {
        IfFailRet(GetStackVariables(ref.frameId, pThread, start, count, variables));
    }
    else
    {
        IfFailRet(GetChildren(ref, pThread, start, count, variables));
    }
    return S_OK;
}

HRESULT Variables::AddVariableReference(Variable &variable, FrameId frameId, ICorDebugValue *pValue, ValueKind valueKind)
{
    std::lock_guard<std::recursive_mutex> lock(m_referencesMutex);

    if (m_references.size() == std::numeric_limits<uint32_t>::max())
        return E_FAIL;

    int numChild = 0;
    GetNumChild(pValue, numChild, valueKind == ValueIsClass);
    if (numChild == 0)
        return S_OK;

    variable.namedVariables = numChild;
    variable.variablesReference = (uint32_t)m_references.size() + 1;
    pValue->AddRef();
    VariableReference variableReference(variable, frameId, pValue, valueKind);
    m_references.emplace(std::make_pair(variable.variablesReference, std::move(variableReference)));

    return S_OK;
}

static HRESULT GetModuleName(ICorDebugThread *pThread, std::string &module)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*)&pMDImport));

    WCHAR mdName[mdNameLen];
    ULONG nameLen;
    IfFailRet(pMDImport->GetScopeProps(mdName, _countof(mdName), &nameLen, nullptr));
    module = to_utf8(mdName);

    return S_OK;
}

HRESULT Variables::GetExceptionVariable(
    FrameId frameId,
    ICorDebugThread *pThread,
    Variable &var)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
    {
        var.name = "$exception";
        var.evaluateName = var.name;

        bool escape = true;
        PrintValue(pExceptionValue, var.value, escape);
        TypePrinter::GetTypeOfValue(pExceptionValue, var.type);

        // AddVariableReference is re-interable function.
        IfFailRet(AddVariableReference(var, frameId, pExceptionValue, ValueIsVariable));

        string excModule;
        IfFailRet(GetModuleName(pThread, excModule));
        var.module = excModule;

        return S_OK;
    }

    return E_FAIL;
}

HRESULT Variables::GetStackVariables(
    FrameId frameId,
    ICorDebugThread *pThread,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    HRESULT Status;
    int currentIndex = -1;
    Variable var;
    if (SUCCEEDED(GetExceptionVariable(frameId, pThread, var))) {
        variables.push_back(var);
        ++currentIndex;
    }

    if (FAILED(Status = m_sharedEvaluator->WalkStackVars(pThread, frameId.getLevel(),
        [&](const std::string &name, Evaluator::GetValueCallback getValue) -> HRESULT
    {
        ++currentIndex;

        if (currentIndex < start)
            return S_OK;
        if (count != 0 && currentIndex >= start + count)
            return E_ABORT; // Fast exit from cycle.

        Variable var;
        var.name = name;
        var.evaluateName = var.name;
        bool escape = true;
        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(getValue(&iCorValue, var.evalFlags));
        PrintValue(iCorValue, var.value, escape);
        TypePrinter::GetTypeOfValue(iCorValue, var.type);
        IfFailRet(AddVariableReference(var, frameId, iCorValue, ValueIsVariable));
        variables.push_back(var);
        return S_OK;
    })) && Status != E_ABORT)
    {
        return Status;
    }

    return S_OK;
}

HRESULT Variables::GetScopes(ICorDebugProcess *pProcess, FrameId frameId, std::vector<Scope> &scopes)
{
    if (pProcess == nullptr)
        return E_FAIL;

    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));
    int namedVariables = 0;
    uint32_t variablesReference = 0;

    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
        namedVariables++;

    IfFailRet(m_sharedEvaluator->WalkStackVars(pThread, frameId.getLevel(),
        [&](const std::string &name, Evaluator::GetValueCallback) -> HRESULT
    {
        namedVariables++;
        return S_OK;
    }));

    if (namedVariables > 0)
    {
        std::lock_guard<std::recursive_mutex> lock(m_referencesMutex);

        if (m_references.size() == std::numeric_limits<uint32_t>::max())
            return E_FAIL;

        variablesReference = (uint32_t)m_references.size() + 1;
        VariableReference scopeReference(variablesReference, frameId, namedVariables);
        m_references.emplace(std::make_pair(variablesReference, std::move(scopeReference)));
    }

    scopes.emplace_back(variablesReference, "Locals", namedVariables);

    return S_OK;
}

void Variables::FixupInheritedFieldNames(std::vector<Member> &members)
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

HRESULT Variables::GetChildren(
    VariableReference &ref,
    ICorDebugThread *pThread,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    if (ref.IsScope())
        return E_INVALIDARG;

    if (!ref.iCorValue)
        return S_OK;

    HRESULT Status;
    std::vector<Member> members;
    bool hasStaticMembers = false;

    IfFailRet(FetchFieldsAndProperties(ref.iCorValue,
                                       pThread,
                                       ref.frameId.getLevel(),
                                       members,
                                       ref.valueKind == ValueIsClass,
                                       hasStaticMembers,
                                       start,
                                       count == 0 ? INT_MAX : start + count,
                                       ref.evalFlags));

    FixupInheritedFieldNames(members);

    for (auto &it : members)
    {
        Variable var(ref.evalFlags);
        var.name = it.name;
        bool isIndex = !it.name.empty() && it.name.at(0) == '[';
        if (var.name.find('(') == std::string::npos) // expression evaluator does not support typecasts
            var.evaluateName = ref.evaluateName + (isIndex ? "" : ".") + var.name;
        FillValueAndType(it, var);
        IfFailRet(AddVariableReference(var, ref.frameId, it.value, ValueIsVariable));
        variables.push_back(var);
    }

    if (ref.valueKind == ValueIsVariable && hasStaticMembers)
    {
        bool staticsInRange = start < ref.namedVariables && (count == 0 || start + count >= ref.namedVariables);
        if (staticsInRange)
        {
            ToRelease<ICorDebugValue2> pValue2;
            IfFailRet(ref.iCorValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
            ToRelease<ICorDebugType> pType;
            IfFailRet(pValue2->GetExactType(&pType));
            // Note, this call could return S_FALSE without ICorDebugValue creation in case type don't have static members.
            IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, nullptr, false));

            Variable var(ref.evalFlags);
            var.name = "Static members";
            TypePrinter::GetTypeOfValue(ref.iCorValue, var.evaluateName); // do not expose type for this fake variable

            IfFailRet(AddVariableReference(var, ref.frameId, ref.iCorValue, ValueIsClass));
            variables.push_back(var);
        }
    }

    return S_OK;
}

HRESULT Variables::Evaluate(
    ICorDebugProcess *pProcess,
    FrameId frameId,
    const std::string &expression,
    Variable &variable,
    std::string &output)
{
    if (pProcess == nullptr)
        return E_FAIL;

    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    FrameLevel frameLevel = frameId.getLevel();

    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));
    ToRelease<ICorDebugValue> pResultValue;

    static std::regex re("^ *(global::)?[A-Za-z\\$_][A-Za-z0-9_]* *(\\[ *\\d+ *(, *\\d+)* *\\])?(( *\\. *[A-Za-z_][A-Za-z0-9_]*)+( *\\[ *\\d+( *, *\\d+)* *\\])?)* *$");

    if (std::regex_match(expression, re))
    {
        // Use simple name parser
        // Note, in case of fail we don't call Roslyn, since it will use simple eval check with same `expression`,
        // but in case we miss something with `regex_match`, Roslyn will use simple eval check from managed part.
        IfFailRet(m_sharedEvaluator->EvalExpr(pThread, frameLevel, expression, &pResultValue, variable.evalFlags));
    }

    int typeId;

    // Use Roslyn for expression evaluation
    if (!pResultValue)
    {
    IfFailRet(Interop::EvalExpression(
        expression, output, &typeId, &pResultValue,
        [&](void *corValue, const std::string &name, int *typeId, void **data) -> bool
    {
        ToRelease<ICorDebugValue> pThisValue;

        if (!corValue) // Scope
        {
            bool found = false;
            if (FAILED(Status = m_sharedEvaluator->WalkStackVars(pThread, frameLevel,
                [&](const std::string &varName, Evaluator::GetValueCallback getValue) -> HRESULT
            {
                if (varName == "this")
                {
                    if (!pThisValue)
                        getValue(&pThisValue, variable.evalFlags);
                }
                if (!found && varName == name)
                {
                    found = true;
                    ToRelease<ICorDebugValue> iCorValue;
                    IfFailRet(getValue(&iCorValue, variable.evalFlags));
                    IfFailRet(MarshalValue(iCorValue, typeId, data));
                    return E_ABORT; // Fast way to exit from stack vars walk routine.
                }

                return S_OK;
            })) && Status != E_ABORT)
            {
                return false;
            }
            if (found)
                return true;
            if (!pThisValue)
                return false;

            corValue = pThisValue;
        }

        std::vector<Member> members;

        const bool fetchOnlyStatic = false;
        bool hasStaticMembers = false;

        ICorDebugValue *pValue = static_cast<ICorDebugValue*>(corValue);

        if (FAILED(FetchFieldsAndProperties(pValue, pThread, frameLevel, members, fetchOnlyStatic,
                                            hasStaticMembers, 0, INT_MAX, variable.evalFlags)))
            return false;

        FixupInheritedFieldNames(members);

        auto memberIt = std::find_if(members.begin(), members.end(), [&name](const Member &m){ return m.name == name; });
        if (memberIt == members.end())
            return false;

        if (!memberIt->value)
            return false;

        if (FAILED(MarshalValue(memberIt->value, typeId, data)))
        {
            return false;
        }

        return true;
    }));
    }

    variable.evaluateName = expression;

    if (pResultValue)
    {
        const bool escape = true;
        PrintValue(pResultValue, variable.value, escape);
        TypePrinter::GetTypeOfValue(pResultValue, variable.type);
    }
    else
    {
        PrintBasicValue(typeId, output, variable.type, variable.value);
    }
    IfFailRet(AddVariableReference(variable, frameId, pResultValue, ValueIsVariable));

    return S_OK;
}

HRESULT Variables::SetVariable(
    ICorDebugProcess *pProcess,
    const std::string &name,
    const std::string &value,
    uint32_t ref,
    std::string &output)
{
    if (pProcess == nullptr)
        return E_FAIL;

    std::lock_guard<std::recursive_mutex> lock(m_referencesMutex);

    auto it = m_references.find(ref);
    if (it == m_references.end())
        return E_FAIL;

    VariableReference &varRef = it->second;
    HRESULT Status;

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(varRef.frameId.getThread()), &pThread));

    if (varRef.IsScope())
    {
        IfFailRet(SetStackVariable(varRef, pThread, name, value, output));
    }
    else
    {
        IfFailRet(SetChild(varRef, pThread, name, value, output));
    }

    return S_OK;
}

HRESULT Variables::SetStackVariable(
    VariableReference &ref,
    ICorDebugThread *pThread,
    const std::string &name,
    const std::string &value,
    std::string &output)
{
    HRESULT Status;

    // TODO Exception?

    if (FAILED(Status = m_sharedEvaluator->WalkStackVars(pThread, ref.frameId.getLevel(),
        [&](const std::string &varName, Evaluator::GetValueCallback getValue) -> HRESULT
    {
        if (varName != name)
            return S_OK;

        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(getValue(&iCorValue, ref.evalFlags));
        IfFailRet(m_sharedEvaluator->SetValue(iCorValue, value, pThread, output));
        bool escape = true;
        PrintValue(iCorValue, output, escape);
        return E_ABORT; // Fast exit from cycle.
    })) && Status != E_ABORT)
    {
        return Status;
    }

    return S_OK;
}

HRESULT Variables::SetChild(
    VariableReference &ref,
    ICorDebugThread *pThread,
    const std::string &name,
    const std::string &value,
    std::string &output)
{
    if (ref.IsScope())
        return E_INVALIDARG;

    if (!ref.iCorValue)
        return S_OK;

    HRESULT Status;

    IfFailRet(m_sharedEvaluator->WalkMembers(ref.iCorValue, pThread, ref.frameId.getLevel(), [&](
        ICorDebugType*,
        bool is_static,
        const std::string &varName,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetValueCallback setValue) -> HRESULT
    {
        if (varName == name)
        {
            IfFailRet(setValue(value, output, ref.evalFlags));
            ToRelease<ICorDebugValue> iCorValue;
            IfFailRet(getValue(&iCorValue, ref.evalFlags));
            bool escape = true;
            PrintValue(iCorValue, output, escape);
        }
        return S_OK;
    }));

    return S_OK;
}

HRESULT Variables::GetValueByExpression(ICorDebugProcess *pProcess, FrameId frameId,
                                        const Variable &variable, ICorDebugValue **ppResult)
{
    if (pProcess == nullptr)
        return E_FAIL;

    HRESULT Status;

    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));

    return m_sharedEvaluator->EvalExpr(pThread, frameId.getLevel(), variable.evaluateName, ppResult, variable.evalFlags);
}

HRESULT Variables::SetVariable(
    ICorDebugProcess *pProcess,
    ICorDebugValue *pVariable,
    const std::string &value,
    FrameId frameId,
    std::string &output)
{
    HRESULT Status;

    if (pProcess == nullptr)
        return E_FAIL;

    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));

    IfFailRet(m_sharedEvaluator->SetValue(pVariable, value, pThread, output));
    bool escape = true;
    PrintValue(pVariable, output, escape);
    return S_OK;
}

} // namespace netcoredbg
