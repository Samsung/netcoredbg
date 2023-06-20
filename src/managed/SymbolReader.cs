// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Text;

namespace NetCoreDbg
{
    public class SymbolReader
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct DebugInfo
        {
            public int lineNumber;
            public int ilOffset;
            public string fileName;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct LocalVarInfo
        {
            public int startOffset;
            public int endOffset;
            public string name;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct MethodDebugInfo
        {
            public IntPtr points;
            public int size;
            public IntPtr locals;
            public int localsSize;

        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct DbgSequencePoint
        {
            public int startLine;
            public int startColumn;
            public int endLine;
            public int endColumn;
            public int offset;
            public IntPtr document;
        }

        /// <summary>
        /// Read memory callback
        /// </summary>
        /// <returns>number of bytes read or 0 for error</returns>
        internal unsafe delegate int ReadMemoryDelegate(ulong address, byte* buffer, int count);

        private sealed class OpenedReader : IDisposable
        {
            public readonly MetadataReaderProvider Provider;
            public readonly MetadataReader Reader;

            public OpenedReader(MetadataReaderProvider provider, MetadataReader reader)
            {
                Debug.Assert(provider != null);
                Debug.Assert(reader != null);

                Provider = provider;
                Reader = reader;
            }

            public void Dispose() => Provider.Dispose();
        }

        /// <summary>
        /// Stream implementation to read debugger target memory for in-memory PDBs
        /// </summary>
        private class TargetStream : Stream
        {
            readonly ulong _address;
            readonly ReadMemoryDelegate _readMemory;

            public override long Position { get; set; }
            public override long Length { get; }
            public override bool CanSeek { get { return true; } }
            public override bool CanRead { get { return true; } }
            public override bool CanWrite { get { return false; } }

            public TargetStream(ulong address, int size, ReadMemoryDelegate readMemory)
                : base()
            {
                _address = address;
                _readMemory = readMemory;
                Length = size;
                Position = 0;
            }

            public override int Read(byte[] buffer, int offset, int count)
            {
                if (Position + count > Length)
                {
                    throw new ArgumentOutOfRangeException();
                }
                unsafe
                {
                    fixed (byte* p = &buffer[offset])
                    {
                        int read  = _readMemory(_address + (ulong)Position, p, count);
                        Position += read;
                        return read;
                    }
                }
            }

            public override long Seek(long offset, SeekOrigin origin)
            {
                switch (origin)
                {
                    case SeekOrigin.Begin:
                        Position = offset;
                        break;
                    case SeekOrigin.End:
                        Position = Length + offset;
                        break;
                    case SeekOrigin.Current:
                        Position += offset;
                        break;
                }
                return Position;
            }

            public override void Flush()
            {
            }

            public override void SetLength(long value)
            {
                throw new NotImplementedException();
            }

            public override void Write(byte[] buffer, int offset, int count)
            {
                throw new NotImplementedException();
            }
        }

        /// <summary>
        /// Quick fix for Path.GetFileName which incorrectly handles Windows-style paths on Linux
        /// </summary>
        /// <param name="pathName"> File path to be processed </param>
        /// <returns>Last component of path</returns>
        private static string GetFileName(string pathName)
        {
            int pos = pathName.LastIndexOfAny(new char[] { '/', '\\'});
            if (pos < 0)
                return pathName;

            return pathName.Substring(pos + 1);
        }

        /// <summary>
        /// Checks availability of debugging information for given assembly.
        /// </summary>
        /// <param name="assemblyPath">
        /// File path of the assembly or null if the module is in-memory or dynamic (generated by Reflection.Emit)
        /// </param>
        /// <param name="isFileLayout">type of in-memory PE layout, if true, file based layout otherwise, loaded layout</param>
        /// <param name="loadedPeAddress">
        /// Loaded PE image address or zero if the module is dynamic (generated by Reflection.Emit). 
        /// Dynamic modules have their PDBs (if any) generated to an in-memory stream 
        /// (pointed to by <paramref name="inMemoryPdbAddress"/> and <paramref name="inMemoryPdbSize"/>).
        /// </param>
        /// <param name="loadedPeSize">loaded PE image size</param>
        /// <param name="inMemoryPdbAddress">in memory PDB address or zero</param>
        /// <param name="inMemoryPdbSize">in memory PDB size</param>
        /// <param name="readMemory">read memory callback</param>
        /// <returns>Symbol reader handle or zero if error</returns>
        internal static IntPtr LoadSymbolsForModule([MarshalAs(UnmanagedType.LPWStr)] string assemblyPath, bool isFileLayout, ulong loadedPeAddress, int loadedPeSize, 
            ulong inMemoryPdbAddress, int inMemoryPdbSize, ReadMemoryDelegate readMemory)
        {
            try
            {
                TargetStream peStream = null;
                if (assemblyPath == null && loadedPeAddress != 0)
                {
                    peStream = new TargetStream(loadedPeAddress, loadedPeSize, readMemory);
                }
                TargetStream pdbStream = null;
                if (inMemoryPdbAddress != 0)
                {
                    pdbStream = new TargetStream(inMemoryPdbAddress, inMemoryPdbSize, readMemory);
                }
                OpenedReader openedReader = GetReader(assemblyPath, isFileLayout, peStream, pdbStream);
                if (openedReader != null)
                {
                    GCHandle gch = GCHandle.Alloc(openedReader);
                    return GCHandle.ToIntPtr(gch);
                }
            }
            catch
            {
            }

            return IntPtr.Zero;
        }

        /// <summary>
        /// Maps global method token to a handle local to the current delta PDB. 
        /// Debug tables referring to methods currently use local handles, not global handles. 
        /// See https://github.com/dotnet/roslyn/issues/16286
        /// </summary>
        private static MethodDefinitionHandle GetDeltaRelativeMethodDefinitionHandle(MetadataReader reader, int methodToken)
        {
            var globalHandle = (MethodDefinitionHandle)MetadataTokens.EntityHandle(methodToken);

            if (reader.GetTableRowCount(TableIndex.EncMap) == 0)
            {
                return globalHandle;
            }

            var globalDebugHandle = globalHandle.ToDebugInformationHandle();

            int rowId = 1;
            foreach (var handle in reader.GetEditAndContinueMapEntries())
            {
                if (handle.Kind == HandleKind.MethodDebugInformation)
                {
                    if (handle == globalDebugHandle)
                    {
                        return MetadataTokens.MethodDefinitionHandle(rowId);
                    }

                    rowId++;
                }
            }

            // compiler generated invalid EncMap table:
            throw new BadImageFormatException();
        }

        /// <summary>
        /// Load delta PDB file.
        /// </summary>
        /// <param name="isFileLayout">Delta PDB file path</param>
        /// <returns>Symbol reader handle or zero if error</returns>
        internal static IntPtr LoadDeltaPdb([MarshalAs(UnmanagedType.LPWStr)] string pdbPath, out IntPtr data, out int count)
        {
            data = IntPtr.Zero;
            count = 0;

            try
            {
                var pdbStream = TryOpenFile(pdbPath);
                if (pdbStream == null)
                    return IntPtr.Zero;

                var provider = MetadataReaderProvider.FromPortablePdbStream(pdbStream);
                var reader = provider.GetMetadataReader();

                OpenedReader openedReader = new OpenedReader(provider, reader);
                if (openedReader == null)
                    return IntPtr.Zero;

                var list = new List<int>();
                foreach (var handle in reader.GetEditAndContinueMapEntries())
                {
                    if (handle.Kind == HandleKind.MethodDebugInformation)
                    {
                        var methodToken = MetadataTokens.GetToken(((MethodDebugInformationHandle)handle).ToDefinitionHandle());
                        list.Add(methodToken);
                    }
                }
                if (list.Count > 0)
                {
                    var methodsArray = list.ToArray();

                    data = Marshal.AllocCoTaskMem(list.Count * 4);
                    IntPtr dataPtr = data;
                    foreach (var p in list)
                    {
                        Marshal.WriteInt32(dataPtr, p);
                        dataPtr = dataPtr + 4;
                    }
                    count = list.Count;
                }

                GCHandle gch = GCHandle.Alloc(openedReader);
                return GCHandle.ToIntPtr(gch);
            }
            catch
            {
            }

            return IntPtr.Zero;
        }

        /// <summary>
        /// Cleanup and dispose of symbol reader handle
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        internal static void Dispose(IntPtr symbolReaderHandle)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                ((OpenedReader)gch.Target).Dispose();
                gch.Free();
            }
            catch
            {
            }
        }

        internal static SequencePointCollection GetSequencePointCollection(int methodToken, MetadataReader reader)
        {
            Handle handle = GetDeltaRelativeMethodDefinitionHandle(reader, methodToken);
            if (handle.Kind != HandleKind.MethodDefinition)
                return new SequencePointCollection();

            MethodDebugInformationHandle methodDebugHandle = ((MethodDefinitionHandle)handle).ToDebugInformationHandle();
            if (methodDebugHandle.IsNil)
                return new SequencePointCollection();

            MethodDebugInformation methodDebugInfo = reader.GetMethodDebugInformation(methodDebugHandle);
            return methodDebugInfo.GetSequencePoints();
        }

        /// <summary>
        /// Find current user code sequence point by IL offset.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="ilOffset">IL offset</param>
        /// <param name="sequencePoint">sequence point return</param>
        /// <returns>"Ok" if information is available</returns>
        private static RetCode GetSequencePointByILOffset(IntPtr symbolReaderHandle, int methodToken, uint ilOffset, out DbgSequencePoint sequencePoint)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            sequencePoint.document = IntPtr.Zero;
            sequencePoint.startLine = 0;
            sequencePoint.startColumn = 0;
            sequencePoint.endLine = 0;
            sequencePoint.endColumn = 0;
            sequencePoint.offset = 0;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                SequencePointCollection sequencePoints = GetSequencePointCollection(methodToken, reader);

                SequencePoint nearestPoint = sequencePoints.GetEnumerator().Current;
                bool found = false;

                foreach (SequencePoint point in sequencePoints)
                {
                    if (found && point.Offset > ilOffset)
                        break;

                    if (point.StartLine != 0 && point.StartLine != SequencePoint.HiddenLine)
                    {
                        nearestPoint = point;
                        found = true;
                    }
                }

                if (!found)
                    return RetCode.Fail;

                var fileName = reader.GetString(reader.GetDocument(nearestPoint.Document).Name);
                sequencePoint.document = Marshal.StringToBSTR(fileName);
                sequencePoint.startLine = nearestPoint.StartLine;
                sequencePoint.startColumn = nearestPoint.StartColumn;
                sequencePoint.endLine = nearestPoint.EndLine;
                sequencePoint.endColumn = nearestPoint.EndColumn;
                sequencePoint.offset = nearestPoint.Offset;
                fileName = null;
            }
            catch
            {
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        /// <summary>
        /// Get list of all sequence points for method.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="points">result - array of sequence points</param>
        /// <param name="pointsCount">result - count of elements in array of sequence points</param>
        /// <returns>"Ok" if information is available</returns>
        private static RetCode GetSequencePoints(IntPtr symbolReaderHandle, int methodToken, out IntPtr points, out int pointsCount)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            var list = new List<DbgSequencePoint>();
            pointsCount = 0;
            points = IntPtr.Zero;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                SequencePointCollection sequencePoints = GetSequencePointCollection(methodToken, reader);

                foreach (SequencePoint p in sequencePoints)
                {
                    if (p.StartLine == 0 || p.StartLine == SequencePoint.HiddenLine)
                        continue;

                    string fileName = reader.GetString(reader.GetDocument(p.Document).Name);
                    list.Add(new DbgSequencePoint()
                    {
                        document =  Marshal.StringToBSTR(fileName),
                        startLine = p.StartLine,
                        endLine = p.EndLine,
                        startColumn = p.StartColumn,
                        endColumn = p.EndColumn,
                        offset = p.Offset
                    });
                }

                if (list.Count == 0)
                    return RetCode.Fail;

                var structSize = Marshal.SizeOf<DbgSequencePoint>();
                IntPtr allPoints = Marshal.AllocCoTaskMem(list.Count * structSize);
                var currentPtr = allPoints;

                foreach (var p in list)
                {
                    Marshal.StructureToPtr(p, currentPtr, false);
                    currentPtr = currentPtr + structSize;
                }

                points = allPoints;
                pointsCount = list.Count;
            }
            catch
            {
                foreach (var p in list)
                {
                    Marshal.FreeBSTR(p.document);
                }
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        /// <summary>
        /// Find IL offset for next close user code sequence point by IL offset.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="ilOffset">IL offset</param>
        /// <param name="sequencePoint">sequence point return</param>
        /// <param name="noUserCodeFound">return 1 in case all sequence points checked and no user code was found, otherwise return 0</param>
        /// <returns>"Ok" if information is available</returns>
        private static RetCode GetNextUserCodeILOffset(IntPtr symbolReaderHandle, int methodToken, uint ilOffset, out uint ilNextOffset, out int noUserCodeFound)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            ilNextOffset = 0;
            noUserCodeFound = 0;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                SequencePointCollection sequencePoints = GetSequencePointCollection(methodToken, reader);

                foreach (SequencePoint point in sequencePoints)
                {
                    if (point.StartLine == 0 || point.StartLine == SequencePoint.HiddenLine)
                        continue;

                    if (point.Offset >= ilOffset)
                    {
                        ilNextOffset = (uint)point.Offset;
                        return RetCode.OK;
                    }
                }

                noUserCodeFound = 1;
                return RetCode.Fail;
            }
            catch
            {
                return RetCode.Exception;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct method_data_t
        {
            public int methodDef;
            public int startLine; // first segment/method SequencePoint's startLine
            public int endLine; // last segment/method SequencePoint's endLine
            public int startColumn; // first segment/method SequencePoint's startColumn
            public int endColumn; // last segment/method SequencePoint's endColumn

            public method_data_t(int methodDef_, int startLine_, int endLine_, int startColumn_, int endColumn_)
            {
                methodDef = methodDef_;
                startLine = startLine_;
                endLine = endLine_;
                startColumn = startColumn_;
                endColumn = endColumn_;
            }
            public void SetRange(int startLine_, int endLine_, int startColumn_, int endColumn_)
            {
                startLine = startLine_;
                endLine = endLine_;
                startColumn = startColumn_;
                endColumn = endColumn_;
            }
            public void ExtendRange(int startLine_, int endLine_, int startColumn_, int endColumn_)
            {
                if (startLine > startLine_)
                {
                    startLine = startLine_;
                    startColumn = startColumn_;
                }
                else if (startLine == startLine_ && startColumn > startColumn_)
                {
                    startColumn = startColumn_;
                }

                if (endLine < endLine_)
                {
                    endLine = endLine_;
                    endColumn = endColumn_;
                }
                else if (endLine == endLine_ && endColumn < endColumn_)
                {
                    endColumn = endColumn_;
                }
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct file_methods_data_t
        {
            public IntPtr document;
            public int methodNum;
            public IntPtr methodsData; // method_data_t*
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct module_methods_data_t
        {
            public int fileNum;
            public IntPtr moduleMethodsData; // file_methods_data_t*
        }

        /// <summary>
        /// Get all method ranges for all methods (in case of constructors ranges for all segments).
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="constrNum">number of constructors tokens in array</param>
        /// <param name="constrTokens">array of constructors tokens</param>
        /// <param name="normalNum">number of normal methods tokens in array</param>
        /// <param name="normalTokens">array of normal methods tokens</param>
        /// <param name="data">pointer to memory with result</param>
        /// <returns>"Ok" if information is available</returns>
        internal static RetCode GetModuleMethodsRanges(IntPtr symbolReaderHandle, uint constrNum, IntPtr constrTokens, uint normalNum, IntPtr normalTokens, out IntPtr data)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            data = IntPtr.Zero;
            var unmanagedPTRList = new List<IntPtr>();
            var unmanagedBSTRList = new List<IntPtr>();

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                Dictionary<DocumentHandle, List<method_data_t>> ModuleData = new Dictionary<DocumentHandle, List<method_data_t>>();

                int elementSize = 4;
                // Make sure we add constructors related data first, since this data can't be nested for sure.
                for (int i = 0; i < constrNum * elementSize; i += elementSize)
                {
                    int methodToken = Marshal.ReadInt32(constrTokens, i);
                    method_data_t currentData = new method_data_t(methodToken, 0, 0, 0, 0);

                    foreach (SequencePoint p in GetSequencePointCollection(methodToken, reader))
                    {
                        if (p.StartLine == 0 || p.StartLine == SequencePoint.HiddenLine)
                            continue;

                        if (!ModuleData.ContainsKey(p.Document))
                                ModuleData[p.Document] = new List<method_data_t>();

                        currentData.SetRange(p.StartLine, p.EndLine, p.StartColumn, p.EndColumn);
                        ModuleData[p.Document].Add(currentData);
                    }
                }

                for (int i = 0; i < normalNum * elementSize; i += elementSize)
                {
                    int methodToken = Marshal.ReadInt32(normalTokens, i);
                    method_data_t currentData = new method_data_t(methodToken, 0, 0, 0, 0);
                    DocumentHandle currentDocHandle = new DocumentHandle();

                    foreach (SequencePoint p in GetSequencePointCollection(methodToken, reader))
                    {
                        if (p.StartLine == 0 || p.StartLine == SequencePoint.HiddenLine)
                            continue;

                        // first access, init all fields and document with proper data from first user code sequence point
                        if (currentData.startLine == 0)
                        {
                            currentData.SetRange(p.StartLine, p.EndLine, p.StartColumn, p.EndColumn);
                            currentDocHandle = p.Document;
                            continue;
                        }

                        currentData.ExtendRange(p.StartLine, p.EndLine, p.StartColumn, p.EndColumn);
                    }

                    if (currentData.startLine != 0)
                    {
                        if (!ModuleData.ContainsKey(currentDocHandle))
                            ModuleData[currentDocHandle] = new List<method_data_t>();

                        ModuleData[currentDocHandle].Add(currentData);
                    }
                }

                if (ModuleData.Count == 0)
                    return RetCode.OK;

                int structModuleMethodsDataSize = Marshal.SizeOf<file_methods_data_t>();
                module_methods_data_t managedData;
                managedData.fileNum = ModuleData.Count;
                managedData.moduleMethodsData = Marshal.AllocCoTaskMem(ModuleData.Count * structModuleMethodsDataSize);
                unmanagedPTRList.Add(managedData.moduleMethodsData);
                IntPtr currentModuleMethodsDataPtr = managedData.moduleMethodsData;

                foreach (KeyValuePair<DocumentHandle, List<method_data_t>> fileData in ModuleData)
                {
                    int structMethodDataSize = Marshal.SizeOf<method_data_t>();
                    file_methods_data_t fileMethodData;
                    fileMethodData.document = Marshal.StringToBSTR(reader.GetString(reader.GetDocument(fileData.Key).Name));
                    unmanagedBSTRList.Add(fileMethodData.document);
                    fileMethodData.methodNum = fileData.Value.Count;
                    fileMethodData.methodsData = Marshal.AllocCoTaskMem(fileData.Value.Count * structMethodDataSize);
                    unmanagedPTRList.Add(fileMethodData.methodsData);
                    IntPtr currentMethodDataPtr = fileMethodData.methodsData;

                    foreach (var p in fileData.Value)
                    {
                        Marshal.StructureToPtr(p, currentMethodDataPtr, false);
                        currentMethodDataPtr = currentMethodDataPtr + structMethodDataSize;
                    }

                    Marshal.StructureToPtr(fileMethodData, currentModuleMethodsDataPtr, false);
                    currentModuleMethodsDataPtr = currentModuleMethodsDataPtr + structModuleMethodsDataSize;
                }
            
                data = Marshal.AllocCoTaskMem(Marshal.SizeOf<module_methods_data_t>());
                unmanagedPTRList.Add(data);
                Marshal.StructureToPtr(managedData, data, false);
            }
            catch
            {
                foreach (var p in unmanagedPTRList)
                {
                    Marshal.FreeCoTaskMem(p);
                }
                foreach (var p in unmanagedBSTRList)
                {
                    Marshal.FreeBSTR(p);
                }
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct resolved_bp_t
        {
            public int startLine;
            public int endLine;
            public int ilOffset;
            public int methodToken;

            public resolved_bp_t(int startLine_, int endLine_, int ilOffset_, int methodToken_)
            {
                startLine = startLine_;
                endLine = endLine_;
                ilOffset = ilOffset_;
                methodToken = methodToken_;
            }
        }

        enum Position {
            First, Last
        };

        /// <summary>
        /// Resolve breakpoints.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="tokenNum">number of elements in Tokens</param>
        /// <param name="Tokens">array of method tokens, that have sequence point with sourceLine</param>
        /// <param name="sourceLine">initial source line for resolve</param>
        /// <param name="nestedToken">close nested token for sourceLine</param>
        /// <param name="Count">entry's count in data</param>
        /// <param name="data">pointer to memory with result</param>
        /// <returns>"Ok" if information is available</returns>
        internal static RetCode ResolveBreakPoints(IntPtr symbolReaderHandles, int tokenNum, IntPtr Tokens, int sourceLine, int nestedToken,
                                                   out int Count, [MarshalAs(UnmanagedType.LPWStr)] string sourcePath, out IntPtr data)
        {
            Debug.Assert(symbolReaderHandles != IntPtr.Zero);
            Count = 0;
            data = IntPtr.Zero;
            var list = new List<resolved_bp_t>();

            try
            {
                // In case nestedToken + sourceLine is part of constructor (tokenNum > 1) we could have cases:
                // 1. type FieldName1 = new Type();
                //    void MethodName() {}; type FieldName2 = new Type(); ...  <-- sourceLine
                // 2. type FieldName1 = new Type(); void MethodName() {}; ...  <-- sourceLine
                //    type FieldName2 = new Type();
                // In first case, we need setup breakpoint in nestedToken's method (MethodName in examples above), in second - ignore it.

                // In case nestedToken + sourceLine in normal method we could have cases:
                // 1. ... line without code ...                                <-- sourceLine
                //    void MethodName { ...
                // 2. ... line with code ... void MethodName { ...             <-- sourceLine
                // We need check if nestedToken's method code closer to sourceLine than code from methodToken's method.
                // If sourceLine closer to nestedToken's method code - setup breakpoint in nestedToken's method.

                SequencePoint SequencePointForSourceLine(Position reqPos, ref MetadataReader reader, int methodToken)
                {
                    // Note, SequencePoints ordered by IL offsets, not by line numbers.
                    // For example, infinite loop `while(true)` will have IL offset after cycle body's code.
                    SequencePoint nearestSP = new SequencePoint();

                    foreach (SequencePoint p in GetSequencePointCollection(methodToken, reader))
                    {
                        if (p.StartLine == 0 || p.StartLine == SequencePoint.HiddenLine || p.EndLine < sourceLine)
                            continue;

                        // Note, in case of constructors, we must care about source too, since we may have situation when field/property have same line in another source.
                        var fileName = reader.GetString(reader.GetDocument(p.Document).Name);
                        if (fileName != sourcePath)
                            continue;

                        // first access, assign to first user code sequence point
                        if (nearestSP.StartLine == 0)
                        {
                            nearestSP = p;
                            continue;
                        }

                        if (p.EndLine != nearestSP.EndLine)
                        {
                            if ((reqPos == Position.First && p.EndLine < nearestSP.EndLine) ||
                                (reqPos == Position.Last && p.EndLine > nearestSP.EndLine))
                                nearestSP = p;
                        }
                        else
                        {
                            if ((reqPos == Position.First && p.EndColumn < nearestSP.EndColumn) ||
                                (reqPos == Position.Last && p.EndColumn > nearestSP.EndColumn))
                                nearestSP = p;
                        }
                    }

                    return nearestSP;
                }

                int elementSize = 4;
                for (int i = 0; i < tokenNum; i++)
                {
                    IntPtr symbolReaderHandle = Marshal.ReadIntPtr(symbolReaderHandles, i * IntPtr.Size);
                    GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                    MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                    int methodToken = Marshal.ReadInt32(Tokens, i * elementSize);
                    SequencePoint current_p = SequencePointForSourceLine(Position.First, ref reader, methodToken);
                    // Note, we don't check that current_p was found or not, since we know for sure, that sourceLine could be resolved in method.
                    // Same idea for nested_p below, if we have nestedToken - it will be resolved for sure.

                    if (nestedToken != 0)
                    {
                        // Check if nestedToken is within range of current_p. Example -
                        //     await Parallel.ForEachAsync(userHandlers, parallelOptions, async (uri, token) =>   <- breakpoint at this line
                        //     {
                        //        await new HttpClient().GetAsync("https://google.com");
                        //     });
                        // nesetedToken here is the annonymous async func, and having a breakpoing at the 1st line should
                        // break on the outer call.
                        SequencePoint nested_start_p = SequencePointForSourceLine(Position.First, ref reader, nestedToken);
                        SequencePoint nested_end_p = SequencePointForSourceLine(Position.Last, ref reader, nestedToken);
                        if ((nested_start_p.StartLine > current_p.StartLine || (nested_start_p.StartLine == current_p.StartLine && nested_start_p.StartColumn > current_p.StartColumn)) &&
                            (nested_end_p.EndLine < current_p.EndLine || (nested_end_p.EndLine == current_p.EndLine && nested_end_p.EndColumn < current_p.EndColumn ))
                        ) {
                            list.Add(new resolved_bp_t(current_p.StartLine, current_p.EndLine, current_p.Offset, methodToken));
                            break;
                        }

                        if (current_p.EndLine > nested_start_p.EndLine || (current_p.EndLine == nested_start_p.EndLine && current_p.EndColumn > nested_start_p.EndColumn))
                        {
                            list.Add(new resolved_bp_t(nested_start_p.StartLine, nested_start_p.EndLine, nested_start_p.Offset, nestedToken));
                            // (tokenNum > 1) can have only lines, that added to multiple constructors, in this case - we will have same for all Tokens,
                            // we need unique tokens only for breakpoints, prevent adding nestedToken multiple times.
                            break;
                        }
                    }
                    nestedToken = 0; // Don't check nested block next cycle (will have same results).

                    list.Add(new resolved_bp_t(current_p.StartLine, current_p.EndLine, current_p.Offset, methodToken));
                }

                if (list.Count == 0)
                    return RetCode.OK;

                int structSize = Marshal.SizeOf<resolved_bp_t>();
                data = Marshal.AllocCoTaskMem(list.Count * structSize);
                IntPtr dataPtr = data;

                foreach (var p in list)
                {
                    Marshal.StructureToPtr(p, dataPtr, false);
                    dataPtr = dataPtr + structSize;
                }

                Count = list.Count;
            }
            catch
            {
                if (data != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(data);

                data = IntPtr.Zero;
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        internal static RetCode GetStepRangesFromIP(IntPtr symbolReaderHandle, int ip, int methodToken, out uint ilStartOffset, out uint ilEndOffset)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            ilStartOffset = 0;
            ilEndOffset = 0;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                var list = new List<SequencePoint>();
                foreach (SequencePoint p in GetSequencePointCollection(methodToken, reader))
                    list.Add(p);

                var pointsArray = list.ToArray();

                for (int i = 1; i < pointsArray.Length; i++)
                {
                    SequencePoint p = pointsArray[i];

                    if (p.Offset > ip && p.StartLine != 0 && p.StartLine != SequencePoint.HiddenLine)
                    {
                        ilStartOffset = (uint)pointsArray[0].Offset;
                        for (int j = i - 1; j > 0; j--)
                        {
                            if (pointsArray[j].Offset <= ip)
                            {
                                ilStartOffset = (uint)pointsArray[j].Offset;
                                break;
                            }
                        }
                        ilEndOffset = (uint)p.Offset;
                        return RetCode.OK;
                    }
                }

                // let's handle correctly last step range from last sequence point till
                // end of the method.
                if (pointsArray.Length > 0)
                {
                    ilStartOffset = (uint)pointsArray[0].Offset;
                    for (int j = pointsArray.Length - 1; j > 0; j--)
                    {
                        if (pointsArray[j].Offset <= ip)
                        {
                            ilStartOffset = (uint)pointsArray[j].Offset;
                            break;
                        }
                    }
                    ilEndOffset = ilStartOffset; // Should set this to IL code size in calling code
                    return RetCode.OK;
                }
            }
            catch
            {
                return RetCode.Exception;
            }

            return RetCode.Fail;
        }

        internal static RetCode GetLocalVariableNameAndScope(IntPtr symbolReaderHandle, int methodToken, int localIndex, out IntPtr localVarName, out int ilStartOffset, out int ilEndOffset)
        {
            localVarName = IntPtr.Zero;
            ilStartOffset = 0;
            ilEndOffset = 0;

            try
            {
                string localVar = null;
                if (!GetLocalVariableAndScopeByIndex(symbolReaderHandle, methodToken, localIndex, out localVar, out ilStartOffset, out ilEndOffset))
                    return RetCode.Fail;

                localVarName = Marshal.StringToBSTR(localVar);
                localVar = null;
            }
            catch
            {
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        internal static bool GetLocalVariableAndScopeByIndex(IntPtr symbolReaderHandle, int methodToken, int localIndex, out string localVarName, out int ilStartOffset, out int ilEndOffset)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            localVarName = null;
            ilStartOffset = 0;
            ilEndOffset = 0;

            // caller must care about exception during this code execution

            GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
            MetadataReader reader = ((OpenedReader)gch.Target).Reader;

            Handle handle = GetDeltaRelativeMethodDefinitionHandle(reader, methodToken);
            if (handle.Kind != HandleKind.MethodDefinition)
                return false;

            MethodDebugInformationHandle methodDebugHandle = ((MethodDefinitionHandle)handle).ToDebugInformationHandle();
            LocalScopeHandleCollection localScopes = reader.GetLocalScopes(methodDebugHandle);
            foreach (LocalScopeHandle scopeHandle in localScopes)
            {
                LocalScope scope = reader.GetLocalScope(scopeHandle);
                LocalVariableHandleCollection localVars = scope.GetLocalVariables();
                foreach (LocalVariableHandle varHandle in localVars)
                {
                    LocalVariable localVar = reader.GetLocalVariable(varHandle);
                    if (localVar.Index == localIndex)
                    {
                        if (localVar.Attributes == LocalVariableAttributes.DebuggerHidden)
                            return false;

                        localVarName = reader.GetString(localVar.Name);
                        ilStartOffset = scope.StartOffset;
                        ilEndOffset = scope.EndOffset;
                        return true;
                    }
                }
            }

            return false;
        }

        /// <summary>
        /// Returns local variable name for given local index and IL offset.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="data">pointer to memory with histed local scopes</param>
        /// <param name="hoistedLocalScopesCount">histed local scopes count</param>
        /// <returns>"Ok" if information is available</returns>
        internal static RetCode GetHoistedLocalScopes(IntPtr symbolReaderHandle, int methodToken, out IntPtr data, out int hoistedLocalScopesCount)
        {
            data = IntPtr.Zero;
            hoistedLocalScopesCount = 0;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                Handle handle = GetDeltaRelativeMethodDefinitionHandle(reader, methodToken);
                if (handle.Kind != HandleKind.MethodDefinition)
                    return RetCode.Fail;

                MethodDebugInformationHandle methodDebugInformationHandle = ((MethodDefinitionHandle)handle).ToDebugInformationHandle();
                var entityHandle = MetadataTokens.EntityHandle(MetadataTokens.GetToken(methodDebugInformationHandle.ToDefinitionHandle()));

                // Guid is taken from Roslyn source code:
                // https://github.com/dotnet/roslyn/blob/afd10305a37c0ffb2cfb2c2d8446154c68cfa87a/src/Dependencies/CodeAnalysis.Debugging/PortableCustomDebugInfoKinds.cs#L14
                Guid stateMachineHoistedLocalScopes = new Guid("6DA9A61E-F8C7-4874-BE62-68BC5630DF71");

                var HoistedLocalScopes = new List<UInt32>();
                foreach (var cdiHandle in reader.GetCustomDebugInformation(entityHandle))
                {
                    var cdi = reader.GetCustomDebugInformation(cdiHandle);

                    if (reader.GetGuid(cdi.Kind) == stateMachineHoistedLocalScopes)
                    {
                        // Format of this blob is taken from Roslyn source code:
                        // https://github.com/dotnet/roslyn/blob/afd10305a37c0ffb2cfb2c2d8446154c68cfa87a/src/Compilers/Core/Portable/PEWriter/MetadataWriter.PortablePdb.cs#L600

                        var blobReader = reader.GetBlobReader(cdi.Value);

                        while (blobReader.Offset < blobReader.Length)
                        {
                            HoistedLocalScopes.Add(blobReader.ReadUInt32()); // StartOffset
                            HoistedLocalScopes.Add(blobReader.ReadUInt32()); // Length
                        }
                    }
                }

                if (HoistedLocalScopes.Count == 0)
                    return RetCode.Fail;

                data = Marshal.AllocCoTaskMem(HoistedLocalScopes.Count * 4);
                IntPtr dataPtr = data;
                foreach (var p in HoistedLocalScopes)
                {
                    Marshal.StructureToPtr(p, dataPtr, false);
                    dataPtr = dataPtr + 4;
                }
                hoistedLocalScopesCount = HoistedLocalScopes.Count / 2;
            }
            catch
            {
                if (data != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(data);

                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        /// <summary>
        /// Returns local variable name for given local index and IL offset.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="localIndex">local variable index</param>
        /// <param name="localVarName">local variable name return</param>
        /// <returns>true if name has been found</returns>
        internal static bool GetLocalVariableName(IntPtr symbolReaderHandle, int methodToken, int localIndex, out IntPtr localVarName)
        {
            localVarName = IntPtr.Zero;

            string localVar = null;
            if (!GetLocalVariableByIndex(symbolReaderHandle, methodToken, localIndex, out localVar))
                return false;

            localVarName = Marshal.StringToBSTR(localVar);
            localVar = null;
            return true;
        }

        /// <summary>
        /// Helper method to return local variable name for given local index and IL offset.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="localIndex">local variable index</param>
        /// <param name="localVarName">local variable name return</param>
        /// <returns>true if name has been found</returns>
        internal static bool GetLocalVariableByIndex(IntPtr symbolReaderHandle, int methodToken, int localIndex, out string localVarName)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            localVarName = null;

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                Handle handle = GetDeltaRelativeMethodDefinitionHandle(reader, methodToken);
                if (handle.Kind != HandleKind.MethodDefinition)
                    return false;

                MethodDebugInformationHandle methodDebugHandle = ((MethodDefinitionHandle)handle).ToDebugInformationHandle();
                LocalScopeHandleCollection localScopes = reader.GetLocalScopes(methodDebugHandle);
                foreach (LocalScopeHandle scopeHandle in localScopes)
                {
                    LocalScope scope = reader.GetLocalScope(scopeHandle);
                    LocalVariableHandleCollection localVars = scope.GetLocalVariables();
                    foreach (LocalVariableHandle varHandle in localVars)
                    {
                        LocalVariable localVar = reader.GetLocalVariable(varHandle);
                        if (localVar.Index == localIndex)
                        {
                            if (localVar.Attributes == LocalVariableAttributes.DebuggerHidden)
                                return false;

                            localVarName = reader.GetString(localVar.Name);
                            return true;
                        }
                    }
                }
            }
            catch
            {
            }

            return false;
        }

        /// <summary>
        /// Returns the portable PDB reader for the assembly path
        /// </summary>
        /// <param name="assemblyPath">file path of the assembly or null if the module is in-memory or dynamic</param>
        /// <param name="isFileLayout">type of in-memory PE layout, if true, file based layout otherwise, loaded layout</param>
        /// <param name="peStream">optional in-memory PE stream</param>
        /// <param name="pdbStream">optional in-memory PDB stream</param>
        /// <returns>reader/provider wrapper instance</returns>
        /// <remarks>
        /// Assumes that neither PE image nor PDB loaded into memory can be unloaded or moved around.
        /// </remarks>
        private static OpenedReader GetReader(string assemblyPath, bool isFileLayout, Stream peStream, Stream pdbStream)
        {
            return (pdbStream != null) ? TryOpenReaderForInMemoryPdb(pdbStream) : TryOpenReaderFromAssembly(assemblyPath, isFileLayout, peStream);
        }

        private static OpenedReader TryOpenReaderForInMemoryPdb(Stream pdbStream)
        {
            Debug.Assert(pdbStream != null);

            byte[] buffer = new byte[sizeof(uint)];
            if (pdbStream.Read(buffer, 0, sizeof(uint)) != sizeof(uint))
            {
                return null;
            }
            uint signature = BitConverter.ToUInt32(buffer, 0);

            // quick check to avoid throwing exceptions below in common cases:
            const uint ManagedMetadataSignature = 0x424A5342;
            if (signature != ManagedMetadataSignature)
            {
                // not a Portable PDB
                return null;
            }

            OpenedReader result = null;
            MetadataReaderProvider provider = null;
            try
            {
                pdbStream.Position = 0;
                provider = MetadataReaderProvider.FromPortablePdbStream(pdbStream);
                result = new OpenedReader(provider, provider.GetMetadataReader());
            }
            catch (Exception e) when (e is BadImageFormatException || e is IOException)
            {
                return null;
            }
            finally
            {
                if (result == null)
                {
                    provider?.Dispose();
                }
            }

            return result;
        }

        private static OpenedReader TryOpenReaderFromAssembly(string assemblyPath, bool isFileLayout, Stream peStream)
        {
            if (assemblyPath == null && peStream == null)
                return null;

            PEStreamOptions options = isFileLayout ? PEStreamOptions.Default : PEStreamOptions.IsLoadedImage;
            if (peStream == null)
            {
                peStream = TryOpenFile(assemblyPath);
                if (peStream == null)
                    return null;
                
                options = PEStreamOptions.Default;
            }

            try
            {
                using (var peReader = new PEReader(peStream, options))
                {
                    DebugDirectoryEntry codeViewEntry, embeddedPdbEntry;
                    ReadPortableDebugTableEntries(peReader, out codeViewEntry, out embeddedPdbEntry);

                    // First try .pdb file specified in CodeView data (we prefer .pdb file on disk over embedded PDB
                    // since embedded PDB needs decompression which is less efficient than memory-mapping the file).
                    if (codeViewEntry.DataSize != 0)
                    {
                        var result = TryOpenReaderFromCodeView(peReader, codeViewEntry, assemblyPath);
                        if (result != null)
                        {
                            return result;
                        }
                    }

                    // if it failed try Embedded Portable PDB (if available):
                    if (embeddedPdbEntry.DataSize != 0)
                    {
                        return TryOpenReaderFromEmbeddedPdb(peReader, embeddedPdbEntry);
                    }
                }
            }
            catch (Exception e) when (e is BadImageFormatException || e is IOException)
            {
                // nop
            }

            return null;
        }

        private static void ReadPortableDebugTableEntries(PEReader peReader, out DebugDirectoryEntry codeViewEntry, out DebugDirectoryEntry embeddedPdbEntry)
        {
            // See spec: https://github.com/dotnet/corefx/blob/master/src/System.Reflection.Metadata/specs/PE-COFF.md

            codeViewEntry = default(DebugDirectoryEntry);
            embeddedPdbEntry = default(DebugDirectoryEntry);

            foreach (DebugDirectoryEntry entry in peReader.ReadDebugDirectory())
            {
                if (entry.Type == DebugDirectoryEntryType.CodeView)
                {
                    const ushort PortableCodeViewVersionMagic = 0x504d;
                    if (entry.MinorVersion != PortableCodeViewVersionMagic)
                    {
                        continue;
                    }

                    codeViewEntry = entry;
                }
                else if (entry.Type == DebugDirectoryEntryType.EmbeddedPortablePdb)
                {
                    embeddedPdbEntry = entry;
                }
            }
        }

        private static OpenedReader TryOpenReaderFromCodeView(PEReader peReader, DebugDirectoryEntry codeViewEntry, string assemblyPath)
        {
            OpenedReader result = null;
            MetadataReaderProvider provider = null;
            try
            {
                var data = peReader.ReadCodeViewDebugDirectoryData(codeViewEntry);

                string pdbPath = data.Path;
                if (assemblyPath != null)
                {
                    try
                    {
                        pdbPath = Path.Combine(Path.GetDirectoryName(assemblyPath), GetFileName(pdbPath));
                    }
                    catch
                    {
                        // invalid characters in CodeView path
                        return null;
                    }
                }

                var pdbStream = TryOpenFile(pdbPath);
                if (pdbStream == null && assemblyPath != null)
                {
                    // workaround, since NI file could be generated in `.native_image` subdirectory
                    // NOTE this is temporary solution until we add option for specifying pdb path
                    try
                    {
                        int tmpLastIndex = assemblyPath.LastIndexOf(".native_image");
                        if (tmpLastIndex == -1)
                        {
                            return null;
                        }
                        string tmpPath = assemblyPath.Substring(0, tmpLastIndex);
                        pdbPath = Path.Combine(Path.GetDirectoryName(tmpPath), GetFileName(pdbPath));
                    }
                    catch
                    {
                        // invalid characters in CodeView path
                        return null;
                    }

                    pdbStream = TryOpenFile(pdbPath);
                }
                if (pdbStream == null)
                {
                    return null;
                }

                provider = MetadataReaderProvider.FromPortablePdbStream(pdbStream);
                var reader = provider.GetMetadataReader();

                // Validate that the PDB matches the assembly version
                if (data.Age == 1 && new BlobContentId(reader.DebugMetadataHeader.Id) == new BlobContentId(data.Guid, codeViewEntry.Stamp))
                {
                    result = new OpenedReader(provider, reader);
                }
            }
            catch (Exception e) when (e is BadImageFormatException || e is IOException)
            {
                return null;
            }
            finally
            {
                if (result == null)
                {
                    provider?.Dispose();
                }
            }

            return result;
        }

        private static OpenedReader TryOpenReaderFromEmbeddedPdb(PEReader peReader, DebugDirectoryEntry embeddedPdbEntry)
        {
            OpenedReader result = null;
            MetadataReaderProvider provider = null;

            try
            {
                // TODO: We might want to cache this provider globally (across stack traces), 
                // since decompressing embedded PDB takes some time.
                provider = peReader.ReadEmbeddedPortablePdbDebugDirectoryData(embeddedPdbEntry);
                result = new OpenedReader(provider, provider.GetMetadataReader());
            }
            catch (Exception e) when (e is BadImageFormatException || e is IOException)
            {
                return null;
            }
            finally
            {
                if (result == null)
                {
                    provider?.Dispose();
                }
            }

            return result;
        }

        private static Stream TryOpenFile(string path)
        {
            if (!File.Exists(path))
            {
                return null;
            }
            try
            {
                return File.OpenRead(path);
            }
            catch
            {
                return null;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct AsyncAwaitInfoBlock
        {
            public uint yield_offset;
            public uint resume_offset;
            public uint token;
        }

        /// <summary>
        /// Helper method to return async method stepping information and return last method's offset for user code.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="methodToken">method token</param>
        /// <param name="asyncInfo">array with all async method stepping information</param>
        /// <param name="asyncInfoCount">entry's count in asyncInfo</param>
        /// <param name="LastIlOffset">return last found IL offset in user code</param>
        /// <returns>"Ok" if method have at least one await block and last IL offset was found</returns>
        internal static RetCode GetAsyncMethodSteppingInfo(IntPtr symbolReaderHandle, int methodToken, out IntPtr asyncInfo, out int asyncInfoCount, out uint LastIlOffset)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);

            asyncInfo = IntPtr.Zero;
            asyncInfoCount = 0;
            LastIlOffset = 0;
            bool foundOffset = false;
            var list = new List<AsyncAwaitInfoBlock>();

            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader reader = ((OpenedReader)gch.Target).Reader;

                Handle handle = GetDeltaRelativeMethodDefinitionHandle(reader, methodToken);
                if (handle.Kind != HandleKind.MethodDefinition)
                    return RetCode.Fail;

                MethodDebugInformationHandle methodDebugInformationHandle = ((MethodDefinitionHandle)handle).ToDebugInformationHandle();
                var entityHandle = MetadataTokens.EntityHandle(MetadataTokens.GetToken(methodDebugInformationHandle.ToDefinitionHandle()));

                // Guid is taken from Roslyn source code:
                // https://github.com/dotnet/roslyn/blob/afd10305a37c0ffb2cfb2c2d8446154c68cfa87a/src/Dependencies/CodeAnalysis.Debugging/PortableCustomDebugInfoKinds.cs#L13
                Guid asyncMethodSteppingInformationBlob = new Guid("54FD2AC5-E925-401A-9C2A-F94F171072F8");

                foreach (var cdiHandle in reader.GetCustomDebugInformation(entityHandle))
                {
                    var cdi = reader.GetCustomDebugInformation(cdiHandle);

                    if (reader.GetGuid(cdi.Kind) == asyncMethodSteppingInformationBlob)
                    {
                        // Format of this blob is taken from Roslyn source code:
                        // https://github.com/dotnet/roslyn/blob/afd10305a37c0ffb2cfb2c2d8446154c68cfa87a/src/Compilers/Core/Portable/PEWriter/MetadataWriter.PortablePdb.cs#L575

                        var blobReader = reader.GetBlobReader(cdi.Value);
                        blobReader.ReadUInt32(); // skip catch_handler_offset

                        while (blobReader.Offset < blobReader.Length)
                        {
                            list.Add(new AsyncAwaitInfoBlock() {
                                yield_offset = blobReader.ReadUInt32(),
                                resume_offset = blobReader.ReadUInt32(),
                                // explicit conversion from int into uint here, see:
                                // https://docs.microsoft.com/en-us/dotnet/api/system.reflection.metadata.blobreader.readcompressedinteger
                                token = (uint)blobReader.ReadCompressedInteger()
                            });
                        }
                    }
                }

                if (list.Count == 0)
                    return RetCode.Fail;

                int structSize = Marshal.SizeOf<AsyncAwaitInfoBlock>();
                asyncInfo = Marshal.AllocCoTaskMem(list.Count * structSize);
                IntPtr currentPtr = asyncInfo;

                foreach (var p in list)
                {
                    Marshal.StructureToPtr(p, currentPtr, false);
                    currentPtr = currentPtr + structSize;
                }

                asyncInfoCount = list.Count;

                // We don't use LINQ in order to reduce memory consumption for managed part, so, Reverse() usage not an option here.
                // Note, SequencePointCollection is IEnumerable based collections.
                foreach (SequencePoint p in GetSequencePointCollection(methodToken, reader))
                {
                    if (p.StartLine == 0 || p.StartLine == SequencePoint.HiddenLine || p.Offset < 0)
                        continue;

                    // Method's IL start only from 0, use uint for IL offset.
                    LastIlOffset = (uint)p.Offset;
                    foundOffset = true;
                }

                if (!foundOffset)
                {
                    if (asyncInfo != IntPtr.Zero)
                        Marshal.FreeCoTaskMem(asyncInfo);

                    asyncInfo = IntPtr.Zero;
                    return RetCode.Fail;
                }
            }
            catch
            {
                if (asyncInfo != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(asyncInfo);

                asyncInfo = IntPtr.Zero;
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        /// <summary>
        /// Get Source Code.
        /// </summary>
        /// <param name="symbolReaderHandle">symbol reader handle returned by LoadSymbolsForModule</param>
        /// <param name="fileName">source file name</param>
        /// <param name="length">length of data</param>
        /// <param name="data">pointer to memory with source code</param>
        /// <returns>"Ok" if information is available</returns>
        internal static RetCode GetSource(IntPtr symbolReaderHandle, [MarshalAs(UnmanagedType.LPWStr)] string fileName, out int length, out IntPtr data)
        {
            Debug.Assert(symbolReaderHandle != IntPtr.Zero);
            length = 0;
            data = IntPtr.Zero;
            try
            {
                GCHandle gch = GCHandle.FromIntPtr(symbolReaderHandle);
                MetadataReader mdReader = ((OpenedReader)gch.Target).Reader;
                foreach (var handle in mdReader.Documents)
                {
                    var doc = mdReader.GetDocument(handle);
                    var docPath = mdReader.GetString(doc.Name);
                    int docSize = 0;
                    if (docPath == fileName)
                    {
                        MemoryStream ms = GetEmbeddedSource(mdReader, handle, out docSize);
                        data = Marshal.AllocCoTaskMem(docSize);
                        Marshal.Copy(ms.ToArray(), 0, data, docSize);
                        length = docSize;
                        return RetCode.OK;
                    }
                }
            }
            catch
            {
                if (data != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(data);

                data = IntPtr.Zero;
                return RetCode.Exception;

            }
            return RetCode.Fail;
        }

        private static readonly Guid guid = new Guid("0E8A571B-6926-466E-B4AD-8AB04611F5FE");

        private static MemoryStream GetEmbeddedSource(MetadataReader reader, DocumentHandle document, out int docSize)
        {
            byte[] bytes = null;

            foreach (var handle in reader.GetCustomDebugInformation(document))
            {
                var cdi = reader.GetCustomDebugInformation(handle);
                if (reader.GetGuid(cdi.Kind) == guid)
                {
                    bytes = reader.GetBlobBytes(cdi.Value);
                    break;
                }
            }

            if (bytes == null)
            {
                docSize = 0;
                return null;
            }

            docSize = BitConverter.ToInt32(bytes, 0);
            var stream = new MemoryStream(bytes, sizeof(int), bytes.Length - sizeof(int));

            if (docSize != 0)
            {
                var decompressed = new MemoryStream(docSize);

                using (var deflater = new DeflateStream(stream, CompressionMode.Decompress))
                {
                    deflater.CopyTo(decompressed);
                }

                if (decompressed.Length != docSize)
                {
                    throw new InvalidDataException();
                }

                stream = decompressed;
            }
            else
            {
                docSize = bytes.Length - sizeof(int);
            }
            return stream;
        }
    }
}
