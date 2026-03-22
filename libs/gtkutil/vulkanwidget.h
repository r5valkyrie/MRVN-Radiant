/*
   vulkanwidget.h — Drop-in QOpenGLWidget replacement for the Vulkan port.

   Each subclass uses the same initializeGL / resizeGL / paintGL interface but
   the underlying QWindow has VulkanSurface type so that
   QVulkanInstance::surfaceForWindow() returns a valid VkSurfaceKHR.

   Input events emitted by the inner QWindow are forwarded back to the outer
   QWidget so all mouse/keyboard handlers in derived classes continue to work
   without change.

   Licensed under the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>
#include <QWindow>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QResizeEvent>
#include <QExposeEvent>

#include "glwidget.h"   // glwidget_vulkanInstance(), glwidget_context_created()

/// Base class that replaces QOpenGLWidget for all Vulkan render windows.
/// Derive from this and override initializeGL / resizeGL / paintGL exactly as
/// you would with QOpenGLWidget.  Call update() to request a repaint.
class VulkanWidget : public QWidget
{
	// ── Inner Vulkan surface window ──────────────────────────────────────────
	class VulkanSurfaceWindow : public QWindow
	{
		VulkanWidget& m_owner;
		bool          m_initialized = false;
	public:
		explicit VulkanSurfaceWindow( VulkanWidget& owner ) : m_owner( owner )
		{
			setSurfaceType( QSurface::VulkanSurface );
			setVulkanInstance( glwidget_vulkanInstance() );
		}

	protected:
		void exposeEvent( QExposeEvent* ) override
		{
			if ( isExposed() && !m_initialized )
			{
				m_initialized = true;
				glwidget_context_created( this );
				m_owner.initializeGL();
				// Mirror QOpenGLWidget's guaranteed ordering: resizeGL is always
				// called before the first paintGL so subclasses can allocate
				// size-dependent resources (FBOs, swap-chains, etc.) safely.
				m_owner.resizeGL( width(), height() );
			}
			if ( isExposed() )
				requestUpdate();
		}

		void resizeEvent( QResizeEvent* e ) override
		{
			if ( m_initialized )
				m_owner.resizeGL( e->size().width(), e->size().height() );
		}

		bool event( QEvent* e ) override
		{
			if ( e->type() == QEvent::UpdateRequest )
			{
				m_owner.paintGL();
				return true;
			}
			return QWindow::event( e );
		}

		// Forward all input events to the outer QWidget so subclass overrides work.
		void mousePressEvent      ( QMouseEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void mouseReleaseEvent    ( QMouseEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void mouseMoveEvent       ( QMouseEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void mouseDoubleClickEvent( QMouseEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void wheelEvent           ( QWheelEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void keyPressEvent        ( QKeyEvent*   e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void keyReleaseEvent      ( QKeyEvent*   e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void focusInEvent         ( QFocusEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
		void focusOutEvent        ( QFocusEvent* e ) override { QCoreApplication::sendEvent( &m_owner, e ); }
	};

	VulkanSurfaceWindow* m_vulkanWindow;

public:
	explicit VulkanWidget( QWidget* parent = nullptr ) : QWidget( parent )
	{
		m_vulkanWindow = new VulkanSurfaceWindow( *this );
		QWidget* container = QWidget::createWindowContainer( m_vulkanWindow, this );
		container->setFocusPolicy( Qt::NoFocus );   // keep focus on the outer widget

		QVBoxLayout* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 0, 0, 0, 0 );
		layout->setSpacing( 0 );
		layout->addWidget( container );
	}

	/// Trigger a repaint — mirrors QOpenGLWidget::update().
	void update()
	{
		if ( m_vulkanWindow )
			m_vulkanWindow->requestUpdate();
	}

	/// Returns the device pixel ratio — mirrors QOpenGLWidget::devicePixelRatioF().
	qreal devicePixelRatioF() const
	{
		return m_vulkanWindow ? m_vulkanWindow->devicePixelRatio() : 1.0;
	}

	/// Returns the underlying QWindow (for e.g. passing to glwidget_context_destroyed).
	QWindow* vulkanWindow() const { return m_vulkanWindow; }

protected:
	virtual void initializeGL()         {}
	virtual void resizeGL( int, int )   {}
	virtual void paintGL()              {}
};
