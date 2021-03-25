using System;
using System.Text;
using System.Collections;
using System.Collections.Generic;
using NetcoreDbgTest.MI;

namespace NetcoreDbgTestCore.MI
{
    public class MIParserException : System.Exception
    {
    }

    public class MIParser
    {
        public MIOutput ParseOutput(string[] output)
        {
            var outOfBandRecordList = new List<MIOutOfBandRecord>();
            MIResultRecord resultRecord = null;
            int i = 0;

            while (IsOutOfBandRecord(output[i])) {
                outOfBandRecordList.Add(ParseOutOfBandRecord(output[i]));
                i++;
            }

            if (IsResultRecord(output[i])) {
                resultRecord = ParseResultRecord(output[i]);
                i++;
            }

            // we still could get async record after result (for example "=library-loaded" or "=thread-created")
            while (IsOutOfBandRecord(output[i])) {
                outOfBandRecordList.Add(ParseOutOfBandRecord(output[i]));
                i++;
            }

            if (!IsEnd(output[i])) {
                throw new MIParserException();
            }

            return new MIOutput(outOfBandRecordList.ToArray(), resultRecord);
        }

        MIResultRecord ParseResultRecord(string response)
        {
            int endIndex;
            var token = ParseToken(response, 0, out endIndex);

            if (response[endIndex] != '^') {
                throw new MIParserException();
            }

            var resultClass = ParseResultClass(response, endIndex + 1, out endIndex);
            var results = new List<MIResult>();

            while (endIndex != response.Length) {
                if (response[endIndex] != ',') {
                    throw new MIParserException();
                }

                results.Add(ParseResult(response, endIndex + 1, out endIndex));
            }

            return new MIResultRecord(token, resultClass, results.ToArray());
        }

        MIOutOfBandRecord ParseOutOfBandRecord(string response)
        {
            MIOutOfBandRecord outOfBandRecord;
            int endIndex;

            if (IsStreamRecord(response)) {
                outOfBandRecord = ParseStreamRecord(response, 0, out endIndex);
            } else {
                outOfBandRecord = ParseAsyncRecord(response, 0, out endIndex);
            }

            if (endIndex != response.Length) {
                throw new MIParserException();
            }

            return outOfBandRecord;
        }

        MIToken ParseToken(string response, int beginIndex, out int endIndex)
        {
            endIndex = beginIndex;

            while (Char.IsDigit(response[endIndex])) {
                endIndex++;
            }

            if (beginIndex == endIndex) {
                return null;
            }

            return new MIToken(
                Convert.ToUInt64(response.Substring(beginIndex, endIndex - beginIndex), 10)
            );
        }

        MIResultClass ParseResultClass(string response, int beginIndex, out int endIndex)
        {
            var resClasses = new MIResultClass[] {
                MIResultClass.Done,
                MIResultClass.Running,
                MIResultClass.Connected,
                MIResultClass.Error,
                MIResultClass.Exit,
            };

            foreach (MIResultClass resClass in resClasses)
            {
                string strClass = resClass.ToString();
                int len = Math.Min(response.Length - beginIndex, strClass.Length);

                if (String.Compare(response, beginIndex, strClass, 0, len) == 0) {
                    endIndex = beginIndex + strClass.Length;
                    return resClass;
                }
            }

            throw new MIParserException();
        }

        MIResult ParseResult(string response, int beginIndex, out int endIndex)
        {
            endIndex = response.IndexOf('=', beginIndex);
            string variable = response.Substring(beginIndex, endIndex - beginIndex);
            MIValue miValue = ParseValue(response, endIndex + 1, out endIndex);

            return new MIResult(variable, miValue);
        }

        MIValue ParseValue(string response, int beginIndex, out int endIndex)
        {
            if (response[beginIndex] == '{') {
                 return ParseTuple(response, beginIndex, out endIndex);
            } else if (response[beginIndex] == '[') {
                return ParseList(response, beginIndex, out endIndex);
            } else if (response[beginIndex] == '"') {
                return ParseConst(response, beginIndex, out endIndex);
            }

            throw new MIParserException();
        }

        MITuple ParseTuple(string response, int beginIndex, out int endIndex)
        {
            beginIndex++; // eat '{'

            if (response[beginIndex] == '}') {
                endIndex = beginIndex + 1;
                return new MITuple(null);
            }

            var results = new List<MIResult>();
            results.Add(ParseResult(response, beginIndex, out endIndex));

            while (response[endIndex] == ',') {
                results.Add(ParseResult(response, endIndex + 1, out endIndex));
            }

            if (response[endIndex] == '}') {
                endIndex++;
                return new MITuple(results.ToArray());
            }

            throw new MIParserException();
        }

        MIListElement ParseListElement(MIListElementType type, string response, int beginIndex, out int endIndex)
        {
            switch (type) {
            case MIListElementType.Value:
                return ParseValue(response, beginIndex, out endIndex);
            case MIListElementType.Result:
                return ParseResult(response, beginIndex, out endIndex);
            }

            throw new MIParserException();
        }

        MIList ParseList(string response, int beginIndex, out int endIndex)
        {
            var elements = new List<MIListElement>();
            MIListElementType type;

            beginIndex++; // eat '['

            if (response[beginIndex] == ']') {
                endIndex = beginIndex + 1;
                // Element type of empty list can be either
                return new MIList(elements, MIListElementType.Value);
            }

            if (response[beginIndex] == '{' ||
                response[beginIndex] == '[' ||
                response[beginIndex] == '"'
            ) {
                type = MIListElementType.Value;
            } else {
                type = MIListElementType.Result;
            }

            elements.Add(ParseListElement(type, response, beginIndex, out endIndex));

            while (response[endIndex] == ',') {
                elements.Add(ParseListElement(type, response, endIndex + 1, out endIndex));
            }

            if (response[endIndex] == ']') {
                endIndex++;
                return new MIList(elements, type);
            }

            throw new MIParserException();
        }

        MIConst ParseConst(string response, int beginIndex, out int endIndex)
        {
            for (endIndex = beginIndex + 1; endIndex < response.Length; endIndex++) {
                if (response[endIndex] == '"' && response[endIndex - 1] != '\\') {
                    break;
                }
            }

            var cstring = response.Substring(beginIndex + 1, endIndex - beginIndex - 1);

            endIndex++;

            return new MIConst(cstring);
        }

        MIOutOfBandRecord ParseAsyncRecord(string response, int beginIndex, out int endIndex)
        {
            MIToken token = ParseToken(response, beginIndex, out endIndex);
            MIAsyncRecordClass asyncRecordClass = ParseAsyncRecordClass(response, endIndex, out endIndex);
            MIAsyncOutput asyncOutput = ParseAsyncOutput(response, endIndex, out endIndex);

            return new MIAsyncRecord(token, asyncRecordClass, asyncOutput);
        }

        MIAsyncRecordClass ParseAsyncRecordClass(string response, int beginIndex, out int endIndex)
        {
            endIndex = beginIndex + 1;

            switch (response[beginIndex]) {
            case '*': return MIAsyncRecordClass.Exec;
            case '+': return MIAsyncRecordClass.Status;
            case '=': return MIAsyncRecordClass.Notify;
            }

            throw new MIParserException();
        }

        MIAsyncOutput ParseAsyncOutput(string response, int beginIndex, out int endIndex)
        {
            MIAsyncOutputClass asyncClass = ParseAsyncOutputClass(response, beginIndex, out endIndex);

            List<MIResult>results = new List<MIResult>();

            while (endIndex != response.Length) {
                if (response[endIndex] != ',') {
                    break;
                }

                results.Add(ParseResult(response, endIndex + 1, out endIndex));
            }

            return new MIAsyncOutput(asyncClass, results.ToArray());
        }

        MIAsyncOutputClass ParseAsyncOutputClass(string response, int beginIndex, out int endIndex)
        {
            endIndex = beginIndex;

            while (endIndex < response.Length) {
                if (response[endIndex] == ',') {
                    break;
                }
                endIndex++;
            }

            string strClass = response.Substring(beginIndex, endIndex - beginIndex);

            if (strClass == "stopped") {
                return MIAsyncOutputClass.Stopped;
            }

            return MIAsyncOutputClass.Others(strClass);
        }

        MIStreamRecord ParseStreamRecord(string response, int beginIndex, out int endIndex)
        {
            MIStreamRecordClass streamRecordClass =
                ParseStreamRecordClass(response, beginIndex, out endIndex);
            MIConst constant = ParseConst(response, endIndex, out endIndex);

            return new MIStreamRecord(streamRecordClass, constant);
        }

        MIStreamRecordClass ParseStreamRecordClass(string response, int beginIndex, out int endIndex)
        {
            endIndex = beginIndex + 1;

            switch (response[beginIndex]) {
            case '~': return MIStreamRecordClass.Console;
            case '@': return MIStreamRecordClass.Target;
            case '&': return MIStreamRecordClass.Log;
            }

            throw new MIParserException();
        }

        bool IsOutOfBandRecord(string response)
        {
            return IsStreamRecord(response) ||
                   IsAsyncRecord(response);
        }

        bool IsStreamRecord(string response)
        {
            return response[0] == '~' ||
                   response[0] == '@' ||
                   response[0] == '&';
        }

        bool IsAsyncRecord(string response)
        {
            int i = 0;
            while (Char.IsDigit(response[i])) {
                i++;
            }

            return response[i] == '*' ||
                   response[i] == '+' ||
                   response[i] == '=';
        }

        bool IsResultRecord(string response)
        {
            int i = 0;
            while (Char.IsDigit(response[i])) {
                i++;
            }

            return response[i] == '^';
        }

        public bool IsEnd(string response)
        {
            return response == "(gdb)";
        }
    }
}
