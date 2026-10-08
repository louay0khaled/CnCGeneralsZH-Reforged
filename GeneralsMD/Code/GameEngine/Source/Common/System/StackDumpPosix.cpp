/*
**	Copyright 2026 İlyas Akın
**	Additional terms under GNU GPL section 7 apply: see LICENSE.md.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: StackDumpPosix.cpp ///////////////////////////////////////////////////////////////////////
// Desc:   Common/StackDump.h's portable half, off Windows.
///////////////////////////////////////////////////////////////////////////////////////////////////

/* StackDump.cpp is the Windows body: dbghelp symbol lookup and an x64 context walk.  This is the
	 same interface on POSIX, from execinfo's backtrace() and dladdr(): frame addresses with the nearest
	 exported symbol and the module, in Windows' line shape, with no file or line.  A crash signal is
	 CrashHandlerPosix.cpp's, which writes ReleaseCrashInfo.txt without calling anything here.

	 Nothing here allocates on the way to its output.  It may run on a crash path, where the heap
	 cannot be trusted: with no callback it writes straight to stderr with backtrace_symbols_fd, and
	 with one each line is formatted into a stack buffer.  backtrace_symbols(), which mallocs, is not
	 used.  What a callback does with its line is the caller's business. */

#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/StackDump.h"

#include <dlfcn.h>
#include <stdint.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

AsciiString g_LastErrorDump;

static const unsigned int MAX_FRAMES = 64;

/* One frame in StackDump.cpp's WriteStackLine shape, "  <file>(<line>) : <function> 0x<address>", into
	 a caller's buffer.  In-process there is no line table, so the module stands where the file does and
	 the line is 0 (a launcher that symbolicates would have to add it): "  generals(0) :
	 GameEngine::update+0x1C4 0x0000000100E1F3A0".  Without a symbol the function is the module and its
	 offset, which atos resolves offline.  The trailing newline is the caller's, as on Windows. */
static void formatFrame( void *address, char *line, size_t lineSize )
{
	Dl_info info;
	const bool found = dladdr( address, &info ) != 0;
	const char *path = (found && info.dli_fname) ? info.dli_fname : "?";
	const char *slash = strrchr( path, '/' );
	const char *module = slash ? slash + 1 : path;
	if (found && info.dli_sname != NULL)
		snprintf( line, lineSize, "  %s(0) : %s+0x%lX 0x%016llX", module, info.dli_sname,
							(unsigned long)((char *)address - (char *)info.dli_saddr), (unsigned long long)(uintptr_t)address );
	else if (found && info.dli_fbase != NULL)
		snprintf( line, lineSize, "  %s(0) : %s+0x%lX 0x%016llX", module, module,
							(unsigned long)((char *)address - (char *)info.dli_fbase), (unsigned long long)(uintptr_t)address );
	else
		snprintf( line, lineSize, "  ?(0) : ? 0x%016llX", (unsigned long long)(uintptr_t)address );
}

void FillStackAddresses( void **addresses, unsigned int count, unsigned int skip )
{
#if defined(__ANDROID__)
	// Android API 28 does not expose the glibc execinfo backtrace API. Leave the optional
	// stack-address buffer empty; CrashHandlerPosix.cpp still records the fault PC/LR directly.
	(void)skip;
	for (unsigned int i = 0; i < count; ++i)
		addresses[ i ] = NULL;
	return;
#else
	void *frames[ MAX_FRAMES ];
	// one more than asked, because this function is itself a frame
	unsigned int wanted = count + skip + 1;
	if (wanted > MAX_FRAMES)
		wanted = MAX_FRAMES;
	const int got = backtrace( frames, (int)wanted );
	for (unsigned int i = 0; i < count; ++i)
	{
		const unsigned int from = i + skip + 1;
		addresses[ i ] = (from < (unsigned int)got) ? frames[ from ] : NULL;
	}
#endif
}

void StackDumpFromAddresses( void **addresses, unsigned int count, void (*callback)( const char * ) )
{
	if (callback == NULL)
	{
#if defined(__ANDROID__)
		(void)addresses;
		(void)count;
		return;
#else
		unsigned int n = 0;
		while (n < count && addresses[ n ] != NULL)
			++n;
		backtrace_symbols_fd( addresses, (int)n, STDERR_FILENO );
		return;
#endif
	}
	char line[ 512 ];
	for (unsigned int i = 0; i < count && addresses[ i ] != NULL; ++i)
	{
		formatFrame( addresses[ i ], line, sizeof( line ) );
		callback( line );
		callback( "\n" );		// WriteStackLine's two calls, so a callback sees the same pieces on both
	}
}

void StackDump( void (*callback)( const char * ) )
{
	void *frames[ MAX_FRAMES ];
	FillStackAddresses( frames, MAX_FRAMES - 2, 0 );
	if (callback)
		callback( "Call Stack\n**********\n" );
	StackDumpFromAddresses( frames, MAX_FRAMES - 2, callback );
}

void GetFunctionDetails( void *pointer, char *name, size_t nameSize, char *filename, size_t filenameSize,
												 unsigned int *linenumber, unsigned int *address )
{
	Dl_info info;
	const bool found = dladdr( pointer, &info ) != 0;
	if (name && nameSize)
		snprintf( name, nameSize, "%s", (found && info.dli_sname) ? info.dli_sname : "?" );
	if (filename && filenameSize)
		snprintf( filename, filenameSize, "%s", (found && info.dli_fname) ? info.dli_fname : "?" );
	if (linenumber)
		*linenumber = 0;		// no line information without a symbolizer; C5's
	if (address)
		*address = (found && info.dli_saddr) ? (unsigned int)((char *)pointer - (char *)info.dli_saddr) : 0;
}
