// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <regex>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <cctype>

#include "metadata/typeprinter.h"
#include "metadata/attributes.h"
#include "valueprint.h"
#include "debugger/variables.h"
#include "debugger/evalhelpers.h"
#include "debugger/evaluator.h"
#include "debugger/frames.h"
#include "debugger/evalstackmachine.h"
#include "managed/interop.h"
#include "utils/logger.h"
#include "utils/utf.h"
#include "interfaces/types.h"

namespace netcoredbg
{

static void GetNumChild(Evaluator *pEvaluator, ICorDebugValue *pValue, int &numChild, bool static_members)
{
    numChild = 0;

    if (pValue == nullptr)
        return;

    int numStatic = 0;
    int numInstance = 0;
    // No thread and FrameLevel{0} here, since we need only count children.
    if (FAILED(pEvaluator->WalkMembers(pValue, nullptr, FrameLevel{0}, false, [&numStatic, &numInstance](
        ICorDebugType *,
        bool is_static,
        const std::string &,
        Evaluator::GetValueCallback,
        Evaluator::SetterData*)
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

struct VariableMember
{
    std::string name;
    std::string ownerType;
    ToRelease<ICorDebugValue> value;
    VariableMember(const std::string &name, const std::string& ownerType, ICorDebugValue *pValue) :
        name(name),
        ownerType(ownerType),
        value(pValue)
    {}
    VariableMember(VariableMember &&that) = default;
    VariableMember(const VariableMember &that) = delete;
};

static bool IsTypeDisplayFallback(const std::string &value, const std::string &typeName)
{
    return value.size() == typeName.size() + 2 &&
           value.front() == '{' &&
           value.back() == '}' &&
           value.compare(1, typeName.size(), typeName) == 0;
}

static std::string Trim(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;

    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;

    return s.substr(begin, end - begin);
}

static bool IsDebuggerDisplayIdentifier(const std::string &s)
{
    if (s.empty())
        return false;

    size_t start = s[0] == '@' ? 1 : 0;
    if (start == s.size())
        return false;

    unsigned char first = static_cast<unsigned char>(s[start]);
    if (!std::isalpha(first) && s[start] != '_')
        return false;

    for (size_t i = start + 1; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c) && s[i] != '_')
            return false;
    }

    return true;
}

// DebuggerDisplay v1 intentionally supports only simple instance-member placeholders:
//   {Field}, {Property}, {Method()}, {Nested.Member}, {this.Member}
// plus the ",nq" string-display modifier handled below. Richer C# expressions
// (method arguments, operators, indexers) cause whole-template fallback to {TypeName}.
static bool IsSupportedDebuggerDisplayExpression(const std::string &expression)
{
    if (expression == "this")
        return true;

    size_t begin = 0;
    bool sawSegment = false;
    while (begin < expression.size())
    {
        size_t end = expression.find('.', begin);
        std::string segment = expression.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        bool isLast = end == std::string::npos;

        bool isCall = false;
        if (segment.size() > 2 && segment.compare(segment.size() - 2, 2, "()") == 0)
        {
            if (!isLast)
                return false;

            isCall = true;
            segment.resize(segment.size() - 2);
        }

        if (!IsDebuggerDisplayIdentifier(segment))
            return false;

        sawSegment = true;
        if (isCall)
            return true;

        if (isLast)
            break;

        begin = end + 1;
    }

    return sawSegment;
}

static bool ShouldTryDebuggerDisplay(ICorDebugValue *pValue)
{
    CorElementType corElemType;
    if (FAILED(pValue->GetType(&corElemType)))
        return false;

    return corElemType == ELEMENT_TYPE_CLASS ||
           corElemType == ELEMENT_TYPE_OBJECT ||
           corElemType == ELEMENT_TYPE_VALUETYPE;
}

// Walk the exact type and its base types for a DebuggerDisplayAttribute and provide its Value
// (display template). Note, metadata import must be requested from the module of each type in
// the walk, since a base type could come from another module.
static bool TryGetDebuggerDisplayTemplate(ICorDebugValue *pInputValue, std::string &displayTemplate)
{
    BOOL isNull = TRUE;
    ToRelease<ICorDebugValue> pValue;
    if (FAILED(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull)) || isNull || !pValue)
        return false;

    ToRelease<ICorDebugValue2> pValue2;
    if (FAILED(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2)))
        return false;

    ToRelease<ICorDebugType> pType;
    if (FAILED(pValue2->GetExactType(&pType)) || !pType)
        return false;

    while (pType)
    {
        CorElementType corElemType;
        if (FAILED(pType->GetType(&corElemType)))
            return false;

        if (corElemType != ELEMENT_TYPE_CLASS && corElemType != ELEMENT_TYPE_VALUETYPE)
            return false;

        std::string typeName;
        if (FAILED(TypePrinter::GetTypeOfValue(pType, typeName)) ||
            typeName == "System.Object" || typeName == "System.ValueType")
            return false;

        ToRelease<ICorDebugClass> pClass;
        if (FAILED(pType->GetClass(&pClass)) || !pClass)
            return false;

        ToRelease<ICorDebugModule> pModule;
        mdTypeDef typeDef = mdTypeDefNil;
        if (FAILED(pClass->GetModule(&pModule)) || FAILED(pClass->GetToken(&typeDef)))
            return false;

        ToRelease<IUnknown> pMDUnknown;
        ToRelease<IMetaDataImport> pMD;
        if (FAILED(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown)) ||
            FAILED(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD)))
            return false;

        if (GetAttributeFixedStringArgument(pMD, typeDef, DebuggerAttribute::Display, displayTemplate))
            return true;

        ToRelease<ICorDebugType> pBaseType;
        if (FAILED(pType->GetBase(&pBaseType)) || !pBaseType)
            return false;

        pType = pBaseType.Detach();
    }

    return false;
}

static bool IsZeroArgStringToString(IMetaDataImport *pMD, mdTypeDef typeDef, mdMethodDef methodDef)
{
    mdTypeDef memTypeDef;
    ULONG nameLen;
    WCHAR methodName[mdNameLen] = {0};
    DWORD methodAttr = 0;
    PCCOR_SIGNATURE pSig = nullptr;
    ULONG cbSig = 0;
    if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef,
                                   methodName, _countof(methodName), &nameLen,
                                   &methodAttr, &pSig, &cbSig, nullptr, nullptr)))
        return false;

    if (memTypeDef != typeDef || (methodAttr & mdStatic) || !(methodAttr & mdVirtual) ||
        (methodAttr & mdNewSlot) || !str_equal(methodName, W("ToString")))
        return false;

    ULONG convFlags = 0;
    ULONG elementSize = CorSigUncompressData(pSig, &convFlags);
    pSig += elementSize;

    if ((convFlags & IMAGE_CEE_CS_CALLCONV_MASK) == IMAGE_CEE_CS_CALLCONV_VARARG)
        return false;

    if (convFlags & IMAGE_CEE_CS_CALLCONV_GENERIC)
    {
        ULONG gParams = 0;
        elementSize = CorSigUncompressData(pSig, &gParams);
        pSig += elementSize;
    }

    ULONG cParams = 0;
    elementSize = CorSigUncompressData(pSig, &cParams);
    pSig += elementSize;

    CorElementType returnType = CorSigUncompressElementType(pSig);
    return cParams == 0 && returnType == ELEMENT_TYPE_STRING;
}

static bool FindDeclaredToStringOverride(IMetaDataImport *pMD, mdTypeDef typeDef, mdMethodDef &foundMethodDef)
{
    ULONG numMethods = 0;
    HCORENUM hEnum = nullptr;
    mdMethodDef methodDef = mdMethodDefNil;
    while (SUCCEEDED(pMD->EnumMethodsWithName(&hEnum, typeDef, W("ToString"), &methodDef, 1, &numMethods)) &&
           numMethods != 0)
    {
        if (IsZeroArgStringToString(pMD, typeDef, methodDef))
        {
            pMD->CloseEnum(hEnum);
            foundMethodDef = methodDef;
            return true;
        }
    }
    pMD->CloseEnum(hEnum);
    return false;
}

// Walk the exact type and its base types for a ToString() override and provide the override itself
// plus its declaring type, so the caller could call it directly on the value it already holds.
static bool FindOverriddenToString(ICorDebugValue *pInputValue, ICorDebugType **ppDeclaringType, ICorDebugFunction **ppFunc)
{
    BOOL isNull = TRUE;
    ToRelease<ICorDebugValue> pValue;
    if (FAILED(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull)))
        return false;
    if (isNull)
        return false;

    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    if (FAILED(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID*) &pValue2)) ||
        FAILED(pValue2->GetExactType(&pType)))
        return false;

    while (pType)
    {
        std::string typeName;
        if (FAILED(TypePrinter::GetTypeOfValue(pType, typeName)))
            return false;
        // Exception.ToString() includes stack traces; keep existing exception variable display.
        if (typeName == "System.Object" || typeName == "System.ValueType" || typeName == "System.Exception")
            return false;

        ToRelease<ICorDebugClass> pClass;
        mdTypeDef typeDef = mdTypeDefNil;
        ToRelease<ICorDebugModule> pModule;
        ToRelease<IUnknown> pMDUnknown;
        ToRelease<IMetaDataImport> pMD;
        if (FAILED(pType->GetClass(&pClass)) ||
            FAILED(pClass->GetToken(&typeDef)) ||
            FAILED(pClass->GetModule(&pModule)) ||
            FAILED(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown)) ||
            FAILED(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD)))
        {
            return false;
        }

        mdMethodDef methodDef = mdMethodDefNil;
        if (FindDeclaredToStringOverride(pMD, typeDef, methodDef))
        {
            if (FAILED(pModule->GetFunctionFromToken(methodDef, ppFunc)))
                return false;
            *ppDeclaringType = pType.Detach();
            return true;
        }

        ToRelease<ICorDebugType> pBaseType;
        if (FAILED(pType->GetBase(&pBaseType)) || pBaseType == nullptr)
            return false;
        pType = pBaseType.Detach();
    }

    return false;
}

static HRESULT CallInstanceZeroArgMethod(Evaluator *pEvaluator, EvalHelpers *pEvalHelpers, ICorDebugThread *pThread,
                                         int evalFlags, ICorDebugValue *pReceiver, const std::string &methodName,
                                         ICorDebugValue **ppResultValue)
{
    HRESULT Status;
    ToRelease<ICorDebugFunction> iCorFunc;
    pEvaluator->WalkMethods(pReceiver, [&](
        bool is_static,
        const std::string &name,
        Evaluator::ReturnElementType&,
        std::vector<Evaluator::ArgElementType> &methodArgs,
        Evaluator::GetFunctionCallback getFunction)
    {
        if (is_static || !methodArgs.empty() || name != methodName)
            return S_OK; // Return with success to continue walk.

        IfFailRet(getFunction(&iCorFunc));
        return E_ABORT; // Fast exit from cycle, since we already found iCorFunc.
    });
    if (!iCorFunc)
        return E_FAIL;

    ToRelease<ICorDebugValue2> iCorValue2;
    IfFailRet(pReceiver->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
    ToRelease<ICorDebugType> iCorType;
    IfFailRet(iCorValue2->GetExactType(&iCorType));

    return pEvalHelpers->EvalFunction(pThread, iCorFunc, iCorType.GetRef(), 1, &pReceiver, 1, ppResultValue, evalFlags);
}

// Evaluate one display template placeholder against the displayed value itself (not the current
// frame), so locals, watch results and child variables all resolve members of the right object.
static bool TryEvaluateDebuggerDisplayExpression(Evaluator *pEvaluator, EvalHelpers *pEvalHelpers,
                                                 ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags,
                                                 ICorDebugValue *pReceiver, const std::string &placeholder,
                                                 std::string &value)
{
    std::string expression = Trim(placeholder);
    if (expression.empty())
        return false;

    bool noQuotes = false;
    size_t comma = expression.find(',');
    if (comma != std::string::npos)
    {
        std::string format = Trim(expression.substr(comma + 1));
        expression = Trim(expression.substr(0, comma));
        if (format != "nq")
            return false;
        noQuotes = true;
    }

    if (expression.empty() || !IsSupportedDebuggerDisplayExpression(expression))
        return false;

    // Note, placeholder results are printed with plain PrintValue() (no nested DebuggerDisplay
    // or ToString() processing), so recursive display evaluation is not possible.
    if (expression == "this")
        return SUCCEEDED(PrintValue(pReceiver, value, !noQuotes));
    if (expression.compare(0, 5, "this.") == 0)
        expression = expression.substr(5);

    bool isCall = expression.size() > 2 && expression.compare(expression.size() - 2, 2, "()") == 0;
    if (isCall)
        expression.resize(expression.size() - 2);

    std::vector<std::string> identifiers;
    size_t begin = 0;
    while (begin <= expression.size())
    {
        size_t end = expression.find('.', begin);
        identifiers.emplace_back(expression.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }

    // Note, ResolveIdentifiers() and EvalFunction() could return S_OK without result, for example
    // in case of eval flags `EVAL_NOFUNCEVAL` and property getter or method call. Fields resolve
    // without function evaluation.
    ToRelease<ICorDebugValue> pResultValue;
    if (isCall)
    {
        std::string methodName = identifiers.back();
        identifiers.pop_back();

        ToRelease<ICorDebugValue> pMethodReceiver;
        if (identifiers.empty())
        {
            pReceiver->AddRef();
            pMethodReceiver = pReceiver;
        }
        else if (FAILED(pEvaluator->ResolveIdentifiers(pThread, frameLevel, pReceiver, nullptr, identifiers,
                                                       &pMethodReceiver, nullptr, nullptr, evalFlags)) ||
                 pMethodReceiver == nullptr)
        {
            return false;
        }

        if (FAILED(CallInstanceZeroArgMethod(pEvaluator, pEvalHelpers, pThread, evalFlags, pMethodReceiver,
                                             methodName, &pResultValue)) ||
            pResultValue == nullptr)
        {
            return false;
        }
    }
    else if (FAILED(pEvaluator->ResolveIdentifiers(pThread, frameLevel, pReceiver, nullptr, identifiers,
                                                   &pResultValue, nullptr, nullptr, evalFlags)) ||
             pResultValue == nullptr)
    {
        return false;
    }

    return SUCCEEDED(PrintValue(pResultValue, value, !noQuotes));
}

// Build the display string from a DebuggerDisplayAttribute.Value template - literal text with
// {expression} placeholders, "{{"/"}}" escaped braces. Any unsupported or failed placeholder
// discards the whole template (caller keeps the {TypeName} display).
static bool TryEvaluateDebuggerDisplay(Evaluator *pEvaluator, EvalHelpers *pEvalHelpers, ICorDebugThread *pThread,
                                       FrameLevel frameLevel, int evalFlags, ICorDebugValue *pReceiver,
                                       const std::string &displayTemplate, std::string &display)
{
    display.clear();

    for (size_t i = 0; i < displayTemplate.size();)
    {
        char c = displayTemplate[i];
        if (c == '{')
        {
            if (i + 1 < displayTemplate.size() && displayTemplate[i + 1] == '{')
            {
                display.push_back('{');
                i += 2;
                continue;
            }

            size_t close = displayTemplate.find('}', i + 1);
            if (close == std::string::npos)
                return false;

            std::string expressionValue;
            if (!TryEvaluateDebuggerDisplayExpression(pEvaluator, pEvalHelpers, pThread, frameLevel, evalFlags,
                                                      pReceiver, displayTemplate.substr(i + 1, close - i - 1),
                                                      expressionValue))
                return false;

            display += expressionValue;
            i = close + 1;
            continue;
        }

        if (c == '}')
        {
            if (i + 1 < displayTemplate.size() && displayTemplate[i + 1] == '}')
            {
                display.push_back('}');
                i += 2;
                continue;
            }
            return false;
        }

        display.push_back(c);
        ++i;
    }

    return true;
}

static HRESULT PrintFormattedValue(ICorDebugValue *pValue, ICorDebugThread *pThread, FrameLevel frameLevel,
                                   int evalFlags, Evaluator *pEvaluator, EvalHelpers *pEvalHelpers, std::string &output)
{
    HRESULT Status;

    IfFailRet(PrintValue(pValue, output, true));

    if (!pThread || !pEvaluator || !pEvalHelpers)
        return S_OK;

    std::string typeName;
    if (FAILED(TypePrinter::GetTypeOfValue(pValue, typeName)))
        return S_OK;
    if (!IsTypeDisplayFallback(output, typeName))
        return S_OK;

    if (ShouldTryDebuggerDisplay(pValue))
    {
        std::string displayTemplate;
        if (TryGetDebuggerDisplayTemplate(pValue, displayTemplate))
        {
            std::string display;
            if (TryEvaluateDebuggerDisplay(pEvaluator, pEvalHelpers, pThread, frameLevel, evalFlags, pValue,
                                           displayTemplate, display))
                output = display;
            // DebuggerDisplay takes precedence over an overridden ToString(), don't try it on failure.
            return S_OK;
        }
    }

    if (evalFlags & EVAL_NOFUNCEVAL)
        return S_OK;

    ToRelease<ICorDebugType> pDeclaringType;
    ToRelease<ICorDebugFunction> pToStringFunc;
    if (!FindOverriddenToString(pValue, &pDeclaringType, &pToStringFunc))
        return S_OK;

    // Note, EvalFunction() return S_OK without result in case EVAL_NOFUNCEVAL flag.
    ToRelease<ICorDebugValue> pToStringValue;
    if (FAILED(pEvalHelpers->EvalFunction(pThread, pToStringFunc, pDeclaringType.GetRef(), 1, &pValue, 1,
                                          &pToStringValue, evalFlags)) ||
        pToStringValue == nullptr)
        return S_OK;

    std::string display;
    if (SUCCEEDED(PrintValue(pToStringValue, display, false)))
        output = display;

    return S_OK;
}

static HRESULT FillValueAndType(VariableMember &member, Variable &var, ICorDebugThread *pThread, FrameLevel frameLevel,
                                Evaluator *pEvaluator, EvalHelpers *pEvalHelpers)
{
    if (member.value == nullptr)
    {
        // "SUCCEEDED" result, variable found but error during value receive itself.
        // For example, in case of eval flags `EVAL_NOFUNCEVAL` and property.
        var.value = "<error>";
        return S_FALSE;
    }

    TypePrinter::GetTypeOfValue(member.value, var.type);
    return PrintFormattedValue(member.value, pThread, frameLevel, var.evalFlags, pEvaluator, pEvalHelpers, var.value);
}

static HRESULT FetchFieldsAndProperties(Evaluator *pEvaluator, ICorDebugValue *pInputValue, ICorDebugThread *pThread,
                                        FrameLevel frameLevel, std::vector<VariableMember> &members, bool fetchOnlyStatic,
                                        bool &hasStaticMembers, int childStart, int childEnd, int evalFlags)
{
    hasStaticMembers = false;
    HRESULT Status;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));

    int currentIndex = -1;

    IfFailRet(pEvaluator->WalkMembers(pInputValue, pThread, frameLevel, false, [&](
        ICorDebugType *pType,
        bool is_static,
        const std::string &name,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetterData*)
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

        // Note, in this case error is not fatal, but if protocol side need cancel command execution, stop walk and return error to caller.
        ToRelease<ICorDebugValue> iCorResultValue;
        if (getValue(&iCorResultValue, evalFlags) == COR_E_OPERATIONCANCELED)
            return COR_E_OPERATIONCANCELED;

        std::string className;
        if (pType)
            IfFailRet(TypePrinter::GetTypeOfValue(pType, className));

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

// Caller should guarantee, that pProcess is not null.
HRESULT Variables::GetVariables(
    ICorDebugProcess *pProcess,
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
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
    GetNumChild(m_sharedEvaluator.get(), pValue, numChild, valueKind == ValueIsClass);
    if (numChild == 0)
        return S_OK;

    variable.namedVariables = numChild;
    variable.variablesReference = (uint32_t)m_references.size() + 1;
    pValue->AddRef();
    VariableReference variableReference(variable, frameId, pValue, valueKind);
    m_references.emplace(std::make_pair(variable.variablesReference, std::move(variableReference)));

    return S_OK;
}

HRESULT Variables::GetExceptionVariable(FrameId frameId, ICorDebugThread *pThread, Variable &var)
{
    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
    {
        var.name = "$exception";
        var.evaluateName = var.name;

        HRESULT Status;
        IfFailRet(PrintValue(pExceptionValue, var.value));
        IfFailRet(TypePrinter::GetTypeOfValue(pExceptionValue, var.type));

        return AddVariableReference(var, frameId, pExceptionValue, ValueIsVariable);
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
    if (SUCCEEDED(GetExceptionVariable(frameId, pThread, var)))
    {
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
        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(getValue(&iCorValue, var.evalFlags));
        IfFailRet(TypePrinter::GetTypeOfValue(iCorValue, var.type));
        IfFailRet(PrintFormattedValue(iCorValue, pThread, frameId.getLevel(), var.evalFlags,
                                      m_sharedEvaluator.get(), m_sharedEvalHelpers.get(), var.value));

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

static void FixupInheritedFieldNames(std::vector<VariableMember> &members)
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
    std::vector<VariableMember> members;
    bool hasStaticMembers = false;

    IfFailRet(FetchFieldsAndProperties(m_sharedEvaluator.get(), ref.iCorValue, pThread, ref.frameId.getLevel(),
                                       members, ref.valueKind == ValueIsClass, hasStaticMembers, start,
                                       count == 0 ? INT_MAX : start + count, ref.evalFlags));

    FixupInheritedFieldNames(members);

    for (auto &it : members)
    {
        Variable var(ref.evalFlags);
        var.name = it.name;
        bool isIndex = !it.name.empty() && it.name.at(0) == '[';
        if (var.name.find('(') == std::string::npos) // expression evaluator does not support typecasts
            var.evaluateName = ref.evaluateName + (isIndex ? "" : ".") + var.name;
        IfFailRet(FillValueAndType(it, var, pThread, ref.frameId.getLevel(), m_sharedEvaluator.get(), m_sharedEvalHelpers.get()));
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
            IfFailRet(TypePrinter::GetTypeOfValue(ref.iCorValue, var.evaluateName)); // do not expose type for this fake variable

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
    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));

    ToRelease<ICorDebugValue> pResultValue;
    FrameLevel frameLevel = frameId.getLevel();
    IfFailRet(m_sharedEvalStackMachine->EvaluateExpression(pThread, frameLevel, variable.evalFlags, expression, &pResultValue, output, &variable.editable));

    variable.evaluateName = expression;
    IfFailRet(TypePrinter::GetTypeOfValue(pResultValue, variable.type));
    IfFailRet(PrintFormattedValue(pResultValue, pThread, frameLevel, variable.evalFlags,
                                  m_sharedEvaluator.get(), m_sharedEvalHelpers.get(), variable.value));

    return AddVariableReference(variable, frameId, pResultValue, ValueIsVariable);
}

HRESULT Variables::SetVariable(
    ICorDebugProcess *pProcess,
    const std::string &name,
    const std::string &value,
    uint32_t ref,
    std::string &output)
{
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
    bool found = false;

    if (FAILED(Status = m_sharedEvaluator->WalkStackVars(pThread, ref.frameId.getLevel(),
        [&](const std::string &varName, Evaluator::GetValueCallback getValue) -> HRESULT
    {
        if (varName != name)
            return S_OK;

        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(getValue(&iCorValue, ref.evalFlags));
        IfFailRet(m_sharedEvaluator->SetValue(pThread, ref.frameId.getLevel(), iCorValue, &getValue, nullptr, value, ref.evalFlags, output));
        IfFailRet(PrintValue(iCorValue, output));
        found = true;
        return E_ABORT; // Fast exit from cycle.
    })) && Status != E_ABORT)
    {
        return Status;
    }

    if (!found)
    {
        output = "Variable name not found.";
        return E_FAIL;
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
    bool found = false;

    if (FAILED(Status = m_sharedEvaluator->WalkMembers(ref.iCorValue, pThread, ref.frameId.getLevel(), true, [&](
        ICorDebugType*,
        bool is_static,
        const std::string &varName,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetterData *setterData) -> HRESULT
    {
        if (varName != name)
            return S_OK;

        if (setterData && !setterData->setterFunction)
            return E_FAIL;

        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(getValue(&iCorValue, ref.evalFlags));
        IfFailRet(m_sharedEvaluator->SetValue(pThread, ref.frameId.getLevel(), iCorValue, &getValue, setterData, value, ref.evalFlags, output));
        IfFailRet(PrintValue(iCorValue, output));
        found = true;
        return E_ABORT; // Fast exit from cycle.
    })) && Status != E_ABORT)
    {
        return Status;
    }

    if (!found)
    {
        output = "Variable name not found.";
        return E_FAIL;
    }
    return S_OK;
}

HRESULT Variables::SetExpression(ICorDebugProcess *pProcess, FrameId frameId, const std::string &expression,
                                 int evalFlags, const std::string &value, std::string &output)
{
    ThreadId threadId = frameId.getThread();
    if (!threadId)
        return E_FAIL;

    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(int(threadId), &pThread));

    ToRelease<ICorDebugValue> iCorValue;
    bool editable = false;
    std::unique_ptr<Evaluator::SetterData> setterData;
    IfFailRet(m_sharedEvalStackMachine->EvaluateExpression(pThread, frameId.getLevel(), evalFlags, expression, &iCorValue, output, &editable, &setterData));
    if (!editable ||
        (editable && setterData.get() && !setterData.get()->setterFunction)) // property, that don't have setter
    {
        output = "'" + expression + "' cannot be assigned to";
        return E_INVALIDARG;
    }

    IfFailRet(m_sharedEvaluator->SetValue(pThread, frameId.getLevel(), iCorValue, nullptr, setterData.get(), value, evalFlags, output));
    IfFailRet(PrintValue(iCorValue, output));
    return S_OK;
}

} // namespace netcoredbg
