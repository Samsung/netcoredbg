#include "cor.h"
#include "interfaces/idebugger.h"

#include <vector>
#include <string>
#include <list>

#define STORAGE_MAX_SIZE    1000000

namespace netcoredbg
{

class SourceStorage 
{
    struct SourceFile
    {
        std::string filePath;
        std::vector<char*> lines;
        char* text;
        int size;
    };

    std::list<SourceFile*> files;
    IDebugger* m_dbg;
    int totalLen;

private:
    HRESULT loadFile(std::string& file);

public:
    SourceStorage(IDebugger* d) 
    {
        m_dbg = d;
        totalLen = 0;
    }
    ~SourceStorage();

    char* getLine(std::string& file, int linenum);

}; // class sourcestorage
} // namespace