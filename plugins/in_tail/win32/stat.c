/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2022 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <Windows.h>
#include <stdlib.h>
#include <stdint.h>
#include <io.h>
#include "interface.h"

/*
 * NTFS stat(2) emulation tailored for in_tail's usage.
 *
 * (1) Support st_ino (inode) for Windows NTFS.
 * (2) Support NTFS symlinks.
 * (3) Support large files >= 2GB.
 *
 * To use it, include "win32.h" and it will transparently
 * replace stat(), lstat() and fstat().
 */

#define UINT64(high, low) ((uint64_t) (high) << 32 | (low))
#define WINDOWS_TICKS_TO_SECONDS_RATIO 10000000
#define WINDOWS_EPOCH_TO_UNIX_EPOCH_DELTA 11644473600

/* 
 * FILETIME timestamps are represented in 100-nanosecond intervals,
 * because of this, that's why we need to divide the number by 10000000
 * in order to convert it to seconds.
 *
 * While UNIX timestamps use January 1, 1970 as epoch Windows FILETIME
 * timestamps use January 1, 1601. Because of this we need to subtract
 * 11644473600 seconds to account for it.
 *
 * Note: Even though this does not account for leap seconds it should be
 * accurate enough.
 */

static uint64_t filetime_to_epoch(FILETIME *ft)
{
    ULARGE_INTEGER timestamp;

    if (ft == NULL) {
        return 0;
    }

    timestamp.HighPart = ft->dwHighDateTime;
    timestamp.LowPart = ft->dwLowDateTime;

    timestamp.QuadPart /= WINDOWS_TICKS_TO_SECONDS_RATIO;
    timestamp.QuadPart -= WINDOWS_EPOCH_TO_UNIX_EPOCH_DELTA;

    return timestamp.QuadPart;
}

static int get_mode(unsigned int attr)
{
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        return WIN32_S_IFDIR;
    }
    return WIN32_S_IFREG;
}



static int is_symlink(const char *path)
{
    WIN32_FIND_DATA data;
    HANDLE h;

    h = FindFirstFileA(path, &data);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    FindClose(h);

    /*
     * A NTFS symlink is a file with a bit of metadata ("reparse point"),
     * So (1) check if the file has metadata and then (2) confirm that
     * it is indeed a symlink.
     */
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
            return 1;
        }
    }
    return 0;
}

static int hstat(HANDLE h, struct win32_stat *wst)
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_STANDARD_INFO std;

    if (!GetFileInformationByHandle(h, &info)) {
        return -1;
    }

    if (!GetFileInformationByHandleEx(h, FileStandardInfo,
                                      &std, sizeof(std))) {
        return -1;
    }

    wst->st_nlink = std.NumberOfLinks;
    if (std.DeletePending) {
        wst->st_nlink = 0;
    }

    wst->st_mode  = get_mode(info.dwFileAttributes);
    wst->st_size  = UINT64(info.nFileSizeHigh, info.nFileSizeLow);
    wst->st_ino   = UINT64(info.nFileIndexHigh, info.nFileIndexLow);
    wst->st_mtime = filetime_to_epoch(&info.ftLastWriteTime);

    return 0;
}

int win32_stat(const char *path, struct win32_stat *wst)
{
    HANDLE h;

    h = CreateFileA(path,
                    GENERIC_READ,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    NULL,           /* lpSecurityAttributes */
                    OPEN_EXISTING,  /* dwCreationDisposition */
                    0,              /* dwFlagsAndAttributes */
                    NULL);          /* hTemplateFile */

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (hstat(h, wst)) {
        CloseHandle(h);
        return -1;
    }

    CloseHandle(h);
    return 0;
}

int win32_lstat(const char *path, struct win32_stat *wst)
{
    HANDLE h;

    h = CreateFileA(path,
                    GENERIC_READ,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    NULL,           /* lpSecurityAttributes */
                    OPEN_EXISTING,  /* dwCreationDisposition */
                    FILE_FLAG_OPEN_REPARSE_POINT,
                    NULL);          /* hTemplateFile */

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (hstat(h, wst)) {
        CloseHandle(h);
        return -1;
    }

    if (is_symlink(path)) {
        wst->st_mode = WIN32_S_IFLNK;
    }

    CloseHandle(h);
    return 0;
}

int win32_fstat(int fd, struct win32_stat *wst)
{
    HANDLE h;

    h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    return hstat(h, wst);
}
