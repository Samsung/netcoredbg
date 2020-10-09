// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "cputil.h"

#include <codecvt>
#include <locale>

#ifdef _MSC_VER

static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t> convert;

std::string to_utf8(const wchar_t *wstr)
{
    return convert.to_bytes(wstr);
}

#else

static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;

std::string to_utf8(const char16_t *wstr)
{
    return convert.to_bytes(wstr);
}

#endif

std::string to_utf8(char16_t wch)
{
    return convert.to_bytes(wch);
}

#ifdef _MSC_VER
std::wstring
#else
std::u16string
#endif
to_utf16(const std::string &utf8)
{
    return convert.from_bytes(utf8);
}

std::vector<std::string> split_on_tokens(const std::string &str, const char delim)
{
    std::vector<std::string> res;
    size_t pos = 0, prev = 0;

    while (true)
    {
        pos = str.find(delim, prev);
        if (pos == std::string::npos)
        {
            res.push_back(std::string(str, prev));
            break;
        }

        res.push_back(std::string(str, prev, pos - prev));
        prev = pos + 1;
    }

    return res;
}

#define CASE_OF(CODE, STRERROR) case CODE: str=STRERROR; break;

const char *errormessage(HRESULT hresult)
{
    const char *str;

    switch (hresult)
    {
// From (winerror.h/palrt.h) 
        CASE_OF(S_OK, "S_OK");
        CASE_OF(S_FALSE, "S_FALSE");
        CASE_OF(E_NOTIMPL, "E_NOTIMPL");
        CASE_OF(E_UNEXPECTED, "E_UNEXPECTED");
        CASE_OF(E_HANDLE, "E_HANDLE");
        CASE_OF(E_ABORT, "E_ABORT");
        CASE_OF(E_FAIL, "E_FAIL");
        CASE_OF(E_PENDING, "E_PENDING");
        CASE_OF(DISP_E_PARAMNOTFOUND, "DISP_E_PARAMNOTFOUND");
        CASE_OF(DISP_E_TYPEMISMATCH, "DISP_E_TYPEMISMATCH");
        CASE_OF(DISP_E_BADVARTYPE, "DISP_E_BADVARTYPE");
        CASE_OF(DISP_E_OVERFLOW, "DISP_E_OVERFLOW");
        CASE_OF(CLASS_E_CLASSNOTAVAILABLE, "CLASS_E_CLASSNOTAVAILABLE");
        CASE_OF(CLASS_E_NOAGGREGATION, "CLASS_E_NOAGGREGATION");
        CASE_OF(CO_E_CLASSSTRING, "CO_E_CLASSSTRING");
        CASE_OF(MK_E_SYNTAX, "MK_E_SYNTAX");
        CASE_OF(STG_E_INVALIDFUNCTION, "STG_E_INVALIDFUNCTION");
        CASE_OF(STG_E_FILENOTFOUND, "STG_E_FILENOTFOUND");
        CASE_OF(STG_E_PATHNOTFOUND, "STG_E_PATHNOTFOUND");
        CASE_OF(STG_E_WRITEFAULT, "STG_E_WRITEFAULT");
        CASE_OF(STG_E_FILEALREADYEXISTS, "STG_E_FILEALREADYEXISTS");
        CASE_OF(STG_E_ABNORMALAPIEXIT, "STG_E_ABNORMALAPIEXIT");
        CASE_OF(NTE_BAD_UID, "NTE_BAD_UID");
        CASE_OF(NTE_BAD_HASH, "NTE_BAD_HASH");
        CASE_OF(NTE_BAD_KEY, "NTE_BAD_KEY");
        CASE_OF(NTE_BAD_LEN, "NTE_BAD_LEN");
        CASE_OF(NTE_BAD_DATA, "NTE_BAD_DATA");
        CASE_OF(NTE_BAD_SIGNATURE, "NTE_BAD_SIGNATURE");
        CASE_OF(NTE_BAD_VER, "NTE_BAD_VER");
        CASE_OF(NTE_BAD_ALGID, "NTE_BAD_ALGID");
        CASE_OF(NTE_BAD_FLAGS, "NTE_BAD_FLAGS");
        CASE_OF(NTE_BAD_TYPE, "NTE_BAD_TYPE");
        CASE_OF(NTE_BAD_KEY_STATE, "NTE_BAD_KEY_STATE");
        CASE_OF(NTE_BAD_HASH_STATE, "NTE_BAD_HASH_STATE");
        CASE_OF(NTE_NO_KEY, "NTE_NO_KEY");
        CASE_OF(NTE_NO_MEMORY, "NTE_NO_MEMORY");
        CASE_OF(NTE_SIGNATURE_FILE_BAD, "NTE_SIGNATURE_FILE_BAD");
        CASE_OF(NTE_FAIL, "NTE_FAIL");
        CASE_OF(CRYPT_E_HASH_VALUE, "CRYPT_E_HASH_VALUE");
        CASE_OF(TYPE_E_SIZETOOBIG, "TYPE_E_SIZETOOBIG");
        CASE_OF(TYPE_E_DUPLICATEID, "TYPE_E_DUPLICATEID");
        CASE_OF(CTL_E_OVERFLOW, "CTL_E_OVERFLOW");
        CASE_OF(CTL_E_OUTOFMEMORY, "CTL_E_OUTOFMEMORY");
        CASE_OF(CTL_E_DIVISIONBYZERO, "CTL_E_DIVISIONBYZERO");
        CASE_OF(CTL_E_OUTOFSTACKSPACE, "CTL_E_OUTOFSTACKSPACE");
        CASE_OF(CTL_E_FILENOTFOUND, "CTL_E_FILENOTFOUND");
        CASE_OF(CTL_E_DEVICEIOERROR, "CTL_E_DEVICEIOERROR");
        CASE_OF(CTL_E_PERMISSIONDENIED, "CTL_E_PERMISSIONDENIED");
        CASE_OF(CTL_E_PATHFILEACCESSERROR, "CTL_E_PATHFILEACCESSERROR");
        CASE_OF(CTL_E_PATHNOTFOUND, "CTL_E_PATHNOTFOUND");
        CASE_OF(INET_E_CANNOT_CONNECT, "INET_E_CANNOT_CONNECT");
        CASE_OF(INET_E_RESOURCE_NOT_FOUND, "INET_E_RESOURCE_NOT_FOUND");
        CASE_OF(INET_E_OBJECT_NOT_FOUND, "INET_E_OBJECT_NOT_FOUND");
        CASE_OF(INET_E_DATA_NOT_AVAILABLE, "INET_E_DATA_NOT_AVAILABLE");
        CASE_OF(INET_E_DOWNLOAD_FAILURE, "INET_E_DOWNLOAD_FAILURE");
        CASE_OF(INET_E_CONNECTION_TIMEOUT, "INET_E_CONNECTION_TIMEOUT");
        CASE_OF(INET_E_UNKNOWN_PROTOCOL, "INET_E_UNKNOWN_PROTOCOL");
        CASE_OF(DBG_PRINTEXCEPTION_C, "DBG_PRINTEXCEPTION_C");
// From corerror.xml        
        CASE_OF(CLDB_S_TRUNCATION, "STATUS: Data value was truncated.");
        CASE_OF(META_S_DUPLICATE, "Attempt to define an object that already exists in valid scenerios.");
        CASE_OF(CORDBG_S_BAD_START_SEQUENCE_POINT, "Attempt to SetIP not at a sequence point sequence point.");
        CASE_OF(CORDBG_S_BAD_END_SEQUENCE_POINT, "Attempt to SetIP when not going to a sequence point. If both this and CORDBG_E_BAD_START_SEQUENCE_POINT are true, only CORDBG_E_BAD_START_SEQUENCE_POINT will be reported.");
        CASE_OF(CORDBG_S_FUNC_EVAL_HAS_NO_RESULT, "Some Func evals will lack a return value,");
        CASE_OF(CORDBG_S_VALUE_POINTS_TO_VOID, "The Debugging API doesn't support dereferencing void pointers.");
        CASE_OF(CORDBG_S_FUNC_EVAL_ABORTED, "The func eval completed, but was aborted.");
        CASE_OF(CORDBG_S_AT_END_OF_STACK, "The stack walk has reached the end of the stack.  There are no more frames to walk.");
        CASE_OF(CORDBG_S_NOT_ALL_BITS_SET, "Not all bits specified were successfully applied");
        CASE_OF(CEE_E_CVTRES_NOT_FOUND, "cvtres.exe not found.");
        CASE_OF(COR_E_TYPEUNLOADED, "Type has been unloaded.");
        CASE_OF(COR_E_APPDOMAINUNLOADED, "Attempted to access an unloaded appdomain.");
        CASE_OF(COR_E_CANNOTUNLOADAPPDOMAIN, "Error while unloading appdomain.");
        CASE_OF(MSEE_E_ASSEMBLYLOADINPROGRESS, "Assembly is still being loaded.");
        CASE_OF(COR_E_ASSEMBLYEXPECTED, "The module was expected to contain an assembly manifest.");
        CASE_OF(COR_E_FIXUPSINEXE, "Attempt to load an unverifiable executable with fixups (IAT with more than 2 sections or a TLS section.)");
        CASE_OF(COR_E_NEWER_RUNTIME, "This assembly is built by a runtime newer than the currently loaded runtime and cannot be loaded.");
        CASE_OF(COR_E_MULTIMODULEASSEMBLIESDIALLOWED, "The module cannot be loaded because only single file assemblies are supported.");
        CASE_OF(HOST_E_DEADLOCK, "Host detected a deadlock on a blocking operation.");
        CASE_OF(HOST_E_INVALIDOPERATION, "Invalid operation.");
        CASE_OF(HOST_E_CLRNOTAVAILABLE, "CLR has been disabled due to unrecoverable error.");
        CASE_OF(HOST_E_EXITPROCESS_THREADABORT, "Process exited due to ThreadAbort escalation.");
        CASE_OF(HOST_E_EXITPROCESS_ADUNLOAD, "Process exited due to AD Unload escalation.");
        CASE_OF(HOST_E_EXITPROCESS_TIMEOUT, "Process exited due to Timeout escalation.");
        CASE_OF(HOST_E_EXITPROCESS_OUTOFMEMORY, "Process exited due to OutOfMemory escalation.");
        CASE_OF(COR_E_MODULE_HASH_CHECK_FAILED, "The check of the module's hash failed.");
        CASE_OF(FUSION_E_REF_DEF_MISMATCH, "The located assembly's manifest definition does not match the assembly reference.");
        CASE_OF(FUSION_E_INVALID_PRIVATE_ASM_LOCATION, "The private assembly was located outside the appbase directory.");
        CASE_OF(FUSION_E_ASM_MODULE_MISSING, "A module specified in the manifest was not found.");
        CASE_OF(FUSION_E_PRIVATE_ASM_DISALLOWED, "A strongly-named assembly is required.");
        CASE_OF(FUSION_E_SIGNATURE_CHECK_FAILED, "Strong name signature could not be verified.  The assembly may have been tampered with, or it was delay signed but not fully signed with the correct private key.");
        CASE_OF(FUSION_E_INVALID_NAME, "The given assembly name or codebase was invalid.");
        CASE_OF(FUSION_E_CODE_DOWNLOAD_DISABLED, "HTTP download of assemblies has been disabled for this appdomain.");
        CASE_OF(FUSION_E_HOST_GAC_ASM_MISMATCH, "Assembly in host store has a different signature than assembly in GAC.");
        CASE_OF(FUSION_E_LOADFROM_BLOCKED, "LoadFrom(), LoadFile(), Load(byte[]) and LoadModule() have been disabled by the host.");
        CASE_OF(FUSION_E_CACHEFILE_FAILED, "Failed to add file to AppDomain cache.");
        CASE_OF(FUSION_E_APP_DOMAIN_LOCKED, "The requested assembly version conflicts with what is already bound in the app domain or specified in the manifest.");
        CASE_OF(FUSION_E_CONFIGURATION_ERROR, "The requested assembly name was neither found in the GAC nor in the manifest or the manifest's specified location is wrong.");
        CASE_OF(FUSION_E_MANIFEST_PARSE_ERROR, "Unexpected error while parsing the specified manifest.");
        CASE_OF(COR_E_LOADING_REFERENCE_ASSEMBLY, "Reference assemblies should not be loaded for execution.  They can only be loaded in the Reflection-only loader context.");
        CASE_OF(COR_E_NI_AND_RUNTIME_VERSION_MISMATCH, "The native image could not be loaded, because it was generated for use by a different version of the runtime.");
        CASE_OF(COR_E_LOADING_WINMD_REFERENCE_ASSEMBLY, "Contract Windows Runtime assemblies cannot be loaded for execution.  Make sure your application only contains non-contract Windows Runtime assemblies.");
        CASE_OF(COR_E_AMBIGUOUSIMPLEMENTATION, "Ambiguous implementation found.");
        CASE_OF(CLDB_E_FILE_BADREAD, "Error occurred during a read.");
        CASE_OF(CLDB_E_FILE_BADWRITE, "Error occurred during a write.");
        CASE_OF(CLDB_E_FILE_OLDVER, "Old version error.");
        CASE_OF(CLDB_E_SMDUPLICATE, "Create of shared memory failed.  A memory mapping of the same name already exists.");
        CASE_OF(CLDB_E_NO_DATA, "No .CLB data in the memory or stream.");
        CASE_OF(CLDB_E_INCOMPATIBLE, "Importing scope is not compatible with the emitting scope.");
        CASE_OF(CLDB_E_FILE_CORRUPT, "File is corrupt.");
        CASE_OF(CLDB_E_BADUPDATEMODE, "Cannot open a incrementally build scope for full update.");
        CASE_OF(CLDB_E_INDEX_NOTFOUND, "Index not found.");
        CASE_OF(CLDB_E_RECORD_NOTFOUND, "Record not found on lookup.");
        CASE_OF(CLDB_E_RECORD_OUTOFORDER, "Record is emitted out of order.");
        CASE_OF(CLDB_E_TOO_BIG, "A blob or string was too big.");
        CASE_OF(META_E_INVALID_TOKEN_TYPE, "A token of the wrong type passed to a metadata function.");
        CASE_OF(TLBX_E_LIBNOTREGISTERED, "Typelib export: Type library is not registered.");
        CASE_OF(META_E_BADMETADATA, "Merge: Inconsistency in meta data import scope.");
        CASE_OF(META_E_BAD_SIGNATURE, "Bad binary signature.");
        CASE_OF(META_E_BAD_INPUT_PARAMETER, "Bad input parameters.");
        CASE_OF(META_E_CANNOTRESOLVETYPEREF, "Cannot resolve typeref.");
        CASE_OF(META_E_STRINGSPACE_FULL, "No logical space left to create more user strings.");
        CASE_OF(META_E_HAS_UNMARKALL, "Unmark all has been called already.");
        CASE_OF(META_E_MUST_CALL_UNMARKALL, "Must call UnmarkAll first before marking.");
        CASE_OF(META_E_CA_INVALID_TARGET, "Known custom attribute on invalid target.");
        CASE_OF(META_E_CA_INVALID_VALUE, "Known custom attribute had invalid value.");
        CASE_OF(META_E_CA_INVALID_BLOB, "Known custom attribute blob has bad format.");
        CASE_OF(META_E_CA_REPEATED_ARG, "Known custom attribute blob has repeated named argument.");
        CASE_OF(META_E_CA_UNKNOWN_ARGUMENT, "Known custom attribute named argument not recognized.");
        CASE_OF(META_E_CA_UNEXPECTED_TYPE, "Known attribute parser found unexpected type.");
        CASE_OF(META_E_CA_INVALID_ARGTYPE, "Known attribute parser only handles fields, not properties.");
        CASE_OF(META_E_CA_INVALID_ARG_FOR_TYPE, "Known attribute parser found an argument that is invalid for the object it is applied to.");
        CASE_OF(META_E_CA_INVALID_UUID, "The format of the UUID was invalid.");
        CASE_OF(META_E_CA_INVALID_MARSHALAS_FIELDS, "The MarshalAs attribute has fields set that are not valid for the specified unmanaged type.");
        CASE_OF(META_E_CA_NT_FIELDONLY, "The specified unmanaged type is only valid on fields.");
        CASE_OF(META_E_CA_NEGATIVE_PARAMINDEX, "The parameter index cannot be negative.");
        CASE_OF(META_E_CA_NEGATIVE_CONSTSIZE, "The constant size cannot be negative.");
        CASE_OF(META_E_CA_FIXEDSTR_SIZE_REQUIRED, "A fixed string requires a size.");
        CASE_OF(META_E_CA_CUSTMARSH_TYPE_REQUIRED, "A custom marshaler requires the custom marshaler type.");
        CASE_OF(META_E_NOT_IN_ENC_MODE, "SaveDelta was called without being in EnC mode.");
        CASE_OF(META_E_CA_BAD_FRIENDS_ARGS, "InternalsVisibleTo can't have a version, culture, or processor architecture.");
        CASE_OF(META_E_CA_FRIENDS_SN_REQUIRED, "Strong-name signed assemblies can only grant friend access to strong name-signed assemblies");
        CASE_OF(VLDTR_E_RID_OUTOFRANGE, "Rid is out of range.");
        CASE_OF(VLDTR_E_STRING_INVALID, "String offset is invalid.");
        CASE_OF(VLDTR_E_GUID_INVALID, "GUID offset is invalid.");
        CASE_OF(VLDTR_E_BLOB_INVALID, "Blob offset if invalid.");
        CASE_OF(VLDTR_E_MR_BADCALLINGCONV, "MemberRef has invalid calling convention.");
        CASE_OF(VLDTR_E_SIGNULL, "Signature specified is zero-sized.");
        CASE_OF(VLDTR_E_MD_BADCALLINGCONV, "Method signature has invalid calling convention.");
        CASE_OF(VLDTR_E_MD_THISSTATIC, "Method is marked static but has HASTHIS/EXPLICITTHIS set on the calling convention.");
        CASE_OF(VLDTR_E_MD_NOTTHISNOTSTATIC, "Method is not marked static but is not HASTHIS or EXPLICITTHIS.");
        CASE_OF(VLDTR_E_MD_NOARGCNT, "Method signature is missing the argument count.");
        CASE_OF(VLDTR_E_SIG_MISSELTYPE, "Signature missing element type.");
        CASE_OF(VLDTR_E_SIG_MISSTKN, "Signature missing token.");
        CASE_OF(VLDTR_E_SIG_TKNBAD, "Signature has bad token.");
        CASE_OF(VLDTR_E_SIG_MISSFPTR, "Signature is missing function pointer.");
        CASE_OF(VLDTR_E_SIG_MISSFPTRARGCNT, "Signature has function pointer missing argument count.");
        CASE_OF(VLDTR_E_SIG_MISSRANK, "Signature is missing rank specification.");
        CASE_OF(VLDTR_E_SIG_MISSNSIZE, "Signature is missing count of sized dimensions.");
        CASE_OF(VLDTR_E_SIG_MISSSIZE, "Signature is missing size of dimension.");
        CASE_OF(VLDTR_E_SIG_MISSNLBND, "Signature is missing count of lower bounds.");
        CASE_OF(VLDTR_E_SIG_MISSLBND, "Signature is missing a lower bound.");
        CASE_OF(VLDTR_E_SIG_BADELTYPE, "Signature has bad element type.");
        CASE_OF(VLDTR_E_TD_ENCLNOTNESTED, "TypeDef not nested has encloser.");
        CASE_OF(VLDTR_E_FMD_PINVOKENOTSTATIC, "Field or method is PInvoke but is not marked Static.");
        CASE_OF(VLDTR_E_SIG_SENTINMETHODDEF, "E_T_SENTINEL in MethodDef signature.");
        CASE_OF(VLDTR_E_SIG_SENTMUSTVARARG, "E_T_SENTINEL <=> VARARG.");
        CASE_OF(VLDTR_E_SIG_MULTSENTINELS, "Multiple E_T_SENTINELs.");
        CASE_OF(VLDTR_E_SIG_MISSARG, "Signature missing argument.");
        CASE_OF(VLDTR_E_SIG_BYREFINFIELD, "Field of ByRef type.");
        CASE_OF(CORDBG_E_UNRECOVERABLE_ERROR, "Unrecoverable API error.");
        CASE_OF(CORDBG_E_PROCESS_TERMINATED, "Process was terminated.");
        CASE_OF(CORDBG_E_PROCESS_NOT_SYNCHRONIZED, "Process not synchronized.");
        CASE_OF(CORDBG_E_CLASS_NOT_LOADED, "A class is not loaded.");
        CASE_OF(CORDBG_E_IL_VAR_NOT_AVAILABLE, "An IL variable is not available at the current native IP.");
        CASE_OF(CORDBG_E_BAD_REFERENCE_VALUE, "A reference value was found to be bad during dereferencing.");
        CASE_OF(CORDBG_E_FIELD_NOT_AVAILABLE, "A field in a class is not available, because the runtime optimized it away.");
        CASE_OF(CORDBG_E_NON_NATIVE_FRAME, "'Native-frame-only' operation on non-native frame.");
        CASE_OF(CORDBG_E_CODE_NOT_AVAILABLE, "The code is currently unavailable.");
        CASE_OF(CORDBG_E_FUNCTION_NOT_IL, "Attempt to get a ICorDebugFunction for a function that is not IL.");
        CASE_OF(CORDBG_E_CANT_SET_IP_INTO_FINALLY, "SetIP is not possible because SetIP would move EIP from outside of an exception handling finally clause to a point inside of one.");
        CASE_OF(CORDBG_E_CANT_SET_IP_OUT_OF_FINALLY, "SetIP is not possible because it would move EIP from within an exception handling finally clause to a point outside of one.");
        CASE_OF(CORDBG_E_CANT_SET_IP_INTO_CATCH, "SetIP is not possible, because SetIP would move EIP from outside of an exception handling catch clause to a point inside of one.");
        CASE_OF(CORDBG_E_SET_IP_NOT_ALLOWED_ON_NONLEAF_FRAME, "SetIP cannot be done on any frame except the leaf frame.");
        CASE_OF(CORDBG_E_SET_IP_IMPOSSIBLE, "SetIP is not allowed.");
        CASE_OF(CORDBG_E_FUNC_EVAL_BAD_START_POINT, "Func eval cannot work. Bad starting point.");
        CASE_OF(CORDBG_E_INVALID_OBJECT, "This object value is no longer valid.");
        CASE_OF(CORDBG_E_FUNC_EVAL_NOT_COMPLETE, "CordbEval::GetResult called before func eval has finished.");
        CASE_OF(CORDBG_E_STATIC_VAR_NOT_AVAILABLE, "A static variable is not available because it has not been initialized yet.");
        CASE_OF(CORDBG_E_CANT_SETIP_INTO_OR_OUT_OF_FILTER, "SetIP cannot leave or enter a filter.");
        CASE_OF(CORDBG_E_CANT_CHANGE_JIT_SETTING_FOR_ZAP_MODULE, "JIT settings for ZAP modules cannot be changed.");
        CASE_OF(CORDBG_E_CANT_SET_IP_OUT_OF_FINALLY_ON_WIN64, "SetIP is not possible because it would move EIP from within a finally clause to a point outside of one on this platforms.");
        CASE_OF(CORDBG_E_CANT_SET_IP_OUT_OF_CATCH_ON_WIN64, "SetIP is not possible because it would move EIP from within a catch clause to a point outside of one on this platforms.");
        CASE_OF(CORDBG_E_CANT_SET_TO_JMC, "Cannot use JMC on this code (likely wrong JIT settings).");
        CASE_OF(CORDBG_E_NO_CONTEXT_FOR_INTERNAL_FRAME, "Internal frame markers have no associated context.");
        CASE_OF(CORDBG_E_NOT_CHILD_FRAME, "The current frame is not a child frame.");
        CASE_OF(CORDBG_E_NON_MATCHING_CONTEXT, "The provided CONTEXT does not match the specified thread.");
        CASE_OF(CORDBG_E_PAST_END_OF_STACK, "The stackwalker is now past the end of stack.  No information is available.");
        CASE_OF(CORDBG_E_FUNC_EVAL_CANNOT_UPDATE_REGISTER_IN_NONLEAF_FRAME, "Func eval cannot update a variable stored in a register on a non-leaf frame.  The most likely cause is that such a variable is passed as a ref/out argument.");
        CASE_OF(CORDBG_E_BAD_THREAD_STATE, "The state of the thread is invalid.");
        CASE_OF(CORDBG_E_DEBUGGER_ALREADY_ATTACHED, "This process has already been attached.");
        CASE_OF(CORDBG_E_SUPERFLOUS_CONTINUE, "Returned from a call to Continue that was not matched with a stopping event.");
        CASE_OF(CORDBG_E_SET_VALUE_NOT_ALLOWED_ON_NONLEAF_FRAME, "Cannot perfrom SetValue on non-leaf frames.");
        CASE_OF(CORDBG_E_ENC_MODULE_NOT_ENC_ENABLED, "Tried to do Edit and Continue on a module that was not started in Edit and Continue mode.");
        CASE_OF(CORDBG_E_SET_IP_NOT_ALLOWED_ON_EXCEPTION, "SetIP cannot be done on any exception.");
        CASE_OF(CORDBG_E_VARIABLE_IS_ACTUALLY_LITERAL, "The 'variable' does not exist because it is a literal optimized away by the compiler.");
        CASE_OF(CORDBG_E_PROCESS_DETACHED, "Process has been detached.");
        CASE_OF(CORDBG_E_ENC_CANT_ADD_FIELD_TO_VALUE_OR_LAYOUT_CLASS, "Adding a field to a value or layout class is prohibited.");
        CASE_OF(CORDBG_E_FIELD_NOT_STATIC, "GetStaticFieldValue called on a non-static field.");
        CASE_OF(CORDBG_E_FIELD_NOT_INSTANCE, "Returned if someone tries to call GetStaticFieldValue on a non-instance field.");
        CASE_OF(CORDBG_E_ENC_JIT_CANT_UPDATE, "The JIT is unable to update the method.");
        CASE_OF(CORDBG_E_ENC_INTERNAL_ERROR, "Internal Runtime Error while doing Edit-and-Continue.");
        CASE_OF(CORDBG_E_ENC_HANGING_FIELD, "The field was added via Edit and Continue after the class was loaded.");
        CASE_OF(CORDBG_E_MODULE_NOT_LOADED, "Module not loaded.");
        CASE_OF(CORDBG_E_UNABLE_TO_SET_BREAKPOINT, "Cannot set a breakpoint here.");
        CASE_OF(CORDBG_E_DEBUGGING_NOT_POSSIBLE, "Debugging is not possible due to an incompatibility within the CLR implementation.");
        CASE_OF(CORDBG_E_KERNEL_DEBUGGER_ENABLED, "A kernel debugger is enabled on the system.  User-mode debugging will trap to the kernel debugger.");
        CASE_OF(CORDBG_E_KERNEL_DEBUGGER_PRESENT, "A kernel debugger is present on the system.  User-mode debugging will trap to the kernel debugger.");
        CASE_OF(CORDBG_E_INCOMPATIBLE_PROTOCOL, "The debugger's protocol is incompatible with the debuggee.");
        CASE_OF(CORDBG_E_TOO_MANY_PROCESSES, "The debugger can only handle a finite number of debuggees.");
        CASE_OF(CORDBG_E_INTEROP_NOT_SUPPORTED, "Interop debugging is not supported.");
        CASE_OF(CORDBG_E_NO_REMAP_BREAKPIONT, "Cannot call RemapFunction until have received RemapBreakpoint.");
        CASE_OF(CORDBG_E_OBJECT_NEUTERED, "Object is in a zombie state.");
        CASE_OF(CORPROF_E_FUNCTION_NOT_COMPILED, "Function not yet compiled.");
        CASE_OF(CORPROF_E_DATAINCOMPLETE, "The ID is not fully loaded/defined yet.");
        CASE_OF(CORPROF_E_FUNCTION_NOT_IL, "The Method has no associated IL.");
        CASE_OF(CORPROF_E_NOT_MANAGED_THREAD, "The thread has never run managed code before.");
        CASE_OF(CORPROF_E_CALL_ONLY_FROM_INIT, "The function may only be called during profiler initialization.");
        CASE_OF(CORPROF_E_NOT_YET_AVAILABLE, "Requested information is not yet available.");
        CASE_OF(CORPROF_E_TYPE_IS_PARAMETERIZED, "The given type is a generic and cannot be used with this method.");
        CASE_OF(CORPROF_E_FUNCTION_IS_PARAMETERIZED, "The given function is a generic and cannot be used with this method.");
        CASE_OF(CORPROF_E_STACKSNAPSHOT_INVALID_TGT_THREAD, "A profiler tried to walk the stack of an invalid thread");
        CASE_OF(CORPROF_E_STACKSNAPSHOT_UNMANAGED_CTX, "A profiler can not walk a thread that is currently executing unmanaged code");
        CASE_OF(CORPROF_E_STACKSNAPSHOT_UNSAFE, "A stackwalk at this point may cause dead locks or data corruption");
        CASE_OF(CORPROF_E_STACKSNAPSHOT_ABORTED, "Stackwalking callback requested the walk to abort");
        CASE_OF(CORPROF_E_LITERALS_HAVE_NO_ADDRESS, "Returned when asked for the address of a static that is a literal.");
        CASE_OF(CORPROF_E_UNSUPPORTED_CALL_SEQUENCE, "A call was made at an unsupported time. Examples include illegally calling a profiling API method asynchronously, calling a method that might trigger a GC at an unsafe time, and calling a method at a time that could cause locks to be taken out of order.");
        CASE_OF(CORPROF_E_ASYNCHRONOUS_UNSAFE, "A legal asynchronous call was made at an unsafe time (e.g., CLR locks are held)");
        CASE_OF(CORPROF_E_CLASSID_IS_ARRAY, "The specified ClassID cannot be inspected by this function because it is an array");
        CASE_OF(CORPROF_E_CLASSID_IS_COMPOSITE, "The specified ClassID is a non-array composite type (e.g., ref) and cannot be inspected");
        CASE_OF(CORPROF_E_PROFILER_DETACHING, "The profiler's call into the CLR is disallowed because the profiler is attempting to detach.");
        CASE_OF(CORPROF_E_PROFILER_NOT_ATTACHABLE, "The profiler does not support attaching to a live process.");
        CASE_OF(CORPROF_E_UNRECOGNIZED_PIPE_MSG_FORMAT, "The message sent on the profiling API attach pipe is in an unrecognized format.");
        CASE_OF(CORPROF_E_PROFILER_ALREADY_ACTIVE, "The request to attach a profiler was denied because a profiler is already loaded.");
        CASE_OF(CORPROF_E_PROFILEE_INCOMPATIBLE_WITH_TRIGGER, "Unable to request a profiler attach because the target profilee's runtime is of a version incompatible with the current process calling AttachProfiler().");
        CASE_OF(CORPROF_E_IPC_FAILED, "AttachProfiler() encountered an error while communicating on the pipe to the target profilee. This is often caused by a target profilee that is shutting down or killed while AttachProfiler() is reading or writing the pipe.");
        CASE_OF(CORPROF_E_PROFILEE_PROCESS_NOT_FOUND, "AttachProfiler() was unable to find a profilee with the specified process ID.");
        CASE_OF(CORPROF_E_CALLBACK3_REQUIRED, "Profiler must implement ICorProfilerCallback3 interface for this call to be supported.");
        CASE_OF(CORPROF_E_UNSUPPORTED_FOR_ATTACHING_PROFILER, "This call was attempted by a profiler that attached to the process after startup, but this call is only supported by profilers that are loaded into the process on startup.");
        CASE_OF(CORPROF_E_IRREVERSIBLE_INSTRUMENTATION_PRESENT, "Detach is impossible because the profiler has either instrumented IL or inserted enter/leave hooks. Detach was not attempted; the profiler is still fully attached.");
        CASE_OF(CORPROF_E_RUNTIME_UNINITIALIZED, "The profiler called a function that cannot complete because the CLR is not yet fully initialized. The profiler may try again once the CLR has fully started.");
        CASE_OF(CORPROF_E_IMMUTABLE_FLAGS_SET, "Detach is impossible because immutable flags were set by the profiler at startup. Detach was not attempted; the profiler is still fully attached.");
        CASE_OF(CORPROF_E_PROFILER_NOT_YET_INITIALIZED, "The profiler called a function that cannot complete because the profiler is not yet fully initialized.");
        CASE_OF(CORPROF_E_INCONSISTENT_WITH_FLAGS, "The profiler called a function that first requires additional flags to be set in the event mask. This HRESULT may also indicate that the profiler called a function that first requires that some of the flags currently set in the event mask be reset.");
        CASE_OF(CORPROF_E_PROFILER_CANCEL_ACTIVATION, "The profiler has requested that the CLR instance not load the profiler into this process.");
        CASE_OF(CORPROF_E_CONCURRENT_GC_NOT_PROFILABLE, "Concurrent GC mode is enabled, which prevents use of COR_PRF_MONITOR_GC");
        CASE_OF(CORPROF_E_DEBUGGING_DISABLED, "This functionality requires CoreCLR debugging to be enabled.");
        CASE_OF(CORPROF_E_TIMEOUT_WAITING_FOR_CONCURRENT_GC, "Timed out on waiting for concurrent GC to finish during attach.");
        CASE_OF(CORPROF_E_MODULE_IS_DYNAMIC, "The specified module was dynamically generated (e.g., via Reflection.Emit API), and is thus not supported by this API method.");
        CASE_OF(CORPROF_E_CALLBACK4_REQUIRED, "Profiler must implement ICorProfilerCallback4 interface for this call to be supported.");
        CASE_OF(CORPROF_E_REJIT_NOT_ENABLED, "This call is not supported unless ReJIT is first enabled during initialization by setting COR_PRF_ENABLE_REJIT via SetEventMask.");
        CASE_OF(CORPROF_E_FUNCTION_IS_COLLECTIBLE, "The specified function is instantiated into a collectible assembly, and is thus not supported by this API method.");
        CASE_OF(CORPROF_E_CALLBACK6_REQUIRED, "Profiler must implement ICorProfilerCallback6 interface for this call to be supported.");
        CASE_OF(CORPROF_E_CALLBACK7_REQUIRED, "Profiler must implement ICorProfilerCallback7 interface for this call to be supported.");
        CASE_OF(CORPROF_E_REJIT_INLINING_DISABLED, "The runtime's tracking of inlined methods for ReJIT is not enabled.");
        CASE_OF(CORDIAGIPC_E_BAD_ENCODING, "The runtime was unable to decode the Header or Payload.");
        CASE_OF(CORDIAGIPC_E_UNKNOWN_COMMAND, "The specified CommandSet or CommandId is unknown.");
        CASE_OF(CORDIAGIPC_E_UNKNOWN_MAGIC, "The magic version of Diagnostics IPC is unknown.");
        CASE_OF(CORDIAGIPC_E_UNKNOWN_ERROR, "An unknown error occurred in the Diagnpostics IPC Server.");
        CASE_OF(CORPROF_E_SUSPENSION_IN_PROGRESS, "The runtime cannot be suspened since a suspension is already in progress.");
        CASE_OF(SECURITY_E_INCOMPATIBLE_SHARE, "Loading this assembly would produce a different grant set from other instances.");
        CASE_OF(SECURITY_E_UNVERIFIABLE, "Unverifiable code failed policy check.");
        CASE_OF(SECURITY_E_INCOMPATIBLE_EVIDENCE, "Assembly already loaded without additional security evidence.");
        CASE_OF(CORSEC_E_POLICY_EXCEPTION, "PolicyException thrown.");
        CASE_OF(CORSEC_E_MIN_GRANT_FAIL, "Failed to grant minimum permission requests.");
        CASE_OF(CORSEC_E_NO_EXEC_PERM, "Failed to grant permission to execute.");
        CASE_OF(CORSEC_E_XMLSYNTAX, "XML Syntax error.");
        CASE_OF(CORSEC_E_INVALID_STRONGNAME, "Strong name validation failed.");
        CASE_OF(CORSEC_E_MISSING_STRONGNAME, "Assembly is not strong named.");
        CASE_OF(CORSEC_E_INVALID_IMAGE_FORMAT, "Invalid assembly file format.");
        CASE_OF(CORSEC_E_INVALID_PUBLICKEY, "Invalid assembly public key.");
        CASE_OF(CORSEC_E_SIGNATURE_MISMATCH, "Signature size mismatch.");
        CASE_OF(CORSEC_E_CRYPTO, "Failure during Cryptographic operation.");
        CASE_OF(CORSEC_E_CRYPTO_UNEX_OPER, "Unexpected Cryptographic operation.");
        CASE_OF(CORSECATTR_E_BAD_ACTION, "Invalid security action code.");
        CASE_OF(COR_E_EXCEPTION, "General Exception");
        CASE_OF(COR_E_SYSTEM, "System.Exception");
        CASE_OF(COR_E_ARGUMENTOUTOFRANGE, "An argument was out of its legal range.");
        CASE_OF(COR_E_ARRAYTYPEMISMATCH, "Attempted to store an object of the wrong type in an array.");
        CASE_OF(COR_E_CONTEXTMARSHAL, "Attempted to marshal an object across a context boundary.");
        CASE_OF(COR_E_TIMEOUT, "Operation timed out.");
        CASE_OF(COR_E_EXECUTIONENGINE, "Internal CLR error.");
        CASE_OF(COR_E_FIELDACCESS, "Access to this field is denied.");
        CASE_OF(COR_E_INDEXOUTOFRANGE, "Array subscript out of range.");
        CASE_OF(COR_E_INVALIDOPERATION, "An operation is not legal in the current state.");
        CASE_OF(COR_E_SECURITY, "An error relating to security occurred.");
        CASE_OF(COR_E_SERIALIZATION, "An error relating to serialization occurred.");
        CASE_OF(COR_E_VERIFICATION, "A verification failure has occurred.");
        CASE_OF(COR_E_METHODACCESS, "Access to this method is denied.");
        CASE_OF(COR_E_MISSINGFIELD, "Field does not exist.");
        CASE_OF(COR_E_MISSINGMEMBER, "Member does not exist.");
        CASE_OF(COR_E_MISSINGMETHOD, "Method does not exist.");
        CASE_OF(COR_E_MULTICASTNOTSUPPORTED, "Attempt to combine delegates that are not multicast.");
        CASE_OF(COR_E_NOTSUPPORTED, "Operation is not supported.");
        CASE_OF(COR_E_OVERFLOW, "Arithmetic, casting or conversion operation overflowed or underflowed.");
        CASE_OF(COR_E_RANK, "An array has the wrong number of dimensions for a particular operation.");
        CASE_OF(COR_E_SYNCHRONIZATIONLOCK, "This operation must be called from a synchronized block.");
        CASE_OF(COR_E_THREADINTERRUPTED, "Thread was interrupted from a waiting state.");
        CASE_OF(COR_E_MEMBERACCESS, "Access to this member is denied.");
        CASE_OF(COR_E_THREADSTATE, "Thread is in an invalid state for this operation.");
        CASE_OF(COR_E_THREADSTOP, "Thread is stopping.");
        CASE_OF(COR_E_TYPELOAD, "Could not find or load a type.");
        CASE_OF(COR_E_ENTRYPOINTNOTFOUND, "Could not find the specified DllImport entrypoint.");
        CASE_OF(COR_E_DLLNOTFOUND, "Could not find the specified DllImport Dll.");
        CASE_OF(COR_E_THREADSTART, "Indicate that a user thread fails to start.");
        CASE_OF(COR_E_INVALIDCOMOBJECT, "An invalid __ComObject has been used.");
        CASE_OF(COR_E_NOTFINITENUMBER, "Not a Number.");
        CASE_OF(COR_E_DUPLICATEWAITOBJECT, "An object appears more than once in the wait objects array.");
        CASE_OF(COR_E_SEMAPHOREFULL, "Reached maximum count for semaphore.");
        CASE_OF(COR_E_WAITHANDLECANNOTBEOPENED, "No semaphore of the given name exists.");
        CASE_OF(COR_E_ABANDONEDMUTEX, "The wait completed due to an abandoned mutex.");
        CASE_OF(COR_E_THREADABORTED, "Thread has aborted.");
        CASE_OF(COR_E_INVALIDOLEVARIANTTYPE, "OLE Variant has an invalid type.");
        CASE_OF(COR_E_MISSINGMANIFESTRESOURCE, "An expected resource in the assembly manifest was missing.");
        CASE_OF(COR_E_SAFEARRAYTYPEMISMATCH, "A mismatch has occurred between the runtime type of the array and the sub type recorded in the metadata.");
        CASE_OF(COR_E_TYPEINITIALIZATION, "Uncaught exception during type initialization.");
        CASE_OF(COR_E_MARSHALDIRECTIVE, "Invalid marshaling directives.");
        CASE_OF(COR_E_MISSINGSATELLITEASSEMBLY, "An expected satellite assembly containing the ultimate fallback resources for a given culture was not found or could not be loaded.");
        CASE_OF(COR_E_FORMAT, "The format of one argument does not meet the contract of the method.");
        CASE_OF(COR_E_SAFEARRAYRANKMISMATCH, "A mismatch has occurred between the runtime rank of the array and the rank recorded in the metadata.");
        CASE_OF(COR_E_PLATFORMNOTSUPPORTED, "Operation is not supported on this platform.");
        CASE_OF(COR_E_INVALIDPROGRAM, "Invalid IL or CLR metadata.");
        CASE_OF(COR_E_OPERATIONCANCELED, "The operation was cancelled.");
        CASE_OF(COR_E_INSUFFICIENTMEMORY, "Not enough memory was available for an operation.");
        CASE_OF(COR_E_RUNTIMEWRAPPED, "An object that does not derive from System.Exception has been wrapped in a RuntimeWrappedException.");
        CASE_OF(COR_E_DATAMISALIGNED, "A datatype misalignment was detected in a load or store instruction.");
        CASE_OF(COR_E_CODECONTRACTFAILED, "A managed code contract (ie, precondition, postcondition, invariant, or assert) failed.");
        CASE_OF(COR_E_TYPEACCESS, "Access to this type is denied.");
        CASE_OF(COR_E_ACCESSING_CCW, "Fail to access a CCW because the corresponding managed object is already collected.");
        CASE_OF(COR_E_KEYNOTFOUND, "The given key was not present in the dictionary.");
        CASE_OF(COR_E_INSUFFICIENTEXECUTIONSTACK, "Insufficient stack to continue executing the program safely. This can happen from having too many functions on the call stack or function on the stack using too much stack space.");
        CASE_OF(COR_E_APPLICATION, "Application exception");
        CASE_OF(COR_E_INVALIDFILTERCRITERIA, "The given filter criteria does not match the filter content.");
        CASE_OF(COR_E_REFLECTIONTYPELOAD, "Could not find or load a specific class that was requested through Reflection.");
        CASE_OF(COR_E_TARGET, "Attempt to invoke non-static method with a null Object.");
        CASE_OF(COR_E_TARGETINVOCATION, "Uncaught exception thrown by method called through Reflection.");
        CASE_OF(COR_E_CUSTOMATTRIBUTEFORMAT, "Custom attribute has invalid format.");
        CASE_OF(COR_E_IO, "Error during managed I/O.");
        CASE_OF(COR_E_FILELOAD, "Could not find or load a specific file.");
        CASE_OF(COR_E_OBJECTDISPOSED, "The object has already been disposed.");
        CASE_OF(COR_E_FAILFAST, "Runtime operation halted by call to System.Environment.FailFast().");
        CASE_OF(COR_E_HOSTPROTECTION, "The host has forbidden this operation.");
        CASE_OF(COR_E_ILLEGAL_REENTRANCY, "Attempted to call into managed code when executing inside a low level extensibility point.");
        CASE_OF(CLR_E_SHIM_RUNTIMELOAD, "Failed to load the runtime.");
        CASE_OF(CLR_E_SHIM_LEGACYRUNTIMEALREADYBOUND, "A runtime has already been bound for legacy activation policy use.");
        CASE_OF(VER_E_FIELD_SIG, "[field sig]");
        CASE_OF(VER_E_CIRCULAR_VAR_CONSTRAINTS, "Method parent has circular class type parameter constraints.");
        CASE_OF(VER_E_CIRCULAR_MVAR_CONSTRAINTS, "Method has circular method type parameter constraints.");
        CASE_OF(COR_E_Data, "COR_E_Data");
        CASE_OF(VLDTR_E_SIG_BADVOID, "Illegal 'void' in signature.");
        CASE_OF(VLDTR_E_GP_ILLEGAL_VARIANT_MVAR, "GenericParam is a method type parameter and must be non-variant.");
        CASE_OF(CORDBG_E_THREAD_NOT_SCHEDULED, "Thread is not scheduled. Thus we may not have OSThreadId, handle, or context.");
        CASE_OF(CORDBG_E_HANDLE_HAS_BEEN_DISPOSED, "Handle has been disposed.");
        CASE_OF(CORDBG_E_NONINTERCEPTABLE_EXCEPTION, "Cannot intercept this exception.");
        CASE_OF(CORDBG_E_INTERCEPT_FRAME_ALREADY_SET, "The intercept frame for this exception has already been set.");
        CASE_OF(CORDBG_E_NO_NATIVE_PATCH_AT_ADDR, "There is no native patch at the given address.");
        CASE_OF(CORDBG_E_MUST_BE_INTEROP_DEBUGGING, "This API is only allowed when interop debugging.");
        CASE_OF(CORDBG_E_NATIVE_PATCH_ALREADY_AT_ADDR, "There is already a native patch at the address.");
        CASE_OF(CORDBG_E_TIMEOUT, "A wait timed out, likely an indication of deadlock.");
        CASE_OF(CORDBG_E_CANT_CALL_ON_THIS_THREAD, "Cannot use the API on this thread.");
        CASE_OF(CORDBG_E_ENC_INFOLESS_METHOD, "Method was not JIT'd in EnC mode.");
        CASE_OF(CORDBG_E_ENC_IN_FUNCLET, "Method is in a callable handler/filter. Cannot increase stack.");
        CASE_OF(CORDBG_E_ENC_EDIT_NOT_SUPPORTED, "Attempt to perform unsupported edit.");
        CASE_OF(CORDBG_E_NOTREADY, "The LS is not in a good spot to perform the requested operation.");
        CASE_OF(CORDBG_E_CANNOT_RESOLVE_ASSEMBLY, "We failed to resolve assembly given an AssemblyRef token. Assembly may be not loaded yet or not a valid token.");
        CASE_OF(CORDBG_E_MUST_BE_IN_LOAD_MODULE, "Must be in context of LoadModule callback to perform requested operation.");
        CASE_OF(CORDBG_E_CANNOT_BE_ON_ATTACH, "Requested operation cannot be performed during an attach operation.");
        CASE_OF(CORDBG_E_NGEN_NOT_SUPPORTED, "NGEN must be supported to perform the requested operation.");
        CASE_OF(CORDBG_E_ILLEGAL_SHUTDOWN_ORDER, "Trying to shutdown out of order.");
        CASE_OF(CORDBG_E_CANNOT_DEBUG_FIBER_PROCESS, "Debugging fiber mode managed process is not supported.");
        CASE_OF(CORDBG_E_MUST_BE_IN_CREATE_PROCESS, "Must be in context of CreateProcess callback to perform requested operation.");
        CASE_OF(CORDBG_E_DETACH_FAILED_OUTSTANDING_EVALS, "All outstanding func-evals have not completed, detaching is not allowed at this time.");
        CASE_OF(CORDBG_E_DETACH_FAILED_OUTSTANDING_STEPPERS, "All outstanding steppers have not been closed, detaching is not allowed at this time.");
        CASE_OF(CORDBG_E_CANT_INTEROP_STEP_OUT, "Cannot have an ICorDebugStepper do a native step-out.");
        CASE_OF(CORDBG_E_DETACH_FAILED_OUTSTANDING_BREAKPOINTS, "All outstanding breakpoints have not been closed, detaching is not allowed at this time.");
        CASE_OF(CORDBG_E_ILLEGAL_IN_STACK_OVERFLOW, "The operation is illegal because of a stack overflow.");
        CASE_OF(CORDBG_E_ILLEGAL_AT_GC_UNSAFE_POINT, "The operation failed because it is a GC unsafe point.");
        CASE_OF(CORDBG_E_ILLEGAL_IN_PROLOG, "The operation failed because the thread is in the prolog.");
        CASE_OF(CORDBG_E_ILLEGAL_IN_NATIVE_CODE, "The operation failed because the thread is in native code.");
        CASE_OF(CORDBG_E_ILLEGAL_IN_OPTIMIZED_CODE, "The operation failed because the thread is in optimized code.");
        CASE_OF(CORDBG_E_APPDOMAIN_MISMATCH, "A supplied object or type belongs to the wrong AppDomain.");
        CASE_OF(CORDBG_E_CONTEXT_UNVAILABLE, "The thread's context is not available.");
        CASE_OF(CORDBG_E_UNCOMPATIBLE_PLATFORMS, "The operation failed because debuggee and debugger are on incompatible platforms.");
        CASE_OF(CORDBG_E_DEBUGGING_DISABLED, "The operation failed because the debugging has been disabled");
        CASE_OF(CORDBG_E_DETACH_FAILED_ON_ENC, "Detach is illegal after an Edit and Continue on a module.");
        CASE_OF(CORDBG_E_CURRENT_EXCEPTION_IS_OUTSIDE_CURRENT_EXECUTION_SCOPE, "Cannot intercept the current exception at the specified frame.");
        CASE_OF(CORDBG_E_HELPER_MAY_DEADLOCK, "The debugger helper thread cannot obtain the locks it needs to perform this operation.");
        CASE_OF(CORDBG_E_MISSING_METADATA, "The operation failed because the debugger could not get the metadata.");
        CASE_OF(CORDBG_E_TARGET_INCONSISTENT, "The debuggee is in a corrupt state.");
        CASE_OF(CORDBG_E_DETACH_FAILED_OUTSTANDING_TARGET_RESOURCES, "Detach failed because there are outstanding resources in the target.");
        CASE_OF(CORDBG_E_TARGET_READONLY, "The debuggee is read-only.");
        CASE_OF(CORDBG_E_MISMATCHED_CORWKS_AND_DACWKS_DLLS, "The version of clr.dll in the target does not match the one mscordacwks.dll was built for.");
        CASE_OF(CORDBG_E_MODULE_LOADED_FROM_DISK, "Symbols are not supplied for modules loaded from disk.");
        CASE_OF(CORDBG_E_SYMBOLS_NOT_AVAILABLE, "The application did not supply symbols when it loaded or created this module, or they are not yet available.");
        CASE_OF(CORDBG_E_DEBUG_COMPONENT_MISSING, "A debug component is not installed.");
        CASE_OF(CORDBG_E_LIBRARY_PROVIDER_ERROR, "The ICLRDebuggingLibraryProvider callback returned an error or did not provide a valid handle.");
        CASE_OF(CORDBG_E_NOT_CLR, "The module at the base address indicated was not recognized as a CLR");
        CASE_OF(CORDBG_E_MISSING_DATA_TARGET_INTERFACE, "The provided data target does not implement the required interfaces for this version of the runtime");
        CASE_OF(CORDBG_E_UNSUPPORTED_DEBUGGING_MODEL, "This debugging model is unsupported by the specified runtime");
        CASE_OF(CORDBG_E_UNSUPPORTED_FORWARD_COMPAT, "The debugger is not designed to support the version of the CLR the debuggee is using.");
        CASE_OF(CORDBG_E_UNSUPPORTED_VERSION_STRUCT, "The version struct has an unrecognized value for wStructVersion");
        CASE_OF(CORDBG_E_READVIRTUAL_FAILURE, "A call into a ReadVirtual implementation returned failure");
        CASE_OF(CORDBG_E_VALUE_POINTS_TO_FUNCTION, "The Debugging API doesn't support dereferencing function pointers.");
        CASE_OF(CORDBG_E_CORRUPT_OBJECT, "The address provided does not point to a valid managed object.");
        CASE_OF(CORDBG_E_GC_STRUCTURES_INVALID, "The GC heap structures are not in a valid state for traversal.");
        CASE_OF(CORDBG_E_INVALID_OPCODE, "The specified IL offset or opcode is not supported for this operation.");
        CASE_OF(CORDBG_E_UNSUPPORTED, "The specified action is unsupported by this version of the runtime.");
        CASE_OF(CORDBG_E_MISSING_DEBUGGER_EXPORTS, "The debuggee memory space does not have the expected debugging export table.");
        CASE_OF(CORDBG_E_DATA_TARGET_ERROR, "Failure when calling a data target method.");
        CASE_OF(CORDBG_E_NO_IMAGE_AVAILABLE, "Couldn't find a native image.");
        CASE_OF(CORDBG_E_UNSUPPORTED_DELEGATE, "The delegate contains a delegate currently not supported by the API.");
        CASE_OF(PEFMT_E_64BIT, "File is PE32+.");
        CASE_OF(PEFMT_E_32BIT, "File is PE32");
        CASE_OF(NGEN_E_SYS_ASM_NI_MISSING, "NGen cannot proceed because Mscorlib.dll does not have a native image");
        CASE_OF(CLDB_E_INTERNALERROR, "CLDB_E_INTERNALERROR");
        CASE_OF(CLR_E_BIND_ASSEMBLY_VERSION_TOO_LOW, "The bound assembly has a version that is lower than that of the request.");
        CASE_OF(CLR_E_BIND_ASSEMBLY_PUBLIC_KEY_MISMATCH, "The assembly version has a public key token that does not match that of the request.");
        CASE_OF(CLR_E_BIND_IMAGE_UNAVAILABLE, "The requested image was not found or is unavailable.");
        CASE_OF(CLR_E_BIND_UNRECOGNIZED_IDENTITY_FORMAT, "The provided identity format is not recognized.");
        CASE_OF(CLR_E_BIND_ASSEMBLY_NOT_FOUND, "A binding for the specified assembly name was not found.");
        CASE_OF(CLR_E_BIND_TYPE_NOT_FOUND, "A binding for the specified type name was not found.");
        CASE_OF(CLR_E_BIND_SYS_ASM_NI_MISSING, "Could not use native image because Mscorlib.dll is missing a native image");
        CASE_OF(CLR_E_BIND_NI_SECURITY_FAILURE, "Native image was generated in a different trust level than present at runtime");
        CASE_OF(CLR_E_BIND_NI_DEP_IDENTITY_MISMATCH, "Native image identity mismatch with respect to its dependencies");
        CASE_OF(CLR_E_GC_OOM, "Failfast due to an OOM during a GC");
        CASE_OF(CLR_E_GC_BAD_AFFINITY_CONFIG, "GCHeapAffinitizeMask or GCHeapAffinitizeRanges didn't specify any CPUs the current process is affinitized to.");
        CASE_OF(CLR_E_GC_BAD_AFFINITY_CONFIG_FORMAT, "GCHeapAffinitizeRanges configuration string has invalid format.");
        CASE_OF(CLR_E_CROSSGEN_NO_IBC_DATA_FOUND, "Cannot compile using the PartialNgen flag because no IBC data was found.");
        CASE_OF(COR_E_UNAUTHORIZEDACCESS, "Access is denied.");
        CASE_OF(COR_E_ARGUMENT, "An argument does not meet the contract of the method.");
        CASE_OF(COR_E_INVALIDCAST, "Indicates a bad cast condition");
        CASE_OF(COR_E_OUTOFMEMORY, "The EE thows this exception when no more memory is avaible to continue execution");
        CASE_OF(COR_E_NULLREFERENCE, "Dereferencing a null reference. In general class libraries should not throw this");
        CASE_OF(COR_E_ARITHMETIC, "Overflow or underflow in mathematical operations.");
        CASE_OF(COR_E_PATHTOOLONG, "The specified path was too long.");
        CASE_OF(COR_E_FILENOTFOUND, "COR_E_FILENOTFOUND");
        CASE_OF(COR_E_ENDOFSTREAM, "Thrown when the End of file is reached");
        CASE_OF(COR_E_DIRECTORYNOTFOUND, "The specified path couldn't be found.");
        CASE_OF(COR_E_STACKOVERFLOW, "Is raised by the EE when the execution stack overflows as it is attempting to ex");
        CASE_OF(COR_E_AMBIGUOUSMATCH, "While late binding to a method via reflection, could not resolve between");
        CASE_OF(COR_E_TARGETPARAMCOUNT, "There was a mismatch between number of arguments provided and the number expected");
        CASE_OF(COR_E_DIVIDEBYZERO, "Attempted to divide a number by zero.");
        CASE_OF(COR_E_BADIMAGEFORMAT, "The format of a DLL or executable being loaded is invalid.");
	default: 
            str = "Unknown HRESULT code";
	    break;
    }
    return str;
}
