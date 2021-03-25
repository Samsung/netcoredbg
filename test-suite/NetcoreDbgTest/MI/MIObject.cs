using System;
using System.Text;
using System.Collections;
using System.Collections.Generic;

namespace NetcoreDbgTest.MI
{
    public enum MIValueType
    {
        Const,
        Tuple,
        List,
    }

    public enum MIListElementType
    {
        Value,
        Result,
    }

    public enum MIOutOfBandRecordType
    {
        Async,
        Stream,
    }

    public class MIOutput
    {
        public MIOutput(MIOutOfBandRecord[] outOfBandRecords,
                        MIResultRecord resultRecord)
        {
            OutOfBandRecords = outOfBandRecords;
            ResultRecord = resultRecord;
        }

        public MIOutOfBandRecord[] OutOfBandRecords;
        public MIResultRecord ResultRecord;
    }

    public class MIResultRecord
    {
        public MIResultRecord(MIToken token,
                              MIResultClass resultClass,
                              MIResult[] results)
        {
            Token = token;
            Class = resultClass;
            Results = new Dictionary<string, MIValue>();

            if (results == null) {
                return;
            }

            foreach (MIResult result in results) {
                Results.Add(result.Variable, result.Value);
            }
        }

        public override string ToString()
        {
            var sb = new StringBuilder ();

            if (Token != null) {
                sb.Append(Token.ToString());
            }

            sb.Append("^");

            sb.Append(Class.ToString());

            foreach (KeyValuePair<string, MIValue>pair in Results) {
                sb.Append("," + pair.Value.ToString());
            }

            return sb.ToString();
        }

        public MIValue this[string variable]
        {
            get { return Results[variable]; }
        }

        public MIToken Token = null;
        public MIResultClass Class = MIResultClass.Done;
        Dictionary<string, MIValue> Results;
    }

    public class MIOutOfBandRecord
    {
        public MIOutOfBandRecord(MIOutOfBandRecordType type)
        {
            Type = type;
        }

        public override string ToString()
        {
            return null;
        }

        public MIOutOfBandRecordType Type;
    }

    public class MIToken
    {
        public MIToken(ulong number)
        {
            Number = number;
        }

        public override string ToString()
        {
            return Number.ToString();
        }

        public ulong Number;
    }

    public class MIListElement
    {
        public MIListElement(MIListElementType type)
        {
            ElementType = type;
        }

        public override string ToString()
        {
            return null;
        }

        public MIListElementType ElementType;
    }

    public class MIValue : MIListElement
    {
        public MIValue(MIValueType type)
            : base(MIListElementType.Value)
        {
        }

        public override string ToString()
        {
            return null;
        }

        public MIListElementType Type;
    }

    public class MITuple : MIValue, IEnumerable
    {
        public MITuple() : base(MIValueType.Tuple)
        {
            Results = new Dictionary<string, MIValue>();
        }

        public MITuple(MIResult[] results) : base(MIValueType.Tuple)
        {
            Results = new Dictionary<string, MIValue>();

            if (results == null) {
                return;
            }

            foreach (MIResult result in results) {
                Results.Add(result.Variable, result.Value);
            }
        }

        public override string ToString()
        {
            bool firstElement = true;
            var sb = new StringBuilder("{");

            foreach (KeyValuePair<string, MIValue> pair in Results)
            {
                if (firstElement) {
                    firstElement = false;
                } else {
                    sb.Append(",");
                }

                sb.Append(pair.Key + "=" + pair.Value.ToString());
            }

            sb.Append("}");

            return sb.ToString();
        }

        public void Add(string variable, MIValue val)
        {
            Results.Add(variable, val);
        }

        public void Add(string variable, string val)
        {
            Results.Add(variable, new MIConst(val));
        }

        public MIValue this[string Variable]
        {
            get {
                return Results[Variable];
            }
        }

        public IEnumerator GetEnumerator()
        {
            return Results.GetEnumerator();
        }

        Dictionary<string, MIValue>Results;
    }

    public class MIList : MIValue, IEnumerable
    {
        public MIList() : base(MIValueType.List)
        {
            Elements = new List<MIListElement>();
        }

        public MIList(List<MIListElement> elements, MIListElementType elementType)
            : base(MIValueType.List)
        {
            ElementsType = elementType;
            Elements = elements;
        }

        public MIList(MIValue[] values) : base(MIValueType.List)
        {
            ElementsType = MIListElementType.Value;

            Elements = new List<MIListElement>(values);
        }

        public MIListElement this[int index]
        {
            get {
                return Elements[index];
            }
        }

        public void Add(string cstring)
        {
            if (Elements.Count == 0) {
                ElementsType = MIListElementType.Value;
            }

            Elements.Add(new MIConst(cstring));
        }

        public void Add(MIListElement element)
        {
            if (Elements.Count == 0) {
                ElementsType = element.ElementType;
            }

            if (ElementsType != element.ElementType) {
                throw new Exception();
            }

            Elements.Add(element);
        }

        public IEnumerator GetEnumerator()
        {
            return Elements.GetEnumerator();
        }

        public override string ToString()
        {
            bool firstElement = true;
            var sb = new StringBuilder("[");

            foreach (MIListElement element in Elements) {
                if (firstElement) {
                    firstElement = false;
                } else {
                    sb.Append(",");
                }

                sb.Append(element.ToString());
            }

            sb.Append("]");

            return sb.ToString();
        }

        public MIListElement[] ToArray()
        {
            return Elements.ToArray();
        }

        public int Count
        {
            get { return Elements.Count; }
        }

        MIListElementType ElementsType;
        List<MIListElement> Elements;
    }

    public class MIConst : MIValue
    {
        public MIConst(string cstring) : base(MIValueType.Const)
        {
            CString = cstring;
        }

        public override string ToString()
        {
            return "\"" + CString + "\"";
        }

        public string CString;

        // return c-string without escape sequences
        // https://en.wikipedia.org/wiki/Escape_sequences_in_C
        // throw exception for invalid c-string
        public string String
        {
            get {
                var sb = new StringBuilder();
                try {
                    for (int i = 0; i < CString.Length; ) {
                        if (CString[i] == '\\') {
                            char c;
                            int hex;
                            switch (CString[i + 1]) {
                            case 'a':
                                c = '\a'; i += 2; break;
                            case 'b':
                                c = '\b'; i += 2; break;
                            case 'f':
                                c = '\f'; i += 2; break;
                            case 'n':
                                c = '\n'; i += 2; break;
                            case 'r':
                                c = '\r'; i += 2; break;
                            case 't':
                                c = '\t'; i += 2; break;
                            case 'v':
                                c = '\v'; i += 2; break;
                            case '\\':
                                c = '\\'; i += 2; break;
                            case '\'':
                                c = '\''; i += 2; break;
                            case '\"':
                                c = '\"'; i += 2; break;
                            case '?':
                                c = '\u003f'; i += 2; break;
                            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                                int num2 = CString[i + 2] - '0';
                                int num1 = CString[i + 3] - '0';
                                int num0 = CString[i + 4] - '0';
                                c = (char)(num2 * 64 + num1 * 8 + num0);
                                i += 5;
                                break;
                            case 'e':
                                c = '\u001b'; i += 2; break;
                            case 'U':
                                hex = Int32.Parse(CString.Substring(i + 2, i + 10),
                                                  System.Globalization.NumberStyles.HexNumber);
                                c = (char)hex;
                                i += 11;
                                break;
                            case 'u':
                                hex = Int32.Parse(CString.Substring(i + 2, i + 6),
                                                  System.Globalization.NumberStyles.HexNumber);
                                c = (char)hex;
                                i += 7;
                                break;
                            default:
                                throw new FormatException();
                            }
                            sb.Append(c);
                        } else {
                            sb.Append(CString[i]);
                            i++;
                        }
                    }
                }
                catch {
                    throw new FormatException("Invalid c-string");
                }

                return sb.ToString();
            }
        }

        public int Int
        {
            get { return Int32.Parse(CString); }
        }
    }

    public class MIResult : MIListElement
    {
        public MIResult(string variable, MIValue val) : base(MIListElementType.Result)
        {
            Variable = variable;
            Value = val;
        }

        public MIResult(string variable, string cstring) : base(MIListElementType.Result)
        {
            Variable = variable;
            Value = new MIConst(cstring);
        }

        public override string ToString()
        {
            return Variable + "=" + Value.ToString();
        }

        public string Variable;
        public MIValue Value;
    }

    public class MIAsyncRecord : MIOutOfBandRecord
    {
        public MIAsyncRecord(MIToken token, MIAsyncRecordClass cl, MIAsyncOutput output)
            : base(MIOutOfBandRecordType.Async)
        {
            Token = token;
            Class = cl;
            Output = output;
        }

        public override string ToString()
        {
            var sb = new StringBuilder();

            if (Token != null) {
                sb.Append(Token.ToString());
            }

            sb.Append(Class.ToString() + Output.ToString());

            return sb.ToString();
        }

        public MIToken Token;
        public MIAsyncRecordClass Class;
        public MIAsyncOutput Output;
    }

    public class MIStreamRecord : MIOutOfBandRecord
    {
        public MIStreamRecord(MIStreamRecordClass cl, MIConst constant)
            : base(MIOutOfBandRecordType.Stream)
        {
            Class = cl;
            Const = constant;
        }

        public override string ToString()
        {
            return Class.ToString() + Const.ToString();
        }

        public MIStreamRecordClass Class;
        public MIConst Const;
    }

    public class MIAsyncOutput
    {
        public MIAsyncOutput(MIAsyncOutputClass cl, MIResult[] results)
        {
            Class = cl;
            Results = new Dictionary<string, MIValue>();

            if (results == null) {
                return;
            }

            foreach(MIResult result in results) {
                Results.Add(result.Variable, result.Value);
            }
        }

        public override string ToString()
        {
            var sb = new StringBuilder(Class.ToString());

            foreach (KeyValuePair<string, MIValue> result in Results) {
                sb.Append("," + result.Value.ToString());
            }

            return sb.ToString();
        }

        public MIValue this[string variable]
        {
            get { return Results[variable]; }
        }

        public MIAsyncOutputClass Class;
        Dictionary<string, MIValue> Results;
    }

    public class MIResultClass
    {
        public static MIResultClass Done { get; private set; } =
            new MIResultClass("done");

        public static MIResultClass Running { get; private set; } =
            new MIResultClass("running");

        public static MIResultClass Connected { get; private set; } =
            new MIResultClass("connected");

        public static MIResultClass Error { get; private set; } =
            new MIResultClass("error");

        public static MIResultClass Exit { get; private set; } =
            new MIResultClass("exit");

        public override string ToString()
        {
            return Representation;
        }

        MIResultClass(string reprsentation)
        {
            Representation = reprsentation;
        }

        string Representation;
    }

    public class MIAsyncOutputClass
    {
        public static MIAsyncOutputClass Stopped { get; private set; } =
            new MIAsyncOutputClass("stopped");

        public static MIAsyncOutputClass Others(string representation)
        {
            return new MIAsyncOutputClass(representation);
        }

        public override string ToString()
        {
            return Represenation;
        }

        MIAsyncOutputClass(string representation)
        {
            Represenation = representation;
        }

        string Represenation;
    }

    public class MIAsyncRecordClass
    {
        public static MIAsyncRecordClass Exec { get; private set; } =
            new MIAsyncRecordClass("*");

        public static MIAsyncRecordClass Status { get; private set; } =
            new MIAsyncRecordClass("+");

        public static MIAsyncRecordClass Notify { get; private set; } =
            new MIAsyncRecordClass("=");

        public override string ToString()
        {
            return Represenation;
        }

        MIAsyncRecordClass(string represenation)
        {
            Represenation = represenation;
        }

        string Represenation;
    }

    public class MIStreamRecordClass
    {
        public override string ToString()
        {
            return Represenation;
        }

        public static MIStreamRecordClass Console { get; private set; } =
            new MIStreamRecordClass("~");

        public static MIStreamRecordClass Target { get; private set; } =
            new MIStreamRecordClass("@");

        public static MIStreamRecordClass Log { get; private set; } =
            new MIStreamRecordClass("&");

        MIStreamRecordClass(string representation)
        {
            Represenation = representation;
        }

        string Represenation;
    }
}
