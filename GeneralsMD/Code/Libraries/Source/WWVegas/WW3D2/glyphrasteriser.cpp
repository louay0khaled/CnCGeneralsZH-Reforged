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

// GlyphRasteriserClass on FreeType: see glyphrasteriser.h for what each answer is meant to match.

#include "glyphrasteriser.h"
#include "gdifontmetrics.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_DRIVER_H
#include FT_GASP_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#if !defined(__APPLE__) && !defined(__ANDROID__) || defined(ZH_GLYPHS_USE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#define ZH_GLYPHS_FONTCONFIG 1
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>

namespace {

GlyphRasteriserClass::AntialiasModeType theAntialiasMode = GlyphRasteriserClass::ANTIALIAS_AS_GASP_SAYS;

/* One FreeType library for the process, with the TrueType interpreter set to v35: the classic one,
	 which takes hdmx advances where the file has them.  FreeType's default, v40, does not, and its
	 advances are not GDI's (D6). */
FT_Library theLibrary( void )
{
	static FT_Library library = NULL;
	static bool tried = false;
	if (!tried)
	{
		tried = true;
		if (FT_Init_FreeType( &library ) != 0)
			library = NULL;
		else
		{
			FT_UInt version = TT_INTERPRETER_VERSION_35;
			FT_Property_Set( library, "truetype", "interpreter-version", &version );
		}
	}
	return library;
}

// ---- Which file a face is ------------------------------------------------------------------------

struct RegisteredFont
{
	std::string path;
	std::string family;
	bool bold;
};

std::vector<RegisteredFont> &theRegisteredFonts( void )
{
	static std::vector<RegisteredFont> fonts;
	return fonts;
}

/* The one substitution table: the face names the game uses (its .wnd files, Language.ini, the code),
	 GDI's case-insensitively, to the family this platform has for each.
	 - macOS has Arial, Times New Roman and Courier New themselves, in /System/Library/Fonts/Supplemental
		 since 10.15, where they are taken from by file name.
	 - Linux has Liberation Sans, Serif and Mono, metric-compatible with the first three (same advance
		 widths), found through fontconfig wherever the distribution or the user put them.
	 - "Courier" (a Windows raster font) and "FixedSys" (debug text) take Courier New.
	 - Placard MT Condensed and Abadi MT Bold, named by 29 shipped .wnd entries, are not Windows fonts
		 either: a Windows player gets whatever GDI's font mapper picks.  Arial is the likely pick and
		 what this takes, unverified.  So does any face not in the table.
	 - "Arial Unicode MS", Language.ini's UnicodeFontName: its own file on macOS; Linux has nothing
		 metric-compatible, so DejaVu Sans, whose coverage is wide.
	 "Generals" never gets here: FontCharsClass turns it into Arial with an lfWidth before CreateFont. */
struct Substitution
{
	const char *face;
	const char *macRegular;
	const char *macBold;			// NULL: synthesise bold from the regular file, as GDI would
	const char *linuxFamily;
	const char *gdiMetrics;		// the Windows face whose GDI line metrics a Linux substitute takes (gdifontmetrics.h); NULL: none
};

const Substitution theSubstitutions[] =
{
	{ "Arial",								"Arial.ttf",						"Arial Bold.ttf",						"Liberation Sans",	"Arial" },
	{ "Times New Roman",			"Times New Roman.ttf",	"Times New Roman Bold.ttf",	"Liberation Serif",	"Times New Roman" },
	{ "Courier New",					"Courier New.ttf",			"Courier New Bold.ttf",			"Liberation Mono",	"Courier New" },
	{ "Courier",							"Courier New.ttf",			"Courier New Bold.ttf",			"Liberation Mono",	"Courier New" },
	{ "FixedSys",							"Courier New.ttf",			"Courier New Bold.ttf",			"Liberation Mono",	"Courier New" },
	{ "Arial Unicode MS",			"Arial Unicode.ttf",		NULL,												"DejaVu Sans",			NULL },
	{ "Placard MT Condensed",	"Arial.ttf",						"Arial Bold.ttf",						"Liberation Sans",	"Arial" },
	{ "Abadi MT Bold",				"Arial.ttf",						"Arial Bold.ttf",						"Liberation Sans",	"Arial" },
};
const Substitution &theFallback = theSubstitutions[0];

const Substitution &substitutionFor( const char *face )
{
	for (size_t i = 0; i < sizeof( theSubstitutions ) / sizeof( theSubstitutions[0] ); ++i)
		if (strcasecmp( face, theSubstitutions[i].face ) == 0)
			return theSubstitutions[i];
	return theFallback;
}

bool fileExists( const char *path )
{
	FILE *file = fopen( path, "rb" );
	if (file == NULL)
		return false;
	fclose( file );
	return true;
}

#if defined(ZH_GLYPHS_FONTCONFIG)
/* fontconfig's best match for the family and weight.  fontconfig always answers with something; an
	 answer from another family (Liberation not installed) is still used, and said once, because text in
	 the wrong metrics beats no text. */
bool fontconfigFile( const char *family, bool bold, std::string &path, bool &isBold )
{
	static FcConfig *config = FcInitLoadConfigAndFonts();
	if (config == NULL)
		return false;
	FcPattern *pattern = FcPatternCreate();
	FcPatternAddString( pattern, FC_FAMILY, (const FcChar8 *)family );
	FcPatternAddInteger( pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR );
	FcPatternAddInteger( pattern, FC_SLANT, FC_SLANT_ROMAN );
	FcConfigSubstitute( config, pattern, FcMatchPattern );
	FcDefaultSubstitute( pattern );
	FcResult result = FcResultNoMatch;
	FcPattern *match = FcFontMatch( config, pattern, &result );
	FcPatternDestroy( pattern );
	if (match == NULL)
		return false;
	FcChar8 *file = NULL, *found = NULL;
	int weight = FC_WEIGHT_REGULAR;
	bool ok = FcPatternGetString( match, FC_FILE, 0, &file ) == FcResultMatch && file != NULL;
	if (ok)
	{
		path = (const char *)file;
		FcPatternGetInteger( match, FC_WEIGHT, 0, &weight );
		isBold = weight >= FC_WEIGHT_BOLD;
		if (FcPatternGetString( match, FC_FAMILY, 0, &found ) == FcResultMatch && found != NULL
				&& strcasecmp( (const char *)found, family ) != 0)
		{
			static std::vector<std::string> said;
			bool before = false;
			for (size_t i = 0; i < said.size(); ++i)
				before = before || said[i] == family;
			if (!before)
			{
				said.push_back( family );
				fprintf( stderr, "GlyphRasteriser: %s is not installed; fontconfig gave %s, whose widths differ from the "
					"game's layout\n", family, (const char *)found );
			}
		}
	}
	FcPatternDestroy( match );
	return ok;
}
#endif

/* GDI makes an lfWidth font that wide by scaling the Windows face against its own OS/2 xAvgCharWidth.  A
	 Linux substitute matches the face's advance widths but not that field: Liberation Sans says 1187 units
	 where Arial says 904 (an average over every glyph, where Arial keeps the older weighted one), so
	 scaling by the substitute's own made every lfWidth font 0.76 as wide as on Windows - the shell's
	 "Generals" titles and buttons among them.  The Windows faces' values, read from Windows 11's files
	 (Arial 7.03, Times New Roman 7.05, Courier New 6.94) and the same in macOS's (5.01.2x, 5.01.4x,
	 5.00.2x), all at 2048 units per em; used only when the file found is the substitute named here. */
struct GdiAverageWidth
{
	const char *face;					// the Windows face, as Substitution::gdiMetrics names it
	const char *substitute;		// the metric-compatible family that stands in for it
	bool bold;
	int xAvgCharWidth;
};

const GdiAverageWidth theGdiAverageWidths[] =
{
	{ "Arial",						"Liberation Sans",	false,	904 },
	{ "Arial",						"Liberation Sans",	true,		980 },
	{ "Times New Roman",	"Liberation Serif",	false,	821 },
	{ "Times New Roman",	"Liberation Serif",	true,		874 },
	{ "Courier New",			"Liberation Mono",	false,	1229 },
	{ "Courier New",			"Liberation Mono",	true,		1229 },
};
const int GDI_AVERAGE_WIDTH_UNITS_PER_EM = 2048;

/* The Windows face's xAvgCharWidth where the file is its substitute; 0 for any other file, which then
	 scales by its own (a family fontconfig fell back to has no Windows value to borrow). */
int gdiAverageWidth( const char *face, bool bold, const char *familyFound )
{
	if (face == NULL || familyFound == NULL)
		return 0;
	for (size_t i = 0; i < sizeof( theGdiAverageWidths ) / sizeof( theGdiAverageWidths[0] ); ++i)
		if (theGdiAverageWidths[i].bold == bold && strcmp( theGdiAverageWidths[i].face, face ) == 0
				&& strcasecmp( theGdiAverageWidths[i].substitute, familyFound ) == 0)
			return theGdiAverageWidths[i].xAvgCharWidth;
	return 0;
}

/* The GDI line metrics of a Windows face at a pixel height, from gdifontmetrics.h: false outside the
	 table's faces and sizes. */
bool gdiMetrics( const char *face, bool bold, int ppem, int &ascent, int &descent )
{
	if (face == NULL || ppem < GDI_METRICS_FIRST_PPEM || ppem > GDI_METRICS_LAST_PPEM)
		return false;
	for (size_t i = 0; i < sizeof( theGdiFontMetrics ) / sizeof( theGdiFontMetrics[0] ); ++i)
		if (theGdiFontMetrics[i].bold == bold && strcmp( theGdiFontMetrics[i].face, face ) == 0)
		{
			ascent = theGdiFontMetrics[i].ascent[ppem - GDI_METRICS_FIRST_PPEM];
			descent = theGdiFontMetrics[i].descent[ppem - GDI_METRICS_FIRST_PPEM];
			return true;
		}
	return false;
}

/* The file for a face, and whether that file is itself bold (if not and bold was asked, synthesise).
	 metricsFace: the Windows face whose GDI line metrics apply, where the file is a metric-compatible
	 substitute found through fontconfig (Liberation for Arial, Times New Roman, Courier New); NULL for a
	 registered file, macOS's own fonts (their VDMX gives GDI's numbers already) and DejaVu Sans. */
bool resolve( const char *face, bool bold, std::string &path, bool &fileIsBold, const char *&metricsFace )
{
	metricsFace = NULL;
	// a registered file first, bold matched where there is a choice
	const std::vector<RegisteredFont> &fonts = theRegisteredFonts();
	const RegisteredFont *any = NULL;
	for (size_t i = 0; i < fonts.size(); ++i)
		if (strcasecmp( fonts[i].family.c_str(), face ) == 0)
		{
			if (fonts[i].bold == bold)
			{
				path = fonts[i].path;
				fileIsBold = fonts[i].bold;
				return true;
			}
			any = &fonts[i];
		}
	if (any != NULL)
	{
		path = any->path;
		fileIsBold = any->bold;
		return true;
	}

	const Substitution &s = substitutionFor( face );
#if defined(ZH_GLYPHS_FONTCONFIG)
	metricsFace = s.gdiMetrics;
	return fontconfigFile( s.linuxFamily, bold, path, fileIsBold );
#elif defined(__ANDROID__)
	// Android exposes a stable system-font directory for FreeType. We deliberately do not depend on
	// fontconfig or Android Java font APIs in the native core. Roboto is the platform fallback used for
	// the game's Arial/Times/Courier families; bold is selected when the platform file exists.
	const char *regular = "/system/fonts/Roboto-Regular.ttf";
	const char *boldFile = "/system/fonts/Roboto-Bold.ttf";
	if (bold && fileExists( boldFile ))
	{
		path = boldFile;
		fileIsBold = true;
		return true;
	}
	path = regular;
	fileIsBold = false;
	return fileExists( path.c_str() );
#else
	const char *directory = "/System/Library/Fonts/Supplemental/";
	if (bold && s.macBold != NULL && fileExists( (std::string( directory ) + s.macBold).c_str() ))
	{
		path = std::string( directory ) + s.macBold;
		fileIsBold = true;
		return true;
	}
	path = std::string( directory ) + s.macRegular;
	fileIsBold = false;
	return fileExists( path.c_str() );
#endif
}

// ---- The tables GDI reads and FreeType does not --------------------------------------------------

unsigned be16( const FT_Byte *p ) { return ((unsigned)p[0] << 8) | p[1]; }
int be16s( const FT_Byte *p ) { return (short)be16( p ); }

/* VDMX: GDI's tmAscent and tmDescent at each pixel height.  The ratio group for a 1:1 device, as
	 GDI's screen DC is: the first whose range holds 1:1, or the first that matches every device. */
bool vdmxHeights( FT_Face face, int ppem, int &ascent, int &descent )
{
	FT_ULong length = 0;
	if (FT_Load_Sfnt_Table( face, TTAG_VDMX, 0, NULL, &length ) != 0 || length < 6)
		return false;
	std::vector<FT_Byte> table( length );
	if (FT_Load_Sfnt_Table( face, TTAG_VDMX, 0, &table[0], &length ) != 0)
		return false;
	const unsigned ratios = be16( &table[4] );
	if (6 + ratios * 6 > length)
		return false;
	int chosen = -1;
	for (unsigned i = 0; i < ratios && chosen < 0; ++i)
	{
		const FT_Byte *r = &table[6 + i * 4];
		const unsigned x = r[1], yStart = r[2], yEnd = r[3];
		if ((x == 0 && yStart == 0 && yEnd == 0) || (x == 1 && yStart <= 1 && yEnd >= 1))
			chosen = (int)i;
	}
	if (chosen < 0)
		return false;
	const unsigned group = be16( &table[6 + ratios * 4 + chosen * 2] );
	if (group + 4 > length)
		return false;
	const unsigned records = be16( &table[group] );
	for (unsigned i = 0; i < records && group + 4 + i * 6 + 6 <= length; ++i)
	{
		const FT_Byte *e = &table[group + 4 + i * 6];
		if ((int)be16( e ) == ppem)
		{
			ascent = be16s( e + 2 );
			descent = -be16s( e + 4 );
			return true;
		}
	}
	return false;
}

}  // namespace

// ---- The class ------------------------------------------------------------------------------------

struct GlyphRasteriserClass::FontStateStruct
{
	FT_Face face;
	std::string path;
	bool embolden;				///< bold asked, the file is not: synthesise, as GDI does
	bool scaled;					///< an lfWidth: x ppem is not y ppem, and hdmx does not apply
	bool gaspBilevel;			///< the file's gasp says no grey at this size
};

GlyphRasteriserClass::GlyphRasteriserClass( void ) : State( NULL ), Box( NULL ), BoxSize( 0 ), Bilevel( false )
{
	memset( &Metrics, 0, sizeof( Metrics ) );
}

GlyphRasteriserClass::~GlyphRasteriserClass( void )
{
	Free_Font();
}

bool GlyphRasteriserClass::Create_Font( const char *face, int pixel_height, int average_width, bool bold, int box_size )
{
	Free_Font();
	FT_Library library = theLibrary();
	std::string path;
	bool fileIsBold = false;
	const char *metricsFace = NULL;
	if (library == NULL || face == NULL || pixel_height <= 0 || box_size <= 0
			|| !resolve( face, bold, path, fileIsBold, metricsFace ))
		return false;
	FT_Face ftFace = NULL;
	if (FT_New_Face( library, path.c_str(), 0, &ftFace ) != 0)
		return false;

	/* lfWidth is the average character width GDI makes the font have: the x scale is it over the
		 file's own average, OS/2 xAvgCharWidth, at this height - or over the Windows face's, where the file
		 is its Linux substitute (theGdiAverageWidths).  Rounded to whole pixels per em, as the hinted
		 rasteriser works in; GDI's exact rounding is not known here (D6). */
	int xPpem = pixel_height;
	TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table( ftFace, FT_SFNT_OS2 );
	const int windowsAverage = average_width > 0 ? gdiAverageWidth( metricsFace, bold, ftFace->family_name ) : 0;
	if (windowsAverage > 0)
		xPpem = (int)floor( (double)average_width * GDI_AVERAGE_WIDTH_UNITS_PER_EM / windowsAverage + 0.5 );
	else if (average_width > 0 && os2 != NULL && os2->xAvgCharWidth > 0)
		xPpem = (int)floor( (double)average_width * ftFace->units_per_EM / os2->xAvgCharWidth + 0.5 );
	if (xPpem < 1)
		xPpem = 1;
	if (FT_Set_Pixel_Sizes( ftFace, (FT_UInt)xPpem, (FT_UInt)pixel_height ) != 0)
	{
		FT_Done_Face( ftFace );
		return false;
	}

	State = new FontStateStruct;
	State->face = ftFace;
	State->path = path;
	State->embolden = bold && !fileIsBold;
	State->scaled = xPpem != pixel_height;
	const FT_Int gasp = FT_Get_Gasp( ftFace, (FT_UInt)pixel_height );
	State->gaspBilevel = gasp != FT_GASP_NO_TABLE && (gasp & FT_GASP_DO_GRAY) == 0;
	Bilevel = State->gaspBilevel && theAntialiasMode == ANTIALIAS_AS_GASP_SAYS;

	/* Heights: GDI's own for the face a Linux substitute stands in for (gdifontmetrics.h: Liberation has no
		 VDMX, and 30 to 38 of 43 sizes came out 1 or 2 pixels short); else VDMX, as GDI reads it; else the
		 rounded Windows ascent and descent. */
	int ascent = 0, descent = 0;
	if (!gdiMetrics( metricsFace, bold, pixel_height, ascent, descent ) && !vdmxHeights( ftFace, pixel_height, ascent, descent ))
	{
		const double scale = (double)pixel_height / ftFace->units_per_EM;
		ascent = (int)floor( (os2 != NULL ? os2->usWinAscent : ftFace->ascender) * scale + 0.5 );
		descent = (int)floor( (os2 != NULL ? os2->usWinDescent : -ftFace->descender) * scale + 0.5 );
	}
	Metrics.Ascent = ascent;
	Metrics.Descent = descent;
	Metrics.Height = ascent + descent;
	Metrics.Overhang = 0;

	BoxSize = box_size;
	Box = new uint8_t[ (size_t)box_size * box_size ];
	memset( Box, 0, (size_t)box_size * box_size );
	return true;
}

void GlyphRasteriserClass::Free_Font( void )
{
	if (State != NULL)
	{
		FT_Done_Face( State->face );
		delete State;
		State = NULL;
	}
	delete [] Box;
	Box = NULL;
	BoxSize = 0;
	Bilevel = false;
	memset( &Metrics, 0, sizeof( Metrics ) );
}

const char *GlyphRasteriserClass::Get_File_Path( void ) const
{
	return State != NULL ? State->path.c_str() : "";
}

/* Loads a glyph hinted by v35.  The hinting target for measuring is always the one gasp gives, so the
	 advance never depends on the antialias switch; drawing takes the switch's. */
int GlyphRasteriserClass::Load_Glyph( uint32_t ch, bool for_drawing )
{
	if (State == NULL)
		return -1;
	const bool bilevel = for_drawing ? Bilevel : State->gaspBilevel;
	FT_Int32 flags = FT_LOAD_DEFAULT | (bilevel ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
	if (State->scaled)
		flags |= FT_LOAD_COMPUTE_METRICS;	// FreeType would take hdmx by x ppem; GDI takes none when scaled
	const FT_UInt index = FT_Get_Char_Index( State->face, (FT_ULong)ch );
	if (FT_Load_Glyph( State->face, index, flags ) != 0)
		return -1;
	int advance = (int)((State->face->glyph->advance.x + 32) >> 6);
	if (State->embolden)
	{
		// GDI's simulated bold is one pixel wider; the outline is thickened by about as much
		FT_Outline_Embolden( &State->face->glyph->outline, 64 );
		advance += 1;
	}
	return advance;
}

int GlyphRasteriserClass::Get_Advance( uint32_t ch )
{
	const int advance = Load_Glyph( ch, false );
	return advance < 0 ? 0 : advance;
}

void GlyphRasteriserClass::Draw_Char( uint32_t ch, int x_origin )
{
	if (State == NULL || Box == NULL)
		return;
	memset( Box, 0, (size_t)BoxSize * BoxSize );	// ETO_OPAQUE: the background colour, black
	if (Load_Glyph( ch, true ) < 0)
		return;
	FT_GlyphSlot slot = State->face->glyph;
	if (FT_Render_Glyph( slot, Bilevel ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL ) != 0)
		return;
	const FT_Bitmap &bitmap = slot->bitmap;
	const int left = x_origin + slot->bitmap_left;
	const int top = Metrics.Ascent - slot->bitmap_top;
	for (int row = 0; row < (int)bitmap.rows; ++row)
	{
		const int y = top + row;
		if (y < 0 || y >= BoxSize)
			continue;
		const unsigned char *line = bitmap.buffer + (bitmap.pitch >= 0 ? row * bitmap.pitch
			: ((int)bitmap.rows - 1 - row) * -bitmap.pitch);
		for (int col = 0; col < (int)bitmap.width; ++col)
		{
			const int x = left + col;
			if (x < 0 || x >= BoxSize)
				continue;
			uint8_t value;
			if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
				value = (line[col >> 3] & (0x80 >> (col & 7))) ? 255 : 0;
			else
				value = line[col];
			Box[y * BoxSize + x] = value;
		}
	}
}

void GlyphRasteriserClass::Set_Antialias_Mode( AntialiasModeType mode )
{
	theAntialiasMode = mode;
}

GlyphRasteriserClass::AntialiasModeType GlyphRasteriserClass::Get_Antialias_Mode( void )
{
	return theAntialiasMode;
}

bool GlyphRasteriserClass::Register_Font_File( const char *path )
{
	FT_Library library = theLibrary();
	FT_Face face = NULL;
	if (library == NULL || path == NULL || FT_New_Face( library, path, 0, &face ) != 0)
		return false;
	RegisteredFont font;
	font.path = path;
	font.family = face->family_name != NULL ? face->family_name : "";
	font.bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
	FT_Done_Face( face );
	if (font.family.empty())
		return false;
	theRegisteredFonts().push_back( font );
	return true;
}

void GlyphRasteriserClass::Unregister_Font_File( const char *path )
{
	std::vector<RegisteredFont> &fonts = theRegisteredFonts();
	for (size_t i = fonts.size(); i-- > 0; )
		if (fonts[i].path == path)
			fonts.erase( fonts.begin() + i );
}

bool GlyphRasteriserClass::Find_Font_File( const char *face, bool bold, char *path, int path_size )
{
	std::string found;
	bool isBold = false;
	const char *metricsFace = NULL;
	if (path_size <= 0)
		return false;
	path[0] = 0;
	if (face == NULL || !resolve( face, bold, found, isBold, metricsFace ) || (int)found.size() + 1 > path_size)
		return false;
	strcpy( path, found.c_str() );
	return true;
}
