/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

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

#include "glfont.h"
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <cstdlib>
#include <cstring>
#include "stream/stringstream.h"
#include "radiant/vktexture.h"


void gray_to_texture( const unsigned int x_max, const unsigned int y_max, const unsigned char *in, unsigned char *out, const unsigned char fontColorR, const unsigned char fontColorG, const unsigned char fontColorB ){ /* normal with shadow */
	unsigned int x, y, bitmapIter = 0;

	const unsigned char backgroundColorR = 0;
	const unsigned char backgroundColorG = 0;
	const unsigned char backgroundColorB = 0;

	for( y = 0; y < y_max; y++ ) {
		for( x = 0; x < x_max; x++ ) {
			const unsigned int iter = ( y * x_max + x ) * 4;
			if( x == 0 || y == 0 || x == 1 || y == 1 ) {
				out[iter] = fontColorB;
				out[iter + 1] = fontColorG;
				out[iter + 2] = fontColorR;
				out[iter + 3] = 0;
				continue;
			}
			if( in[bitmapIter] == 0 ){
				out[iter] = fontColorB;
				out[iter + 1] = fontColorG;
				out[iter + 2] = fontColorR;
				out[iter + 3] = 0;
			}
			else{
				out[iter] = backgroundColorB;
				out[iter + 1] = backgroundColorG;
				out[iter + 2] = backgroundColorR;
				out[iter + 3] = in[bitmapIter];
			}
			++bitmapIter;
		}
	}

	bitmapIter = 0;
	for( y = 0; y < y_max; y++ ) {
		for( x = 0; x < x_max; x++ ) {
			const unsigned int iter = ( y * x_max + x ) * 4;
			if( x == 0 || y == 0 || x == ( x_max - 1 ) || y == ( y_max - 1 ) ) {
				continue;
			}
			if( in[bitmapIter] != 0 ) {
				if( out[iter + 3] == 0 ){
					out[iter] = fontColorB;
					out[iter + 1] = fontColorG;
					out[iter + 2] = fontColorR;
					out[iter + 3] = in[bitmapIter];
				}
				else{
					/* Calculate alpha (opacity). */
					const float opacityFont = in[bitmapIter] / 255.f;
					const float opacityBack = out[iter + 3] / 255.f;
					out[iter] = fontColorB * opacityFont + ( 1 - opacityFont ) * backgroundColorB;
					out[iter + 1] = fontColorG * opacityFont + ( 1 - opacityFont ) * backgroundColorG;
					out[iter + 2] = fontColorR * opacityFont + ( 1 - opacityFont ) * backgroundColorR;
					out[iter + 3] = ( opacityFont + ( 1 - opacityFont ) * opacityBack ) * 255.f;
				}
			}
			++bitmapIter;
		}
	}
}


// generic string printing with Vulkan textures
class GLFontCallList final : public GLFont
{
	uint32_t m_atlas;  ///< VK texture slot for glyph atlas
	QFont m_font;
	QFontMetrics m_metrics;
	const int m_pixelHeight;
	const int m_pixelAscent;
	const int m_pixelDescent;
	int m_glyphAdvance[128];   ///< pixel advance per glyph
	float m_atlasGlyphW;       ///< normalised glyph width in atlas
	float m_atlasGlyphH;       ///< normalised glyph height in atlas
public:
	GLFontCallList( uint32_t atlas, const QFont& font, const QFontMetrics& metrics,
	                const int advance[128], float agw, float agh ) :
		m_atlas( atlas ), m_font( font ), m_metrics( metrics ),
		m_pixelHeight( m_metrics.height() ), m_pixelAscent( m_metrics.ascent() ), m_pixelDescent( m_metrics.descent() ),
		m_atlasGlyphW( agw ), m_atlasGlyphH( agh ){
		for ( int i = 0; i < 128; ++i ) m_glyphAdvance[i] = advance[i];
	}
	~GLFontCallList(){
		VKTexture_destroy( m_atlas );
	}
	/// printString: immediate-mode text rendering via the Vulkan text overlay.
	/// Full implementation requires a running command buffer (Phase 6).
	/// For now it is a no-op; callers that need text in views will be wired in Phase 6.
	void printString( const char* /*s*/ ){
		// Phase 6: bind atlas (m_atlas), emit textured quads per glyph into streaming VB
	}

	void renderString( const char *s, const uint32_t& tex, const unsigned char colour[3], unsigned int& out_wid, unsigned int& out_hei ){
		// proper way would be using painter.metrics, however this requires it being active()...
		// same for painter.boundingRect() + result is not correct wrt width for some reason
		const QRect rect = m_metrics.boundingRect( s );
		unsigned int wid = rect.width();
		unsigned int hei = rect.height();

		if ( wid > 0 && hei > 0 ) {
			QImage image( wid, hei, QImage::Format::Format_Alpha8 );
			image.fill( 0 );
			QPainter painter;
			painter.begin( &image );
			painter.setFont( m_font );
			painter.drawText( 0, m_metrics.height() - m_metrics.descent(), s );
			painter.end();

			// using image.constBits() is inconsistently buggy for some reason
			unsigned char *boo = (unsigned char *) malloc( wid * hei );
			for( unsigned int w = 0; w < wid; ++w )
				for( unsigned int h = 0; h < hei; ++h )
					boo[wid * h + w] = qAlpha( image.pixel( w, h ) );

			hei += 2;
			wid += 2;
			// Allocate triple-wide RGBA buffer: normal | yellow-selected | orange-selected
			unsigned char *buf  = (unsigned char *) malloc( 4 * hei * wid );
			unsigned char *bufY = (unsigned char *) malloc( 4 * hei * wid );
			unsigned char *bufO = (unsigned char *) malloc( 4 * hei * wid );
			memset( buf,  0x00, 4 * hei * wid );
			memset( bufY, 0x00, 4 * hei * wid );
			memset( bufO, 0x00, 4 * hei * wid );

			gray_to_texture( wid, hei, boo, buf,  colour[0], colour[1], colour[2] );
			gray_to_texture( wid, hei, boo, bufY, 255, 255, 0 );
			gray_to_texture( wid, hei, boo, bufO, 255, 128, 0 );

			// Compose into one contiguous wid*3 × hei image (row-major interleave)
			unsigned char *full = (unsigned char *) malloc( 4 * hei * wid * 3 );
			for ( unsigned int row = 0; row < hei; ++row ) {
				memcpy( full + row * 4 * wid * 3,            buf  + row * 4 * wid, 4 * wid );
				memcpy( full + row * 4 * wid * 3 + 4 * wid,  bufY + row * 4 * wid, 4 * wid );
				memcpy( full + row * 4 * wid * 3 + 8 * wid,  bufO + row * 4 * wid, 4 * wid );
			}

			// Upload to a Vulkan texture slot; written into the caller-provided slot id.
			const_cast<uint32_t&>( tex ) = VKTexture_create2D(
				full, wid * 3, hei, VK_TEX_NEAREST, false );

			free( full );
			free( bufO );
			free( bufY );
			free( buf );
			free( boo );

			out_wid = wid;
			out_hei = hei;
		}
	}

	int getPixelAscent() const {
		return m_pixelAscent;
	}
	int getPixelDescent() const {
		return m_pixelDescent;
	}
	int getPixelHeight() const {
		return m_pixelHeight;
	}
};

#include "debugging/debugging.h"

GLFont *glfont_create( const char* family, int fontSize, const char* appPath ){
	QFont font;
	font.setPointSize( fontSize );
	{
		const int id = QFontDatabase::addApplicationFont( QString( appPath ) + "bitmaps/MyriadPro-Regular.ttf" );
		if( id >= 0 && string_equal( family, "Myriad Pro" ) )
			font.setFamily( QFontDatabase::applicationFontFamilies( id ).at( 0 ) );
		else if( !string_empty( family ) )
			font.setFamily( family );
	}
	globalOutputStream() << "Using Vulkan font " << makeQuoted( font.toString().toLatin1().constData() ) << '\n';

	QFontMetrics metrics( font );

	const int glyphW = metrics.maxWidth();  // max glyph width
	const int glyphH = metrics.height();
	const int cols   = 12;
	const int rows   = ( 128 + cols - 1 ) / cols;
	const int awidth  = glyphW * cols;
	const int aheight = glyphH * rows;

	// Per-glyph advance widths
	int advances[128];

	// Render all 128 ASCII glyphs into a single alpha8 atlas
	QImage image( awidth, aheight, QImage::Format::Format_Alpha8 );
	image.fill( 0 );
	{
		QPainter painter;
		painter.begin( &image );
		painter.setFont( font );
		for( unsigned char c = 0; c < 128; ++c ){
			advances[c] = metrics.horizontalAdvance( c );
			painter.drawText( c % cols * glyphW, ( c / cols + 1 ) * glyphH - metrics.descent(), QString( c ) );
		}
		painter.end();
	}

	// Convert alpha8 → RGBA for Vulkan upload
	const int totalPx = awidth * aheight;
	unsigned char* rgba = (unsigned char*) malloc( totalPx * 4 );
	for( int i = 0; i < totalPx; ++i ){
		const unsigned char a = qAlpha( image.pixel( i % awidth, i / awidth ) );
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = a;
	}
	const uint32_t atlasSlot = VKTexture_create2D( rgba, awidth, aheight, VK_TEX_NEAREST, false );
	free( rgba );

	const float agw = static_cast<float>( glyphW ) / awidth;
	const float agh = static_cast<float>( glyphH ) / aheight;

	return new GLFontCallList( atlasSlot, font, metrics, advances, agw, agh );
}
