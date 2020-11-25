// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.Scripting;
using Microsoft.CodeAnalysis;
using System.Reflection;
using System.Dynamic;
using Microsoft.CodeAnalysis.CSharp;
using System.Text;

namespace NetCoreDbg
{
    public class Evaluation
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct BlittableChar
        {
            public char Value;

            public static explicit operator BlittableChar(char value)
            {
                return new BlittableChar { Value = value };
            }

            public static implicit operator char (BlittableChar value)
            {
                return value.Value;
            }
        }

        public struct BlittableBoolean
        {
            private byte byteValue;

            public bool Value
            {
                get { return Convert.ToBoolean(byteValue); }
                set { byteValue = Convert.ToByte(value); }
            }

            public static explicit operator BlittableBoolean(bool value)
            {
                return new BlittableBoolean { Value = value };
            }

            public static implicit operator bool (BlittableBoolean value)
            {
                return value.Value;
            }
        }

        private static string[] basicTypes = new string[] {
            "System.Object",
            "System.Boolean",
            "System.Byte",
            "System.SByte",
            "System.Char",
            "System.Double",
            "System.Single",
            "System.Int32",
            "System.UInt32",
            "System.Int64",
            "System.UInt64",
            "System.Int16",
            "System.UInt16",
            "System.IntPtr",
            "System.UIntPtr",
            "System.Decimal",
            "System.String"
        };

        internal delegate bool GetChildDelegate(IntPtr opaque, IntPtr corValue, [MarshalAs(UnmanagedType.LPWStr)] string name, out int dataTypeId, out IntPtr dataPtr);

        private static GetChildDelegate getChild;

        internal static void RegisterGetChild(GetChildDelegate cb)
        {
            getChild = cb;
        }

        public class ContextVariable : DynamicObject
        {
            private IntPtr m_opaque;
            public IntPtr m_corValue { get; }

            public ContextVariable(IntPtr opaque, IntPtr corValue)
            {
                this.m_opaque = opaque;
                this.m_corValue = corValue;
            }

            private bool UnmarshalResult(int dataTypeId, IntPtr dataPtr, out object result)
            {
                if (dataTypeId < 0)
                {
                    result = new ContextVariable(m_opaque, dataPtr);
                    return true;
                }
                if (dataTypeId == 0) // special case for null object
                {
                    result = null;
                    return true;
                }
                if (dataTypeId >= basicTypes.Length)
                {
                    result = null;
                    return false;
                }
                Type dataType = Type.GetType(basicTypes[dataTypeId]);
                if (dataType == typeof(string))
                {
                    if (dataPtr == IntPtr.Zero)
                    {
                        result = string.Empty;
                        return true;
                    }
                    result = Marshal.PtrToStringBSTR(dataPtr);
                    Marshal.FreeBSTR(dataPtr);
                    return true;
                }
                if (dataType == typeof(char))
                {
                    BlittableChar c = Marshal.PtrToStructure<BlittableChar>(dataPtr);
                    Marshal.FreeCoTaskMem(dataPtr);
                    result = (char)c;
                    return true;
                }
                if (dataType == typeof(bool))
                {
                    BlittableBoolean b = Marshal.PtrToStructure<BlittableBoolean>(dataPtr);
                    Marshal.FreeCoTaskMem(dataPtr);
                    result = (bool)b;
                    return true;
                }
                result = Marshal.PtrToStructure(dataPtr, dataType);
                Marshal.FreeCoTaskMem(dataPtr);
                return true;
            }

            public override bool TryGetMember(
                GetMemberBinder binder, out object result)
            {
                IntPtr dataPtr;
                int dataTypeId;
                if (!getChild(m_opaque, m_corValue, binder.Name, out dataTypeId, out dataPtr))
                {
                    result = null;
                    return false;
                }
                return UnmarshalResult(dataTypeId, dataPtr, out result);
            }

            public override bool TryGetIndex(GetIndexBinder binder, object[] indexes, out object result)
            {
                IntPtr dataPtr;
                int dataTypeId;
                if (!getChild(m_opaque, m_corValue, "[" + string.Join(", ", indexes) + "]", out dataTypeId, out dataPtr))
                {
                    result = null;
                    return false;
                }
                return UnmarshalResult(dataTypeId, dataPtr, out result);
            }
        }

        public class Globals
        {
            public dynamic __context;
        }

        // Stores unresolved symbols, now only variables are supported
        // Symbols are unique in list
        class SyntaxAnalyzer
        {
            class FrameVars
            {
                Stack<string> vars = new Stack<string>();
                Stack<int> varsInFrame = new Stack<int>();
                int curFrameVars = 0;

                public void Add(string name)
                {
                    vars.Push(name);
                    curFrameVars++;
                }

                public void NewFrame()
                {
                    varsInFrame.Push(curFrameVars);
                    curFrameVars = 0;
                }

                public void ExitFrame()
                {
                    for (int i = 0; i < curFrameVars; i++)
                        vars.Pop();

                    curFrameVars = varsInFrame.Pop();
                }

                public bool Contains(string name)
                {
                    return vars.Contains(name);
                }
            }

            enum ParsingState
            {
                Common,
                InvocationExpression,
                GenericName
            };

            public List<string> unresolvedSymbols { get; private set; } = new List<string>();
            FrameVars frameVars = new FrameVars();
            SyntaxTree tree;

            public SyntaxAnalyzer(string expression)
            {
                tree = CSharpSyntaxTree.ParseText(expression, options: new CSharpParseOptions(kind: SourceCodeKind.Script));
                var root = tree.GetCompilationUnitRoot();
                foreach (SyntaxNode sn in root.ChildNodes())
                    ParseNode(sn, ParsingState.Common);
            }

            void ParseAccessNode(SyntaxNode sn, ParsingState state)
            {
                SyntaxNodeOrToken snt = sn.ChildNodesAndTokens().First();

                if (snt.Kind().Equals(SyntaxKind.SimpleMemberAccessExpression))
                    ParseAccessNode(snt.AsNode(), state);
                else if (snt.IsNode)
                    ParseNode(snt.AsNode(), state);
                else if (snt.IsToken)
                    ParseCommonToken(snt.AsToken(), state);
            }

            void ParseBlock(SyntaxNode sn, ParsingState state)
            {
                frameVars.NewFrame();
                foreach (SyntaxNode snc in sn.ChildNodes())
                    ParseNode(sn, ParsingState.Common);
                frameVars.ExitFrame();
            }

            void ParseNode(SyntaxNode sn, ParsingState state)
            {
                if (sn.Kind().Equals(SyntaxKind.InvocationExpression))
                    state = ParsingState.InvocationExpression;
                else if (sn.Kind().Equals(SyntaxKind.GenericName))
                    state = ParsingState.GenericName;
                else if (sn.Kind().Equals(SyntaxKind.ArgumentList))
                    state = ParsingState.Common;

                foreach (SyntaxNodeOrToken snt in sn.ChildNodesAndTokens())
                {
                    if (snt.IsNode)
                    {
                        if (snt.Kind().Equals(SyntaxKind.SimpleMemberAccessExpression))
                            ParseAccessNode(snt.AsNode(), state);
                        else if (snt.Kind().Equals(SyntaxKind.Block))
                            ParseBlock(snt.AsNode(), state);
                        else
                            ParseNode(snt.AsNode(), state);
                    }
                    else
                    {
                        if (sn.Kind().Equals(SyntaxKind.VariableDeclarator))
                            ParseDeclarator(snt.AsToken());
                        else
                            ParseCommonToken(snt.AsToken(), state);
                    }
                }
            }

            void ParseCommonToken(SyntaxToken st, ParsingState state)
            {
                if (state == ParsingState.InvocationExpression ||
                    state == ParsingState.GenericName)
                    return;

                if (st.Kind().Equals(SyntaxKind.IdentifierToken) &&
                    !unresolvedSymbols.Contains(st.Value.ToString()) &&
                    !frameVars.Contains(st.Value.ToString()))
                    unresolvedSymbols.Add(st.Value.ToString());
            }

            void ParseDeclarator(SyntaxToken st)
            {
                if (st.Kind().Equals(SyntaxKind.IdentifierToken) && !frameVars.Contains(st.Value.ToString()))
                    frameVars.Add(st.Value.ToString());
            }
        };

        static void MarshalValue(object value, out int size, out IntPtr data)
        {
            if (value is string)
            {
                data = Marshal.StringToBSTR(value as string);
                size = 0;
            }
            else if (value is char)
            {
                BlittableChar c = (BlittableChar)((char)value);
                size = Marshal.SizeOf(c);
                data = Marshal.AllocCoTaskMem(size);
                Marshal.StructureToPtr(c, data, false);
            }
            else if (value is bool)
            {
                BlittableBoolean b = (BlittableBoolean)((bool)value);
                size = Marshal.SizeOf(b);
                data = Marshal.AllocCoTaskMem(size);
                Marshal.StructureToPtr(b, data, false);
            }
            else
            {
                size = Marshal.SizeOf(value);
                data = Marshal.AllocCoTaskMem(size);
                Marshal.StructureToPtr(value, data, false);
            }
        }

        internal static bool EvalExpression([MarshalAs(UnmanagedType.LPWStr)] string expr, IntPtr opaque, out IntPtr errorText, out int typeId, out int size, out IntPtr result)
        {
            SyntaxAnalyzer sa = new SyntaxAnalyzer(expr);

            StringBuilder scriptText = new StringBuilder("#line hidden\n");

            // Generate prefix with variables assignment to __context members
            foreach (string us in sa.unresolvedSymbols)
                scriptText.AppendFormat("var {0} = __context.{0};\n", us);

            scriptText.Append("#line 1\n");
            scriptText.Append(expr);

            errorText = IntPtr.Zero;
            result = IntPtr.Zero;
            typeId = 0;
            size = 0;
            try
            {
                var scriptOptions = ScriptOptions.Default
                    .WithImports("System")
                    .WithReferences(typeof(Microsoft.CSharp.RuntimeBinder.CSharpArgumentInfo).Assembly);
                var script = CSharpScript.Create(scriptText.ToString(), scriptOptions, globalsType: typeof(Globals));
                script.Compile();
                var returnValue = script.RunAsync(new Globals { __context = new ContextVariable(opaque, IntPtr.Zero) }).Result.ReturnValue;
                if (returnValue is ContextVariable)
                {
                    typeId = -1;
                    result = (returnValue as ContextVariable).m_corValue;
                }
                else
                {
                    if (returnValue is null)
                    {
                        typeId = 0;
                        return true;
                    }
                    for (int i = 1; i < basicTypes.Length; i++)
                    {
                        if (returnValue.GetType() == Type.GetType(basicTypes[i]))
                        {
                            typeId = i;
                            MarshalValue(returnValue, out size, out result);
                            return true;
                        }
                    }
                    return false;
                }
            }
            catch(Exception e)
            {
                errorText = Marshal.StringToBSTR(e.ToString());
                return false;
            }
            return true;
        }

        internal static bool ParseExpression([MarshalAs(UnmanagedType.LPWStr)] string expr, [MarshalAs(UnmanagedType.LPWStr)] string resultTypeName, out IntPtr data, out int size, out IntPtr errorText)
        {
            object value = null;
            data = IntPtr.Zero;
            size = 0;
            errorText = IntPtr.Zero;
            Type resultType = Type.GetType(resultTypeName);
            if (resultType == null)
            {
                errorText = Marshal.StringToBSTR("Unknown type: " + resultTypeName);
                return false;
            }
            try
            {
                MethodInfo genericMethod = null;

                foreach (System.Reflection.MethodInfo m in typeof(CSharpScript).GetTypeInfo().GetMethods())
                {
                    if (m.Name == "EvaluateAsync" && m.ContainsGenericParameters)
                    {
                        genericMethod = m.MakeGenericMethod(resultType);
                        break;
                    }
                }

                dynamic v = genericMethod.Invoke(null, new object[]{expr, null, null, null, default(System.Threading.CancellationToken)});
                value = v.Result;

            }
            catch (TargetInvocationException e)
            {
                if (e.InnerException is CompilationErrorException)
                {
                    errorText = Marshal.StringToBSTR(string.Join(Environment.NewLine, (e.InnerException as CompilationErrorException).Diagnostics));
                }
                else
                {
                    errorText = Marshal.StringToBSTR(e.InnerException.ToString());
                }
                return false;
            }
            if (value == null)
            {
                errorText = Marshal.StringToBSTR("Value can not be null");
                return false;
            }
            MarshalValue(value, out size, out data);
            return true;
        }
    }
}
