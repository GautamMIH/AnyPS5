#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_NATIVESTAT_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_NATIVESTAT_HPP

#include <filesystem>
#include "SceTypes.hpp"

namespace File {

int FillFileStat(const std::filesystem::path& nativePath, FileStat* sb);
int FillFileStatFromDescriptor(int descriptor, FileStat* sb);

}

#endif
