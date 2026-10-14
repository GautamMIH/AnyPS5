#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_NATIVESTAT_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_NATIVESTAT_HPP

#include <filesystem>
#include "SceTypes.hpp"

namespace File {

std::filesystem::path PathOrRelinkedModule(const std::filesystem::path& nativePath);
// stat/fstat: 0, or the host errno when the path or descriptor cannot be read.
int FillFileStat(const std::filesystem::path& nativePath, FileStat* sb);
int FillFileStatFromDescriptor(int descriptor, FileStat* sb);
// lstat: false with errno set when the path cannot be read.
bool FillLinkStat(const std::filesystem::path& nativePath, FileStat* sb);

}

#endif
