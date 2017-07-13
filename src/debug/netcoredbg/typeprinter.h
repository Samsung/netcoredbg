class TypePrinter
{
    static HRESULT NameForTypeDef(mdTypeDef tkTypeDef, IMetaDataImport *pImport, std::string &mdName,
                                  std::list<std::string> &args);
    static HRESULT NameForToken(mdTypeDef mb, IMetaDataImport *pImport, std::string &mdName, bool bClassName,
                                std::list<std::string> &args);
    static HRESULT AddGenericArgs(ICorDebugType *pType, std::list<std::string> &args);
    static HRESULT AddGenericArgs(ICorDebugFrame *pFrame, std::list<std::string> &args);
public:
    static HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &output);
    static HRESULT GetTypeOfValue(ICorDebugValue *pValue, std::string &output);
    static HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &elementType, std::string &arrayType);
    static HRESULT GetMethodName(ICorDebugFrame *pFrame, std::string &output);
};
