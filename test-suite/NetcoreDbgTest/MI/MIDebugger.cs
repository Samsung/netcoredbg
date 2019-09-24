using System.Collections.Generic;
using NetcoreDbgTestCore;
using NetcoreDbgTestCore.MI;

namespace NetcoreDbgTest.MI
{
    public class MIDebugger
    {
        public MIResultRecord Request(string command, int timeout = -1)
        {
            MIResultRecord resultRecord = null;
            EventsAddedOnLastRequest = false;

            Logger.LogLine("> " + command);

            if (!Debuggee.DebuggerClient.Send(command)) {
                throw new DebuggerNotResponsesException();
            }

            while (true) {
                string[] response = Debuggee.DebuggerClient.Receive(timeout);

                if (response == null) {
                    throw new DebuggerNotResponsesException();
                }

                foreach (string line in response) {
                    Logger.LogLine("< " + line);
                }

                MIOutput output = MIParser.ParseOutput(response);

                if (output.ResultRecord != null) {
                    resultRecord = output.ResultRecord;
                    break;
                } else {
                    OutOfBandRecords.AddRange(output.OutOfBandRecords);
                    EventsAddedOnLastRequest = true;
                }
            }

            return resultRecord;
        }

        public MIOutOfBandRecord[] Receive(int timeout = -1)
        {
            if (EventsAddedOnLastRequest) {
                EventsAddedOnLastRequest = false;
                return OutOfBandRecords.ToArray();
            }

            string[] response = Debuggee.DebuggerClient.Receive(timeout);

            if (response == null) {
                throw new DebuggerNotResponsesException();
            }

            foreach (string line in response) {
                Logger.LogLine("< " + line);
            }

            MIOutput output = MIParser.ParseOutput(response);

            if (output.ResultRecord != null) {
                // this output must hasn't result record
                throw new MIParserException();
            }

            OutOfBandRecords.AddRange(output.OutOfBandRecords);

            return OutOfBandRecords.ToArray();
        }

        List<MIOutOfBandRecord> OutOfBandRecords = new List<MIOutOfBandRecord>();
        MIParser MIParser = new MIParser();
        bool EventsAddedOnLastRequest = false;
    }
}
