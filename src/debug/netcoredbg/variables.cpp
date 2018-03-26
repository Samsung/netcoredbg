// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "manageddebugger.h"

#include <unordered_set>
#include <vector>

#include "typeprinter.h"
#include "valueprint.h"
#include "frames.h"


HRESULT Variables::GetNumChild(
    ICorDebugValue *pValue,
    unsigned int &numchild,
    bool static_members)
{
    HRESULT Status = S_OK;
    numchild = 0;

    ULONG numstatic = 0;
    ULONG numinstance = 0;

    if (pValue == nullptr)
        return 0;

    IfFailRet(m_evaluator.WalkMembers(pValue, nullptr, nullptr, [&numstatic, &numinstance](
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

struct Variables::Member
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
    Member(const Member &that) = delete;
};

HRESULT Variables::FetchFieldsAndProperties(
    ICorDebugValue *pInputValue,
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

    IfFailRet(m_evaluator.WalkMembers(pInputValue, pThread, pILFrame, [&](
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
                m_evaluator.EvalFunction(pThread, pFunc, pType, is_static ? nullptr : pInputValue, &pResultValue);
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

int ManagedDebugger::GetNamedVariables(uint32_t variablesReference)
{
    return m_variables.GetNamedVariables(variablesReference);
}

int Variables::GetNamedVariables(uint32_t variablesReference)
{
    auto it = m_variables.find(variablesReference);
    if (it == m_variables.end())
        return 0;
    return it->second.namedVariables;
}

HRESULT ManagedDebugger::GetVariables(
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    return m_variables.GetVariables(m_pProcess, variablesReference, filter, start, count, variables);
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

    auto it = m_variables.find(variablesReference);
    if (it == m_variables.end())
        return E_FAIL;

    VariableReference &ref = it->second;

    HRESULT Status;

    StackFrame stackFrame(ref.frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
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

void Variables::AddVariableReference(Variable &variable, uint64_t frameId, ICorDebugValue *value, ValueKind valueKind)
{
    HRESULT Status;
    unsigned int numChild = 0;
    GetNumChild(value, numChild, valueKind == ValueIsClass);
    if (numChild == 0)
        return;

    variable.namedVariables = numChild;
    variable.variablesReference = m_nextVariableReference++;
    value->AddRef();
    VariableReference variableReference(variable, frameId, value, valueKind);
    m_variables.emplace(std::make_pair(variable.variablesReference, std::move(variableReference)));
}

HRESULT Variables::GetStackVariables(
    uint64_t frameId,
    ICorDebugThread *pThread,
    ICorDebugFrame *pFrame,
    int start,
    int count,
    std::vector<Variable> &variables)
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
            AddVariableReference(var, frameId, pExceptionValue, ValueIsVariable);
            variables.push_back(var);
        }
    }

    IfFailRet(m_evaluator.WalkStackVars(pFrame, [&](
        ICorDebugILFrame *pILFrame,
        ICorDebugValue *pValue,
        const std::string &name) -> HRESULT
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
        AddVariableReference(var, frameId, pValue, ValueIsVariable);
        variables.push_back(var);
        return S_OK;
    }));

    return S_OK;
}

HRESULT ManagedDebugger::GetScopes(uint64_t frameId, std::vector<Scope> &scopes)
{
    return m_variables.GetScopes(m_pProcess, frameId, scopes);
}

HRESULT Variables::GetScopes(ICorDebugProcess *pProcess, uint64_t frameId, std::vector<Scope> &scopes)
{
    if (pProcess == nullptr)
        return E_FAIL;

    HRESULT Status;

    StackFrame stackFrame(frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, stackFrame.GetLevel(), &pFrame));

    int namedVariables = 0;
    uint32_t variablesReference = 0;

    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
        namedVariables++;

    IfFailRet(m_evaluator.WalkStackVars(pFrame, [&](
        ICorDebugILFrame *pILFrame,
        ICorDebugValue *pValue,
        const std::string &name) -> HRESULT
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
                                       ref.valueKind == ValueIsClass,
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
        if (it.value == nullptr)
        {
            var.value = "<error>";
        }
        else
        {
            PrintValue(it.value, var.value, escape);
            TypePrinter::GetTypeOfValue(it.value, var.type);
        }
        AddVariableReference(var, ref.frameId, it.value, ValueIsVariable);
        variables.push_back(var);
    }

    if (ref.valueKind == ValueIsVariable && hasStaticMembers)
    {
        bool staticsInRange = start < ref.namedVariables && (count == 0 || start + count >= ref.namedVariables);
        if (staticsInRange)
        {
            m_evaluator.RunClassConstructor(pThread, ref.value);

            Variable var;
            var.name = "Static members";
            TypePrinter::GetTypeOfValue(ref.value, var.evaluateName); // do not expose type for this fake variable
            AddVariableReference(var, ref.frameId, ref.value, ValueIsClass);
            variables.push_back(var);
        }
    }

    return S_OK;
}

HRESULT ManagedDebugger::Evaluate(uint64_t frameId, const std::string &expression, Variable &variable)
{
    return m_variables.Evaluate(m_pProcess, frameId, expression, variable);
}

HRESULT Variables::Evaluate(
    ICorDebugProcess *pProcess,
    uint64_t frameId,
    const std::string &expression,
    Variable &variable)
{
    if (pProcess == nullptr)
        return E_FAIL;

    HRESULT Status;

    StackFrame stackFrame(frameId);
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(stackFrame.GetThreadId(), &pThread));
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, stackFrame.GetLevel(), &pFrame));

    ToRelease<ICorDebugValue> pResultValue;
    IfFailRet(m_evaluator.EvalExpr(pThread, pFrame, expression, &pResultValue));

    variable.evaluateName = expression;

    bool escape = true;
    PrintValue(pResultValue, variable.value, escape);
    TypePrinter::GetTypeOfValue(pResultValue, variable.type);
    AddVariableReference(variable, frameId, pResultValue, ValueIsVariable);

    return S_OK;
}
