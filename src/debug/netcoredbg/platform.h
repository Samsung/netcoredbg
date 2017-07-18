unsigned long OSPageSize();
void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList);
std::string GetExeAbsPath();
std::string GetCoreCLRPath(int pid);
std::string GetFileName(const std::string &path);
