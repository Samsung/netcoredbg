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

// Valuewalk
typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;
HRESULT WalkMembers(ICorDebugValue *pValue, ICorDebugILFrame *pILFrame, WalkMembersCallback cb);
HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb);
HRESULT EvalFunction(
    ICorDebugThread *pThread,
    ICorDebugFunction *pFunc,
    ICorDebugType *pType, // may be nullptr
    ICorDebugValue *pArgValue, // may be nullptr
    ICorDebugValue **ppEvalResult);

HRESULT EvalObjectNoConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppEvalResult);

// Valueprint
HRESULT PrintValue(ICorDebugValue *pInputValue, ICorDebugILFrame * pILFrame, std::string &output);
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = NULL);

// Modules
HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule);

HRESULT GetNumChild(ICorDebugValue *pValue,
                    unsigned int &numchild,
                    bool static_members = false)
{
    HRESULT Status = S_OK;
    numchild = 0;

    ULONG numstatic = 0;
    ULONG numinstance = 0;

    IfFailRet(WalkMembers(pValue, nullptr, [&numstatic, &numinstance](
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

static HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugILFrame *pILFrame, ICorDebugValue *pValue)
{
    HRESULT Status;

    if (!g_pRunClassConstructor && !g_pGetTypeHandle)
    {
        ToRelease<ICorDebugModule> pModule;
        IfFailRet(GetModuleWithName("System.Private.CoreLib.dll", &pModule));

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

    IfFailRet(WalkMembers(pInputValue, pILFrame, [&](
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
                     ICorDebugILFrame *pILFrame,
                     std::string &output)
{
    std::stringstream ss;

    std::string editable = "noneditable";

    ss << "name=\"" << v.varobjName << "\",";
    if (print_values)
    {
        std::string strVal;
        if (v.value && !v.statics_only)
            PrintValue(v.value, pILFrame, strVal);
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

static void PrintChildren(std::vector<VarObjValue> &members, int print_values, ICorDebugILFrame *pILFrame, bool has_more, std::string &output)
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
        PrintVar(InsertVar(m), print_values, pILFrame, varout);

        ss << sep;
        sep = ",";
        ss << "child={" << varout << "}";
    }

    ss << "]";
    ss << ",has_more=\"" << (has_more ? 1 : 0) << "\"";
    output = ss.str();
}

HRESULT ListChildren(
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
            RunClassConstructor(pThread, pILFrame, objValue.value);

            objValue.value->AddRef();
            members.emplace_back(objValue.threadId, objValue.value);
        }

        FixupInheritedFieldNames(members);
    }

    PrintChildren(members, print_values, pILFrame, has_more, output);

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

HRESULT ListVariables(ICorDebugThread *pThread, ICorDebugFrame *pFrame, std::string &output)
{
    const bool printValues = true;
    const bool printTypes = false;

    HRESULT Status;

    std::stringstream ss;
    ss << "variables=[";
    const char *sep = "";

    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)) && pExceptionValue != nullptr)
    {
        ToRelease<ICorDebugILFrame> pILFrame;
        IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

        ss << sep;
        sep = ",";

        ss << "{name=\"" << "$exception" << "\"";
        if (printValues)
        {
            std::string strVal;
            if (SUCCEEDED(PrintValue(pExceptionValue, pILFrame, strVal)))
                ss << ",value=\"" << strVal << "\"";
        }
        if (printTypes)
        {
            std::string strVal;
            if (SUCCEEDED(TypePrinter::GetTypeOfValue(pExceptionValue, strVal)))
                ss << ",type=\"" << strVal << "\"";
        }

        ss << "}";
    }

    IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
    {
        ss << sep;
        sep = ",";

        ss << "{name=\"" << name << "\"";
        if (printValues)
        {
            std::string strVal;
            if (SUCCEEDED(PrintValue(pValue, pILFrame, strVal)))
                ss << ",value=\"" << strVal << "\"";
        }
        if (printTypes)
        {
            std::string strVal;
            if (SUCCEEDED(TypePrinter::GetTypeOfValue(pValue, strVal)))
                ss << ",type=\"" << strVal << "\"";
        }

        ss << "}";

        return S_OK;
    }));

    ss << "]";
    output = ss.str();
    return S_OK;
}

HRESULT CreateVar(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ToRelease<ICorDebugValue> pResultValue;

    if (expression == "$exception")
    {
        pThread->GetCurrentException(&pResultValue);
    }
    else
    {
        IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
        {
            if (pResultValue)
                return S_OK; // TODO: Create a fast way to exit

            if (name == expression && pValue)
            {
                pValue->AddRef();
                pResultValue = pValue;
            }
            return S_OK;
        }));
    }

    if (!pResultValue)
        return E_FAIL;

    VarObjValue tmpobj(threadId, expression, pResultValue.Detach(), "", varobjName);
    int print_values = 1;
    PrintVar(InsertVar(tmpobj), print_values, pILFrame, output);

    return S_OK;
}

HRESULT DeleteVar(const std::string &varobjName)
{
    return g_vars.erase(varobjName) == 0 ? E_FAIL : S_OK;
}
