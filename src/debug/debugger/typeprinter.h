class TypePrinter
{
    static HRESULT NameForTypeDef(mdTypeDef tkTypeDef, IMetaDataImport *pImport, std::string &mdName);
    static HRESULT NameForToken(mdTypeDef mb, IMetaDataImport *pImport, std::string &mdName, bool bClassName);
    static HRESULT AddGenericArgs(ICorDebugType *pType, std::stringstream &ss);
    static HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &elementType, std::string &arrayType);
public:
    static HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &output);
    static HRESULT GetTypeOfValue(ICorDebugValue *pValue, std::string &output);
    static HRESULT GetMethodName(ICorDebugFrame *pFrame, std::string &output);
};
