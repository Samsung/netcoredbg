// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Globalization;

namespace MICore
{
    public class MIException : Exception
    {
        public MIException(int hr)
        {
            this.HResult = hr;
        }

        public MIException(int hr, Exception innerException)
            : base(string.Empty, innerException)
        {
        }
    }

    public class MIResultFormatException : MIException
    {
        private const int COMQC_E_BAD_MESSAGE = unchecked((int)0x80110604);
        public readonly string Field;
        public ResultValue Result;

        public MIResultFormatException(string field, ResultValue value)
            : base(COMQC_E_BAD_MESSAGE)
        {
            Field = field;
            Result = value;
        }

        public MIResultFormatException(string field, ResultValue value, Exception innerException)
            : base(COMQC_E_BAD_MESSAGE, innerException)
        {
            Field = field;
            Result = value;
        }

        public override string Message
        {
            get
            {
                string message = string.Format(CultureInfo.CurrentCulture, "Unrecognized format of field \"{0}\" in result: {1}", Field, Result.ToString());
                return message;
            }
        }
    }

    public class UnexpectedMIResultException : MIException
    {
        // We want to have a message which is vaguely reasonable if it winds up getting converted to an HRESULT. So
        // we will use take this one.
        //    MessageId: COMQC_E_BAD_MESSAGE
        //
        //    MessageText:
        //      The message is improperly formatted or was damaged in transit
        private const int COMQC_E_BAD_MESSAGE = unchecked((int)0x80110604);
        private readonly string _debuggerName;
        private readonly string _command;
        private readonly string _miError;

        /// <summary>
        /// Creates a new UnexpectedMIResultException
        /// </summary>
        /// <param name="debuggerName">[Required] Name of the underlying MI debugger (ex: 'GDB')</param>
        /// <param name="command">[Required] MI command that was issued</param>
        /// <param name="mi">[Optional] Error message from MI</param>
        public UnexpectedMIResultException(string debuggerName, string command, string mi) : base(COMQC_E_BAD_MESSAGE)
        {
            _debuggerName = debuggerName;
            _command = command;
            _miError = mi;
        }

        public override string Message
        {
            get
            {
                string message = string.Format(CultureInfo.CurrentCulture, "Unexpected {0} output from command \"{1}\".", _debuggerName, _command);
                if (!string.IsNullOrWhiteSpace(_miError))
                {
                    message = string.Concat(message, " ", _miError);
                }

                return message;
            }
        }

        public string MIError
        {
            get { return _miError; }
        }
    }

    public class MIDebuggerInitializeFailedException : Exception
    {
        [System.Diagnostics.CodeAnalysis.SuppressMessage("Microsoft.Security", "CA2104:DoNotDeclareReadOnlyMutableReferenceTypes")]
        public readonly IReadOnlyList<string> OutputLines;
        private readonly string _debuggerName;
        private readonly IReadOnlyList<string> _errorLines;
        private string _message;

        public MIDebuggerInitializeFailedException(string debuggerName, IReadOnlyList<string> errorLines, IReadOnlyList<string> outputLines)
        {
            this.OutputLines = outputLines;
            _debuggerName = debuggerName;
            _errorLines = errorLines;
        }

        public override string Message
        {
            get
            {
                if (_message == null)
                {
                    if (_errorLines.Any(x => !string.IsNullOrWhiteSpace(x)))
                    {
                        _message = string.Format(CultureInfo.InvariantCulture, "Unable to establish a connection to {0}. The following message was written to stderr:\n\n{1}\n\nSee Output Window for details.", _debuggerName, string.Join("\r\n", _errorLines));
                    }
                    else
                    {
                        _message = string.Format(CultureInfo.InvariantCulture, "Unable to establish a connection to {0}. Debug output may contain more information.", _debuggerName);
                    }
                }

                return _message;
            }
        }
    }
}
