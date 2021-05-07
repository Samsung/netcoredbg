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

static HRESULT WriteValue(
    ICorDebugValue *pValue,
    const std::string &value,
    ICorDebugThread *pThread,
    std::shared_ptr<Evaluator> &sharedEvaluator,
    std::string &errorText)
{
    HRESULT Status;

    ULONG32 size;
    IfFailRet(pValue->GetSize(&size));

    std::string data;

    CorElementType corType;
    IfFailRet(pValue->GetType(&corType));

    static const std::unordered_map<int, std::string> cor2name = {
        {ELEMENT_TYPE_BOOLEAN, "System.Boolean"},
        {ELEMENT_TYPE_U1,      "System.Byte"},
        {ELEMENT_TYPE_I1,      "System.SByte"},
        {ELEMENT_TYPE_CHAR,    "System.Char"},
        {ELEMENT_TYPE_R8,      "System.Double"},
        {ELEMENT_TYPE_R4,      "System.Single"},
        {ELEMENT_TYPE_I4,      "System.Int32"},
        {ELEMENT_TYPE_U4,      "System.UInt32"},
        {ELEMENT_TYPE_I8,      "System.Int64"},
        {ELEMENT_TYPE_U8,      "System.UInt64"},
        {ELEMENT_TYPE_I2,      "System.Int16"},
        {ELEMENT_TYPE_U2,      "System.UInt16"},
        {ELEMENT_TYPE_I,       "System.IntPtr"},
        {ELEMENT_TYPE_U,       "System.UIntPtr"}
    };
    auto renamed = cor2name.find(corType);
    if (renamed != cor2name.end())
    {
        IfFailRet(Interop::ParseExpression(value, renamed->second, data, errorText));
    }
    else if (corType == ELEMENT_TYPE_STRING)
    {
        IfFailRet(Interop::ParseExpression(value, "System.String", data, errorText));

        ToRelease<ICorDebugValue> pNewString;
        IfFailRet(sharedEvaluator->CreateString(pThread, data, &pNewString));

        // Switch object addresses
        ToRelease<ICorDebugReferenceValue> pRefNew;
        IfFailRet(pNewString->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefNew));
        ToRelease<ICorDebugReferenceValue> pRefOld;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefOld));

        CORDB_ADDRESS addr;
        IfFailRet(pRefNew->GetValue(&addr));
        IfFailRet(pRefOld->SetValue(addr));

        return S_OK;
    }
    else if (corType == ELEMENT_TYPE_VALUETYPE || corType == ELEMENT_TYPE_CLASS)
    {
        std::string typeName;
        TypePrinter::GetTypeOfValue(pValue, typeName);
        if (typeName != "decimal")
        {
            errorText = "Unable to set value of type '" + typeName + "'";
            return E_FAIL;
        }
        IfFailRet(Interop::ParseExpression(value, "System.Decimal", data, errorText));
    }
    else
    {
        errorText = "Unable to set value";
        return E_FAIL;
    }

    if (size != data.size())
    {
        errorText = "Marshalling size mismatch: " + std::to_string(size) + " != " + std::to_string(data.size());
        return E_FAIL;
    }

    ToRelease<ICorDebugGenericValue> pGenValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &pGenValue));
    IfFailRet(pGenValue->SetValue((LPVOID) &data[0]));

    return S_OK;
}

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
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *,
        bool is_static,
        const std::string &)
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
    Member(const std::string &name, const std::string& ownerType, ToRelease<ICorDebugValue> value) :
        name(name),
        ownerType(ownerType),
        value(std::move(value))
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

        string className;
        if (pType)
            TypePrinter::GetTypeOfValue(pType, className);

        ToRelease<ICorDebugValue> pResultValue;

        if (mdGetter != mdMethodDefNil)
        {
            ToRelease<ICorDebugFunction> pFunc;
            if (SUCCEEDED(pModule->GetFunctionFromToken(mdGetter, &pFunc)))
            {
                if (is_static)
                    m_sharedEvaluator->EvalFunction(pThread, pFunc, &pType, 1, nullptr, 0, &pResultValue, evalFlags);
                else
                    m_sharedEvaluator->EvalFunction(pThread, pFunc, &pType, 1, &pInputValue, 1, &pResultValue, evalFlags);
            }
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

    IfFailRet(m_sharedEvaluator->WalkStackVars(pThread, frameId.getLevel(), [&](ICorDebugValue *pValue,
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
        IfFailRet(AddVariableReference(var, frameId, pValue, ValueIsVariable));
        variables.push_back(var);
        return S_OK;
    }));

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

    IfFailRet(m_sharedEvaluator->WalkStackVars(pThread, frameId.getLevel(), [&](ICorDebugValue *pValue,
                                                                                const std::string &name) -> HRESULT
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
            IfFailRet(m_sharedEvaluator->CreatTypeObjectStaticConstructor(pThread, pType, nullptr, false));

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
            if (FAILED(m_sharedEvaluator->WalkStackVars(pThread, frameLevel, [&](ICorDebugValue *pValue,
                                                                                 const std::string &varName) -> HRESULT
            {
                if (!found && varName == "this")
                {
                    pValue->AddRef();
                    pThisValue = pValue;
                }
                if (!found && varName == name)
                {
                    found = true;
                    IfFailRet(MarshalValue(pValue, typeId, data));
                }

                return S_OK;
            })))
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
        IfFailRet(SetStackVariable(varRef.frameId, pThread, name, value, output));
    }
    else
    {
        IfFailRet(SetChild(varRef, pThread, name, value, output));
    }

    return S_OK;
}

HRESULT Variables::SetStackVariable(
    FrameId frameId,
    ICorDebugThread *pThread,
    const std::string &name,
    const std::string &value,
    std::string &output)
{
    HRESULT Status;

    // TODO Exception?

    IfFailRet(m_sharedEvaluator->WalkStackVars(pThread, frameId.getLevel(), [&](ICorDebugValue *pValue,
                                                                                const std::string &varName) -> HRESULT
    {
        if (varName == name)
        {
            IfFailRet(WriteValue(pValue, value, pThread, m_sharedEvaluator, output));
            bool escape = true;
            PrintValue(pValue, output, escape);
        }

        return S_OK;
    }));

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
        mdMethodDef mdGetter,
        ICorDebugModule *pModule,
        ICorDebugType *pType,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &varName) -> HRESULT
    {
        if (varName == name)
        {
            IfFailRet(WriteValue(pValue, value, pThread, m_sharedEvaluator, output));
            bool escape = true;
            PrintValue(pValue, output, escape);
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

    IfFailRet(WriteValue(pVariable, value, pThread, m_sharedEvaluator, output));
    bool escape = true;
    PrintValue(pVariable, output, escape);
    return S_OK;
}

} // namespace netcoredbg
