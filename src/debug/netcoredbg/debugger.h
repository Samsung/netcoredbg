// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

class ManagedCallback;

class Debugger
{
    friend class ManagedCallback;
    ManagedCallback *m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;
    bool m_exit;

    static bool m_justMyCode;
    static std::mutex m_outMutex;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;

    HRESULT CheckNoProcess();

    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    HRESULT RunProcess();

    void Cleanup();

    enum StepType {
        STEP_IN = 0,
        STEP_OVER,
        STEP_OUT
    };

    static HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);
    static HRESULT StepCommand(ICorDebugProcess *pProcess,
                               const std::vector<std::string> &args,
                               std::string &output, StepType stepType);
public:
    static bool IsJustMyCode() { return m_justMyCode; }
    static void SetJustMyCode(bool enable) { m_justMyCode = enable; }

    Debugger(ManagedCallback *cb) :
        m_managedCallback(cb),
        m_pDebug(nullptr),
        m_pProcess(nullptr),
        m_exit(false),
        m_startupReady(false),
        m_startupResult(S_OK),
        m_unregisterToken(nullptr),
        m_processId(0) {}

    ~Debugger();

    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
    static void Message(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
    static std::string EscapeMIValue(const std::string &str);

    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    void CommandLoop();
};

HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);
int GetLastStoppedThreadId();
void WaitProcessExited();
