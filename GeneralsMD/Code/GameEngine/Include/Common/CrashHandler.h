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

// FILE: CrashHandler.h ///////////////////////////////////////////////////////////////////////////
// Desc:   What a crash leaves behind off Windows: ReleaseCrashInfo.txt from a signal (C5).
///////////////////////////////////////////////////////////////////////////////////////////////////

/* Windows turns a fault into a structured exception, dumps it (StackDump.cpp's DumpExceptionInfo) and
	 writes ReleaseCrashInfo.txt through ReleaseCrash.  Off Windows a fault is a signal, and a signal
	 handler may do almost nothing: CrashHandlerPosix.cpp writes the same file, in the same sections, with
	 only async-signal-safe calls, from what installCrashHandlers prepared beforehand, and then lets the
	 signal kill the process as it would have, so the operating system's own crash report is still made.
	 (C5.) */

#pragma once

#ifndef __CRASHHANDLER_H_
#define __CRASHHANDLER_H_

#if !defined(_WIN32)

/** Installs the handlers for the crash signals and prepares everything they need.  Call once, first
	* thing in main, before another thread exists; it also gives the calling thread its alternate stack. */
void installCrashHandlers( void );

/** Gives the calling thread an alternate signal stack, so a stack overflow there is still reported.
	* Threads that do not call it still report every other crash.  Safe to call more than once. */
void installThreadCrashStack( void );

/** The debug log's file descriptor, for the handler to write the report into as well; -1 when the
	* log is closed.  Debug.cpp calls it. */
void setCrashLogDescriptor( int fd );

/** Appends one normal, non-signal-handler line to Android startup diagnostics. */
void appendAndroidDiagnostic( const char *message );

#endif

#endif // __CRASHHANDLER_H_
