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

struct VarObjValue
{
    std::string name;
    ToRelease<ICorDebugValue> value;
    std::string owningType;
    std::string typeName;

    int threadId;
    std::string varobjName;
    bool statics_only;

    unsigned int numchild;

    VarObjValue(
        int tid,
        const std::string &n,
        ICorDebugValue *v,
        const std::string t = "",
        const std::string vn = "") : name(n), value(v), owningType(t), threadId(tid), varobjName(vn),
                                     statics_only(false), numchild(0)
    {
        GetTypeNameAndNumChild();
    }

    VarObjValue(
        int tid,
        ICorDebugValue *v) : name("Static members"), value(v), threadId(tid),
                             statics_only(true), numchild(0)
    {
        GetTypeNameAndNumChild();
    }

    VarObjValue(VarObjValue &&that) = default;

private:
    VarObjValue(const VarObjValue &that) = delete;

    void GetTypeNameAndNumChild()
    {
        if (!value)
            return;

        GetNumChild(value, numchild, statics_only);
        if (!statics_only)
            TypePrinter::GetTypeOfValue(value, typeName);
    }
};

static unsigned int g_varCounter = 0;
static std::unordered_map<std::string, VarObjValue> g_vars;
static ToRelease<ICorDebugFunction> g_pRunClassConstructor;
static ToRelease<ICorDebugFunction> g_pGetTypeHandle;

void CleanupVars()
{
    g_vars.clear();
    g_varCounter = 0;
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

static HRESULT FetchFieldsAndProperties(ICorDebugValue *pInputValue,
                                        ICorDebugType *pTypeCast,
                                        ICorDebugThread *pThread,
                                        ICorDebugILFrame *pILFrame,
                                        std::vector<VarObjValue> &members,
                                        bool static_members,
                                        bool &has_static_members,
                                        int childStart,
                                        int childEnd,
                                        bool &has_more)
{
    has_static_members = false;
    HRESULT Status;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));

    has_more = false;
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
            has_static_members = true;

        bool add_member = static_members ? is_static : !is_static;
        if (!add_member)
            return S_OK;

        ++currentIndex;
        if (currentIndex < childStart)
            return S_OK;
        if (currentIndex >= childEnd)
        {
            has_more = true;
            return S_OK;
        }

        std::string className;
        if (pType)
            TypePrinter::GetTypeOfValue(pType, className);

        ICorDebugValue *pResultValue = nullptr;

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

        members.emplace_back(threadId, name, pResultValue, className);
        return S_OK;
    }));

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

static HRESULT FetchFieldsAndProperties2(ICorDebugValue *pInputValue,
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

static void FixupInheritedFieldNames(std::vector<VarObjValue> &members)
{
    std::unordered_set<std::string> names;
    for (auto &it : members)
    {
        auto r = names.insert(it.name);
        if (!r.second)
        {
            it.name += " (" + it.owningType + ")";
        }
    }
}

static void PrintVar(VarObjValue &v,
                     int print_values,
                     std::string &output)
{
    std::stringstream ss;

    std::string editable = "noneditable";

    ss << "name=\"" << v.varobjName << "\",";
    if (print_values)
    {
        std::string strVal;
        if (v.value && !v.statics_only)
            PrintValue(v.value, strVal);
        ss << "value=\"" << strVal << "\",";
    }
    ss << "attributes=\"" << editable << "\",";
    ss << "exp=\"" << v.name << "\",";
    ss << "numchild=\"" << v.numchild << "\",";
    ss << "type=\"" << v.typeName << "\",";
    ss << "thread-id=\"" << v.threadId << "\"";
    //,has_more="0"}

    output = ss.str();
}

static VarObjValue & InsertVar(VarObjValue &varobj)
{
    std::string varName = varobj.varobjName;

    if (varName.empty() || varName == "-")
    {
        varName = "var" + std::to_string(g_varCounter++);
    }

    varobj.varobjName = varName;

    auto it = g_vars.find(varName);
    if (it != g_vars.end())
        g_vars.erase(it);

    return g_vars.emplace(std::make_pair(varName, std::move(varobj))).first->second;
}

static void PrintChildren(std::vector<VarObjValue> &members, int print_values, bool has_more, std::string &output)
{
    std::stringstream ss;
    ss << "numchild=\"" << members.size() << "\"";

    if (members.empty())
    {
        output = ss.str();
        return;
    }
    ss << ",children=[";

    const char *sep = "";
    for (auto &m : members)
    {
        std::string varout;
        PrintVar(InsertVar(m), print_values, varout);

        ss << sep;
        sep = ",";
        ss << "child={" << varout << "}";
    }

    ss << "]";
    ss << ",has_more=\"" << (has_more ? 1 : 0) << "\"";
    output = ss.str();
}

static HRESULT ListChildren(
    int childStart,
    int childEnd,
    VarObjValue &objValue,
    int print_values,
    ICorDebugThread *pThread,
    ICorDebugFrame *pFrame,
    std::string &output)
{
    HRESULT Status;

    ToRelease<ICorDebugILFrame> pILFrame;
    if (pFrame)
        IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    std::vector<VarObjValue> members;

    bool has_static_members = false;
    bool has_more = false;

    if (objValue.value)
    {
        IfFailRet(FetchFieldsAndProperties(objValue.value,
                                           NULL,
                                           pThread,
                                           pILFrame,
                                           members,
                                           objValue.statics_only,
                                           has_static_members,
                                           childStart,
                                           childEnd,
                                           has_more));

        if (!objValue.statics_only && has_static_members)
        {
            RunClassConstructor(pThread, objValue.value);

            objValue.value->AddRef();
            members.emplace_back(objValue.threadId, objValue.value);
        }

        FixupInheritedFieldNames(members);
    }

    PrintChildren(members, print_values, has_more, output);

    return S_OK;
}

HRESULT ListChildren(
    int childStart,
    int childEnd,
    const std::string &name,
    int print_values,
    ICorDebugThread *pThread,
    ICorDebugFrame *pFrame,
    std::string &output)
{
    auto it = g_vars.find(name);
    if (it == g_vars.end())
        return E_FAIL;
    return ListChildren(childStart, childEnd, it->second, print_values, pThread, pFrame, output);
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
    VariableReference variableReference(variable.variablesReference, frameId, value, valueKind);
    variableReference.evaluateName = variable.evaluateName;
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

static void FixupInheritedFieldNames2(std::vector<Member> &members)
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

    IfFailRet(FetchFieldsAndProperties2(ref.value,
                                       pThread,
                                       pILFrame,
                                       members,
                                       ref.valueKind == VariableReference::ValueIsClass,
                                       hasStaticMembers,
                                       start,
                                       count == 0 ? INT_MAX : start + count));

    FixupInheritedFieldNames2(members);

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

HRESULT CreateVar(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    ToRelease<ICorDebugValue> pResultValue;

    IfFailRet(EvalExpr(pThread, pFrame, expression, &pResultValue));

    VarObjValue tmpobj(threadId, expression, pResultValue.Detach(), "", varobjName);
    int print_values = 1;
    PrintVar(InsertVar(tmpobj), print_values, output);

    return S_OK;
}

HRESULT DeleteVar(const std::string &varobjName)
{
    return g_vars.erase(varobjName) == 0 ? E_FAIL : S_OK;
}
