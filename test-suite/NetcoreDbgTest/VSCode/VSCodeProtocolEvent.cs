using System;
using System.Collections.Generic;

namespace NetcoreDbgTest.VSCode
{
    public class Event : ProtocolMessage {
    }

    public class ThreadEvent : Event {
        public ThreadEventBody body;
    }

    public class ThreadEventBody {
        public string reason;
        public int threadId;
    }

    public class StoppedEvent : Event {
        public StoppedEventBody body;
    }

    public class StoppedEventBody {
        public string reason;
        public string description;
        public int ?threadId;
        public bool ?preserveFocusHint;
        public string text;
        public bool ?allThreadsStopped;
    }

    public class ExitedEvent : Event {
        public ExitedEventBody body;
    }

    public class ExitedEventBody {
        public int exitCode;
    }
}
