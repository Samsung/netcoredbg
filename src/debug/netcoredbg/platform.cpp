#include <string>
#include <cstring>
#include <set>
#include <fstream>

#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_traps.h>
#include <mach/mach_init.h>

#include <mach-o/dyld_images.h>
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

#if defined(__APPLE__)
static unsigned char *readProcessMemory(task_t t,
                                        mach_vm_address_t addr,
                                        mach_msg_type_number_t* size)
{
    mach_msg_type_number_t  dataCnt = (mach_msg_type_number_t) *size;
    vm_offset_t readMem;

    // Use vm_read, rather than mach_vm_read, since the latter is different in
    // iOS.

    kern_return_t kr = vm_read(t,           // vm_map_t target_task,
                    addr,                   // mach_vm_address_t address,
                    *size,                  // mach_vm_size_t size
                    &readMem,               // vm_offset_t *data,
                    &dataCnt);              // mach_msg_type_number_t *dataCnt

    if (kr) {
        fprintf (stderr, "Unable to read target task's memory @%p - kr 0x%x\n" ,
                (void *) addr, kr);
            return NULL;
    }

    return ( (unsigned char *) readMem);
}
#endif

std::string GetCoreCLRPath(int pid)
{
#if defined(__APPLE__)
    static const char *coreclr_dylib = "/libcoreclr.dylib";
    static const std::size_t coreclr_dylib_len = strlen(coreclr_dylib);
    task_t task;
    std::string result;

    if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS)
        return result;

    struct task_dyld_info dyld_info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld_info, &count) != KERN_SUCCESS)
        return result;

    mach_msg_type_number_t size = sizeof(struct dyld_all_image_infos);

    uint8_t* data =
        readProcessMemory(task, dyld_info.all_image_info_addr, &size);
    struct dyld_all_image_infos* infos = (struct dyld_all_image_infos *) data;

    mach_msg_type_number_t size2 =
        sizeof(struct dyld_image_info) * infos->infoArrayCount;
    uint8_t* info_addr =
        readProcessMemory(task, (mach_vm_address_t) infos->infoArray, &size2);
    struct dyld_image_info* info = (struct dyld_image_info*) info_addr;

    for (int im = 0; im < infos->infoArrayCount; im++) {
        mach_msg_type_number_t size3 = PATH_MAX;

        uint8_t* fpath_addr = readProcessMemory(task,
                (mach_vm_address_t) info[im].imageFilePath, &size3);

        if (!fpath_addr)
            continue;

        std::string filepath = (char *)fpath_addr;
        vm_deallocate(mach_task_self(), (mach_vm_address_t) fpath_addr, size3);

        std::size_t i = filepath.rfind(coreclr_dylib);
        if (i == std::string::npos)
            continue;
        if (i + coreclr_dylib_len != filepath.size())
            continue;

        result = filepath;
        break;
    }

    vm_deallocate(mach_task_self(), (mach_vm_address_t) info_addr, size2);
    vm_deallocate(mach_task_self(), (mach_vm_address_t) data, size);
    return result;
#else
    static const char *coreclr_so = "/libcoreclr.so";
    static const std::size_t coreclr_so_len = strlen(coreclr_so);

    char maps_name[100];
    snprintf(maps_name, sizeof(maps_name), "/proc/%i/maps", pid);
    std::ifstream input(maps_name);

    for(std::string line; std::getline(input, line); )
    {
        std::size_t i = line.rfind(coreclr_so);
        if (i == std::string::npos)
            continue;
        if (i + coreclr_so_len != line.size())
            continue;
        std::size_t si = line.rfind(' ', i);
        if (i == std::string::npos)
            continue;
        return line.substr(si + 1);//, i - si - 1);
    }
    return std::string();
#endif
}
