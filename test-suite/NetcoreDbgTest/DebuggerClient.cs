namespace NetcoreDbgTestCore
{
    public enum ProtocolType {
        None,
        MI,
        VSCode,
    }

    public class DebuggerClient
    {
        public DebuggerClient(ProtocolType protocol)
        {
            Protocol = protocol;
        }

        // Protocol specific handshake gives a guarantee
        // of a debugger ability to receive commands and
        // response messages
        public virtual bool DoHandshake(int timeout)
        {
            return false;
        }

        // Send command to debugger
        public virtual bool Send(string cmd)
        {
            return false;
        }

        // Receive protocol specific response
        public virtual string[] Receive(int timeout)
        {
            return null;
        }

        // Close session with debugger
        public virtual void Close()
        {
        }

        public ProtocolType Protocol;
    }
}
