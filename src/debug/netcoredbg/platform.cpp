// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>
#include <cstring>
#include <set>
#include <fstream>

#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <linux/limits.h>
#endif


unsigned long OSPageSize()
{
    static unsigned long pageSize = 0;
    if (pageSize == 0)
        pageSize = sysconf(_SC_PAGESIZE);

    return pageSize;
}

std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList)
{
    const char * const tpaExtensions[] = {
                ".ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
                ".dll",
                ".ni.exe",
                ".exe",
                };

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr)
    {
        return;
    }

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        int extLength = strlen(ext);

        struct dirent* entry;

        // For all entries in the directory
        while ((entry = readdir(dir)) != nullptr)
        {
            // We are interested in files only
            switch (entry->d_type)
            {
            case DT_REG:
                break;

            // Handle symlinks and file systems that do not support d_type
            case DT_LNK:
            case DT_UNKNOWN:
                {
                    std::string fullFilename;

                    fullFilename.append(directory);
                    fullFilename.append("/");
                    fullFilename.append(entry->d_name);

                    struct stat sb;
                    if (stat(fullFilename.c_str(), &sb) == -1)
                    {
                        continue;
                    }

                    if (!S_ISREG(sb.st_mode))
                    {
                        continue;
                    }
                }
                break;

            default:
                continue;
            }

            std::string filename(entry->d_name);

            // Check if the extension matches the one we are looking for
            int extPos = filename.length() - extLength;
            if ((extPos <= 0) || (filename.compare(extPos, extLength, ext) != 0))
            {
                continue;
            }

            std::string filenameWithoutExt(filename.substr(0, extPos));

            // Make sure if we have an assembly with multiple extensions present,
            // we insert only one version of it.
            if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
            {
                addedAssemblies.insert(filenameWithoutExt);

                tpaList.append(directory);
                tpaList.append("/");
                tpaList.append(filename);
                tpaList.append(":");
            }
        }

        // Rewind the directory stream to be able to iterate over it for the next extension
        rewinddir(dir);
    }

    closedir(dir);
}

std::string GetExeAbsPath()
{
#if defined(__APPLE__)
    // On Mac, we ask the OS for the absolute path to the entrypoint executable
    uint32_t lenActualPath = 0;
    if (_NSGetExecutablePath(nullptr, &lenActualPath) == -1)
    {
        // OSX has placed the actual path length in lenActualPath,
        // so re-attempt the operation
        std::string resizedPath(lenActualPath, '\0');
        char *pResizedPath = const_cast<char *>(resizedPath.data());
        if (_NSGetExecutablePath(pResizedPath, &lenActualPath) == 0)
        {
            return pResizedPath;
        }
    }
    return std::string();
#else
    static const char* self_link = "/proc/self/exe";

    char exe[PATH_MAX];

    ssize_t r = readlink(self_link, exe, PATH_MAX - 1);

    if (r < 0)
    {
        return std::string();
    }

    exe[r] = '\0';

    return exe;
#endif
}

bool SetWorkDir(const std::string &path)
{
    return chdir(path.c_str()) == 0;
}

void USleep(uint32_t duration)
{
    usleep(duration);
}

void *DLOpen(const std::string &path)
{
    return dlopen(path.c_str(), RTLD_GLOBAL | RTLD_NOW);
}

void *DLSym(void *handle, const std::string &name)
{
    return dlsym(handle, name.c_str());
}
