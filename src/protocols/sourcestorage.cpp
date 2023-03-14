#include "sourcestorage.h"
#include "utils/torelease.h"

namespace netcoredbg
{

    SourceStorage::~SourceStorage()
    {
        // Free memory and clear list elements
        while(!files.empty())
        {
            files.back()->lines.clear();
            m_dbg->FreeUnmanaged(files.back()->text);
            delete files.back();
            files.pop_back();
        }
    }

    char* SourceStorage::getLine(std::string& file, int linenum)
    {
        if(files.empty() || files.front()->filePath != file)
        {
            auto files_it = files.begin();
            bool notFound = true;
            for ( ; files_it != files.end(); files_it++)
            {
                if((*files_it)->filePath == file)
                {
                    files.push_front(*files_it);
                    files.erase(files_it);
                    notFound = false;
                    break;
                }
            }

            if (notFound)
            {
                // file is not in the list -- try to load it from pdb
                if (loadFile(file) != S_OK)
                    return NULL;
            }
        }
        
        if ((int)files.front()->lines.size() > linenum)
            return files.front()->lines[linenum];
        else
            return NULL;
    }

    HRESULT SourceStorage::loadFile(std::string& file)
    {
        char* fileBuff = NULL;
        int fileLen = 0;
        HRESULT Status = S_OK;

        IfFailRet(m_dbg->GetSourceFile(file, &fileBuff, &fileLen));
        SourceFile *sf = new SourceFile();
        sf->filePath = file;
        sf->size = fileLen;
        sf->text = fileBuff;

        sf->lines.push_back(NULL); // The lines count begins from 1, the 0th line is empty
        for(char* bufptr = sf->text; bufptr < sf->text + fileLen; )
        {
            sf->lines.push_back(bufptr);
            while(*bufptr != '\r')
                bufptr++;
            *bufptr++ = '\0';
            if (*bufptr == '\n')
                *bufptr++ = '\0';
        }

        totalLen += fileLen;
        files.push_front(sf);

        // Check if the storage exceeds max size and remove the oldest files if it does.
        // Do not remove the most recent file even if it's size exceeds max size of the storage
        while (totalLen > STORAGE_MAX_SIZE && files.size() > 1) {
            m_dbg->FreeUnmanaged(files.back()->text);
            totalLen -= files.back()->size;
            files.pop_back();
        }
        return Status;
    }
} //namespace netcoredbg