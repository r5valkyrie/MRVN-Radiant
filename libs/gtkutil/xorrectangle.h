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

#pragma once

#include "rect_t.h"
#include <QPainter>
#include <QWidget>

/// Selection-rectangle overlay drawn via Qt QPainter.
/// Call render() from the owning widget's paintEvent or an overlay paint pass.
class XORRectangle {
public:
	XORRectangle() {
	}
	~XORRectangle() {
	}

	/// Draw the selection rectangle onto \p widget using QPainter.
	/// \p rect   normalised view-space coords in [-1,1]; we map to pixel coords.
	/// \p viewWidth / \p viewHeight   widget pixel dimensions.
	void render( rect_t rect, int viewWidth, int viewHeight ) {
		if( rect.max[0] == rect.min[0] || rect.max[1] == rect.min[1] )
			return;

		// The rect coordinates are in the [-1,1] space used by the old GL viewport.
		// Map to [0, viewWidth/Height] pixels.
		const float scaleX = viewWidth  * 0.5f;
		const float scaleY = viewHeight * 0.5f;
		const int x0 = static_cast<int>( ( rect.min[0] + 1.0f ) * scaleX );
		const int y0 = static_cast<int>( ( 1.0f - rect.max[1] ) * scaleY );
		const int x1 = static_cast<int>( ( rect.max[0] + 1.0f ) * scaleX );
		const int y1 = static_cast<int>( ( 1.0f - rect.min[1] ) * scaleY );
		const QRect qrect( x0, y0, x1 - x0, y1 - y0 );

		QColor fillColor, borderColor;
		switch ( rect.modifier )
		{
		case rect_t::eSelect:
			fillColor  = QColor( 255, 128,   0,  40 );
			borderColor= QColor( 255, 128,   0, 200 );
			break;
		case rect_t::eDeselect:
			fillColor  = QColor(   0,   0, 255,  40 );
			borderColor= QColor(   0,   0, 255, 200 );
			break;
		default: /* eToggle */
			fillColor  = QColor( 200, 200, 200,  40 );
			borderColor= QColor( 200, 200, 200, 200 );
			break;
		}

		// We can't call QPainter here directly (no widget reference), so we
		// store the pending rect and let the owning widget render it in its
		// overlay paint pass via paintRect().
		m_pending     = qrect;
		m_fill        = fillColor;
		m_border      = borderColor;
		m_hasPending  = true;
	}

	/// Paint the pending rectangle onto \p painter (call from widget's paintEvent).
	void paintRect( QPainter& painter ) const {
		if( !m_hasPending )
			return;
		painter.save();
		painter.setBrush( QBrush( m_fill ) );
		painter.setPen( QPen( m_border, 1 ) );
		painter.drawRect( m_pending );
		painter.restore();
	}

	bool hasPending() const { return m_hasPending; }
	void clearPending()     { m_hasPending = false; }

private:
	QRect  m_pending;
	QColor m_fill;
	QColor m_border;
	bool   m_hasPending = false;
};
