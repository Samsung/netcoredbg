using System;
using System.Collections.Generic;

namespace NetcoreDbgTest.VSCode
{
    // https://github.com/Microsoft/vscode-debugadapter-node/blob/master/debugProtocol.json
    // https://github.com/Microsoft/vscode-debugadapter-node/blob/master/protocol/src/debugProtocol.ts
    public class ProtocolMessage {
        public int seq;
        public string type;
    }
}
