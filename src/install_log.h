#pragma once
#include "data_root.h"
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
inline void installLog(const char*format,...){
 (void)format;
}
