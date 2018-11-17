// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Diagnostics;
using System.Collections;
using System.Globalization;

namespace MICore
{
    /// <summary>
    /// Prefix from the output indicating the class of result. According to the documentation, these are:
    ///
    ///    result-class ==> "done" | "running" | "connected" | "error" | "exit" | "stopped"
    /// </summary>
    public enum ResultClass
    {
        /// <summary>
        /// ResultClass is not set
        /// </summary>
        None,

        done,
        running,
        connected,
        error,
        exit,
        stopped
    }

    public class ResultValue
    {
        public virtual ResultValue Find(string name)
        {
            throw new MIResultFormatException(name, this);
        }

        public virtual bool TryFind(string name, out ResultValue result)
        {
            if (Contains(name))
            {
                result = Find(name);
            }
            else
            {
                result = null;
            }
            return result != null;
        }

        public virtual bool Contains(string name)
        {
            return false;
        }

        public ConstValue FindConst(string name)
        {
            return Find<ConstValue>(name);
        }

        public int FindInt(string name)
        {
            try
            {
                return FindConst(name).ToInt;
            }
            catch (MIResultFormatException)
            {
                throw;
            }
            catch (Exception e)
            {
                throw new MIResultFormatException(name, this, e);
            }
        }
        public uint FindUint(string name)
        {
            try
            {
                return FindConst(name).ToUint;
            }
            catch (MIResultFormatException)
            {
                throw;
            }
            catch (Exception e)
            {
                throw new MIResultFormatException(name, this, e);
            }
        }

        /// <summary>
        /// Try to find a uint property. Throws if the property can be found but is not a uint.
        /// </summary>
        /// <param name="name">[Required] name of the property to search for</param>
        /// <returns>The value of the property or null if it cannot be found</returns>
        public uint? TryFindUint(string name)
        {
            ConstValue c;
            if (!TryFind(name, out c))
            {
                return null;
            }

            try
            {
                return c.ToUint;
            }
            catch (OverflowException)
            {
                return null;
            }
            catch (MIResultFormatException)
            {
                throw;
            }
            catch (Exception e)
            {
                throw new MIResultFormatException(name, this, e);
            }
        }

        public ulong FindAddr(string name)
        {
            try
            {
                return FindConst(name).ToAddr;
            }
            catch (MIResultFormatException)
            {
                throw;
            }
            catch (Exception e)
            {
                throw new MIResultFormatException(name, this, e);
            }
        }

        /// <summary>
        /// Try and find an address property. Returns null if there is no property. Will throw if that property exists but it is not an address.
        /// </summary>
        /// <param name="name">[Required] Name of the property to look for</param>
        /// <returns>The value of the address or null if it can't be found</returns>
        public ulong? TryFindAddr(string name)
        {
            ConstValue c;
            if (!TryFind(name, out c))
            {
                return null;
            }

            try
            {
                return c.ToAddr;
            }
            catch (MIResultFormatException)
            {
                throw;
            }
            catch (Exception e)
            {
                throw new MIResultFormatException(name, this, e);
            }
        }


        public string FindString(string name)
        {
            return FindConst(name).AsString;
        }

        public string TryFindString(string name)
        {
            ConstValue c;
            if (!TryFind(name, out c))
            {
                return string.Empty;
            }
            return c.AsString;
        }

        public T Find<T>(string name) where T : ResultValue
        {
            var c = Find(name);
            if (c is T)
            {
                return c as T;
            }
            throw new MIResultFormatException(name, this);
        }

        public ResultListValue FindResultListValue(string name)
        {
            var c = Find(name);
            if (c is ResultListValue)
            {
                return c as ResultListValue;
            }
            throw new MIResultFormatException(name, this);
        }

        public bool TryFind<T>(string name, out T result) where T : ResultValue
        {
            if (Contains(name))
            {
                result = Find(name) as T;
            }
            else
            {
                result = null;
            }
            return result != null;
        }

        public T TryFind<T>(string name) where T : ResultValue
        {
            T result;
            if (!TryFind(name, out result))
            {
                return null;
            }
            return result;
        }
    }

    public class ConstValue : ResultValue
    {
        public readonly string Content;

        public ConstValue(string str)
        {
            Content = str ?? string.Empty;
        }

        public static ulong ParseAddr(string addr, bool throwOnError = false)
        {
            ulong res = 0;
            if (string.IsNullOrEmpty(addr))
            {
                if (throwOnError)
                {
                    throw new ArgumentNullException();
                }
                return 0;
            }
            else if (addr.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                if (throwOnError)
                {
                    res = ulong.Parse(addr.Substring(2), System.Globalization.NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                }
                else
                {
                    ulong.TryParse(addr.Substring(2), System.Globalization.NumberStyles.HexNumber, CultureInfo.InvariantCulture, out res);
                }
            }
            else
            {
                if (throwOnError)
                {
                    res = ulong.Parse(addr, System.Globalization.NumberStyles.Integer, CultureInfo.InvariantCulture);
                }
                else
                {
                    ulong.TryParse(addr, System.Globalization.NumberStyles.Integer, CultureInfo.InvariantCulture, out res);
                }
            }
            return res;
        }

        public static uint ParseUint(string str, bool throwOnError = false)
        {
            uint value = 0;
            if (string.IsNullOrEmpty(str))
            {
                if (throwOnError)
                {
                    throw new ArgumentException();
                }
                return value;
            }
            else if (str.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                if (throwOnError)
                {
                    value = uint.Parse(str.Substring(2), System.Globalization.NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                }
                else
                {
                    uint.TryParse(str.Substring(2), System.Globalization.NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
                }
            }
            else
            {
                if (throwOnError)
                {
                    value = uint.Parse(str, System.Globalization.NumberStyles.Integer, CultureInfo.InvariantCulture);
                }
                else
                {
                    uint.TryParse(str, System.Globalization.NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
                }
            }
            return value;
        }

        public ulong ToAddr
        {
            get
            {
                return ParseAddr(Content, throwOnError: true);
            }
        }
        public int ToInt
        {
            get
            {
                return int.Parse(Content, CultureInfo.InvariantCulture);
            }
        }
        public uint ToUint
        {
            get
            {
                return ParseUint(Content, throwOnError: true);
            }
        }

        public string AsString
        {
            get
            {
                return Content;
            }
        }

        public override string ToString()
        {
            return Content;
        }
    }

    [DebuggerDisplay("{DisplayValue,nq}", Name = "{Name,nq}")]
    [DebuggerTypeProxy(typeof(NamedResultValueTypeProxy))]
    public class NamedResultValue
    {
        internal class NamedResultValueTypeProxy
        {
            private ResultValue _value;
            public NamedResultValueTypeProxy(NamedResultValue namedResultValue)
            {
                _value = namedResultValue.Value;
            }

            [DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
            public NamedResultValue[] Content
            {
                get
                {
                    List<NamedResultValue> values = null;

                    if (_value is ValueListValue)
                    {
                        var valueListValue = (ValueListValue)_value;
                        values = new List<NamedResultValue>(valueListValue.Length);
                        for (int i = 0; i < valueListValue.Length; i++)
                        {
                            string name = string.Format(CultureInfo.InvariantCulture, "[{0}]", i); // fake the [0], [1], [2], etc...
                            values.Add(new NamedResultValue(name, valueListValue.Content[i]));
                        }
                    }
                    else if (_value is ResultListValue)
                    {
                        var resultListValue = (ResultListValue)_value;
                        var namedResultValues = resultListValue.Content.Select(value => new NamedResultValue(value));
                        values = new List<NamedResultValue>(namedResultValues);
                    }
                    else if (_value is TupleValue)
                    {
                        var tupleValue = (TupleValue)_value;
                        values = new List<NamedResultValue>(tupleValue.Content.Count);
                        tupleValue.Content.ForEach((namedResultValue) =>
                        {
                            values.Add(new NamedResultValue(namedResultValue.Name, namedResultValue.Value));
                        });
                    }

                    return values?.ToArray();
                }
            }
        }

        public string Name { get; private set; }
        public ResultValue Value { get; private set; }

        public NamedResultValue(string name, ResultValue value)
        {
            this.Name = name;
            this.Value = value;
        }

        public NamedResultValue(NamedResultValue namedResultValue) : this(namedResultValue.Name, namedResultValue.Value)
        {
        }

        internal string DisplayValue
        {
            get
            {
                if (this.Value is ConstValue)
                {
                    return string.Format(CultureInfo.InvariantCulture, "\"{0}\"", ((ConstValue)this.Value).AsString);
                }
                else if (this.Value is TupleValue)
                {
                    return string.Format(CultureInfo.InvariantCulture, "{{...}}");
                }
                else if (this.Value is ListValue)
                {
                    return string.Format(CultureInfo.InvariantCulture, "[...] count = {0}", ((ListValue)this.Value).Length);
                }

                return "<Unknown ResultValue Type>";
            }
        }

        public override string ToString()
        {
            StringBuilder builder = new StringBuilder();
            builder.Append(Name);
            builder.Append("=");
            builder.Append(Value.ToString());
            return builder.ToString();
        }
    }

    public class TupleValue : ResultValue
    {
        public List<NamedResultValue> Content { get; private set; }

        public TupleValue(List<NamedResultValue> list)
        {
            Content = list;
        }
        public override ResultValue Find(string name)
        {
            var item = Content.Find((c) => c.Name == name);
            if (item == null)
            {
                throw new MIResultFormatException(name, this);
            }
            return item.Value;
        }

        public override bool Contains(string name)
        {
            var item = Content.Find((c) => c.Name == name);
            return item != null;
        }

        public override string ToString()
        {
            StringBuilder outTuple = new StringBuilder();
            outTuple.Append("{");
            for (int i = 0; i < Content.Count; ++i)
            {
                if (i != 0)
                {
                    outTuple.Append(",");
                }
                outTuple.Append(Content[i].ToString());
            }
            outTuple.Append("}");
            return outTuple.ToString();
        }
        public ResultValue[] FindAll(string name)
        {
            return Content.FindAll((c) => c.Name == name).Select((c) => c.Value).ToArray();
        }

        public T[] FindAll<T>(string name) where T : class
        {
            return FindAll(name).OfType<T>().ToArray();
        }

        /// <summary>
        /// Creates a new TupleValue with a subset of values from this TupleValue.
        /// </summary>
        /// <param name="requiredNames">The list of names that must be added to the TupleValue.</param>
        /// <param name="optionalNames">The list of names that will be added to the TupleValue if they exist in this TupleValue.</param>
        public TupleValue Subset(IEnumerable<string> requiredNames, IEnumerable<string> optionalNames = null)
        {
            List<NamedResultValue> values = new List<NamedResultValue>();

            // Iterate the required list and add the values.
            // Will throw if a name cannot be found.
            foreach (string name in requiredNames)
            {
                values.Add(new NamedResultValue(name, this.Find(name)));
            }

            // Iterate the optional list and add the values of the name exists.
            if (null != optionalNames)
            {
                foreach (string name in optionalNames)
                {
                    ResultValue value;
                    if (this.TryFind(name, out value))
                    {
                        values.Add(new NamedResultValue(name, value));
                    }
                }
            }

            return new TupleValue(values);
        }
    }

    public abstract class ListValue : ResultValue
    {
        public abstract int Length { get; }
        public bool IsEmpty()
        {
            return this.Length == 0;
        }
    }

    public class ValueListValue : ListValue
    {
        public ResultValue[] Content { get; private set; }

        public override int Length { get { return Content.Length; } }

        public ValueListValue(List<ResultValue> list)
        {
            Content = list.ToArray();
        }
        public T[] AsArray<T>() where T : ResultValue
        {
            return Content.Cast<T>().ToArray();
        }

        public string[] AsStrings
        {
            get { return Content.Cast<ConstValue>().Select(c => c.AsString).ToArray(); }
        }

        public override string ToString()
        {
            StringBuilder outList = new StringBuilder();
            outList.Append("[");
            for (int i = 0; i < Content.Length; ++i)
            {
                if (i != 0)
                {
                    outList.Append(",");
                }
                outList.Append(Content[i].ToString());
            }
            outList.Append("]");
            return outList.ToString();
        }
    }

    public class ResultListValue : ListValue
    {
        public NamedResultValue[] Content { get; private set; }

        public override int Length { get { return Content.Length; } }

        public ResultListValue(List<NamedResultValue> list)
        {
            Content = list.ToArray();
        }
        public override ResultValue Find(string name)
        {
            var item = Array.Find(Content, (c) => c.Name == name);
            if (item == null)
            {
                throw new MIResultFormatException(name, this);
            }
            return item.Value;
        }

        public override bool Contains(string name)
        {
            var item = Array.Find(Content, (c) => c.Name == name);
            return item != null;
        }

        public ResultValue[] FindAll(string name)
        {
            return Array.FindAll(Content, (c) => c.Name == name).Select((c) => c.Value).ToArray();
        }

        public T[] FindAll<T>(string name) where T : class
        {
            return FindAll(name).OfType<T>().ToArray();
        }

        public string[] FindAllStrings(string name)
        {
            return FindAll<ConstValue>(name).Select((c) => c.AsString).ToArray();
        }

        public int CountOf(string name)
        {
            return Content.Count(c => c.Name == name);
        }

        public ResultValue FindWith(string name, string value)
        {
            for (int i = 0; i < Length; i++)
                if (Content[i].Value.Find(name).ToString() == value)
                    return Content[i].Value;
            throw new MIResultFormatException(name, this);
        }

        public override string ToString()
        {
            StringBuilder outList = new StringBuilder();
            outList.Append("[");
            for (int i = 0; i < Content.Length; ++i)
            {
                if (i != 0)
                {
                    outList.Append(",");
                }
                outList.Append(Content[i].Name);
                outList.Append("=");
                outList.Append(Content[i].Value.ToString());
            }
            outList.Append("]");
            return outList.ToString();
        }
    }

    [DebuggerTypeProxy(typeof(ResultsTypeProxy))]
    [DebuggerDisplay("{ResultClass}, Length={Length}")]
    public class Results : ResultListValue
    {
        internal class ResultsTypeProxy
        {
            public ResultsTypeProxy(Results results)
            {
                this.Content = results.Content;
            }

            [DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
            public NamedResultValue[] Content { get; private set; }
        }

        public readonly ResultClass ResultClass;

        public Results(ResultClass resultsClass, List<NamedResultValue> list = null)
            : base(list ?? new List<NamedResultValue>())
        {
            ResultClass = resultsClass;
        }

        public Results Add(string name, ResultValue value)
        {
            var l = Content.ToList();
            l.Add(new NamedResultValue(name, value));
            return new Results(ResultClass, l);
        }

        public override string ToString()
        {
            StringBuilder outList = new StringBuilder();
            outList.Append("result-class: " + ResultClass.ToString());
            for (int i = 0; i < Content.Length; ++i)
            {
                outList.Append("\r\n");
                outList.Append(Content[i].Name);
                outList.Append(": ");
                outList.Append(Content[i].Value.ToString());
            }
            outList.Append("\r\n");
            return outList.ToString();
        }
    };

    public class MIResults
    {
        struct Span
        {
            static Span _emptySpan;
            public int Start { get; private set; }  // index first character in the substring
            public int Length { get; private set; } // length of the substring
            public int Extent { get { return Start + Length; } }
            public bool IsEmpty { get { return Length == 0; } }
            public static Span Empty { get { return _emptySpan; } }

            static Span()
            {
                _emptySpan = new Span(0, 0);
            }

            public Span(string s)
            {
                Start = 0;
                Length = s.Length;
            }
            public Span(int start, int len)
            {
                if (start < 0)
                {
                    throw new ArgumentException("start");
                }
                Start = start;
                Length = len;
            }
            public Span Advance(int len)
            {
                if (len > Length)
                {
                    throw new ArgumentException("len");
                }
                return new Span(Start + len, Length - len);
            }
            public Span AdvanceTo(int pos)
            {
                if (Start > pos || pos > Start + Length)
                {
                    throw new ArgumentException("pos");
                }
                return new Span(pos, Length - (pos - Start));
            }
            public Span Prefix(int len)
            {
                if (len > Length)
                {
                    throw new ArgumentException("len");
                }
                return new Span(Start, len);
            }
            public string Extract(string theString)
            {
                if (Extent > theString.Length)
                {
                    throw new ArgumentException("theSpan");
                }
                return theString.Substring(Start, Length);
            }
            public int IndexOf(string theString, char c)
            {
                int i = theString.IndexOf(c, Start);
                if (i < 0 || i >= Extent)
                {
                    return -1;
                }
                return i - Start;   // Span relative offset
            }
            public bool StartsWith(string theString, string pattern)
            {
                if (Length < pattern.Length)
                {
                    return false;
                }
                for (int i = 0; i < pattern.Length; ++i)
                {
                    if (theString[Start + i] != pattern[i])
                    {
                        return false;
                    }
                }
                return true;
            }
        }

        private string _resultString;

        /// <summary>
        /// result-record ==> result-class ( "," result )*
        /// </summary>
        /// <param name="output"></param>
        public Results ParseCommandOutput(string output)
        {
            string token = ParseToken(ref output);
            output = output.Remove(0, 1); // Remove first char (^, *, e.t.c)
            _resultString = output.Trim();
            int comma = _resultString.IndexOf(',');
            Results results;
            ResultClass resultClass = ResultClass.None;
            if (comma < 0)
            {
                // no comma, so entire string should be the result class
                results = new Results(ParseResultClass(output), new List<NamedResultValue>());
            }
            else
            {
                resultClass = ParseResultClass(output.Substring(0, comma));
                Span wholeString = new Span(_resultString);
                results = ParseResultList(wholeString.AdvanceTo(comma + 1), resultClass);
            }
            return results;
        }

        private string ParseToken(ref string output)
        {
            if (char.IsDigit(output, 0))
            {
                int i;
                for (i = 1; i < output.Length; i++)
                {
                    if (!char.IsDigit(output, i))
                        break;
                }
                if (i < output.Length)
                {
                    string token = output.Substring(0, i);
                    output = output.Substring(i);
                    return token;
                }
            }
            return null;
        }

        public Results ParseResultList(string listStr, ResultClass resultClass = ResultClass.None)
        {
            _resultString = listStr.Trim();
            return ParseResultList(new Span(_resultString), resultClass);
        }

        private Results ParseResultList(Span listStr, ResultClass resultClass = ResultClass.None)
        {
            Span rest;
            var list = ParseResultList((Span s, ref int i) =>
                {
                    return true;
                }, (Span s, ref int i) =>
                    {
                        return i == s.Extent;
                    }, listStr, out rest);
            if (!rest.IsEmpty)
            {
                ParseError("trailing chars", rest);
                return null;
            }
            return new Results(resultClass, list);
        }

        public string ParseCString(string input)
        {
            if (input == null)
            {
                throw new ArgumentNullException("input");
            }
            else if (input == String.Empty)
            {
                return string.Empty;
            }

            string cstr = input.Trim();
            if (cstr[0] != '\"')   // not a Cstring, just return the string
            {
                return input;
            }
            _resultString = cstr;
            Span rest;
            var s = ParseCString(new Span(cstr), out rest);
            return s == null ? string.Empty : s.AsString;
        }

        private string ParseCString(Span input)
        {
            if (input.IsEmpty)
            {
                return string.Empty;
            }

            if (_resultString[input.Start] != '\"')   // not a Cstring, just return the string
            {
                return input.Extract(_resultString);
            }
            Span rest;
            var s = ParseCString(input, out rest);
            return s == null ? string.Empty : s.AsString;
        }

        /// <summary>
        /// value ==>const | tuple | list
        /// </summary>
        /// <returns></returns>
        private ResultValue ParseValue(Span resultStr, out Span rest)
        {
            ResultValue value = null;
            rest = Span.Empty;
            if (resultStr.IsEmpty)
            {
                return null;
            }
            switch (_resultString[resultStr.Start])
            {
                case '\"':
                    value = ParseCString(resultStr, out rest);
                    break;
                case '{':
                    value = ParseTuple(resultStr, out rest);
                    break;
                case '[':
                    value = ParseList(resultStr, out rest);
                    break;
                default:
                    ParseError("unexpected char", resultStr);
                    break;
            }
            return value;
        }

        /// <summary>
        /// GDB (on x86) sometimes returns a tuple list in a context requiring a tuple (&lt;MULTIPLE&gt; breakpoints).
        /// The grammar does not allow this, but we recognize it and accept it only in the special case when it is contained
        /// in a result value.
        ///     tuplelist --  tuple ("," tuple)*
        ///     value -- const | tuple | tuplelist | list
        /// </summary>
        /// <returns></returns>
        private ResultValue ParseResultValue(Span resultStr, out Span rest)
        {
            ResultValue value = null;
            rest = Span.Empty;
            if (resultStr.IsEmpty)
            {
                return null;
            }
            switch (_resultString[resultStr.Start])
            {
                case '\"':
                    value = ParseCString(resultStr, out rest);
                    break;
                case '{':
                    value = ParseResultTuple(resultStr, out rest);
                    break;
                case '[':
                    value = ParseList(resultStr, out rest);
                    break;
                default:
                    ParseError("unexpected char", resultStr);
                    break;
            }
            return value;
        }

        /// <summary>
        /// IsValueChar - true is the char is a start-char for a value
        /// </summary>
        private static bool IsValueChar(char c)
        {
            return c == '\"' || c == '{' || c == '[';
        }

        /// <summary>
        /// result ==> variable "=" value
        /// </summary>
        /// <param name="resultStr">trimmed input string</param>
        /// <param name="rest">trimmed remainder after result</param>
        private NamedResultValue ParseResult(Span resultStr, out Span rest)
        {
            rest = Span.Empty;
            int equals = resultStr.IndexOf(_resultString, '=');
            if (equals < 1)
            {
                ParseError("variable not found", resultStr);
                return null;
            }
            string name = resultStr.Prefix(equals).Extract(_resultString);
            ResultValue value = ParseResultValue(resultStr.Advance(equals + 1), out rest);
            if (value == null)
            {
                return null;
            }
            return new NamedResultValue(name, value);
        }

        private static ResultClass ParseResultClass(string resultClass)
        {
            switch (resultClass)
            {
                case "done": return ResultClass.done;
                case "running": return ResultClass.running;
                case "connected": return ResultClass.connected;
                case "error": return ResultClass.error;
                case "exit": return ResultClass.exit;
                case "stopped": return ResultClass.stopped;
                default:
                    {
                        Debug.Fail("unexpected result class");
                        return ResultClass.None;
                    }
            }
        }

        private ConstValue ParseCString(Span input, out Span rest)
        {
            rest = input;
            StringBuilder output = new StringBuilder();
            if (input.IsEmpty || _resultString[input.Start] != '\"')
            {
                ParseError("Cstring expected", input);
                return null;
            }
            int i = input.Start + 1;
            bool endFound = false;
            for (; i < input.Extent; i++)
            {
                char c = _resultString[i];
                if (c == '\"')
                {
                    // closing quote, so we are (probably) done
                    i++;
                    if ((i < input.Extent) && (_resultString[i] == c))
                    {
                        // double quotes mean we emit a single quote, and carry on
                        ;
                    }
                    else
                    {
                        endFound = true;
                        break;
                    }
                }
                else if (c == '\\')
                {
                    // escaped character
                    c = _resultString[++i];
                    switch (c)
                    {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        default: break;
                    }
                }
                output.Append(c);
            }
            if (!endFound)
            {
                ParseError("CString not terminated", input);
                return null;
            }
            rest = input.AdvanceTo(i);
            return new ConstValue(output.ToString());
        }

        private delegate bool EdgeCondition(Span s, ref int i);

        private List<NamedResultValue> ParseResultList(EdgeCondition begin, EdgeCondition end, Span input, out Span rest)
        {
            rest = Span.Empty;
            List<NamedResultValue> list = new List<NamedResultValue>();
            int i = input.Start;
            if (!begin(input, ref i))
            {
                ParseError("Unexpected opening character", input);
                return null;
            }
            if (end(input, ref i))    // tuple is empty
            {
                rest = input.AdvanceTo(i);  // eat through the closing brace
                return list;
            }
            input = input.AdvanceTo(i);
            var item = ParseResult(input, out rest);
            if (item == null)
            {
                ParseError("Result expected", input);
                return null;
            }
            list.Add(item);
            input = rest;
            while (!input.IsEmpty && _resultString[input.Start] == ',')
            {
                item = ParseResult(input.Advance(1), out rest);
                if (item == null)
                {
                    ParseError("Result expected", input);
                    return null;
                }
                list.Add(item);
                input = rest;
            }

            i = input.Start;
            if (!end(input, ref i))    // tuple is not closed
            {
                ParseError("Unexpected list termination", input);
                rest = Span.Empty;
                return null;
            }
            rest = input.AdvanceTo(i);
            return list;
        }

        private List<NamedResultValue> ParseResultList(char begin, char end, Span input, out Span rest)
        {
            return ParseResultList((Span s, ref int i) =>
            {
                if (_resultString[i] == begin)
                {
                    i++;
                    return true;
                }
                return false;
            }, (Span s, ref int i) =>
            {
                if (i < s.Extent && _resultString[i] == end)
                {
                    i++;
                    return true;
                }
                return false;
            }, input, out rest);
        }

        /// <summary>
        /// tuple ==> "{}" | "{" result ( "," result )* "}"
        /// </summary>
        /// <returns>if one tuple found a TupleValue, otherwise a ValueListValue of TupleValues</returns>
        private ResultValue ParseResultTuple(Span input, out Span rest)
        {
            var list = ParseResultList('{', '}', input, out rest);
            if (list == null)
            {
                return null;
            }
            var tlist = new List<ResultValue>();
            TupleValue v;
            while (rest.StartsWith(_resultString, ",{"))
            {
                // a tuple list
                v = new TupleValue(list);
                tlist.Add(v);
                list = ParseResultList('{', '}', rest.Advance(1), out rest);
            }
            v = new TupleValue(list);
            if (tlist.Count != 0)
            {
                tlist.Add(v);
                return new ValueListValue(tlist);
            }
            return v;
        }

        /// <summary>
        /// tuple ==> "{}" | "{" result ( "," result )* "}"
        /// </summary>
        private TupleValue ParseTuple(Span input, out Span rest)
        {
            var list = ParseResultList('{', '}', input, out rest);
            if (list == null)
            {
                return null;
            }
            return new TupleValue(list);
        }

        /// <summary>
        /// list ==> "[]" | "[" value ( "," value )* "]" | "[" result ( "," result )* "]"
        /// </summary>
        private ResultValue ParseList(Span input, out Span rest)
        {
            rest = Span.Empty;
            if (_resultString[input.Start] != '[')
            {
                ParseError("List expected", input);
                return null;
            }
            if (_resultString[input.Start + 1] == ']')    // list is empty
            {
                rest = input.Advance(2);  // eat through the closing brace
                return new ValueListValue(new List<ResultValue>());
            }
            if (IsValueChar(_resultString[input.Start + 1]))
            {
                return ParseValueList(input, out rest);
            }
            else
            {
                return ParseResultList(input, out rest);
            }
        }

        /// <summary>
        /// list ==>  "[" value ( "," value )* "]"
        /// </summary>
        private ValueListValue ParseValueList(Span input, out Span rest)
        {
            rest = Span.Empty;
            List<ResultValue> list = new List<ResultValue>();
            if (_resultString[input.Start] != '[')
            {
                ParseError("List expected", input);
                return null;
            }
            input = input.Advance(1);
            var item = ParseValue(input, out rest);
            if (item == null)
            {
                ParseError("Value expected", input);
                return null;
            }
            list.Add(item);
            input = rest;
            while (!input.IsEmpty && _resultString[input.Start] == ',')
            {
                item = ParseValue(input.Advance(1), out rest);
                if (item == null)
                {
                    ParseError("Value expected", input);
                    return null;
                }
                list.Add(item);
                input = rest;
            }

            if (input.IsEmpty || _resultString[input.Start] != ']')    // list is not closed
            {
                ParseError("List not terminated", input);
                rest = Span.Empty;
                return null;
            }
            rest = input.Advance(1);
            return new ValueListValue(list);
        }

        /// <summary>
        /// list ==>  "[" result ( "," result )* "]"
        /// </summary>
        private ResultListValue ParseResultList(Span input, out Span rest)
        {
            var list = ParseResultList('[', ']', input, out rest);
            if (list == null)
            {
                return null;
            }
            return new ResultListValue(list);
        }

        private void ParseError(string message, Span input)
        {
            if (input.Length > 1000)
            {
                input = new Span(input.Start, 1000);    // don't show more than 1000 chars
            }
            string result = input.Extract(_resultString);
            Debug.Fail(message + ": " + result);
        }
    }
}
