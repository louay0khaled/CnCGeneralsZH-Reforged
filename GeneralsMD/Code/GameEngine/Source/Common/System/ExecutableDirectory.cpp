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

// FILE: ExecutableDirectory.cpp //////////////////////////////////////////////////////////////////
// Desc:   The directory the running executable is in.
///////////////////////////////////////////////////////////////////////////////////////////////////

/* MiniLog.cpp, MemoryInit.cpp and Debug.cpp each had this inline: GetModuleFileName, then a walk
	 back from the end to the last '\\'.  On Windows the body below is that code, once.  Off Windows
	 only the path differs, and the walk is the same with '/'. */

#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/ExecutableDirectory.h"
#if !defined(_WIN32)
#include "Common/EarlyOptions.h"	// findUserDataDirectory, for getLogDirectory
#endif

#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <stdlib.h>
#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/* The executable's path with symbolic links resolved, or "" if it cannot be had whole.  POSIX has
	 no call for it, so each platform answers in its own way.  Linux's /proc/self/exe is already the
	 resolved file.  Darwin's _NSGetExecutablePath is the path the program was started by - through a
	 symbolic link, the link's directory (measured) - so it goes through realpath, and the two agree:
	 the directory the binary itself is in.  A path that does not fit is "", never a truncated one,
	 which would name some other directory. */
static void readExecutablePath( char *buf, size_t size )
{
#if defined(__APPLE__)
	char started[ PATH_MAX ];
	uint32_t length = sizeof( started );
	char resolved[ PATH_MAX ];
	if (_NSGetExecutablePath( started, &length ) != 0 || realpath( started, resolved ) == NULL
			|| strlcpy( buf, resolved, size ) >= size)
		buf[0] = 0;
#elif defined(__linux__)
	const ssize_t length = readlink( "/proc/self/exe", buf, size );		// readlink does not terminate
	buf[ (length > 0 && (size_t)length < size) ? length : 0 ] = 0;		// length == size: it may not all be there
#else
#error "getExecutableDirectory: find the running executable's path on this platform"
#endif
}
#endif

void getExecutableDirectory( char *buf, size_t size, Bool keepTrailingSeparator )
{
#ifdef _WIN32
	const char separator = '\\';
	::GetModuleFileName( NULL, buf, (DWORD)size );
#else
	const char separator = '/';
	readExecutablePath( buf, size );
#endif
	char *pEnd = buf + strlen( buf );
	while( pEnd != buf )
	{
		if( *pEnd == separator )
		{
			if (keepTrailingSeparator)
				*(pEnd + 1) = 0;
			else
				*pEnd = 0;
			break;
		}
		pEnd--;
	}
}

#if !defined(_WIN32)
Bool isExecutableInAppBundle( void )
{
	char exe[ 4096 ];
	getExecutableDirectory( exe, sizeof( exe ), FALSE );
	static const char bundled[] = ".app/Contents/MacOS";
	const size_t length = strlen( exe ), tail = sizeof( bundled ) - 1;
	return length >= tail && strcmp( exe + length - tail, bundled ) == 0;
}

Bool isExecutableInLinuxPackage( void )
{
	char exe[ 4096 ];
	getExecutableDirectory( exe, sizeof( exe ), FALSE );
	if (exe[0] == 0 || strlcat( exe, "/../share/zero-hour-reforged/overlay", sizeof( exe ) ) >= sizeof( exe ))
		return FALSE;
	struct stat status;
	return stat( exe, &status ) == 0 && S_ISDIR( status.st_mode );
}

Bool isExecutablePackaged( void )
{
	return isExecutableInAppBundle() || isExecutableInLinuxPackage();
}

void getLogDirectory( char *buf, size_t size, Bool keepTrailingSeparator )
{
#if defined(__ANDROID__)
	/*
	 * Android's native .so directory is immutable. Use the explicit external app-specific
	 * diagnostics directory selected during the early Android bootstrap. It is a real POSIX path.
	 */
	const char *override = getenv( "ZH_ANDROID_DIAGNOSTICS_DIR" );
	char androidLogs[ 4096 ];
	buf[0] = 0;
	if (override != NULL && override[0] != '\0')
	{
		if (strlcpy( androidLogs, override, sizeof( androidLogs) ) >= sizeof( androidLogs ))
			return;
	}
	else
	{
		char userData[ 4096 ];
		if (!findUserDataDirectory( userData, sizeof( userData) ))
			return;
		size_t length = strlen( userData );
		while (length > 0 && (userData[ length - 1 ] == '\\' || userData[ length - 1 ] == '/'))
			userData[ --length ] = 0;
		if (snprintf( androidLogs, sizeof( androidLogs ), "%s/Logs", userData ) <= 0)
			return;
	}
	mkdir( androidLogs, 0777 );
	if (strlcpy( buf, androidLogs, size ) >= size ||
			(keepTrailingSeparator && strlcat( buf, "/", size) >= size))
		buf[0] = 0;
	return;
#endif

	getExecutableDirectory( buf, size, keepTrailingSeparator );
	if (!isExecutablePackaged())
		return;

	char logs[ 4096 ];
	buf[0] = 0;
	if (!findUserDataDirectory( logs, sizeof( logs ) ) || strlcat( logs, "Logs", sizeof( logs ) ) >= sizeof( logs ))
		return;
	zh_mkdir( logs );		// absolute, so the read-only root does not apply; EEXIST after the first run
	if (strlcpy( buf, logs, size ) >= size || (keepTrailingSeparator && strlcat( buf, "\\", size ) >= size))
		buf[0] = 0;
}
#endif
