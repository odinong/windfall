#pragma once
#define BOOST_OS_WINDOWS 1
#define BOOST_OS_LINUX 0
#define BOOST_OS_MACOS 0
#define BOOST_OS_BSD 0
#define BOOST_OS_UNIX 0
#define BOOST_ARCH_X86_64 1
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

class SlimRWLock
{
public:
    SlimRWLock() { InitializeSRWLock(&m_lock); }
    void LockRead() { AcquireSRWLockShared(&m_lock); }
    void UnlockRead() { ReleaseSRWLockShared(&m_lock); }
    void LockWrite() { AcquireSRWLockExclusive(&m_lock); }
    void UnlockWrite() { ReleaseSRWLockExclusive(&m_lock); }
private:
    SRWLOCK m_lock;
};

inline uint32_t GetExceptionError() { return (uint32_t)GetLastError(); }
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
