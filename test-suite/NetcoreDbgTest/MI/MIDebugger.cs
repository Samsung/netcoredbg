using System;
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

                // we could get async record, in this case we could have two "(gdb)" prompts one by one
                // NOTE in this case we have only one line response, that contain prompt only
                if (MIParser.IsEnd(response[0]))
                    continue;

                MIOutput output = MIParser.ParseOutput(response);

                foreach (var record in output.OutOfBandRecords) {
                    EventQueue.Enqueue(record);
                }

                if (output.ResultRecord != null) {
                    resultRecord = output.ResultRecord;
                    break;
                }
            }

            return resultRecord;
        }

        void ReceiveEvents(int timeout = -1)
        {
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

            foreach (var record in output.OutOfBandRecords) {
                EventQueue.Enqueue(record);
            }
        }

        public bool IsEventReceived(Func<MIOutOfBandRecord, bool> filter)
        {
            // check previously received events first
            while (EventQueue.Count > 0) {
                if (filter(EventQueue.Dequeue()))
                    return true;
            }

            // receive new events and check them
            ReceiveEvents();
            while (EventQueue.Count > 0) {
                if (filter(EventQueue.Dequeue()))
                    return true;
            }

            return false;
        }

        Queue<MIOutOfBandRecord> EventQueue = new Queue<MIOutOfBandRecord>();
        MIParser MIParser = new MIParser();
    }
}
