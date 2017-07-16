class ManagedCallback;

class Debugger
{
    ManagedCallback *m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;
    bool m_exit;

    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

public:

    Debugger(ManagedCallback *cb) :
        m_managedCallback(cb),
        m_pDebug(nullptr),
        m_pProcess(nullptr),
        m_exit(false) {}

    ~Debugger();

    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    void CommandLoop();
};
