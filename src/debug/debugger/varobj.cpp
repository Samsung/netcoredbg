#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iomanip>

#include "torelease.h"
#include "arrayholder.h"

// Modules
HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue);

#include "typeprinter.h"

// Valuewalk
typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;
HRESULT WalkMembers(ICorDebugValue *pValue, ICorDebugILFrame *pILFrame, WalkMembersCallback cb);
HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb);
HRESULT EvalProperty(
    mdMethodDef methodDef,
    ICorDebugModule *pModule,
    ICorDebugType *pType,
    ICorDebugValue *pInputValue,
    bool is_static,
    ICorDebugValue **ppEvalResult);

// Valueprint
HRESULT PrintValue(ICorDebugValue *pInputValue, ICorDebugILFrame * pILFrame, std::string &output);

struct VarObjValue
{
    std::string name;
    ICorDebugValue *value;
    std::string owningType;
    std::string typeName;

    std::string varobjName;
    bool statics_only;

    unsigned int numchild;

    VarObjValue(
        const std::string &n,
        ICorDebugValue *v,
        const std::string t = "") : name(n), value(v), owningType(t),
                                statics_only(false), numchild(0) {}
    VarObjValue(
        ICorDebugValue *v) : name("Static members"), value(v),
                                statics_only(true), numchild(0) {}
};

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

static HRESULT FetchFieldsAndProperties(ICorDebugValue *pInputValue,
                                        ICorDebugType *pTypeCast,
                                        ICorDebugILFrame *pILFrame,
                                        std::vector<VarObjValue> &members,
                                        bool static_members,
                                        bool &has_static_members)
{
    has_static_members = false;
    HRESULT Status;

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

        std::string className;
        TypePrinter::GetTypeOfValue(pType, className);

        ICorDebugValue *pResultValue = nullptr;

        if (mdGetter != mdMethodDefNil)
        {
            EvalProperty(mdGetter, pModule, pType, pInputValue, is_static, &pResultValue);
        }
        else
        {
            if (pValue)
                pValue->AddRef();
            pResultValue = pValue;
        }

        members.emplace_back(name, pResultValue, className);
        return S_OK;
    }));

    return S_OK;
}

void FixupInheritedFieldNames(std::vector<VarObjValue> &members)
{
    std::unordered_set<std::string> names;
    for (auto it = members.rbegin(); it != members.rend(); ++it)
    {
        auto r = names.insert(it->name);
        if (!r.second)
        {
            it->name += " (" + it->owningType + ")";
        }
    }
}

void PrintChildren(std::vector<VarObjValue> &members, std::string &output)
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
    for (auto m : members)
    {
        ss << sep;
        sep = ",";

        ss << "child={name=\"" << m.varobjName << "\",exp=\"" << m.name << "\",";
        ss << "numchild=\"" << m.numchild << "\",type=\"" << m.typeName << "\"}";
        //thread-id="452958",has_more="0"}
    }

    ss << "]";
    output = ss.str();
}

HRESULT ListChildren(VarObjValue &objValue, ICorDebugFrame *pFrame, std::string &output)
{
    HRESULT Status;

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    std::vector<VarObjValue> members;

    bool has_static_members;

    IfFailRet(FetchFieldsAndProperties(objValue.value,
                                       NULL,
                                       pILFrame,
                                       members,
                                       objValue.statics_only,
                                       has_static_members));

    if (!objValue.statics_only && has_static_members)
    {
        objValue.value->AddRef();
        members.emplace_back(objValue.value);
    }

    FixupInheritedFieldNames(members);

    for (auto &m : members)
    {
        std::string className;

        if (!m.value)
            continue;

        Status = GetNumChild(m.value, m.numchild, m.statics_only);
        if (!m.statics_only)
            TypePrinter::GetTypeOfValue(m.value, m.typeName);
    }

    PrintChildren(members, output);

    for (auto m : members)
    {
        if (m.value)
            m.value->Release();
    }

    return S_OK;
}

HRESULT ListChildren(ICorDebugValue *pInputValue, ICorDebugFrame *pFrame, std::string &output)
{
    VarObjValue val("?", pInputValue, "");
    return ListChildren(val, pFrame, output);
}

HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output)
{
    bool printValues = true;
    bool printTypes = true;

    HRESULT Status;

    std::stringstream ss;
    ss << "variables=[";
    const char *sep = "";

    IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
    {
        ss << sep << "{name=\"" << name << "\"";
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

        std::string test;
        ListChildren(pValue, pFrame, test);
        ss << test;

        ss << "}";
        sep = ",";
        return S_OK;
    }));

    ss << "]";
    output = ss.str();
    return S_OK;
}
