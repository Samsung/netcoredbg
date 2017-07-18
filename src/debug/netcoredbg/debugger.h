class ManagedCallback;

class Debugger
{
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

    HRESULT CheckNoProcess();

    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    HRESULT RunProcess();
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
