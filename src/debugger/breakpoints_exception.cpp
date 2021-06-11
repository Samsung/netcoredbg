// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_exception.h"
#include "debugger/evaluator.h"
#include "debugger/valueprint.h"
#include "metadata/typeprinter.h"
#include <sstream>

namespace netcoredbg
{

void ExceptionBreakpoints::ManagedExceptionBreakpoint::ToBreakpoint(Breakpoint &breakpoint) const
{
    breakpoint.id = this->id;
    breakpoint.verified = true;
}

void ExceptionBreakpoints::DeleteAll()
{
    m_breakpointsMutex.lock();
    for (auto &filterMap : m_exceptionBreakpoints)
    {
        filterMap.clear();
    }
    m_breakpointsMutex.unlock();
}

static std::string CalculateExceptionBreakpointHash(const ExceptionBreakpoint &expb)
{
    std::ostringstream ss;

    ss << (int)expb.categoryHint;

    if (expb.negativeCondition)
        ss << "!";

    for (auto &entry : expb.condition)
    {
        ss << ":" << entry << ":";
    }

    return ss.str();
}

HRESULT ExceptionBreakpoints::SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints,
                                                      std::function<uint32_t()> getId)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    // Remove old breakpoints
    std::vector<std::unordered_set<std::string>> expBreakpoints((size_t)ExceptionBreakpointFilter::Size);
    for (const auto &expb : exceptionBreakpoints)
    {
        expBreakpoints[(size_t)expb.filterId].insert(CalculateExceptionBreakpointHash(expb));
    }
    for (size_t filter = 0; filter < (size_t)ExceptionBreakpointFilter::Size; ++filter)
    {
        for (auto it = m_exceptionBreakpoints[filter].begin(); it != m_exceptionBreakpoints[filter].end();)
        {
            if (expBreakpoints[filter].find(it->first) == expBreakpoints[filter].end())
                it = m_exceptionBreakpoints[filter].erase(it);
            else
                ++it;
        }
    }

    if (exceptionBreakpoints.empty())
        return S_OK;

    // Export exception breakpoints
    for (const auto &expb : exceptionBreakpoints)
    {
        std::string expHash(CalculateExceptionBreakpointHash(expb));

        Breakpoint breakpoint;

        auto b = m_exceptionBreakpoints[(size_t)expb.filterId].find(expHash);
        if (b == m_exceptionBreakpoints[(size_t)expb.filterId].end())
        {
            // New breakpoint
            ManagedExceptionBreakpoint bp;
            bp.id = getId();
            bp.categoryHint = expb.categoryHint;
            bp.condition = expb.condition;
            bp.negativeCondition = expb.negativeCondition;

            bp.ToBreakpoint(breakpoint);
            m_exceptionBreakpoints[(size_t)expb.filterId].insert(std::make_pair(expHash, std::move(bp)));
        }
        else
        {
            ManagedExceptionBreakpoint &bp = b->second;
            bp.ToBreakpoint(breakpoint);
        }

        breakpoints.push_back(breakpoint);
    }

    return S_OK;
}

// Return:
// true - covered by filter, need emit exception event
// false - not covered by filter, ignore exception
bool ExceptionBreakpoints::CoveredByFilter(ExceptionBreakpointFilter filterId, const std::string &excType, ExceptionCategory excCategory)
{
    assert(excCategory != ExceptionCategory::ANY); // caller must know category: CLR = Exception() callback, MDA = MDANotification() callback
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &expb : m_exceptionBreakpoints[(size_t)filterId])
    {
        if (expb.second.categoryHint != excCategory &&
            expb.second.categoryHint != ExceptionCategory::ANY)
            continue;

        if (expb.second.condition.empty())
            return true;

        bool isCoveredByCondition = expb.second.condition.find(excType) == expb.second.condition.end() ?
                                    expb.second.negativeCondition : !expb.second.negativeCondition;
        if (isCoveredByCondition)
            return true;
    }

    return false;
}

static void GetExceptionShorDescription(ExceptionBreakMode breakMode, const std::string &excType, const std::string &excModule, std::string &result)
{
    switch(breakMode)
    {
        case ExceptionBreakMode::THROW:
            result = "Exception thrown: '" + excType + "' in " + excModule;
            break;

        case ExceptionBreakMode::USER_UNHANDLED:
            result = "An exception of type '" + excType + "' occurred in " + excModule + " but was not handled in user code";
            break;

        case ExceptionBreakMode::UNHANDLED:
            result = "An unhandled exception of type '" + excType + "' occurred in " + excModule;
            break;

        default:
            result = "";
            break;
    }
}

static void GetExceptionStageName(ExceptionBreakMode breakMode, std::string &result)
{
    switch(breakMode)
    {
        case ExceptionBreakMode::THROW:
            result = "throw";
            break;

        case ExceptionBreakMode::USER_UNHANDLED:
            result = "user-unhandled";
            break;

        case ExceptionBreakMode::UNHANDLED:
            result = "unhandled";
            break;

        default:
            result = "";
            break;
    }
}

static void GetExceptionBreakModeName(ExceptionBreakMode breakMode, std::string &result)
{
    switch(breakMode)
    {
        case ExceptionBreakMode::THROW:
            result = "always";
            break;

        case ExceptionBreakMode::USER_UNHANDLED:
            result = "userUnhandled";
            break;

        case ExceptionBreakMode::UNHANDLED:
            result = "unhandled";
            break;

        default:
            result = "";
            break;
    }
}

HRESULT ExceptionBreakpoints::GetExceptionDetails(ICorDebugThread *pThread, ICorDebugValue *pExceptionValue, ExceptionDetails &details)
{
    std::string excType;
    if (FAILED(TypePrinter::GetTypeOfValue(pExceptionValue, details.fullTypeName)))
    {
        excType = "<unknown exception>";
    }

    auto lastDotPosition = details.fullTypeName.find_last_of(".");
    if (lastDotPosition < details.fullTypeName.size())
        details.typeName = details.fullTypeName.substr(lastDotPosition + 1);
    else
        details.typeName = details.fullTypeName;

    details.evaluateName = "$exception";

    HRESULT Status;
    ToRelease<ICorDebugValue> iCorInnerExceptionValue;
    const bool escape = false;
    m_sharedEvaluator->WalkMembers(pExceptionValue, pThread, FrameLevel{0}, [&](
        ICorDebugType*,
        bool,
        const std::string &memberName,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetValueCallback)
    {
        auto getMemberWithName = [&](const std::string &name, std::string &result) -> HRESULT
        {
            if (memberName != name)
                return S_FALSE;

            ToRelease<ICorDebugValue> iCorResultValue;
            IfFailRet(getValue(&iCorResultValue, defaultEvalFlags));

            BOOL isNull = TRUE;
            ToRelease<ICorDebugReferenceValue> iCorReferenceValue;
            if (SUCCEEDED(iCorResultValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &iCorReferenceValue)) &&
                SUCCEEDED(iCorReferenceValue->IsNull(&isNull)) &&
                isNull == FALSE)
            {
                PrintValue(iCorResultValue, result, escape);
            }
            return S_OK;
        };

        IfFailRet(Status = getMemberWithName("_message", details.message));
        if (Status == S_OK)
            return S_OK;

        IfFailRet(Status = getMemberWithName("StackTrace", details.stackTrace));
        if (Status == S_OK)
            return S_OK;

        IfFailRet(Status = getMemberWithName("Source", details.source));
        if (Status == S_OK)
            return S_OK;

        if (memberName == "InnerException")
        {
            IfFailRet(getValue(&iCorInnerExceptionValue, defaultEvalFlags));
            BOOL isNull = FALSE;
            ToRelease<ICorDebugReferenceValue> iCorReferenceValue;
            if (SUCCEEDED(iCorInnerExceptionValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &iCorReferenceValue)) &&
                SUCCEEDED(iCorReferenceValue->IsNull(&isNull)) &&
                isNull == TRUE)
            {
                iCorInnerExceptionValue.Free();
            }
        }

        return S_OK;
    });

    details.formattedDescription = "**" + details.fullTypeName + "**";
    if (!details.message.empty())
        details.formattedDescription += " '" + details.message + "'";

    if (iCorInnerExceptionValue != nullptr)
    {
        details.innerException.reset(new ExceptionDetails);
        GetExceptionDetails(pThread, iCorInnerExceptionValue, *details.innerException.get());
    }

    return S_OK;
}

HRESULT ExceptionBreakpoints::GetExceptionInfo(ICorDebugThread *pThread, ExceptionInfo &exceptionInfo)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> iCorExceptionValue;
    IfFailRet(pThread->GetCurrentException(&iCorExceptionValue));
    if (iCorExceptionValue == nullptr)
        return E_FAIL;

    DWORD tid = 0;
    IfFailRet(pThread->GetID(&tid));

    std::lock_guard<std::mutex> lock(m_threadsExceptionMutex);

    auto findBreakMode = m_threadsExceptionBreakMode.find(tid);
    if (findBreakMode == m_threadsExceptionBreakMode.end() || findBreakMode->second == ExceptionBreakMode::NEVER)
        return E_FAIL;

    IfFailRet(GetExceptionDetails(pThread, iCorExceptionValue, exceptionInfo.details));

    std::string excModule;
    if (exceptionInfo.details.source.empty())
        excModule = "<unknown module>";
    else
        excModule = exceptionInfo.details.source + ".dll";

    GetExceptionShorDescription(findBreakMode->second, exceptionInfo.details.fullTypeName, excModule, exceptionInfo.description);

    if (!exceptionInfo.details.message.empty())
        exceptionInfo.description += ": '" + exceptionInfo.details.message + "'";

    if (exceptionInfo.details.innerException != nullptr)
    {
        exceptionInfo.description += "\n Inner exceptions found, see $exception in variables window for more details.\n Innermost exception: " +
                                     exceptionInfo.details.innerException->fullTypeName;
    }

    GetExceptionBreakModeName(findBreakMode->second, exceptionInfo.breakMode);
    // CLR only for now, MDA not implemented
    // TODO need store info about category too (not only BreakMode) during Exception() (CLR) and MDANotification() (MDA) callbacks.
    exceptionInfo.exceptionId = "CLR/" + exceptionInfo.details.fullTypeName;

    return S_OK;
}

/*
    Implemented exception callback logic by dwEventType (CorDebugExceptionCallbackType):

                  DEBUG_EXCEPTION_FIRST_CHANCE -> DEBUG_EXCEPTION_CATCH_HANDLER_FOUND
    enabled  JMC: throw                           none (reset thread status)
    disabled JMC: throw                           none (reset thread status)

    * case with ICorDebugProcess8::EnableExceptionCallbacksOutsideOfMyCode(FALSE), not used in our debugger, but must be included
                  DEBUG_EXCEPTION_USER_FIRST_CHANCE -> DEBUG_EXCEPTION_CATCH_HANDLER_FOUND
    enabled  JMC: throw                                [outside JMC] user-unhandled (reset thread status)
                                                       [inside  JMC] none (reset thread status)
    disabled JMC: throw                                none (reset thread status)

                  DEBUG_EXCEPTION_FIRST_CHANCE -> DEBUG_EXCEPTION_USER_FIRST_CHANCE -> DEBUG_EXCEPTION_CATCH_HANDLER_FOUND
    enabled  JMC: throw                           none                                 [outside JMC] user-unhandled (reset thread status)
                                                                                       [inside  JMC] none (reset thread status)
    disabled JMC: throw                           none                                 none (reset thread status)

    * fatal exception from runtime itself
                  DEBUG_EXCEPTION_UNHANDLED
    enabled  JMC: unhandled (reset thread status)
    disabled JMC: unhandled (reset thread status)

                  DEBUG_EXCEPTION_FIRST_CHANCE -> DEBUG_EXCEPTION_UNHANDLED
    enabled  JMC: throw                           unhandled (reset thread status)
    disabled JMC: throw                           unhandled (reset thread status)

    * case with ICorDebugProcess8::EnableExceptionCallbacksOutsideOfMyCode(FALSE), not used in our debugger, but must be included
                  DEBUG_EXCEPTION_USER_FIRST_CHANCE -> DEBUG_EXCEPTION_UNHANDLED
    enabled  JMC: throw                                unhandled (reset thread status)
    disabled JMC: throw                                unhandled (reset thread status)

                  DEBUG_EXCEPTION_FIRST_CHANCE -> DEBUG_EXCEPTION_USER_FIRST_CHANCE -> DEBUG_EXCEPTION_UNHANDLED
    enabled  JMC: throw                           none                                 unhandled (reset thread status)
    disabled JMC: throw                           none                                 unhandled (reset thread status)

    Reset exception thread status not only for `catch` but for `unhandled` too, since we may have not fatal unhandled exceptions,
    for example, System.AppDomainUnloadedException (https://docs.microsoft.com/en-us/dotnet/api/system.appdomainunloadedexception).

    More related info:
    https://github.com/OmniSharp/omnisharp-vscode/blob/master/debugger.md#exception-settings
    https://docs.microsoft.com/en-us/visualstudio/debugger/managing-exceptions-with-the-debugger
*/
HRESULT ExceptionBreakpoints::ManagedCallbackException(ICorDebugThread *pThread, ExceptionCallbackType eventType, std::string excModule, StoppedEvent &event)
{
    HRESULT Status;
    DWORD tid = 0;
    IfFailRet(pThread->GetID(&tid));

    ToRelease<ICorDebugValue> iCorExceptionValue;
    IfFailRet(pThread->GetCurrentException(&iCorExceptionValue));
    if (iCorExceptionValue == nullptr)
        return E_FAIL;

    std::string excType;
    if (FAILED(TypePrinter::GetTypeOfValue(iCorExceptionValue, excType)))
    {
        excType = "<unknown exception>";
    }

    std::lock_guard<std::mutex> lock(m_threadsExceptionMutex);

    switch(eventType)
    {
        case ExceptionCallbackType::FIRST_CHANCE:
        {
            assert(m_threadsExceptionStatus.find(tid) == m_threadsExceptionStatus.end());

            // Important, reset previous stage for this thread.
            m_threadsExceptionBreakMode[tid] = ExceptionBreakMode::NEVER;

            m_threadsExceptionStatus[tid].m_lastEvent = ExceptionCallbackType::FIRST_CHANCE;
            m_threadsExceptionStatus[tid].m_excModule = excModule;

            if (!CoveredByFilter(ExceptionBreakpointFilter::THROW, excType, ExceptionCategory::CLR) &&
                !CoveredByFilter(ExceptionBreakpointFilter::THROW_USER_UNHANDLED, excType, ExceptionCategory::CLR))
                return S_OK;

            m_threadsExceptionBreakMode[tid] = ExceptionBreakMode::THROW;
            break;
        }

        case ExceptionCallbackType::USER_FIRST_CHANCE:
        {
            // In case we already "THROW" at FIRST CHANCE, don't emit "THROW" event again.
            auto find = m_threadsExceptionStatus.find(tid);
            if (find != m_threadsExceptionStatus.end())
            {
                m_threadsExceptionStatus[tid].m_lastEvent = ExceptionCallbackType::USER_FIRST_CHANCE;
                if (find->second.m_excModule.empty())
                    find->second.m_excModule = excModule;

                return S_OK;
            }

            m_threadsExceptionStatus[tid].m_lastEvent = ExceptionCallbackType::USER_FIRST_CHANCE;
            m_threadsExceptionStatus[tid].m_excModule = excModule;

            if (!CoveredByFilter(ExceptionBreakpointFilter::THROW, excType, ExceptionCategory::CLR) &&
                !CoveredByFilter(ExceptionBreakpointFilter::THROW_USER_UNHANDLED, excType, ExceptionCategory::CLR))
                return S_OK;

            m_threadsExceptionBreakMode[tid] = ExceptionBreakMode::THROW;
            break;
        }

        case ExceptionCallbackType::CATCH_HANDLER_FOUND:
        {
            assert(m_threadsExceptionStatus.find(tid) != m_threadsExceptionStatus.end());

            if (!m_justMyCode || m_threadsExceptionStatus[tid].m_lastEvent == ExceptionCallbackType::FIRST_CHANCE)
            {
                m_threadsExceptionStatus.erase(tid);
                return S_OK;
            }

            if (!CoveredByFilter(ExceptionBreakpointFilter::USER_UNHANDLED, excType, ExceptionCategory::CLR) &&
                !CoveredByFilter(ExceptionBreakpointFilter::THROW_USER_UNHANDLED, excType, ExceptionCategory::CLR))
            {
                m_threadsExceptionStatus.erase(tid);
                return S_OK;
            }

            excModule = m_threadsExceptionStatus[tid].m_excModule;
            m_threadsExceptionStatus.erase(tid);

            m_threadsExceptionBreakMode[tid] = ExceptionBreakMode::USER_UNHANDLED;
            break;
        }

        case ExceptionCallbackType::USER_CATCH_HANDLER_FOUND:
        {
            assert(m_threadsExceptionStatus.find(tid) != m_threadsExceptionStatus.end());
            assert(m_threadsExceptionStatus[tid].m_lastEvent == ExceptionCallbackType::USER_FIRST_CHANCE);

            m_threadsExceptionStatus.erase(tid);
            return S_OK;
        }

        case ExceptionCallbackType::UNHANDLED:
        {
            // By current logic, debugger must stop at all unhandled exception (that will crash application), no matter what user has configured.
            // TODO some exception like System.AppDomainUnloadedException or System.Threading.ThreadAbortException, could be ignored at unhandled,
            // since they don't crash application, in this case:
            //     if (CoveredByFilter(ExceptionBreakpointFilter::UNHANDLED, excType, ExceptionCategory::CLR)) - forced to emit event

            auto find = m_threadsExceptionStatus.find(tid);
            if (find != m_threadsExceptionStatus.end())
            {
                excModule = find->second.m_excModule;
                m_threadsExceptionStatus.erase(find);
            }

            m_threadsExceptionBreakMode[tid] = ExceptionBreakMode::UNHANDLED;
            break;
        }

        default:
            return E_INVALIDARG;
    }

    if (excModule.empty())
        excModule = "<unknown module>";

    // Custom message, provided by runtime (in case internal runtime exception) or directly by user as exception constructor argument on throw.
    // Note, this is optional field in exception object that could have nulled reference.
    std::string excMessage;
    const bool escape = false;
    m_sharedEvaluator->WalkMembers(iCorExceptionValue, pThread, FrameLevel{0}, [&](
        ICorDebugType*,
        bool,
        const std::string &memberName,
        Evaluator::GetValueCallback getValue,
        Evaluator::SetValueCallback)
    {
        ToRelease<ICorDebugValue> pResultValue;

        if (memberName != "_message")
            return S_OK;

        IfFailRet(getValue(&pResultValue, defaultEvalFlags));

        BOOL isNull = TRUE;
        ToRelease<ICorDebugReferenceValue> pReferenceValue;
        if (SUCCEEDED(pResultValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue)) &&
            SUCCEEDED(pReferenceValue->IsNull(&isNull)) &&
            isNull == FALSE)
        {
            PrintValue(pResultValue, excMessage, escape);
            return E_ABORT; // Fast exit from cycle.
        }

        return S_OK;
    });

    GetExceptionShorDescription(m_threadsExceptionBreakMode[tid], excType, excModule, event.text);
    GetExceptionStageName(m_threadsExceptionBreakMode[tid], event.exception_stage);
    event.exception_category = "clr"; // ManagedCallbackException() called for CLR exceptions only
    event.exception_name = excType;
    event.exception_message = excMessage;

    return S_FALSE; // S_FALSE - breakpoint hit, not affect on callback (callback will emit stop event)
}

HRESULT ExceptionBreakpoints::ManagedCallbackExitThread(ICorDebugThread *pThread)
{
    HRESULT Status;
    DWORD tid = 0;
    IfFailRet(pThread->GetID(&tid));

    m_threadsExceptionMutex.lock();
    m_threadsExceptionBreakMode.erase(tid);
    m_threadsExceptionStatus.erase(tid);
    m_threadsExceptionMutex.unlock();

    return S_OK;
}

} // namespace netcoredbg
