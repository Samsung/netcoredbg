class ManagedCallback;

class Debugger
{
    ManagedCallback *m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;
    bool m_exit;
    static bool m_justMyCode;
    static std::mutex m_outMutex;

    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

public:
    static bool IsJustMyCode() { return m_justMyCode; }
    static void SetJustMyCode(bool enable) { m_justMyCode = enable; }

    Debugger(ManagedCallback *cb) :
        m_managedCallback(cb),
        m_pDebug(nullptr),
        m_pProcess(nullptr),
        m_exit(false) {}

    ~Debugger();

    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
    static std::string EscapeMIValue(const std::string &str);

    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    void CommandLoop();
};
