static inline std::string to_utf8(const WCHAR *wstr, int len = -1)
{
    if (len == -1)
        len = _wcslen(wstr);
    if (len == 0)
        return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, len, NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
