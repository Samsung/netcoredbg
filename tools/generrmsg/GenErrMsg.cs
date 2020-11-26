using System;
using System.Xml;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace generrmsg
{
    public class ErrMsg
    {
        public string SymName { get; set; }
        public string Message { get; set; }
        public int Code { get; set;}
    }

    public class GenErrMsg
    {
        // See https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/0642cb2f-2075-4469-918c-4441e69c548a for more details
        private const int SEVERITY_BIT_OFFSET = 31;
        private const int FACILITY_BITS_OFFSET = 16;

        public static void Main(string[] args)
        {
            if (args.Length < 1) {
                Console.WriteLine("Usage: generrmessage XML-file [result-cpp-file] [result-h-file]");
                return;
            }
            Dictionary<string, ErrMsg> errmsgs = new Dictionary<string, ErrMsg>();
            string hresult = null;
            string msg=null;
            string symname = null;
            string comment = null;
            string sourcefilename = args[0];
            string outputfilename = null;
            string hfilename = null;
            int FaciltyUrt=0x13; // The source of the error code is .NET CLR.
            int SeveritySuccess=0;
            int SeverityError=1;

            int minSR = MakeHresult(SeveritySuccess,FaciltyUrt,0);
            int maxSR = MakeHresult(SeveritySuccess,FaciltyUrt,0xffff);
            int minHR = MakeHresult(SeverityError,FaciltyUrt,0);
            int maxHR = MakeHresult(SeverityError,FaciltyUrt,0xffff);

            if (string.IsNullOrEmpty(sourcefilename)) {
                sourcefilename = "corerror.xml";
            }
            if (args.Length < 2) {
                outputfilename = "errormessage.cpp";
            } else {
                outputfilename = args[1];
            }
            if (args.Length < 3) {
                hfilename = "errormessage.h";
            } else {
                hfilename = args[2];
            }
            StreamWriter outfile = File.CreateText("temp.cpp");
            StreamWriter hfile = File.CreateText("temp.h");
            PrintHeader(outfile);
            PrintHeaderH(hfile);
            XmlTextReader reader = new XmlTextReader(sourcefilename);
       
            while (reader.Read()) {
                switch(reader.NodeType) {

                     case XmlNodeType.Element:
                         if (reader.Name.ToString() == "HRESULT") {
                             hresult = reader.GetAttribute("NumericValue");
                         } else if (reader.Name.ToString() == "SymbolicName") {
                             symname = reader.ReadString();
                         } else if (reader.Name.ToString() == "Message") {
                             msg = reader.ReadString();
                         } else if (reader.Name.ToString() == "Comment") {
                             comment = reader.ReadString();
                         }
                         break;

                     case XmlNodeType.EndElement:
                         if (reader.Name.ToString() == "HRESULT") {
                             ErrMsg errmsg = new ErrMsg();
                             int code = 0;
                             if (hresult.StartsWith("0x") || hresult.StartsWith("0X")) {
                                 code = int.Parse(hresult.Substring(2), System.Globalization.NumberStyles.HexNumber);
                                 hresult = code.ToString();
                             }
                             errmsg.SymName = symname;
                             errmsg.Code = code;
                             if (!string.IsNullOrEmpty(msg)) {
                                   errmsg.Message = msg;
                             } else if (!string.IsNullOrEmpty(comment)) {
                                 int found = comment.IndexOf("//");
                                 if (found > 0) {
                                       errmsg.Message = "\"" + comment.Substring(found + 2) + "\"";
                                 } else {
                                       errmsg.Message = "\"" + comment + "\"";
                                 }
                             } else {
                                   errmsg.Message = "\"" + symname + "\"";
                             }
                             if (!errmsgs.ContainsKey(hresult)) {
                                 errmsgs.Add(key: hresult, value: errmsg);
                             }
                         }
                         hresult = null;
                         msg = null;
                         comment = null;
                         symname = null;
                         break;
                 }
            }
            foreach (KeyValuePair<string, ErrMsg> it in errmsgs) {
                ErrMsg errmsg = it.Value;
                outfile.WriteLine("        CASE_OF({0}, {1});", errmsg.SymName, errmsg.Message);
                if (errmsg.Code != 0) {
                    if ((errmsg.Code>minSR) && (errmsg.Code <= maxSR)) {
                        errmsg.Code = errmsg.Code & 0xffff;
                        hfile.WriteLine("#define " + errmsg.SymName + " SMAKEHR(0x" + errmsg.Code.ToString("x") + ")");
                    } else if ((errmsg.Code > minHR) && (errmsg.Code <= maxHR)) {
                        errmsg.Code = errmsg.Code & 0xffff;
                        hfile.WriteLine("#define " + errmsg.SymName + " EMAKEHR(0x" + errmsg.Code.ToString("x") + ")");
                    } else {
                        hfile.WriteLine("#define " + errmsg.SymName + " " + it.Key);
                    }
                } else {
                    hfile.WriteLine("#define " + errmsg.SymName + " " + it.Key);
                }
            }
            PrintFooter(outfile);
            PrintFooterH(hfile);
            outfile.Close();
            hfile.Close();
            System.IO.File.Move("temp.cpp", outputfilename, true);
            System.IO.File.Move("temp.h", hfilename, true);
        }

        private static void PrintHeader(StreamWriter sw) {
            sw.WriteLine("// Do not edit this file. It's generated from corerror.xml automatically");
            sw.WriteLine("#include \"errormessage.h\"");
            sw.WriteLine("");
            sw.WriteLine("#define CASE_OF(CODE, STRERROR) case CODE: str=STRERROR; break;");
            sw.WriteLine("");
            sw.WriteLine("const char *errormessage(HRESULT hresult)");
            sw.WriteLine("{");
            sw.WriteLine("    const char *str;");
            sw.WriteLine("");
            sw.WriteLine("    switch (hresult)");
            sw.WriteLine("    {");
            sw.WriteLine("// From (winerror.h/palrt.h)"); 
            sw.WriteLine("        CASE_OF(S_OK, \"S_OK\");");
            sw.WriteLine("        CASE_OF(S_FALSE, \"S_FALSE\");");
            sw.WriteLine("        CASE_OF(E_NOTIMPL, \"E_NOTIMPL\");");
            sw.WriteLine("        CASE_OF(E_UNEXPECTED, \"E_UNEXPECTED\");");
            sw.WriteLine("        CASE_OF(E_HANDLE, \"E_HANDLE\");");
            sw.WriteLine("        CASE_OF(E_ABORT, \"E_ABORT\");");
            sw.WriteLine("        CASE_OF(E_FAIL, \"E_FAIL\");");
            sw.WriteLine("        CASE_OF(E_PENDING, \"E_PENDING\");");
            sw.WriteLine("        CASE_OF(DISP_E_PARAMNOTFOUND, \"DISP_E_PARAMNOTFOUND\");");
            sw.WriteLine("        CASE_OF(DISP_E_TYPEMISMATCH, \"DISP_E_TYPEMISMATCH\");");
            sw.WriteLine("        CASE_OF(DISP_E_BADVARTYPE, \"DISP_E_BADVARTYPE\");");
            sw.WriteLine("        CASE_OF(DISP_E_OVERFLOW, \"DISP_E_OVERFLOW\");");
            sw.WriteLine("        CASE_OF(CLASS_E_CLASSNOTAVAILABLE, \"CLASS_E_CLASSNOTAVAILABLE\");");
            sw.WriteLine("        CASE_OF(CLASS_E_NOAGGREGATION, \"CLASS_E_NOAGGREGATION\");");
            sw.WriteLine("        CASE_OF(CO_E_CLASSSTRING, \"CO_E_CLASSSTRING\");");
            sw.WriteLine("        CASE_OF(MK_E_SYNTAX, \"MK_E_SYNTAX\");");
            sw.WriteLine("        CASE_OF(STG_E_INVALIDFUNCTION, \"STG_E_INVALIDFUNCTION\");");
            sw.WriteLine("        CASE_OF(STG_E_FILENOTFOUND, \"STG_E_FILENOTFOUND\");");
            sw.WriteLine("        CASE_OF(STG_E_PATHNOTFOUND, \"STG_E_PATHNOTFOUND\");");
            sw.WriteLine("        CASE_OF(STG_E_WRITEFAULT, \"STG_E_WRITEFAULT\");");
            sw.WriteLine("        CASE_OF(STG_E_FILEALREADYEXISTS, \"STG_E_FILEALREADYEXISTS\");");
            sw.WriteLine("        CASE_OF(STG_E_ABNORMALAPIEXIT, \"STG_E_ABNORMALAPIEXIT\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_UID, \"NTE_BAD_UID\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_HASH, \"NTE_BAD_HASH\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_KEY, \"NTE_BAD_KEY\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_LEN, \"NTE_BAD_LEN\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_DATA, \"NTE_BAD_DATA\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_SIGNATURE, \"NTE_BAD_SIGNATURE\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_VER, \"NTE_BAD_VER\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_ALGID, \"NTE_BAD_ALGID\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_FLAGS, \"NTE_BAD_FLAGS\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_TYPE, \"NTE_BAD_TYPE\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_KEY_STATE, \"NTE_BAD_KEY_STATE\");");
            sw.WriteLine("        CASE_OF(NTE_BAD_HASH_STATE, \"NTE_BAD_HASH_STATE\");");
            sw.WriteLine("        CASE_OF(NTE_NO_KEY, \"NTE_NO_KEY\");");
            sw.WriteLine("        CASE_OF(NTE_NO_MEMORY, \"NTE_NO_MEMORY\");");
            sw.WriteLine("        CASE_OF(NTE_SIGNATURE_FILE_BAD, \"NTE_SIGNATURE_FILE_BAD\");");
            sw.WriteLine("        CASE_OF(NTE_FAIL, \"NTE_FAIL\");");
            sw.WriteLine("        CASE_OF(CRYPT_E_HASH_VALUE, \"CRYPT_E_HASH_VALUE\");");
            sw.WriteLine("        CASE_OF(TYPE_E_SIZETOOBIG, \"TYPE_E_SIZETOOBIG\");");
            sw.WriteLine("        CASE_OF(TYPE_E_DUPLICATEID, \"TYPE_E_DUPLICATEID\");");
            sw.WriteLine("        CASE_OF(INET_E_CANNOT_CONNECT, \"INET_E_CANNOT_CONNECT\");");
            sw.WriteLine("        CASE_OF(INET_E_RESOURCE_NOT_FOUND, \"INET_E_RESOURCE_NOT_FOUND\");");
            sw.WriteLine("        CASE_OF(INET_E_OBJECT_NOT_FOUND, \"INET_E_OBJECT_NOT_FOUND\");");
            sw.WriteLine("        CASE_OF(INET_E_DATA_NOT_AVAILABLE, \"INET_E_DATA_NOT_AVAILABLE\");");
            sw.WriteLine("        CASE_OF(INET_E_DOWNLOAD_FAILURE, \"INET_E_DOWNLOAD_FAILURE\");");
            sw.WriteLine("        CASE_OF(INET_E_CONNECTION_TIMEOUT, \"INET_E_CONNECTION_TIMEOUT\");");
            sw.WriteLine("        CASE_OF(INET_E_UNKNOWN_PROTOCOL, \"INET_E_UNKNOWN_PROTOCOL\");");
            if (!System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) {
                // The following codes don't exist on Windows platform and should not be generated there 
                sw.WriteLine("        CASE_OF(CTL_E_OVERFLOW, \"CTL_E_OVERFLOW\");");
                sw.WriteLine("        CASE_OF(CTL_E_OUTOFMEMORY, \"CTL_E_OUTOFMEMORY\");");
                sw.WriteLine("        CASE_OF(CTL_E_DIVISIONBYZERO, \"CTL_E_DIVISIONBYZERO\");");
                sw.WriteLine("        CASE_OF(CTL_E_OUTOFSTACKSPACE, \"CTL_E_OUTOFSTACKSPACE\");");
                sw.WriteLine("        CASE_OF(CTL_E_FILENOTFOUND, \"CTL_E_FILENOTFOUND\");");
                sw.WriteLine("        CASE_OF(CTL_E_DEVICEIOERROR, \"CTL_E_DEVICEIOERROR\");");
                sw.WriteLine("        CASE_OF(CTL_E_PERMISSIONDENIED, \"CTL_E_PERMISSIONDENIED\");");
                sw.WriteLine("        CASE_OF(CTL_E_PATHFILEACCESSERROR, \"CTL_E_PATHFILEACCESSERROR\");");
                sw.WriteLine("        CASE_OF(CTL_E_PATHNOTFOUND, \"CTL_E_PATHNOTFOUND\");");
                sw.WriteLine("        CASE_OF(DBG_PRINTEXCEPTION_C, \"DBG_PRINTEXCEPTION_C\");");
            } // end if
            sw.WriteLine("//from corerror.h");
        }

        private static void PrintFooter(StreamWriter sw) {
            sw.WriteLine("        default:"); 
            sw.WriteLine("            str = \"Unknown HRESULT code\";");
            sw.WriteLine("            break;");
            sw.WriteLine("    }");
            sw.WriteLine("    return str;");
            sw.WriteLine("}");
        }
      
        private static void PrintHeaderH(StreamWriter sw) {
            sw.WriteLine("// Licensed to the .NET Foundation under one or more agreements.");
            sw.WriteLine("// The .NET Foundation licenses this file to you under the MIT license.");
            sw.WriteLine("// See the LICENSE file in the project root for more information.");
            sw.WriteLine();
            sw.WriteLine("#ifndef __COMMON_LANGUAGE_RUNTIME_HRESULTS__");
            sw.WriteLine("#define __COMMON_LANGUAGE_RUNTIME_HRESULTS__");
            sw.WriteLine();
            sw.WriteLine("#include <winerror.h>");
            sw.WriteLine();
            sw.WriteLine();
            sw.WriteLine("//");
            sw.WriteLine("//This file is AutoGenerated -- Do Not Edit by hand!!!");
            sw.WriteLine("//");
            sw.WriteLine("//Add new HRESULTS along with their corresponding error messages to");
            sw.WriteLine("//corerror.xml");
            sw.WriteLine("//");
            sw.WriteLine();
            sw.WriteLine("#ifndef FACILITY_URT");
            sw.WriteLine("// The source of the error code is .NET CLR.");
            sw.WriteLine("// See https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/0642cb2f-2075-4469-918c-4441e69c548a for more details"); 
            sw.WriteLine("#define FACILITY_URT            0x13");
            sw.WriteLine("#endif");
            sw.WriteLine("#ifndef EMAKEHR");
            sw.WriteLine("#define SMAKEHR(val) MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_URT, val)");
            sw.WriteLine("#define EMAKEHR(val) MAKE_HRESULT(SEVERITY_ERROR, FACILITY_URT, val)");
            sw.WriteLine("#endif");
            sw.WriteLine();
        }

        private static void PrintFooterH(StreamWriter sw) {
            sw.WriteLine();
            sw.WriteLine("#endif // __COMMON_LANGUAGE_RUNTIME_HRESULTS__");
        }

        private static int MakeHresult(int sev, int fac, int code) {
            return ((sev<<SEVERITY_BIT_OFFSET) | (fac<<FACILITY_BITS_OFFSET) | (code));
        }
    }
}
