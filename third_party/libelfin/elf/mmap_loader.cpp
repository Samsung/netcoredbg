// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "elf++.h"

#include <system_error>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

ELFPP_BEGIN_NAMESPACE

class mmap_loader : public loader
{
        void *base;
        size_t lim;

public:
        mmap_loader(int fd)
        {
                off_t end = lseek(fd, 0, SEEK_END);
                if (end == (off_t)-1)
                        throw std::system_error(errno, std::system_category(),
                                                "finding file length");
                lim = end;

                base = mmap(nullptr, lim, PROT_READ, MAP_SHARED, fd, 0);
                if (base == MAP_FAILED)
                        throw std::system_error(errno, std::system_category(),
                                                "mmap'ing file");
                close(fd);
        }

        ~mmap_loader()
        {
                munmap(base, lim);
        }

        const void *load(off_t offset, size_t size)
        {
                if (offset + size > lim)
                        throw std::range_error("offset exceeds file size");
                return (const char*)base + offset;
        }
};

std::shared_ptr<loader>
create_mmap_loader(int fd)
{
        return std::make_shared<mmap_loader>(fd);
}

ELFPP_END_NAMESPACE
