class TypePrinter
{
    static HRESULT NameForTypeDef_s(mdTypeDef tkTypeDef, IMetaDataImport *pImport,
                                    WCHAR *mdName, size_t capacity_mdName);
    static HRESULT NameForToken_s(mdTypeDef mb, IMetaDataImport *pImport, WCHAR *mdName, size_t capacity_mdName,
                                  bool bClassName);
    static HRESULT AddGenericArgs(ICorDebugType *pType, std::stringstream &ss);
public:
    static HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &output);
    static HRESULT GetTypeOfValue(ICorDebugValue *pValue, std::string &output);
    static HRESULT GetMethodName(ICorDebugFrame *pFrame, std::string &output);
};
