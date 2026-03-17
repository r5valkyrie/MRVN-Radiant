/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

//-----------------------------------------------------------------------------
//
//
// DESCRIPTION:
// deal with in/out tasks, for either stdin/stdout or network/XML stream
//

#include "cmdlib.h"
#include "inout.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <io.h>
#include "generic/vector.h"

#ifdef WIN32
#include <direct.h>
#include <windows.h>
#endif

// network broadcasting
#include "l_net/l_net.h"
#include "libxml/tree.h"

// utf8 conversion
#include <glib.h>

socket_t *brdcst_socket;
netmessage_t msg;

bool verbose = false;

// our main document
// is streamed through the network to Radiant
// possibly written to disk at the end of the run
//++timo FIXME: need to be global, required when creating nodes?
xmlDocPtr doc;

// some useful stuff
xmlNodePtr xml_NodeForVec( const Vector3& v ){
	xmlNodePtr ret;
	char buf[1024];

	sprintf( buf, "%f %f %f", v[0], v[1], v[2] );
	ret = xmlNewNode( NULL, (const xmlChar*)"point" );
	xmlNodeAddContent( ret, (const xmlChar*)buf );
	return ret;
}

void xml_message_flush();

// send a node down the stream, add it to the document
void xml_SendNode( xmlNodePtr node ){
	xml_message_flush(); /* flush regular print messages buffer, so that special ones will appear at correct spot */

	xmlBufferPtr xml_buf;
	char xmlbuf[MAX_NETMESSAGE]; // we have to copy content from the xmlBufferPtr into an aux buffer .. that sucks ..
	// this index loops through the node buffer
	int pos = 0;
	int size;

	xmlAddChild( doc->children, node );

	if ( brdcst_socket ) {
		xml_buf = xmlBufferCreate();
		xmlNodeDump( xml_buf, doc, node, 0, 0 );

		// the XML node might be too big to fit in a single network message
		// l_net library defines an upper limit of MAX_NETMESSAGE
		// there are some size check errors, so we use MAX_NETMESSAGE-10 to be safe
		// if the size of the buffer exceeds MAX_NETMESSAGE-10 we'll send in several network messages
		while ( pos < (int)xml_buf->use )
		{
			// what size are we gonna send now?
			if( xml_buf->use - pos < MAX_NETMESSAGE - 10 ){
				size = xml_buf->use - pos;
			}
			else{
				size = MAX_NETMESSAGE - 10;
				Sys_FPrintf( SYS_NOXMLflag | SYS_WRN, "Got to split the buffer\n" ); //++timo just a debug thing
			}
			memcpy( xmlbuf, xml_buf->content + pos, size );
			xmlbuf[size] = '\0';
			NMSG_Clear( &msg );
			NMSG_WriteString( &msg, xmlbuf );
			Net_Send( brdcst_socket, &msg );
			// now that the thing is sent prepare to loop again
			pos += size;
		}

#if 0
		// NOTE: the NMSG_WriteString is limited to MAX_NETMESSAGE
		// we will need to split into chunks
		// (we could also go lower level, in the end it's using send and receiv which are not size limited)
		//++timo FIXME: MAX_NETMESSAGE is not exactly the max size we can stick in the message
		//  there's some tweaking to do in l_net for that .. so let's give us a margin for now

		//++timo we need to handle the case of a buffer too big to fit in a single message
		// try without checks for now
		if ( xml_buf->use > MAX_NETMESSAGE - 10 ) {
			// if we send that we are probably gonna break the stream at the other end..
			// and Error will call right there
			//Error( "MAX_NETMESSAGE exceeded for XML feedback stream in FPrintf (%d)\n", xml_buf->use);
			Sys_FPrintf( SYS_NOXMLflag | SYS_WRN, "MAX_NETMESSAGE exceeded for XML feedback stream in FPrintf (%d)\n", xml_buf->use );
			xml_buf->content[xml_buf->use] = '\0'; //++timo this corrupts the buffer but we don't care it's for printing
			Sys_FPrintf( SYS_NOXMLflag | SYS_WRN, xml_buf->content );

		}

		size = xml_buf->use;
		memcpy( xmlbuf, xml_buf->content, size );
		xmlbuf[size] = '\0';
		NMSG_Clear( &msg );
		NMSG_WriteString( &msg, xmlbuf );
		Net_Send( brdcst_socket, &msg );
#endif

		xmlBufferFree( xml_buf );
	}
}

void xml_Select( const char *msg, int entitynum, int brushnum, bool bError ){
	xmlNodePtr node, select;
	char buf[1024];
	char level[2];

	// now build a proper "select" XML node
	sprintf( buf, "Entity %i, Brush %i: %s", entitynum, brushnum, msg );
	node = xmlNewNode( NULL, (const xmlChar*)"select" );
	xmlNodeAddContent( node, (const xmlChar*)buf );
	level[0] = (int)'0' + ( bError ? SYS_ERR : SYS_WRN );
	level[1] = 0;
	xmlSetProp( node, (const xmlChar*)"level", (const xmlChar *)level );
	// a 'select' information
	sprintf( buf, "%i %i", entitynum, brushnum );
	select = xmlNewNode( NULL, (const xmlChar*)"brush" );
	xmlNodeAddContent( select, (const xmlChar*)buf );
	xmlAddChild( node, select );
	xml_SendNode( node );

	sprintf( buf, "Entity %i, Brush %i: %s", entitynum, brushnum, msg );
	if ( bError ) {
		Error( buf );
	}
	else{
		Sys_FPrintf( SYS_NOXMLflag | SYS_WRN, "%s\n", buf );
	}
}

void xml_Point( const char *msg, const Vector3& pt ){
	xmlNodePtr node, point;
	char buf[1024];
	char level[2];

	node = xmlNewNode( NULL, (const xmlChar*)"pointmsg" );
	xmlNodeAddContent( node, (const xmlChar*)msg );
	level[0] = (int)'0' + SYS_ERR;
	level[1] = 0;
	xmlSetProp( node, (const xmlChar*)"level", (const xmlChar *)level );
	// a 'point' node
	sprintf( buf, "%g %g %g", pt[0], pt[1], pt[2] );
	point = xmlNewNode( NULL, (const xmlChar*)"point" );
	xmlNodeAddContent( point, (const xmlChar*)buf );
	xmlAddChild( node, point );
	xml_SendNode( node );

	sprintf( buf, "%s (%g %g %g)", msg, pt[0], pt[1], pt[2] );
	Error( buf );
}

#define WINDING_BUFSIZE 2048
void xml_Winding( const char *msg, const Vector3 p[], int numpoints, bool die ){
	xmlNodePtr node, winding;
	char buf[WINDING_BUFSIZE];
	char smlbuf[128];
	char level[2];

	node = xmlNewNode( NULL, (const xmlChar*)"windingmsg" );
	xmlNodeAddContent( node, (const xmlChar*)msg );
	level[0] = (int)'0' + SYS_ERR;
	level[1] = 0;
	xmlSetProp( node, (const xmlChar*)"level", (const xmlChar *)level );
	// a 'winding' node
	sprintf( buf, "%i ", numpoints );
	for ( int i = 0; i < numpoints; ++i )
	{
		sprintf( smlbuf, "(%g %g %g)", p[i][0], p[i][1], p[i][2] );
		// don't overflow
		if ( strlen( buf ) + strlen( smlbuf ) >= WINDING_BUFSIZE ) {
			break;
		}
		strcat( buf, smlbuf );
	}

	winding = xmlNewNode( NULL, (const xmlChar*)"winding" );
	xmlNodeAddContent( winding, (const xmlChar*)buf );
	xmlAddChild( node, winding );
	xml_SendNode( node );

	if ( die ) {
		Error( msg );
	}
	else
	{
		Sys_Printf( msg );
		Sys_Printf( "\n" );
	}
}

void set_console_colour_for_flag( int flag ){
#ifdef WIN32
	static int curFlag = SYS_STD;
	static bool ok = true;
	static bool initialized = false;
	static HANDLE hConsole;
	static WORD colour_saved;
	if( !ok )
		return;
	if( !initialized ){
		hConsole = GetStdHandle( STD_OUTPUT_HANDLE );
		CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
		if( hConsole == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo( hConsole, &consoleInfo ) ){
			ok = false;
			return;
		}
		colour_saved = consoleInfo.wAttributes;
		initialized = true;
	}
	if( curFlag != flag ){
		curFlag = flag;
		SetConsoleTextAttribute( hConsole, flag == SYS_WRN ? FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY
		                                 : flag == SYS_ERR ? FOREGROUND_RED | FOREGROUND_INTENSITY
		                                 : colour_saved );
	}
#endif
}

// in include
#include "stream_version.h"

void Broadcast_Setup( const char *dest ){
	address_t address;
	char sMsg[1024];

	Net_Setup();
	Net_StringToAddress( dest, &address );
	brdcst_socket = Net_Connect( &address, 0 );
	if ( brdcst_socket ) {
		// send in a header
		sprintf( sMsg, "<?xml version=\"1.0\"?><q3map_feedback version=\"" Q3MAP_STREAM_VERSION "\">" );
		NMSG_Clear( &msg );
		NMSG_WriteString( &msg, sMsg );
		Net_Send( brdcst_socket, &msg );
	}
}

void Broadcast_Shutdown(){
	if ( brdcst_socket ) {
		Sys_Printf( "Disconnecting\n" );
		xml_message_flush();
		Net_Disconnect( brdcst_socket );
		brdcst_socket = NULL;
	}
	set_console_colour_for_flag( SYS_STD ); //restore default on exit
}

#define MAX_MESEGE      MAX_NETMESSAGE / 2
char mesege[MAX_MESEGE];
size_t mesege_len = 0;
int mesege_flag = SYS_STD;

void xml_message_flush(){
	if( mesege_len == 0 )
		return;
	xmlNodePtr node;
	node = xmlNewNode( NULL, (const xmlChar*)"message" );
	{
		mesege[mesege_len] = '\0';
		mesege_len = 0;
		gchar* utf8 = g_locale_to_utf8( mesege, -1, NULL, NULL, NULL );
		xmlNodeAddContent( node, (const xmlChar*)utf8 );
		g_free( utf8 );
	}
	char level[2];
	level[0] = (int)'0' + mesege_flag;
	level[1] = 0;
	xmlSetProp( node, (const xmlChar*)"level", (const xmlChar *)level );

	xml_SendNode( node );
}

#include <algorithm>
#undef min

void xml_message_push( int flag, const char* characters, size_t length ){
	if( flag != mesege_flag ){
		xml_message_flush();
		mesege_flag = flag;
	}

	const char* end = characters + length;
	while ( characters != end )
	{
		size_t space = MAX_MESEGE - 1 - mesege_len;
		if ( space == 0 ) {
			xml_message_flush();
		}
		else
		{
			size_t size = std::min( space, static_cast<size_t>( end - characters ) );
			memcpy( mesege + mesege_len, characters, size );
			mesege_len += size;
			characters += size;
		}
	}
}

// =====================================================================
// TUI console display
// =====================================================================

static constexpr int TUI_MAX_PHASES = 16;
static constexpr int TUI_MAX_WARNINGS = 64;
static constexpr int TUI_BAR_WIDTH = 30;
static constexpr int TUI_HEADER_LINES = 6;   // blank + ===== + title + ===== + map + blank

static int TUI_GetConsoleWidth() {
#ifdef WIN32
	HANDLE hOut = GetStdHandle( STD_OUTPUT_HANDLE );
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if ( GetConsoleScreenBufferInfo( hOut, &csbi ) ) {
		int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		if ( w > 40 ) return w;
	}
#else
	struct winsize ws;
	if ( ioctl( STDOUT_FILENO, TIOCGWINSZ, &ws ) == 0 && ws.ws_col > 40 ) {
		return ws.ws_col;
	}
#endif
	return 120;
}

static struct TUIState {
	bool active;            // true when drawing in TUI mode
	bool fallback;          // true when stdout is not a terminal (plain text)
	int  totalPhases;
	int  currentPhase;      // -1 = none started

	struct Phase {
		char name[48];
		char summary[256];
		double elapsed;
		bool started;
		bool complete;
		std::chrono::steady_clock::time_point startTime;
	} phases[TUI_MAX_PHASES];

	// Progress bar within current phase
	bool hasProgress;
	char progressLabel[64];
	int  progressTotal;
	int  progressCurrent;
	int  progressLastPct;
	std::chrono::steady_clock::time_point progressStartTime;

	// Phase output buffer (captures Sys_Printf during a phase)
	char phaseBuffer[4096];
	int  phaseBufferLen;

	// Accumulated warnings
	char warnings[TUI_MAX_WARNINGS][256];
	int  warningCount;

	// Map info
	char mapName[256];

	std::chrono::steady_clock::time_point startTime;

#ifdef WIN32
	DWORD origConsoleMode;
	UINT  origCodePage;
	bool  modesSaved;
#endif
} s_tui = {};

// Forward declarations
static void TUI_Draw();
static void TUI_DrawStatus();

#ifdef WIN32
static void TUI_EnableVT() {
	HANDLE hOut = GetStdHandle( STD_OUTPUT_HANDLE );
	if ( hOut == INVALID_HANDLE_VALUE ) return;

	DWORD mode = 0;
	if ( GetConsoleMode( hOut, &mode ) ) {
		s_tui.origConsoleMode = mode;
		s_tui.origCodePage = GetConsoleOutputCP();
		s_tui.modesSaved = true;
		SetConsoleMode( hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING );
		SetConsoleOutputCP( CP_UTF8 );
	}
}

static void TUI_RestoreConsole() {
	if ( s_tui.modesSaved ) {
		HANDLE hOut = GetStdHandle( STD_OUTPUT_HANDLE );
		SetConsoleMode( hOut, s_tui.origConsoleMode );
		SetConsoleOutputCP( s_tui.origCodePage );
		s_tui.modesSaved = false;
	}
}
#else
static void TUI_EnableVT() {}
static void TUI_RestoreConsole() {}
#endif

static double TUI_Elapsed() {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - s_tui.startTime ).count();
}

// Condense phaseBuffer into a short one-line summary
static void TUI_BuildPhaseSummary( char *dest, int destSize ) {
	dest[0] = '\0';
	if ( s_tui.phaseBufferLen == 0 ) return;

	char temp[1024];
	int len = 0;
	const char *src = s_tui.phaseBuffer;

	while ( *src && len < (int)sizeof(temp) - 4 ) {
		// Skip leading whitespace on each line
		while ( *src == ' ' || *src == '\t' ) src++;
		if ( *src == '\n' || *src == '\r' ) { src++; continue; }

		// If we already have content, add separator
		if ( len > 0 ) {
			temp[len++] = ';';
			temp[len++] = ' ';
		}

		// Copy until newline
		while ( *src && *src != '\n' && *src != '\r' && len < (int)sizeof(temp) - 2 ) {
			temp[len++] = *src++;
		}
	}
	temp[len] = '\0';

	// Truncate to fit
	if ( len >= destSize - 1 ) {
		temp[destSize - 4] = '.';
		temp[destSize - 3] = '.';
		temp[destSize - 2] = '.';
		temp[destSize - 1] = '\0';
	}
	snprintf( dest, destSize, "%s", temp );
}

// Draw entire TUI from top
static void TUI_Draw() {
	if ( !s_tui.active ) return;

	// Cursor home + clear screen + clear scrollback
	printf( "\033[H\033[2J\033[3J" );

	// Header
	printf( "\n" );
	printf( "  \033[36m============================================\033[0m\n" );
	printf( "    \033[1;36mR E M A P\033[0m  \033[90m|\033[0m  Resource BSP Compiler\n" );
	printf( "  \033[36m============================================\033[0m\n" );
	printf( "  Map: \033[1m%s\033[0m\n", s_tui.mapName );
	printf( "\n" );

	// Phases
	int conWidth = TUI_GetConsoleWidth();
	// prefix: "  [*] " (6) + name (24) + " " (1) + time (5) + "  " (2) = 38 visible chars
	int summaryMax = conWidth - 39;
	if ( summaryMax < 10 ) summaryMax = 10;

	for ( int i = 0; i < s_tui.totalPhases; i++ ) {
		const auto &p = s_tui.phases[i];
		if ( p.complete ) {
			char clipped[256];
			snprintf( clipped, sizeof( clipped ), "%s", p.summary );
			if ( (int)strlen( clipped ) > summaryMax ) {
				if ( summaryMax > 3 ) {
					clipped[summaryMax - 3] = '.';
					clipped[summaryMax - 2] = '.';
					clipped[summaryMax - 1] = '.';
				}
				clipped[summaryMax] = '\0';
			}
			printf( "  \033[32m[*]\033[0m %-24s \033[90m%5.1fs\033[0m  \033[90m%s\033[0m\n",
				p.name, p.elapsed, clipped );
		} else if ( i == s_tui.currentPhase ) {
			double dt = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - p.startTime ).count();
			printf( "  \033[33m[>]\033[0m \033[1m%-24s\033[0m \033[33m%5.1fs\033[0m\n",
				p.name, dt );
		} else {
			printf( "  \033[90m[ ] %-24s\033[0m\n", p.name );
		}
	}

	// Progress bar (below phases)
	if ( s_tui.hasProgress ) {
		int pct = s_tui.progressTotal > 0
			? ( s_tui.progressCurrent * 100 ) / s_tui.progressTotal : 0;
		if ( pct > 100 ) pct = 100;
		int filled = ( pct * TUI_BAR_WIDTH ) / 100;
		char bar[TUI_BAR_WIDTH + 1];
		for ( int i = 0; i < TUI_BAR_WIDTH; i++ )
			bar[i] = ( i < filled ) ? '#' : '-';
		bar[TUI_BAR_WIDTH] = '\0';
		printf( "\n      %s \033[36m[%s]\033[0m %3d%% \033[90m|\033[0m %d/%d\n",
			s_tui.progressLabel, bar, pct,
			s_tui.progressCurrent, s_tui.progressTotal );
	} else {
		printf( "\n\n" );
	}

	// Elapsed
	printf( "\n  Elapsed: \033[1m%.1fs\033[0m\n", TUI_Elapsed() );

	fflush( stdout );
}

// Fast-path: only update the status area below phases (progress + elapsed)
static void TUI_DrawStatus() {
	if ( !s_tui.active ) return;

	// Status area starts at: HEADER_LINES + totalPhases + 1  (1-based)
	int statusLine = TUI_HEADER_LINES + s_tui.totalPhases + 1;

	// Also update the current phase line (elapsed time changes)
	if ( s_tui.currentPhase >= 0 ) {
		int phaseLine = TUI_HEADER_LINES + s_tui.currentPhase + 1;
		const auto &p = s_tui.phases[s_tui.currentPhase];
		double dt = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - p.startTime ).count();
		printf( "\033[%d;1H\033[K", phaseLine );
		printf( "  \033[33m[>]\033[0m \033[1m%-24s\033[0m \033[33m%5.1fs\033[0m",
			p.name, dt );
	}

	// Jump to status area
	printf( "\033[%d;1H", statusLine );

	// Progress bar
	if ( s_tui.hasProgress ) {
		int pct = s_tui.progressTotal > 0
			? ( s_tui.progressCurrent * 100 ) / s_tui.progressTotal : 0;
		if ( pct > 100 ) pct = 100;
		int filled = ( pct * TUI_BAR_WIDTH ) / 100;
		char bar[TUI_BAR_WIDTH + 1];
		for ( int i = 0; i < TUI_BAR_WIDTH; i++ )
			bar[i] = ( i < filled ) ? '#' : '-';
		bar[TUI_BAR_WIDTH] = '\0';
		printf( "\033[K\n\033[K      %s \033[36m[%s]\033[0m %3d%% \033[90m|\033[0m %d/%d\n",
			s_tui.progressLabel, bar, pct,
			s_tui.progressCurrent, s_tui.progressTotal );
	} else {
		printf( "\033[K\n\033[K\n" );
	}

	// Elapsed
	printf( "\033[K\n\033[K  Elapsed: \033[1m%.1fs\033[0m\n", TUI_Elapsed() );

	fflush( stdout );
}

void Sys_ConsoleInit( const char *mapName, int totalPhases, const char *phaseNames[] ) {
	memset( &s_tui, 0, sizeof( s_tui ) );
	s_tui.currentPhase = -1;
	s_tui.totalPhases = totalPhases < TUI_MAX_PHASES ? totalPhases : TUI_MAX_PHASES;

	// Extract just the filename from the path
	const char *name = mapName;
	const char *slash = strrchr( mapName, '/' );
	if ( slash ) name = slash + 1;
	slash = strrchr( name, '\\' );
	if ( slash ) name = slash + 1;
	snprintf( s_tui.mapName, sizeof( s_tui.mapName ), "%s", name );

	for ( int i = 0; i < s_tui.totalPhases; i++ ) {
		snprintf( s_tui.phases[i].name, sizeof( s_tui.phases[i].name ), "%s", phaseNames[i] );
	}

	s_tui.startTime = std::chrono::steady_clock::now();

	// Check if stdout is a terminal
#ifdef WIN32
	bool isTTY = _isatty( _fileno( stdout ) ) != 0;
#else
	bool isTTY = isatty( fileno( stdout ) ) != 0;
#endif

	if ( isTTY ) {
		TUI_EnableVT();
		s_tui.active = true;
		s_tui.fallback = false;
		printf( "\033[?25l" );   // hide cursor
		TUI_Draw();
	} else {
		s_tui.active = false;
		s_tui.fallback = true;
		printf( "\n  REMAP | Resource BSP Compiler\n" );
		printf( "  Map: %s\n\n", s_tui.mapName );
	}
}

void Sys_ConsoleShutdown( double totalSeconds ) {
	if ( s_tui.active ) {
		// Final redraw with all phases complete
		TUI_Draw();

		// Move below the TUI area
		int bottomLine = TUI_HEADER_LINES + s_tui.totalPhases + 5;
		printf( "\033[%d;1H", bottomLine );
		printf( "  \033[1;32mComplete (%.2fs)\033[0m\n\n", totalSeconds );

		// Show accumulated warnings
		if ( s_tui.warningCount > 0 ) {
			printf( "  \033[33mWarnings:\033[0m\n" );
			for ( int i = 0; i < s_tui.warningCount; i++ ) {
				printf( "    \033[33m%s\033[0m", s_tui.warnings[i] );
			}
			printf( "\n" );
		}

		printf( "\033[?25h" );   // show cursor
		fflush( stdout );
		TUI_RestoreConsole();
		s_tui.active = false;
	} else if ( s_tui.fallback ) {
		printf( "\n--- Compile complete (%.2fs) ---\n\n", totalSeconds );
		s_tui.fallback = false;
	}
}

void Sys_ConsoleRestore() {
	if ( s_tui.active ) {
		// Move below TUI area and show cursor
		int bottomLine = TUI_HEADER_LINES + s_tui.totalPhases + 5;
		printf( "\033[%d;1H\033[?25h\n", bottomLine );
		fflush( stdout );
		TUI_RestoreConsole();
		s_tui.active = false;
	}
}

void Sys_PhaseBegin( int phase ) {
	// End previous phase if any
	if ( s_tui.currentPhase >= 0 ) {
		Sys_PhaseEnd();
	}

	if ( phase < 0 || phase >= s_tui.totalPhases ) return;

	s_tui.currentPhase = phase;
	s_tui.phases[phase].started = true;
	s_tui.phases[phase].startTime = std::chrono::steady_clock::now();
	s_tui.phaseBufferLen = 0;
	s_tui.phaseBuffer[0] = '\0';

	if ( s_tui.active ) {
		TUI_Draw();
	} else if ( s_tui.fallback ) {
		printf( "--- [%d/%d] %s ---\n", phase + 1, s_tui.totalPhases,
			s_tui.phases[phase].name );
	}
}

void Sys_PhaseEnd() {
	if ( s_tui.currentPhase < 0 ) return;

	auto &p = s_tui.phases[s_tui.currentPhase];
	p.elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - p.startTime ).count();
	p.complete = true;

	// Build summary from buffered output
	if ( s_tui.active ) {
		TUI_BuildPhaseSummary( p.summary, sizeof( p.summary ) );
	}

	s_tui.phaseBufferLen = 0;
	s_tui.phaseBuffer[0] = '\0';
	s_tui.hasProgress = false;
}

// =====================================================================
// Progress bar
// =====================================================================

void Sys_ProgressBegin( const char *label, int total ) {
	if ( s_tui.active ) {
		s_tui.hasProgress = true;
		s_tui.progressTotal = ( std::max )( total, 1 );
		s_tui.progressCurrent = 0;
		s_tui.progressLastPct = -1;
		snprintf( s_tui.progressLabel, sizeof( s_tui.progressLabel ), "%s", label );
		TUI_DrawStatus();
	} else {
		// Non-TUI fallback: \r-based progress
		s_tui.hasProgress = true;
		s_tui.progressTotal = ( std::max )( total, 1 );
		s_tui.progressCurrent = 0;
		s_tui.progressLastPct = -1;
		s_tui.progressStartTime = std::chrono::steady_clock::now();
		snprintf( s_tui.progressLabel, sizeof( s_tui.progressLabel ), "%s", label );
		Sys_ProgressUpdate( 0 );
	}
}

void Sys_ProgressUpdate( int current ) {
	if ( !s_tui.hasProgress ) return;
	s_tui.progressCurrent = current;

	int pct = ( current * 100 ) / s_tui.progressTotal;
	if ( pct < 0 ) pct = 0;
	if ( pct > 100 ) pct = 100;
	if ( pct == s_tui.progressLastPct && pct < 100 ) return;
	s_tui.progressLastPct = pct;

	if ( s_tui.active ) {
		TUI_DrawStatus();
	} else {
		// Non-TUI fallback: \r-based
		double elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - s_tui.progressStartTime ).count();
		int filled = ( pct * TUI_BAR_WIDTH ) / 100;
		char bar[TUI_BAR_WIDTH + 1];
		for ( int i = 0; i < TUI_BAR_WIDTH; i++ )
			bar[i] = ( i < filled ) ? '#' : '-';
		bar[TUI_BAR_WIDTH] = '\0';
		set_console_colour_for_flag( SYS_STD );
		printf( "\r  %-22s [%s] %3d%% | %d/%d | %.1fs",
			s_tui.progressLabel, bar, pct,
			current, s_tui.progressTotal, elapsed );
		fflush( stdout );
	}
}

void Sys_ProgressEnd() {
	if ( !s_tui.hasProgress ) return;
	if ( s_tui.active ) {
		s_tui.hasProgress = false;
		TUI_DrawStatus();
	} else {
		// Non-TUI fallback
		Sys_ProgressUpdate( s_tui.progressTotal );
		printf( "\n" );
		fflush( stdout );
	}
	s_tui.hasProgress = false;
}

// all output ends up through here
void FPrintf( int flag, char *buf ){
	static bool bGotXML = false;

	// TUI interception: redirect output instead of printing
	if ( s_tui.active ) {
		int cleanFlag = flag & ~( SYS_NOXMLflag | SYS_VRBflag );
		if ( cleanFlag == SYS_WRN ) {
			// Accumulate warnings for display at end
			if ( s_tui.warningCount < TUI_MAX_WARNINGS ) {
				snprintf( s_tui.warnings[s_tui.warningCount],
					sizeof( s_tui.warnings[0] ), "%s", buf );
				s_tui.warningCount++;
			}
			return;
		}
		if ( flag & SYS_VRBflag ) {
			return;  // suppress verbose in TUI mode
		}
		if ( cleanFlag == SYS_ERR ) {
			// Errors fall through to normal output
		} else {
			// SYS_STD: buffer for phase summary
			int len = (int)strlen( buf );
			int remaining = (int)sizeof( s_tui.phaseBuffer ) - s_tui.phaseBufferLen - 1;
			if ( len > remaining ) len = remaining;
			if ( len > 0 ) {
				memcpy( s_tui.phaseBuffer + s_tui.phaseBufferLen, buf, len );
				s_tui.phaseBufferLen += len;
				s_tui.phaseBuffer[s_tui.phaseBufferLen] = '\0';
			}
			return;
		}
	}

	set_console_colour_for_flag( flag & ~( SYS_NOXMLflag | SYS_VRBflag ) );
	printf( "%s", buf );

	// the following part is XML stuff only.. but maybe we don't want that message to go down the XML pipe?
	if ( flag & SYS_NOXMLflag ) {
		return;
	}

	// output an XML file of the run
	// use the DOM interface to build a tree
	/*
	   <message level='flag'>
	   message string
	   .. various nodes to describe corresponding geometry ..
	   </message>
	 */
	if ( !bGotXML ) {
		// initialize
		doc = xmlNewDoc( (const xmlChar*)"1.0" );
		doc->children = xmlNewDocRawNode( doc, NULL, (const xmlChar*)"q3map_feedback", NULL );
		bGotXML = true;
	}
	xml_message_push( flag & ~( SYS_NOXMLflag | SYS_VRBflag ), buf, strlen( buf ) );
}

#ifdef DBG_XML
void DumpXML(){
	xmlSaveFile( "XMLDump.xml", doc );
}
#endif

void Sys_FPrintf( int flag, const char *format, ... ){
	char out_buffer[4096];
	va_list argptr;

	if ( ( flag & SYS_VRBflag ) && !verbose ) {
		return;
	}

	va_start( argptr, format );
	vsprintf( out_buffer, format, argptr );
	va_end( argptr );

	FPrintf( flag, out_buffer );
}

void Sys_Printf( const char *format, ... ){
	char out_buffer[4096];
	va_list argptr;

	va_start( argptr, format );
	vsprintf( out_buffer, format, argptr );
	va_end( argptr );

	FPrintf( SYS_STD, out_buffer );
}

void Sys_Warning( const char *format, ... ){
	char out_buffer[4096];
	va_list argptr;

	va_start( argptr, format );
	sprintf( out_buffer, "WARNING: " );
	vsprintf( out_buffer + strlen( "WARNING: " ), format, argptr );
	va_end( argptr );

	FPrintf( SYS_WRN, out_buffer );
}

/*
   =================
   Error

   For abnormal program terminations
   =================
 */
void Error( const char *error, ... ){
	char out_buffer[4096];
	char tmp[4096];
	va_list argptr;

	// Restore console before printing error
	Sys_ConsoleRestore();

	va_start( argptr, error );
	vsprintf( tmp, error, argptr );
	va_end( argptr );

	sprintf( out_buffer, "************ ERROR ************\n%s\n", tmp );

	FPrintf( SYS_ERR, out_buffer );
	xml_message_flush();

#ifdef DBG_XML
	DumpXML();
#endif

	//++timo HACK ALERT .. if we shut down too fast the xml stream won't reach the listener.
	// a clean solution is to send a sync request node in the stream and wait for an answer before exiting
	Sys_Sleep( 1000 );

	exit( 1 );
}