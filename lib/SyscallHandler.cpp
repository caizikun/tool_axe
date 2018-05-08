// Copyright (c) 2011, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "Exceptions.h"
#include "Core.h"
#include "SyscallHandler.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include "Tracer.h"
#include "Endianness.h"

#ifndef _MSC_VER
#include <unistd.h>
#else
#include <io.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define dup _dup
#define open _open
#define close _close
#define read _read
#define write _write
#define lseek _lseek
#endif

using namespace axe;
using namespace Register;

enum SyscallType {
  OSCALL_EXIT = 0,
  OSCALL_PRINTC = 1,
  OSCALL_PRINTINT = 2,
  OSCALL_OPEN = 3,
  OSCALL_CLOSE = 4,
  OSCALL_READ = 5,
  OSCALL_WRITE = 6,
  OSCALL_DONE = 7,
  OSCALL_LSEEK = 8,
  OSCALL_RENAME = 9,
  OSCALL_TIME = 10,
  OSCALL_REMOVE = 11,
  OSCALL_SYSTEM = 12,
  OSCALL_ARGV = 13,
  OSCALL_IS_SIMULATION = 99,
  OSCALL_LOAD_IMAGE = 101
};

enum LseekType {
  XCORE_SEEK_CUR = 1,
  XCORE_SEEK_END = 2,
  XCORE_SEEK_SET = 0
};

enum OpenMode {
  XCORE_S_IREAD = 0400,
  XCORE_S_IWRITE = 0200,
  XCORE_S_IEXEC = 0100
};

enum OpenFlags {
  XCORE_O_RDONLY = 0x0001,
  XCORE_O_WRONLY = 0x0002,
  XCORE_O_RDWR = 0x0004,
  XCORE_O_CREAT = 0x0100,
  XCORE_O_TRUNC = 0x0200,
  XCORE_O_APPEND = 0x0800,
  XCORE_O_TMPFILE = 0x1000,  /* host to delete when closed */
  XCORE_O_BINARY = 0x8000
};

const unsigned MAX_FDS = 512;

static bool defaultLoadImageCallback(Core &, void *, uint32_t, uint32_t)
{
  return false;
}

static bool
defaultDescribeExceptionCallback(const Thread &, uint32_t, uint32_t,
                                 std::string &)
{
  return false;
}

SyscallHandler::SyscallHandler() :
  fds(new int[MAX_FDS]), doneSyscallsRequired(1),
  loadImageCallback(&defaultLoadImageCallback),
  describeExceptionCallback(&defaultDescribeExceptionCallback)
{
  // Duplicate the standard file descriptors.
  fds[0] = dup(STDIN_FILENO);
  fds[1] = dup(STDOUT_FILENO);
  fds[2] = dup(STDERR_FILENO);

  for (unsigned i = 3; i<6; i++) {
    fds[i] = 0;
  }
 
  // The rest are initialised to -1.
  for (unsigned i = 6; i < MAX_FDS; i++) {
    fds[i] = - 1;
  }
}

/// Returns a pointer to a string in memory at the given address.
/// Returns 0 if the address is invalid or the string is not null terminated.
char *SyscallHandler::getString(Thread &thread, uint32_t startAddress)
{
  // TODO update for ROM.
  Core &core = thread.getParent();
  if (!core.isValidRamAddress(startAddress))
    return 0;
  // Check the string is null terminated
  uint32_t address = startAddress;
  uint32_t end = core.getRamSize() + core.getRamBase();
  for (; address < end && core.loadRamByte(address); address++) {}
  if (address >= end) {
    return 0;
  }
  return reinterpret_cast<char*>(core.ramBytePtr(startAddress));
}

/// Returns a pointer to a buffer in memory of the given size.
/// Returns 0 if the buffer address is invalid.
const void *SyscallHandler::
getBuffer(Thread &thread, uint32_t address, uint32_t size)
{
  Core &core = thread.getParent();
  if (core.isValidRamAddress(address)) {
    if (!core.isValidRamAddress(address + size)) {
      return 0;
    }
  } else if (core.isValidRomAddress(address)) {
    if (!core.isValidRomAddress(address + size)) {
      return 0;
    }
  } else {
    return 0;
  }
  return core.memPtr(address);
}

/// Returns a pointer to a buffer in memory of the given size.
/// Returns 0 if the buffer address is invalid.
void *SyscallHandler::
getRamBuffer(Thread &thread, uint32_t address, uint32_t size)
{
  Core &core = thread.getParent();
  if (!core.isValidRamAddress(address) ||
      !core.isValidRamAddress(address + size))
    return 0;
  return core.ramBytePtr(address);
}

/// Either returns an unused number for a file descriptor or -1 if there are no
/// file descriptors available.
int SyscallHandler::getNewFd()
{
  for (unsigned i = 0; i < MAX_FDS; i++) {
    if (fds[i] == -1) {
      return i;
    }
  }
  return -1;
}

bool SyscallHandler::isValidFd(int fd)
{
  return fd >= 0 && (unsigned)fd < MAX_FDS && fds[fd] != -1;
}

int SyscallHandler::convertOpenFlags(int flags)
{
  int converted = 0;
  if (flags & XCORE_O_RDONLY)
    converted |= O_RDONLY;
  if (flags & XCORE_O_WRONLY)
    converted |= O_WRONLY;
  if (flags & XCORE_O_RDWR)
    converted |= O_RDWR;
  if (flags & XCORE_O_CREAT)
    converted |= O_CREAT;
  if (flags & XCORE_O_TRUNC)
    converted |= O_TRUNC;
  if (flags & XCORE_O_APPEND)
    converted |= O_APPEND;
#ifdef O_BINARY
  if (flags & XCORE_O_BINARY)
    converted |= O_BINARY;
#endif
  return converted;
}

int SyscallHandler::convertOpenMode(int mode)
{
  int converted = 0;
  if (mode & XCORE_S_IREAD)
    converted |= S_IREAD;
  if (mode & XCORE_S_IWRITE)
    converted |= S_IWRITE;
  if (mode & XCORE_S_IEXEC)
    converted |= S_IEXEC;
  return converted;
}

bool SyscallHandler::convertLseekType(int whence, int &converted)
{
  switch (whence) {
  case XCORE_SEEK_CUR:
    converted = SEEK_CUR;
    return true;
  case XCORE_SEEK_END:
    converted = SEEK_END;
    return true;
  case XCORE_SEEK_SET:
    converted = SEEK_SET;
    return true;
  default:
    return false;
  }
}

void SyscallHandler::doException(const Thread &thread, uint32_t et, uint32_t ed)
{
  std::cout << "Unhandled exception: ";
  std::string description;
  if (describeExceptionCallback(thread, et, ed, description)) {
    std::cout << description << '\n';
  } else {
    std::cout << Exceptions::getExceptionName(et)
              << ", data: 0x" << std::hex << ed << std::dec << "\n";
    std::cout << "Register state:\n";
    thread.dump();
  }
}

void SyscallHandler::doException(const Thread &thread)
{
  doException(thread, thread.regs[ET], thread.regs[ED]);
}

void SyscallHandler::setCmdLine(int clientArgc, char **clientArgv) {
  cmdLine.arg.clear();
  cmdLine.arg.reserve(clientArgc);
  cmdLine.minBufBytes = 4;  // argV[argc] = null
  for(int i = 0; i < clientArgc; ++i) {
    cmdLine.arg.push_back(std::string(clientArgv[i]));
    // cmd line bytes = ptr + string + '\0'
    cmdLine.minBufBytes += (4 + cmdLine.arg.back().length() + 1);
  }
}

void SyscallHandler::setDoneSyscallsRequired(unsigned count) {
  doneSyscallsRequired = count;
}

#define TRACE(...) \
do { \
  Tracer *tracer = thread.getParent().getTracer(); \
  if (tracer) { \
    tracer->syscall(thread, __VA_ARGS__); \
  } \
} while(0)

SyscallHandler::SycallOutcome SyscallHandler::doOsCallExit(Thread &thread, int &retval)
{
  TRACE("exit", thread.regs[R1]);
  retval = thread.regs[R1];
  return SyscallHandler::EXIT;
}

SyscallHandler::SycallOutcome SyscallHandler::doOsCallDone(Thread &thread, int &retval)
{
  TRACE("done");
  if (doneSyscallsSeen.insert(&thread.getParent()).second &&
      --doneSyscallsRequired == 0) {
    doneSyscallsSeen.clear();
    retval = 0;
    return SyscallHandler::EXIT;
  }
  return SyscallHandler::CONTINUE;
}

SyscallHandler::SycallOutcome SyscallHandler::doOsCallOpen(Thread &thread, int &retval)
{
  uint32_t PathAddr = thread.regs[R1];
  const char *path = getString(thread, PathAddr);
  if (!path) {
    // Invalid argument
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  int argFlag = thread.regs[R2];
  int flags = convertOpenFlags(argFlag);
#ifdef _WIN32
  if (argFlag & XCORE_O_TMPFILE)
    flags |= _O_TEMPORARY;
#endif
  int mode = convertOpenMode(thread.regs[R3]);
  int fd = getNewFd();
  if (fd == -1) {
    // No free file descriptors
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  int hostfd = open(path, flags, mode);
  if (hostfd == -1) {
    // Call to open failed.
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
#ifndef _WIN32
  if (argFlag & XCORE_O_TMPFILE)
   (void) std::remove(path);  // A file is not really removed until it is closed.
#endif
  fds[fd] = hostfd;
  thread.regs[R0] = fd;
  return SyscallHandler::CONTINUE;
}

SyscallHandler::SycallOutcome SyscallHandler::doOsCallClose(Thread &thread, int &retval)
{
  if (!isValidFd(thread.regs[R1])) {
    // Invalid fd
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  int close_retval = close(fds[thread.regs[R1]]);
  if (close_retval == 0) {
    fds[thread.regs[R1]] = (uint32_t)-1;
  }
  thread.regs[R0] = close_retval;
  return SyscallHandler::CONTINUE;
}

SyscallHandler::SycallOutcome SyscallHandler::doOsCallRead(Thread &thread, int &retval)
{
  if (!isValidFd(thread.regs[R1])) {
    // Invalid fd
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  void *buf = getRamBuffer(thread, thread.regs[R2], thread.regs[R3]);
  if (!buf) {
    // Invalid buffer
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.regs[R0] = read(fds[thread.regs[R1]], buf, thread.regs[R3]);
  return SyscallHandler::CONTINUE;
}

SyscallHandler::SycallOutcome SyscallHandler::doOsCallWrite(Thread &thread, int &retval)
{
  if (!isValidFd(thread.regs[R1])) {
    // Invalid fd
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  const void *buf = getBuffer(thread, thread.regs[R2], thread.regs[R3]);
  if (!buf) {
    // Invalid buffer
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.regs[R0] = write(fds[thread.regs[R1]], buf, thread.regs[R3]);
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallLseek(Thread &thread, int &retval)
{
  if (!isValidFd(thread.regs[R1])) {
    // Invalid fd
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  int whence;
  if (! convertLseekType(thread.regs[R3], whence)) {
    // Invalid seek type
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.regs[R0] = lseek(fds[thread.regs[R1]], (int32_t)thread.regs[R2], whence);
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallRename(Thread &thread, int &retval)
{
  uint32_t OldPathAddr = thread.regs[R1];
  uint32_t NewPathAddr = thread.regs[R2];
  const char *oldpath = getString(thread, OldPathAddr);
  const char *newpath = getString(thread, NewPathAddr);
  if (!oldpath || !newpath) {
    // Invalid argument
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.regs[R0] = std::rename(oldpath, newpath);
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallTime(Thread &thread, int &retval)
{
  uint32_t TimeAddr = thread.regs[R1];
  uint32_t Time = (uint32_t)std::time(0);
  Core &core = thread.getParent();
  if (TimeAddr != 0) {
    if (!core.isValidRamAddress(TimeAddr) || (TimeAddr & 3)) {
      // Invalid address
      thread.regs[R0] = (uint32_t)-1;
      return SyscallHandler::CONTINUE;
    }
    core.storeWord(Time, TimeAddr);
    core.invalidateWord(TimeAddr);
  }
  thread.regs[R0] = Time;
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallRemove(Thread &thread, int &retval)
{
  uint32_t FileAddr = thread.regs[R1];
  const char *file = getString(thread, FileAddr);
  if (file == 0) {
    // Invalid argument
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.regs[R0] = std::remove(file);
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallSystem(Thread &thread, int &retval)
{
  uint32_t CmdAddr = thread.regs[R1];
  const char *command = 0;
  if (CmdAddr != 0) {
    command = getString(thread, CmdAddr);
    if (command == 0) {
      // Invalid argument
      thread.regs[R0] = (uint32_t)-1;
      return SyscallHandler::CONTINUE;
    }
  }
  thread.regs[R0] = std::system(command);
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallArgv(Thread &thread, int &retval)
{
  const int clientBuf = thread.regs[R1];
  const int bufBytes = thread.regs[R2];
  if (cmdLine.minBufBytes > bufBytes) {
    thread.regs[R0] = (uint32_t)-2;
    return SyscallHandler::CONTINUE;
  }
  uint8_t * const hostBuf = (uint8_t*)getRamBuffer(thread, clientBuf,
                                                   bufBytes);
  if (!hostBuf) {
    // Invalid buffer
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  int numArgs = cmdLine.arg.size();
  int argvPos = 0;
  int strPos = (4 * numArgs) + 4;  // address beyond argV[arg1,...,null]
  for (int i = 0; i < numArgs; ++i) {
    endianness::write32le(&hostBuf[argvPos], clientBuf+strPos);
    argvPos += 4;
    int argLen = cmdLine.arg[i].length();
    memcpy(&hostBuf[strPos], cmdLine.arg[i].data(), argLen);
    strPos += argLen;
    hostBuf[strPos++] = 0;
  }
  assert(strPos == cmdLine.minBufBytes
         && "cmdLine.minBufBytes incorrectly calculated\n");
  endianness::write32le(&hostBuf[argvPos], 0);
  thread.regs[R0] = numArgs;
  retval = 0;
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallIsSimulation(Thread &thread, int &retval)
{
  thread.regs[R0] = 1;
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::doOsCallLoadImage(Thread &thread, int &retval)
{
  uint32_t dst = thread.regs[R1];
  uint32_t src = thread.regs[R2];
  uint32_t size = thread.regs[R3];
  void *buf = getRamBuffer(thread, dst, size);
  if (!buf ||
      !loadImageCallback(thread.getParent(), buf, src, size)) {
    thread.regs[R0] = (uint32_t)-1;
    return SyscallHandler::CONTINUE;
  }
  thread.getParent().invalidateRange(dst, dst + size);
  thread.regs[R0] = 0;
  return SyscallHandler::CONTINUE;
}


SyscallHandler::SycallOutcome SyscallHandler::
doSyscall(Thread &thread, int &retval)
{
  switch (thread.regs[R0]) {
  case OSCALL_EXIT:
    return doOsCallExit(thread, retval);
  case OSCALL_DONE:
    return doOsCallDone(thread, retval);
  case OSCALL_OPEN:
    return doOsCallOpen(thread, retval); 
  case OSCALL_CLOSE:
    return doOsCallClose(thread, retval);
  case OSCALL_READ:
    return doOsCallRead(thread, retval);
  case OSCALL_WRITE:
    return doOsCallWrite(thread, retval); 
  case OSCALL_LSEEK:
    return doOsCallLseek(thread, retval);
  case OSCALL_RENAME:
    return doOsCallRename(thread, retval);
  case OSCALL_TIME:
    return doOsCallTime(thread, retval);
  case OSCALL_REMOVE:
    return doOsCallRemove(thread, retval);
  case OSCALL_SYSTEM:
    return doOsCallSystem(thread, retval);
  case OSCALL_ARGV:
    return doOsCallArgv(thread, retval);
  case OSCALL_IS_SIMULATION:
    return doOsCallIsSimulation(thread, retval);
  case OSCALL_LOAD_IMAGE:
    return doOsCallLoadImage(thread, retval);
  default:
    std::cerr << "Error: unknown system call number: " << thread.regs[R0] << "\n";
    retval = 1;
    return SyscallHandler::EXIT;
  }
}
