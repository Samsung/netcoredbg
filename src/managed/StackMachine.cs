// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using System.Text;

namespace NetCoreDbg
{
    public partial class Evaluation
    {
        enum ePredefinedType
        {
            BoolKeyword,
            ByteKeyword,
            CharKeyword,
            DecimalKeyword,
            DoubleKeyword,
            FloatKeyword,
            IntKeyword,
            LongKeyword,
            ObjectKeyword,
            SByteKeyword,
            ShortKeyword,
            StringKeyword,
            UShortKeyword,
            UIntKeyword,
            ULongKeyword
        }

        static readonly Dictionary<Type, ePredefinedType> TypeAlias = new Dictionary<Type, ePredefinedType>
        {
            { typeof(bool), ePredefinedType.BoolKeyword },
            { typeof(byte), ePredefinedType.ByteKeyword },
            { typeof(char), ePredefinedType.CharKeyword },
            { typeof(decimal), ePredefinedType.DecimalKeyword },
            { typeof(double), ePredefinedType.DoubleKeyword },
            { typeof(float), ePredefinedType.FloatKeyword },
            { typeof(int), ePredefinedType.IntKeyword },
            { typeof(long), ePredefinedType.LongKeyword },
            { typeof(object), ePredefinedType.ObjectKeyword },
            { typeof(sbyte), ePredefinedType.SByteKeyword },
            { typeof(short), ePredefinedType.ShortKeyword },
            { typeof(string), ePredefinedType.StringKeyword },
            { typeof(ushort), ePredefinedType.UShortKeyword },
            { typeof(uint), ePredefinedType.UIntKeyword },
            { typeof(ulong), ePredefinedType.ULongKeyword }
        };

        static readonly Dictionary<SyntaxKind, ePredefinedType> TypeKindAlias = new Dictionary<SyntaxKind, ePredefinedType>
        {
            { SyntaxKind.BoolKeyword,    ePredefinedType.BoolKeyword },
            { SyntaxKind.ByteKeyword,    ePredefinedType.ByteKeyword },
            { SyntaxKind.CharKeyword,    ePredefinedType.CharKeyword },
            { SyntaxKind.DecimalKeyword, ePredefinedType.DecimalKeyword },
            { SyntaxKind.DoubleKeyword,  ePredefinedType.DoubleKeyword },
            { SyntaxKind.FloatKeyword,   ePredefinedType.FloatKeyword },
            { SyntaxKind.IntKeyword,     ePredefinedType.IntKeyword },
            { SyntaxKind.LongKeyword,    ePredefinedType.LongKeyword },
            { SyntaxKind.ObjectKeyword,  ePredefinedType.ObjectKeyword },
            { SyntaxKind.SByteKeyword,   ePredefinedType.SByteKeyword },
            { SyntaxKind.ShortKeyword,   ePredefinedType.ShortKeyword },
            { SyntaxKind.StringKeyword,  ePredefinedType.StringKeyword },
            { SyntaxKind.UShortKeyword,  ePredefinedType.UShortKeyword },
            { SyntaxKind.UIntKeyword,    ePredefinedType.UIntKeyword },
            { SyntaxKind.ULongKeyword,   ePredefinedType.ULongKeyword }
        };

        public enum eOpCode
        {
            IdentifierName,
            GenericName,
            InvocationExpression,
            ObjectCreationExpression,
            ElementAccessExpression,
            ElementBindingExpression,
            NumericLiteralExpression,
            StringLiteralExpression,
            CharacterLiteralExpression,
            PredefinedType,
            QualifiedName,
            AliasQualifiedName,
            MemberBindingExpression,
            ConditionalExpression,
            SimpleMemberAccessExpression,
            PointerMemberAccessExpression,
            CastExpression,
            AsExpression,
            AddExpression,
            MultiplyExpression,
            SubtractExpression,
            DivideExpression,
            ModuloExpression,
            LeftShiftExpression,
            RightShiftExpression,
            BitwiseAndExpression,
            BitwiseOrExpression,
            ExclusiveOrExpression,
            LogicalAndExpression,
            LogicalOrExpression,
            EqualsExpression,
            NotEqualsExpression,
            GreaterThanExpression,
            LessThanExpression,
            GreaterThanOrEqualExpression,
            LessThanOrEqualExpression,
            IsExpression,
            UnaryPlusExpression,
            UnaryMinusExpression,
            LogicalNotExpression,
            BitwiseNotExpression,
            TrueLiteralExpression,
            FalseLiteralExpression,
            NullLiteralExpression,
            PreIncrementExpression,
            PostIncrementExpression,
            PreDecrementExpression,
            PostDecrementExpression,
            SizeOfExpression,
            TypeOfExpression,
            CoalesceExpression,
            ThisExpression
        }

        static readonly Dictionary<SyntaxKind, eOpCode> KindAlias = new Dictionary<SyntaxKind, eOpCode>
        {
            { SyntaxKind.IdentifierName,                eOpCode.IdentifierName },
            { SyntaxKind.GenericName,                   eOpCode.GenericName },
            { SyntaxKind.InvocationExpression,          eOpCode.InvocationExpression },
            { SyntaxKind.ObjectCreationExpression,      eOpCode.ObjectCreationExpression },
            { SyntaxKind.ElementAccessExpression,       eOpCode.ElementAccessExpression },
            { SyntaxKind.ElementBindingExpression,      eOpCode.ElementBindingExpression },
            { SyntaxKind.NumericLiteralExpression,      eOpCode.NumericLiteralExpression },
            { SyntaxKind.StringLiteralExpression,       eOpCode.StringLiteralExpression },
            { SyntaxKind.CharacterLiteralExpression,    eOpCode.CharacterLiteralExpression },
            { SyntaxKind.PredefinedType,                eOpCode.PredefinedType },
            { SyntaxKind.QualifiedName,                 eOpCode.QualifiedName },
            { SyntaxKind.AliasQualifiedName,            eOpCode.AliasQualifiedName },
            { SyntaxKind.MemberBindingExpression,       eOpCode.MemberBindingExpression },
            { SyntaxKind.ConditionalExpression,         eOpCode.ConditionalExpression },
            { SyntaxKind.SimpleMemberAccessExpression,  eOpCode.SimpleMemberAccessExpression },
            { SyntaxKind.PointerMemberAccessExpression, eOpCode.PointerMemberAccessExpression },
            { SyntaxKind.CastExpression,                eOpCode.CastExpression },
            { SyntaxKind.AsExpression,                  eOpCode.AsExpression },
            { SyntaxKind.AddExpression,                 eOpCode.AddExpression },
            { SyntaxKind.MultiplyExpression,            eOpCode.MultiplyExpression },
            { SyntaxKind.SubtractExpression,            eOpCode.SubtractExpression },
            { SyntaxKind.DivideExpression,              eOpCode.DivideExpression },
            { SyntaxKind.ModuloExpression,              eOpCode.ModuloExpression },
            { SyntaxKind.LeftShiftExpression,           eOpCode.LeftShiftExpression },
            { SyntaxKind.RightShiftExpression,          eOpCode.RightShiftExpression },
            { SyntaxKind.BitwiseAndExpression,          eOpCode.BitwiseAndExpression },
            { SyntaxKind.BitwiseOrExpression,           eOpCode.BitwiseOrExpression },
            { SyntaxKind.ExclusiveOrExpression,         eOpCode.ExclusiveOrExpression },
            { SyntaxKind.LogicalAndExpression,          eOpCode.LogicalAndExpression },
            { SyntaxKind.LogicalOrExpression,           eOpCode.LogicalOrExpression },
            { SyntaxKind.EqualsExpression,              eOpCode.EqualsExpression },
            { SyntaxKind.NotEqualsExpression,           eOpCode.NotEqualsExpression },
            { SyntaxKind.GreaterThanExpression,         eOpCode.GreaterThanExpression },
            { SyntaxKind.LessThanExpression,            eOpCode.LessThanExpression },
            { SyntaxKind.GreaterThanOrEqualExpression,  eOpCode.GreaterThanOrEqualExpression },
            { SyntaxKind.LessThanOrEqualExpression,     eOpCode.LessThanOrEqualExpression },
            { SyntaxKind.IsExpression,                  eOpCode.IsExpression },
            { SyntaxKind.UnaryPlusExpression,           eOpCode.UnaryPlusExpression },
            { SyntaxKind.UnaryMinusExpression,          eOpCode.UnaryMinusExpression },
            { SyntaxKind.LogicalNotExpression,          eOpCode.LogicalNotExpression },
            { SyntaxKind.BitwiseNotExpression,          eOpCode.BitwiseNotExpression },
            { SyntaxKind.TrueLiteralExpression,         eOpCode.TrueLiteralExpression },
            { SyntaxKind.FalseLiteralExpression,        eOpCode.FalseLiteralExpression },
            { SyntaxKind.NullLiteralExpression,         eOpCode.NullLiteralExpression },
            { SyntaxKind.PreIncrementExpression,        eOpCode.PreIncrementExpression },
            { SyntaxKind.PostIncrementExpression,       eOpCode.PostIncrementExpression },
            { SyntaxKind.PreDecrementExpression,        eOpCode.PreDecrementExpression },
            { SyntaxKind.PostDecrementExpression,       eOpCode.PostDecrementExpression },
            { SyntaxKind.SizeOfExpression,              eOpCode.SizeOfExpression },
            { SyntaxKind.TypeOfExpression,              eOpCode.TypeOfExpression },
            { SyntaxKind.CoalesceExpression,            eOpCode.CoalesceExpression },
            { SyntaxKind.ThisExpression,                eOpCode.ThisExpression }
        };

        internal const int S_OK = 0;
        internal const int S_FALSE = 1;
        internal const int E_INVALIDARG = unchecked((int)0x80070057);

        public abstract class ICommand
        {
            public eOpCode OpCode { get; protected set; }
            protected uint Flags;
            protected IntPtr argsStructPtr;
            public abstract IntPtr GetStructPtr();
        }

        public class NoOperandsCommand : ICommand
        {
            [StructLayout(LayoutKind.Sequential)]
            internal struct FormatF
            {
                public uint Flags;
            }

            public NoOperandsCommand(SyntaxKind kind, uint flags)
            {
                OpCode = KindAlias[kind];
                Flags = flags;
                argsStructPtr = IntPtr.Zero;
            }

            ~NoOperandsCommand()
            {
                if (argsStructPtr != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(argsStructPtr);
            }

            public override IntPtr GetStructPtr()
            {
                if (argsStructPtr != IntPtr.Zero)
                    return argsStructPtr;

                FormatF argsStruct;
                argsStruct.Flags = Flags;
                argsStructPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf<FormatF>());
                Marshal.StructureToPtr(argsStruct, argsStructPtr, false);
                return argsStructPtr;
            }

            public override string ToString()
            {
                StringBuilder sb = new StringBuilder();
                sb.AppendFormat("{0}    flags={1}", OpCode, Flags);
                return sb.ToString();
            }
        }

        public class OneOperandCommand : ICommand
        {
            [StructLayout(LayoutKind.Sequential)]
            internal struct FormatFS
            {
                public uint Flags;
                public IntPtr String;
            }
            
            [StructLayout(LayoutKind.Sequential)]
            internal struct FormatFI
            {
                public uint Flags;
                public int Int;
            }

            dynamic Argument;
            dynamic argsStruct;

            public OneOperandCommand(SyntaxKind kind, uint flags, dynamic arg)
            {
                OpCode = KindAlias[kind];
                Flags = flags;
                Argument = arg;
                argsStructPtr = IntPtr.Zero;
            }

            ~OneOperandCommand()
            {
                if (argsStructPtr == IntPtr.Zero)
                    return;

                if (argsStruct.GetType() == typeof(FormatFS))
                {
                    Marshal.FreeBSTR(argsStruct.String);
                }

                Marshal.FreeCoTaskMem(argsStructPtr);
            }

            public override IntPtr GetStructPtr()
            {
                if (argsStructPtr != IntPtr.Zero)
                    return argsStructPtr;

                if (Argument.GetType() == typeof(string) || Argument.GetType() == typeof(char))
                {
                    argsStruct = new FormatFS();
                    argsStructPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf<FormatFS>());
                    argsStruct.String = Marshal.StringToBSTR(Argument.ToString());
                }
                else if (Argument.GetType() == typeof(int) || Argument.GetType() == typeof(ePredefinedType))
                {
                    argsStruct = new FormatFI();
                    argsStructPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf<FormatFI>());
                    argsStruct.Int = (int)Argument; // Note, enum must be explicitly converted to int.
                }
                else
                {
                    throw new NotImplementedException(Argument.GetType() + " type not implemented in OneOperandCommand!");
                }

                argsStruct.Flags = Flags;
                Marshal.StructureToPtr(argsStruct, argsStructPtr, false);
                return argsStructPtr;
            }

            public override string ToString()
            {
                StringBuilder sb = new StringBuilder();
                sb.AppendFormat("{0}    flags={1}    {2}", OpCode, Flags, Argument);
                return sb.ToString();
            }
        }

        public class TwoOperandCommand : ICommand
        {
            [StructLayout(LayoutKind.Sequential)]
            internal struct FormatFIS
            {
                public uint Flags;
                public int Int;
                public IntPtr String;
            }
            
            [StructLayout(LayoutKind.Sequential)]
            internal struct FormatFIP
            {
                public uint Flags;
                public int Int;
                public IntPtr Ptr;
            }

            dynamic[] Arguments;
            dynamic argsStruct;

            public TwoOperandCommand(SyntaxKind kind, uint flags, params dynamic[] args)
            {
                OpCode = KindAlias[kind];
                Flags = flags;
                Arguments = args;
                argsStructPtr = IntPtr.Zero;
            }

            ~TwoOperandCommand()
            {
                if (argsStructPtr == IntPtr.Zero)
                    return;

                if (argsStruct.GetType() == typeof(FormatFIS))
                {
                    Marshal.FreeBSTR(argsStruct.String);
                }
                else if (argsStruct.GetType() == typeof(FormatFIP))
                {
                    Marshal.FreeCoTaskMem(argsStruct.Ptr);
                }

                Marshal.FreeCoTaskMem(argsStructPtr);
            }

            public override IntPtr GetStructPtr()
            {
                if (argsStructPtr != IntPtr.Zero)
                    return argsStructPtr;

                if (Arguments[0].GetType() == typeof(string) && Arguments[1].GetType() == typeof(int))
                {
                    argsStruct = new FormatFIS();
                    argsStructPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf<FormatFIS>());
                    argsStruct.String = Marshal.StringToBSTR(Arguments[0].ToString());
                    argsStruct.Int = (int)Arguments[1];
                }
                else if (Arguments[0].GetType() == typeof(ePredefinedType))
                {
                    argsStruct = new FormatFIP();
                    argsStructPtr = Marshal.AllocCoTaskMem(Marshal.SizeOf<FormatFIP>());
                    argsStruct.Int = (int)Arguments[0]; // Note, enum must be explicitly converted to int.
                    int size = 0;
                    IntPtr data = IntPtr.Zero;
                    MarshalValue(Arguments[1], out size, out data);
                    argsStruct.Ptr = data;
                }
                else
                {
                    throw new NotImplementedException(Arguments[0].GetType() + " + " + Arguments[1].GetType() + " pair not implemented in TwoOperandCommand!");
                }              

                argsStruct.Flags = Flags;
                Marshal.StructureToPtr(argsStruct, argsStructPtr, false);
                return argsStructPtr;
            }

            public override string ToString()
            {
                StringBuilder sb = new StringBuilder();
                sb.AppendFormat("{0}    flags={1}", OpCode, Flags);
                foreach (var arg in Arguments)
                {
                    sb.AppendFormat("    {0}", arg);
                }
                return sb.ToString();
            }
        }

        public class StackMachineProgram
        {
            public static readonly int ProgramFinished = -1;
            public static readonly int BeforeFirstCommand = -2;
            public int CurrentPosition = BeforeFirstCommand;
            public List<ICommand> Commands = new List<ICommand>();
        }

        public class SyntaxKindNotImplementedException : NotImplementedException
        {
            public SyntaxKindNotImplementedException()
            {
            }

            public SyntaxKindNotImplementedException(string message)
                : base(message)
            {
            }

            public SyntaxKindNotImplementedException(string message, Exception inner)
                : base(message, inner)
            {
            }
        }

        public class TreeWalker : CSharpSyntaxWalker
        {
            bool ExpressionStatementBody = false;
            public int ExpressionStatementCount = 0;
#if DEBUG_STACKMACHINE
            // Gather AST data for DebugText.
            List<string> ST = new List<string>();
            int CurrentNodeDepth = 0;
#endif

            // CheckedExpression/UncheckedExpression syntax kind related
            static readonly uint maskChecked = 0xFFFFFFFE;
            static readonly uint flagChecked = 0x00000001;
            static readonly uint flagUnchecked = 0x00000000;
            // Tracking current AST scope flags.
            static readonly uint defaultScopeFlags = flagUnchecked;
            Stack<uint> CurrentScopeFlags = new Stack<uint>();

            public StackMachineProgram stackMachineProgram = new StackMachineProgram();

            public override void Visit(SyntaxNode node)
            {
                if (node.Kind() == SyntaxKind.ExpressionStatement)
                {
                    ExpressionStatementCount++;
                    ExpressionStatementBody = true;
                    base.Visit(node);
                    ExpressionStatementBody = false;
                }
                else if (ExpressionStatementBody)
                {
                    // Setup flags before base.Visit() call, since all nested Kinds must have proper flags.
                    if (CurrentScopeFlags.Count == 0)
                    {
                        CurrentScopeFlags.Push(defaultScopeFlags);
                    }
                    else
                    {
                        CurrentScopeFlags.Push(CurrentScopeFlags.Peek());
                    }

                    switch (node.Kind())
                    {
                        case SyntaxKind.UncheckedExpression:
                            CurrentScopeFlags.Push((CurrentScopeFlags.Pop() & maskChecked) | flagUnchecked);
                            break;
                        case SyntaxKind.CheckedExpression:
                            CurrentScopeFlags.Push((CurrentScopeFlags.Pop() & maskChecked) | flagChecked);
                            break;
                    }
#if DEBUG_STACKMACHINE
                    CurrentNodeDepth++;

                    // Gather AST data for DebugText.
                    var indents = new String(' ', CurrentNodeDepth * 4);
                    ST.Add(indents + node.Kind() + " --- " + node.ToString());
#endif
                    // Visit nested Kinds in proper order.
                    // Note, we should setup flags before and parse Kinds after this call.
                    base.Visit(node);
#if DEBUG_STACKMACHINE
                    CurrentNodeDepth--;
#endif
                    switch (node.Kind())
                    {
                        /*
                        DefaultExpression - should not be in expression AST
                        */

                        case SyntaxKind.IdentifierName:
                        case SyntaxKind.StringLiteralExpression:
                            stackMachineProgram.Commands.Add(new OneOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), node.GetFirstToken().Value));
                            break;
/* TODO
                        case SyntaxKind.GenericName:
                            // GenericName
                            //     \ TypeArgumentList
                            //           \ types OR OmittedTypeArgument
                            int? GenericNameArgs = null;
                            bool OmittedTypeArg = false;
                            foreach (var child in node.ChildNodes())
                            {
                                if (child.Kind() != SyntaxKind.TypeArgumentList)
                                    continue;

                                GenericNameArgs = 0;

                                foreach (var ArgumentListChild in child.ChildNodes())
                                {
                                    if (ArgumentListChild.Kind() == SyntaxKind.OmittedTypeArgument)
                                    {
                                        OmittedTypeArg = true;
                                        break;
                                    }

                                    GenericNameArgs++;
                                }
                            }
                            if (GenericNameArgs == null || (GenericNameArgs < 1 && !OmittedTypeArg))
                            {
                                throw new ArgumentOutOfRangeException(node.Kind() + " must have at least one type!");
                            }
                            stackMachineProgram.Commands.Add(new TwoOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), node.GetFirstToken().Value, GenericNameArgs));
                            break;

                        case SyntaxKind.InvocationExpression:
                        case SyntaxKind.ObjectCreationExpression:
                            // InvocationExpression/ObjectCreationExpression
                            //     \ ArgumentList
                            //           \ Argument
                            int? ArgsCount = null;
                            foreach (var child in node.ChildNodes())
                            {
                                if (child.Kind() != SyntaxKind.ArgumentList)
                                    continue;

                                ArgsCount = new int();

                                foreach (var ArgumentListChild in child.ChildNodes())
                                {
                                    if (ArgumentListChild.Kind() != SyntaxKind.Argument)
                                        continue;

                                    ArgsCount++;
                                }
                            }
                            if (ArgsCount == null)
                            {
                                throw new ArgumentOutOfRangeException(node.Kind() + " must have at least one argument!");
                            }
                            stackMachineProgram.Commands.Add(new OneOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), ArgsCount));
                            break;
*/
                        case SyntaxKind.ElementAccessExpression:
                        case SyntaxKind.ElementBindingExpression:
                            // ElementAccessExpression/ElementBindingExpression
                            //     \ BracketedArgumentList
                            //           \ Argument
                            int? ElementAccessArgs = null;
                            foreach (var child in node.ChildNodes())
                            {
                                if (child.Kind() != SyntaxKind.BracketedArgumentList)
                                    continue;

                                ElementAccessArgs = new int();

                                foreach (var ArgumentListChild in child.ChildNodes())
                                {
                                    if (ArgumentListChild.Kind() != SyntaxKind.Argument)
                                        continue;

                                    ElementAccessArgs++;
                                }
                            }
                            if (ElementAccessArgs == null)
                            {
                                throw new ArgumentOutOfRangeException(node.Kind() + " must have at least one argument!");
                            }
                            stackMachineProgram.Commands.Add(new OneOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), ElementAccessArgs));
                            break;

                        case SyntaxKind.NumericLiteralExpression:
                        case SyntaxKind.CharacterLiteralExpression: // 1 wchar
                            stackMachineProgram.Commands.Add(new TwoOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), TypeAlias[node.GetFirstToken().Value.GetType()], node.GetFirstToken().Value));
                            break;
/* TODO
                        case SyntaxKind.PredefinedType:
                            stackMachineProgram.Commands.Add(new OneOperandCommand(node.Kind(), CurrentScopeFlags.Peek(), TypeKindAlias[node.GetFirstToken().Kind()]));
                            break;
*/
                        // skip, in case of stack machine program creation we don't use this kinds directly
                        case SyntaxKind.Argument:
                        case SyntaxKind.BracketedArgumentList:
                        case SyntaxKind.ConditionalAccessExpression:
/* TODO
                        case SyntaxKind.ArgumentList:
                        case SyntaxKind.TypeArgumentList:
                        case SyntaxKind.OmittedTypeArgument:
                        case SyntaxKind.ParenthesizedExpression:
                        case SyntaxKind.UncheckedExpression:
                        case SyntaxKind.CheckedExpression:
*/
                            break;

                        case SyntaxKind.SimpleMemberAccessExpression:
                        case SyntaxKind.TrueLiteralExpression:
                        case SyntaxKind.FalseLiteralExpression:
                        case SyntaxKind.NullLiteralExpression:
                        case SyntaxKind.ThisExpression:
                        case SyntaxKind.MemberBindingExpression:
/* TODO
                        case SyntaxKind.QualifiedName:
                        case SyntaxKind.AliasQualifiedName:
                        case SyntaxKind.ConditionalExpression:
                        case SyntaxKind.PointerMemberAccessExpression:
                        case SyntaxKind.CastExpression:
                        case SyntaxKind.AsExpression:
                        case SyntaxKind.AddExpression:
                        case SyntaxKind.MultiplyExpression:
                        case SyntaxKind.SubtractExpression:
                        case SyntaxKind.DivideExpression:
                        case SyntaxKind.ModuloExpression:
                        case SyntaxKind.LeftShiftExpression:
                        case SyntaxKind.RightShiftExpression:
                        case SyntaxKind.BitwiseAndExpression:
                        case SyntaxKind.BitwiseOrExpression:
                        case SyntaxKind.ExclusiveOrExpression:
                        case SyntaxKind.LogicalAndExpression:
                        case SyntaxKind.LogicalOrExpression:
                        case SyntaxKind.EqualsExpression:
                        case SyntaxKind.NotEqualsExpression:
                        case SyntaxKind.GreaterThanExpression:
                        case SyntaxKind.LessThanExpression:
                        case SyntaxKind.GreaterThanOrEqualExpression:
                        case SyntaxKind.LessThanOrEqualExpression:
                        case SyntaxKind.IsExpression:
                        case SyntaxKind.UnaryPlusExpression:
                        case SyntaxKind.UnaryMinusExpression:
                        case SyntaxKind.LogicalNotExpression:
                        case SyntaxKind.BitwiseNotExpression:
                        case SyntaxKind.PreIncrementExpression:
                        case SyntaxKind.PostIncrementExpression:
                        case SyntaxKind.PreDecrementExpression:
                        case SyntaxKind.PostDecrementExpression:
                        case SyntaxKind.SizeOfExpression:
                        case SyntaxKind.TypeOfExpression:
                        case SyntaxKind.CoalesceExpression:
*/
                            stackMachineProgram.Commands.Add(new NoOperandsCommand(node.Kind(), CurrentScopeFlags.Peek()));
                            break;

                        default:
                            throw new SyntaxKindNotImplementedException(node.Kind() + " not implemented!");
                    }

                    CurrentScopeFlags.Pop();
                }
                else
                {
                    // skip CompilationUnit, GlobalStatement and ExpressionStatement kinds
                    base.Visit(node);
                }
            }

#if DEBUG_STACKMACHINE
            public string GenerateDebugText()
            {
                // We cannot derive from sealed type 'StringBuilder' and it use platform-dependant Environment.NewLine for new line.
                // Use '\n' directly, since netcoredbg use only '\n' for new line.
                StringBuilder sb = new StringBuilder();
                sb.Append("=======================================\n");
                sb.Append("Source tree:\n");
                foreach (var line in ST)
                {
                    sb.AppendFormat("{0}\n", line);
                }
                sb.Append("=======================================\n");
                sb.Append("Stack machine commands:\n");
                foreach (var command in stackMachineProgram.Commands)
                {
                    sb.AppendFormat("    {0}\n", command.ToString());
                }

                return sb.ToString();
            }
#endif
        }

        /// <summary>
        /// Generate stack machine program by expression string.
        /// </summary>
        /// <param name="expression">expression string</param>
        /// <param name="stackProgram">stack machine program handle return</param>
        /// <param name="textOutput">BSTR with text information return</param>
        /// <returns>HResult code with execution status</returns>
        internal static int GenerateStackMachineProgram([MarshalAs(UnmanagedType.LPWStr)] string expression, out IntPtr stackProgram, out IntPtr textOutput)
        {
            stackProgram = IntPtr.Zero;
            textOutput = IntPtr.Zero;

            try
            {
                var parseOptions = CSharpParseOptions.Default.WithKind(SourceCodeKind.Script); // in order to parse individual expression
                var tree = CSharpSyntaxTree.ParseText(expression, parseOptions);

                var parseErrors = tree.GetDiagnostics(tree.GetRoot());
                StringBuilder sbTextOutput = new StringBuilder();
                bool errorDetected = false;
                foreach (var error in parseErrors)
                {
                    sbTextOutput.AppendFormat("error {0}: {1}\n", error.Id, error.GetMessage());
                    errorDetected = true;
                }

                if (errorDetected)
                {
                    textOutput = Marshal.StringToBSTR(sbTextOutput.ToString());
                    return E_INVALIDARG;
                }

                var treeWalker = new TreeWalker();
                treeWalker.Visit(tree.GetRoot());

                if (treeWalker.ExpressionStatementCount == 1)
                {
#if DEBUG_STACKMACHINE
                    textOutput = Marshal.StringToBSTR(treeWalker.GenerateDebugText());
#endif
                    GCHandle gch = GCHandle.Alloc(treeWalker.stackMachineProgram);
                    stackProgram = GCHandle.ToIntPtr(gch);
                    return S_OK;
                }
                else if (treeWalker.ExpressionStatementCount > 1)
                {
                    textOutput = Marshal.StringToBSTR("error: only one expression must be provided, expressions found: " + treeWalker.ExpressionStatementCount);
                    return E_INVALIDARG;
                }
                else
                {
                    textOutput = Marshal.StringToBSTR("error: no expression found");
                    return E_INVALIDARG;
                }
            }
            catch (SyntaxKindNotImplementedException e)
            {
                // Note, return not error but S_FALSE in case some syntax kind not implemented.
                // TODO remove this when new eval will be fully implemented
                textOutput = Marshal.StringToBSTR(e.GetType().ToString() + ": " + e.Message);
                return S_FALSE;
            }
            catch (Exception e)
            {
                textOutput = Marshal.StringToBSTR(e.GetType().ToString() + ": " + e.Message);
                return e.HResult;
            }
        }

        /// <summary>
        /// Release previously allocated memory.
        /// </summary>
        /// <param name="StackProgram">stack machine program handle returned by GenerateStackMachineProgram()</param>
        /// <returns></returns>
        internal static void ReleaseStackMachineProgram(IntPtr StackProgram)
        {
            Debug.Assert(StackProgram != IntPtr.Zero);
            try
            {
                GCHandle gch = GCHandle.FromIntPtr(StackProgram);
                gch.Free();
            }
            catch
            {
                // suppress any exceptions and continue execution
            }
        }

        /// <summary>
        /// Return next stack program command and pointer to argument's structure.
        /// Note, managed part will release Arguments unmanaged memory at object finalizer call after ReleaseStackMachineProgram() call.
        /// Native part must not release Arguments memory, allocated by managed part in this method.
        /// </summary>
        /// <param name="StackProgram">stack machine program handle returned by GenerateStackMachineProgram()</param>
        /// <param name="Command">next stack machine program command return</param>
        /// <param name="Arguments">pointer to Arguments unmanaged memory return</param>
        /// <param name="textOutput">BSTR with text information return</param>
        /// <returns>HResult code with execution status</returns>
        internal static int NextStackCommand(IntPtr StackProgram, out int Command, out IntPtr Arguments, out IntPtr textOutput)
        {
            Debug.Assert(StackProgram != IntPtr.Zero);

            Command = 0;
            Arguments = IntPtr.Zero;
            textOutput = IntPtr.Zero;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(StackProgram);
                StackMachineProgram stackProgram = (StackMachineProgram)gch.Target;

                if (stackProgram.CurrentPosition == StackMachineProgram.BeforeFirstCommand)
                {
                    stackProgram.CurrentPosition = 0;
                }
                else
                {
                     stackProgram.CurrentPosition++;
                }

                if (stackProgram.CurrentPosition >= stackProgram.Commands.Count)
                {
                    Command = StackMachineProgram.ProgramFinished;
                }
                else
                {
                    Command = (int)stackProgram.Commands[stackProgram.CurrentPosition].OpCode; // Note, enum must be explicitly converted to int.
                    Arguments = stackProgram.Commands[stackProgram.CurrentPosition].GetStructPtr();
                }

                return S_OK;
            }
            catch (Exception e)
            {
                textOutput = Marshal.StringToBSTR(e.GetType().ToString() + ": " + e.Message);
                return e.HResult;
            }
        }
    }
}
