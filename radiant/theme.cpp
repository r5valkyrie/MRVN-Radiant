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

#include <QStyle>
#include <QApplication>
#include <QMenu>
#include <QActionGroup>

#include "preferences.h"
#include "mainframe.h"
#include "preferencesystem.h"
#include "stringio.h"


enum class ETheme{
	Default = 0,
	Dark,
	Darker,
	EvenDarker,
	Modern
};

static QActionGroup *s_theme_group;
static ETheme s_theme = ETheme::Modern;

void theme_set( ETheme theme ){
	s_theme = theme;
#ifdef WIN32
//	QSettings settings( "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat );
//	if( settings.value( "AppsUseLightTheme" ) == 0 )
#endif
	static struct
	{
		bool is1stThemeApplication = true; // guard to not apply possibly wrong defaults while app is started with Default theme
		const QPalette palette = qApp->palette();
		const QString style = qApp->style()->objectName();
	}
	defaults;

	const char* sheet = R"(
	QToolTip {
		color: #ffffff;
		background-color: #4D4F4B;
		border: 1px solid white;
	}

	QScrollBar:vertical {
		background: rgb( 73, 74, 71 );
		border: 0px solid grey;
		width: 7px;
		margin: 0px 0px 0px 0px;
	}
	QScrollBar::handle:vertical {
		border: 1px solid gray;
		background: rgb( 111, 105, 100 );
		min-height: 20px;
	}
	QScrollBar::add-line:vertical {
		border: 0px solid grey;
		background: #32CC99;
		height: 0px;
		subcontrol-position: bottom;
		subcontrol-origin: margin;
	}
	QScrollBar::sub-line:vertical {
		border: 0px solid grey;
		background: #32CC99;
		height: 0px;
		subcontrol-position: top;
		subcontrol-origin: margin;
	}

	QScrollBar:horizontal {
		background: rgb( 73, 74, 71 );
		border: 0px solid grey;
		height: 7px;
		margin: 0px 0px 0px 0px;
	}
	QScrollBar::handle:horizontal {
		border: 1px solid gray;
		background: rgb( 111, 105, 100 );
		min-width: 20px;
	}
	QScrollBar::add-line:horizontal {
		border: 0px solid grey;
		background: #32CC99;
		width: 0px;
		subcontrol-position: right;
		subcontrol-origin: margin;
	}
	QScrollBar::sub-line:horizontal {
		border: 0px solid grey;
		background: #32CC99;
		width: 0px;
		subcontrol-position: left;
		subcontrol-origin: margin;
	}

	QScrollBar::handle:hover {
		background: rgb( 250, 203, 129 );
	}

	QToolBar::separator:horizontal {
		width: 1px;
		margin: 3px 1px;
		background-color: #aaaaaa;
	}
	QToolBar::separator:vertical {
		height: 1px;
		margin: 1px 3px;
		background-color: #aaaaaa;
	}
	QToolButton {
		padding: 0;
		margin: 0;
	}

	QMenu::separator {
		background: rgb( 93, 94, 91 );
		height: 1px;
		margin-top: 3px;
		margin-bottom: 3px;
		margin-left: 5px;
		margin-right: 7px;
	}
	)";

	if( theme == ETheme::Default ){
		if( !defaults.is1stThemeApplication ){
			qApp->setPalette( defaults.palette );
			qApp->setStyleSheet( "" );
			qApp->setStyle( defaults.style );
		}
	}
	else if( theme == ETheme::Dark ){
		qApp->setStyle( "Fusion" );
		QPalette darkPalette;
		QColor darkColor = QColor( 83, 84, 81 );
		QColor disabledColor = QColor( 127, 127, 127 );
		darkPalette.setColor( QPalette::Window, darkColor );
		darkPalette.setColor( QPalette::WindowText, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::WindowText, disabledColor );
		darkPalette.setColor( QPalette::Base, QColor( 46, 52, 54 ) );
		darkPalette.setColor( QPalette::AlternateBase, darkColor );
		darkPalette.setColor( QPalette::ToolTipBase, Qt::white );
		darkPalette.setColor( QPalette::ToolTipText, Qt::white );
		darkPalette.setColor( QPalette::Text, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::Text, disabledColor );
		darkPalette.setColor( QPalette::Button, darkColor.lighter( 130 ) );
		darkPalette.setColor( QPalette::ButtonText, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledColor.lighter( 130 ) );
		darkPalette.setColor( QPalette::BrightText, Qt::red );
		darkPalette.setColor( QPalette::Link, QColor( 42, 130, 218 ) );

		darkPalette.setColor( QPalette::Highlight, QColor( 250, 203, 129 ) );
		darkPalette.setColor( QPalette::Inactive, QPalette::Highlight, disabledColor );
		darkPalette.setColor( QPalette::HighlightedText, Qt::black );
		darkPalette.setColor( QPalette::Disabled, QPalette::HighlightedText, disabledColor );

		qApp->setPalette( darkPalette );

		qApp->setStyleSheet( sheet );
	}
	else if( theme == ETheme::Darker ){
		qApp->setStyle( "Fusion" );
		QPalette darkPalette;
		QColor darkColor = QColor( 45, 45, 45 );
		QColor disabledColor = QColor( 127, 127, 127 );
		darkPalette.setColor( QPalette::Window, darkColor );
		darkPalette.setColor( QPalette::WindowText, Qt::white );
		darkPalette.setColor( QPalette::Base, QColor( 18, 18, 18 ) );
		darkPalette.setColor( QPalette::AlternateBase, darkColor );
		darkPalette.setColor( QPalette::ToolTipBase, Qt::white );
		darkPalette.setColor( QPalette::ToolTipText, Qt::white );
		darkPalette.setColor( QPalette::Text, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::Text, disabledColor );
		darkPalette.setColor( QPalette::Button, darkColor );
		darkPalette.setColor( QPalette::ButtonText, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledColor );
		darkPalette.setColor( QPalette::BrightText, Qt::red );
		darkPalette.setColor( QPalette::Link, QColor( 42, 130, 218 ) );

		darkPalette.setColor( QPalette::Highlight, QColor( 42, 130, 218 ) );
		darkPalette.setColor( QPalette::HighlightedText, Qt::black );
		darkPalette.setColor( QPalette::Disabled, QPalette::HighlightedText, disabledColor );

		qApp->setPalette( darkPalette );

		qApp->setStyleSheet( sheet );
	}
	else if( theme == ETheme::EvenDarker ){
		qApp->setStyle( "Fusion" );
		QPalette darkPalette;
		QColor darkColor = QColor( 30, 30, 30 );
		QColor disabledColor = QColor( 100, 100, 100 );
		darkPalette.setColor( QPalette::Window, darkColor );
		darkPalette.setColor( QPalette::WindowText, Qt::white );
		darkPalette.setColor( QPalette::Base, QColor( 12, 12, 12 ) );
		darkPalette.setColor( QPalette::AlternateBase, QColor( 24, 24, 24 ) );
		darkPalette.setColor( QPalette::ToolTipBase, Qt::white );
		darkPalette.setColor( QPalette::ToolTipText, Qt::white );
		darkPalette.setColor( QPalette::Text, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::Text, disabledColor );
		darkPalette.setColor( QPalette::Button, QColor( 36, 36, 36 ) );
		darkPalette.setColor( QPalette::ButtonText, Qt::white );
		darkPalette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledColor );
		darkPalette.setColor( QPalette::BrightText, Qt::red );
		darkPalette.setColor( QPalette::Link, QColor( 42, 130, 218 ) );

		darkPalette.setColor( QPalette::Highlight, QColor( 42, 130, 218 ) );
		darkPalette.setColor( QPalette::HighlightedText, Qt::black );
		darkPalette.setColor( QPalette::Disabled, QPalette::HighlightedText, disabledColor );

		qApp->setPalette( darkPalette );

		qApp->setStyleSheet( sheet );
	}
	else if( theme == ETheme::Modern ){
		qApp->setStyle( "Fusion" );

		QPalette modernPalette;
		// VS Code / One Dark inspired colors
		QColor bgColor( 24, 26, 31 );           // Slightly darker base
		QColor fgColor( 204, 210, 220 );        // Softer white
		QColor accentColor( 86, 156, 214 );     // VS Code blue
		QColor accentHover( 104, 176, 234 );    // Lighter accent
		QColor secondaryBg( 30, 33, 39 );       // Panel background
		QColor inputBg( 37, 40, 47 );           // Input fields
		QColor borderColor( 48, 52, 62 );       // Subtle borders
		QColor disabledColor( 90, 95, 105 );    // Muted disabled
		QColor successColor( 152, 195, 121 );   // Green accent
		QColor warningColor( 229, 192, 123 );   // Yellow/orange accent
		QColor errorColor( 224, 108, 117 );     // Red accent

		modernPalette.setColor( QPalette::Window, bgColor );
		modernPalette.setColor( QPalette::WindowText, fgColor );
		modernPalette.setColor( QPalette::Disabled, QPalette::WindowText, disabledColor );
		modernPalette.setColor( QPalette::Base, inputBg );
		modernPalette.setColor( QPalette::AlternateBase, secondaryBg );
		modernPalette.setColor( QPalette::ToolTipBase, QColor( 45, 48, 56 ) );
		modernPalette.setColor( QPalette::ToolTipText, fgColor );
		modernPalette.setColor( QPalette::Text, fgColor );
		modernPalette.setColor( QPalette::Disabled, QPalette::Text, disabledColor );
		modernPalette.setColor( QPalette::Button, secondaryBg );
		modernPalette.setColor( QPalette::ButtonText, fgColor );
		modernPalette.setColor( QPalette::Disabled, QPalette::ButtonText, disabledColor );
		modernPalette.setColor( QPalette::BrightText, errorColor );
		modernPalette.setColor( QPalette::Link, accentColor );
		modernPalette.setColor( QPalette::Highlight, accentColor );
		modernPalette.setColor( QPalette::Inactive, QPalette::Highlight, QColor( 55, 65, 80 ) );
		modernPalette.setColor( QPalette::HighlightedText, Qt::white );
		modernPalette.setColor( QPalette::Disabled, QPalette::HighlightedText, disabledColor );
		modernPalette.setColor( QPalette::Mid, borderColor );
		modernPalette.setColor( QPalette::Dark, QColor( 20, 22, 26 ) );
		modernPalette.setColor( QPalette::Shadow, QColor( 10, 11, 13 ) );
		modernPalette.setColor( QPalette::Light, QColor( 60, 65, 75 ) );
		modernPalette.setColor( QPalette::Midlight, QColor( 45, 50, 58 ) );

		qApp->setPalette( modernPalette );

		const char* modernSheet = R"(
			/* Global */
			* {
				font-family: 'Segoe UI', 'Roboto', 'Helvetica Neue', sans-serif;
				font-size: 9pt;
				outline: none;
			}

			/* Tooltip - More refined */
			QToolTip {
				color: #CCD2DC;
				background-color: #2D3038;
				border: 1px solid #569CD6;
				border-radius: 3px;
				padding: 5px 8px;
			}

			/* Scrollbar - Sleeker minimal design */
			QScrollBar:vertical {
				background: transparent;
				width: 12px;
				margin: 0px;
				border: none;
			}
			QScrollBar::handle:vertical {
				background: rgba(120, 130, 145, 0.4);
				min-height: 40px;
				border-radius: 4px;
				margin: 2px 3px;
			}
			QScrollBar::handle:vertical:hover {
				background: rgba(120, 130, 145, 0.7);
			}
			QScrollBar::handle:vertical:pressed {
				background: #569CD6;
			}
			QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
				height: 0px;
			}
			QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
				background: none;
			}

			/* Scrollbar - Horizontal */
			QScrollBar:horizontal {
				background: transparent;
				height: 12px;
				margin: 0px;
				border: none;
			}
			QScrollBar::handle:horizontal {
				background: rgba(120, 130, 145, 0.4);
				min-width: 40px;
				border-radius: 4px;
				margin: 3px 2px;
			}
			QScrollBar::handle:horizontal:hover {
				background: rgba(120, 130, 145, 0.7);
			}
			QScrollBar::handle:horizontal:pressed {
				background: #569CD6;
			}
			QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
				width: 0px;
			}
			QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
				background: none;
			}

			/* Button - Modern flat with subtle depth */
			QPushButton {
				background-color: #2D323C;
				color: #CCD2DC;
				border: 1px solid #404550;
				border-radius: 4px;
				padding: 5px 14px;
				min-width: 70px;
			}
			QPushButton:hover {
				background-color: #363C48;
				border-color: #569CD6;
			}
			QPushButton:pressed {
				background-color: #252930;
				border-color: #569CD6;
			}
			QPushButton:disabled {
				background-color: #1E2127;
				color: #5A5F69;
				border-color: #2A2E35;
			}
			QPushButton:default {
				background-color: #37424E;
				border-color: #569CD6;
			}

			/* Tool Button - Clean and minimal */
			QToolButton {
				background-color: transparent;
				border: 1px solid transparent;
				border-radius: 3px;
				padding: 3px;
				margin: 1px;
			}
			QToolButton:hover {
				background-color: rgba(86, 156, 214, 0.15);
				border-color: rgba(86, 156, 214, 0.3);
			}
			QToolButton:pressed, QToolButton:checked {
				background-color: rgba(86, 156, 214, 0.25);
				border-color: #569CD6;
			}

			/* Toolbar - Cleaner look */
			QToolBar {
				background-color: #1E2127;
				border: none;
				border-bottom: 1px solid #30343E;
				spacing: 1px;
				padding: 2px 4px;
			}
			QToolBar::separator:horizontal {
				width: 1px;
				margin: 6px 4px;
				background-color: #30343E;
			}
			QToolBar::separator:vertical {
				height: 1px;
				margin: 4px 6px;
				background-color: #30343E;
			}

			/* Menu - Refined dropdown */
			QMenu {
				background-color: #252830;
				border: 1px solid #30343E;
				border-radius: 4px;
				padding: 4px 0px;
			}
			QMenu::item {
				padding: 6px 28px 6px 20px;
				margin: 1px 4px;
				border-radius: 3px;
			}
			QMenu::item:selected {
				background-color: rgba(86, 156, 214, 0.25);
				color: #FFFFFF;
			}
			QMenu::item:disabled {
				color: #5A5F69;
			}
			QMenu::separator {
				height: 1px;
				background: #30343E;
				margin: 4px 8px;
			}
			QMenu::indicator {
				width: 14px;
				height: 14px;
				left: 6px;
			}
			QMenu::right-arrow {
				width: 10px;
				height: 10px;
			}

			/* Menu Bar - Minimal */
			QMenuBar {
				background-color: #1E2127;
				border: none;
				border-bottom: 1px solid #30343E;
				padding: 1px;
			}
			QMenuBar::item {
				padding: 5px 10px;
				background-color: transparent;
				border-radius: 3px;
				margin: 1px;
			}
			QMenuBar::item:selected {
				background-color: rgba(86, 156, 214, 0.2);
			}
			QMenuBar::item:pressed {
				background-color: rgba(86, 156, 214, 0.3);
			}

			/* Line Edit - Clean input fields */
			QLineEdit {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				padding: 5px 8px;
				selection-background-color: #264F78;
				selection-color: #FFFFFF;
			}
			QLineEdit:hover {
				border-color: #404550;
			}
			QLineEdit:focus {
				border-color: #569CD6;
				background-color: #22262D;
			}
			QLineEdit:disabled {
				background-color: #181A1F;
				color: #5A5F69;
				border-color: #252830;
			}

			/* Text Edit - Consistent with LineEdit */
			QTextEdit, QPlainTextEdit {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				selection-background-color: #264F78;
				selection-color: #FFFFFF;
			}
			QTextEdit:focus, QPlainTextEdit:focus {
				border-color: #569CD6;
			}

			/* Combo Box - Refined dropdown */
			QComboBox {
				background-color: #2D323C;
				color: #CCD2DC;
				border: 1px solid #404550;
				border-radius: 3px;
				padding: 4px 8px;
				padding-right: 24px;
				min-width: 70px;
			}
			QComboBox:hover {
				border-color: #505560;
			}
			QComboBox:focus {
				border-color: #569CD6;
			}
			QComboBox::drop-down {
				border: none;
				width: 20px;
				subcontrol-origin: padding;
				subcontrol-position: center right;
			}
			QComboBox::down-arrow {
				width: 10px;
				height: 10px;
			}
			QComboBox QAbstractItemView {
				background-color: #252830;
				border: 1px solid #30343E;
				border-radius: 3px;
				selection-background-color: rgba(86, 156, 214, 0.3);
				selection-color: #FFFFFF;
				padding: 2px;
				outline: none;
			}

			/* Spin Box - Cleaner arrows */
			QSpinBox, QDoubleSpinBox {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				padding: 4px 6px;
				padding-right: 20px;
			}
			QSpinBox:hover, QDoubleSpinBox:hover {
				border-color: #404550;
			}
			QSpinBox:focus, QDoubleSpinBox:focus {
				border-color: #569CD6;
			}
			QSpinBox::up-button, QDoubleSpinBox::up-button,
			QSpinBox::down-button, QDoubleSpinBox::down-button {
				width: 18px;
				border: none;
				background-color: #2D323C;
			}
			QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
			QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
				background-color: rgba(86, 156, 214, 0.3);
			}
			QSpinBox::up-button, QDoubleSpinBox::up-button {
				subcontrol-position: top right;
				border-top-right-radius: 3px;
			}
			QSpinBox::down-button, QDoubleSpinBox::down-button {
				subcontrol-position: bottom right;
				border-bottom-right-radius: 3px;
			}

			/* Slider - Modern look */
			QSlider::groove:horizontal {
				height: 4px;
				background: #30343E;
				border-radius: 2px;
			}
			QSlider::handle:horizontal {
				background: #569CD6;
				width: 14px;
				height: 14px;
				margin: -5px 0;
				border-radius: 7px;
			}
			QSlider::handle:horizontal:hover {
				background: #68B0EA;
			}
			QSlider::sub-page:horizontal {
				background: #569CD6;
				border-radius: 2px;
			}
			QSlider::groove:vertical {
				width: 4px;
				background: #30343E;
				border-radius: 2px;
			}
			QSlider::handle:vertical {
				background: #569CD6;
				width: 14px;
				height: 14px;
				margin: 0 -5px;
				border-radius: 7px;
			}
			QSlider::handle:vertical:hover {
				background: #68B0EA;
			}

			/* Check Box - Cleaner design */
			QCheckBox {
				spacing: 6px;
			}
			QCheckBox::indicator {
				width: 16px;
				height: 16px;
				border-radius: 3px;
				border: 1px solid #404550;
				background-color: #1E2127;
			}
			QCheckBox::indicator:hover {
				border-color: #569CD6;
				background-color: rgba(86, 156, 214, 0.1);
			}
			QCheckBox::indicator:checked {
				background-color: #569CD6;
				border-color: #569CD6;
			}
			QCheckBox::indicator:disabled {
				background-color: #181A1F;
				border-color: #30343E;
			}

			/* Radio Button - Consistent with checkbox */
			QRadioButton {
				spacing: 6px;
			}
			QRadioButton::indicator {
				width: 16px;
				height: 16px;
				border-radius: 8px;
				border: 1px solid #404550;
				background-color: #1E2127;
			}
			QRadioButton::indicator:hover {
				border-color: #569CD6;
			}
			QRadioButton::indicator:checked {
				background-color: #569CD6;
				border-color: #569CD6;
			}

			/* Group Box - Subtle grouping */
			QGroupBox {
				background-color: transparent;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 4px;
				margin-top: 10px;
				padding: 10px;
				padding-top: 16px;
			}
			QGroupBox::title {
				subcontrol-origin: margin;
				subcontrol-position: top left;
				left: 10px;
				padding: 0 4px;
				color: #9DA5B4;
				background-color: #181A1F;
			}

			/* Tab Widget - Clean tabs */
			QTabWidget::pane {
				background-color: #1E2127;
				border: 1px solid #30343E;
				border-radius: 3px;
				top: -1px;
			}
			QTabBar {
				background-color: transparent;
			}
			QTabBar::tab {
				background-color: transparent;
				color: #808590;
				border: none;
				padding: 8px 16px;
				margin-right: 1px;
				border-bottom: 2px solid transparent;
			}
			QTabBar::tab:selected {
				color: #CCD2DC;
				border-bottom: 2px solid #569CD6;
			}
			QTabBar::tab:hover:!selected {
				color: #B0B5C0;
				background-color: rgba(86, 156, 214, 0.08);
			}

			/* Dock Widget - Integrated panels */
			QDockWidget {
				background-color: #1E2127;
				color: #CCD2DC;
				border: none;
			}
			QDockWidget::title {
				background-color: #252830;
				padding: 6px 8px;
				border-bottom: 1px solid #30343E;
				text-align: left;
			}
			QDockWidget::close-button, QDockWidget::float-button {
				background-color: transparent;
				border: none;
				border-radius: 2px;
				width: 14px;
				height: 14px;
				padding: 2px;
			}
			QDockWidget::close-button:hover, QDockWidget::float-button:hover {
				background-color: rgba(86, 156, 214, 0.25);
			}

			/* List View - Clean lists */
			QListView, QListWidget {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				outline: none;
			}
			QListView::item, QListWidget::item {
				padding: 4px 8px;
				border-radius: 2px;
			}
			QListView::item:selected, QListWidget::item:selected {
				background-color: rgba(86, 156, 214, 0.3);
				color: #FFFFFF;
			}
			QListView::item:hover:!selected, QListWidget::item:hover:!selected {
				background-color: rgba(86, 156, 214, 0.1);
			}

			/* Tree View - Minimal tree */
			QTreeView, QTreeWidget {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				outline: none;
			}
			QTreeView::item, QTreeWidget::item {
				padding: 3px 6px;
				border-radius: 2px;
			}
			QTreeView::item:selected, QTreeWidget::item:selected {
				background-color: rgba(86, 156, 214, 0.3);
				color: #FFFFFF;
			}
			QTreeView::item:hover:!selected, QTreeWidget::item:hover:!selected {
				background-color: rgba(86, 156, 214, 0.1);
			}
			QTreeView::branch {
				background-color: transparent;
			}

			/* Table View - Clean grid */
			QTableView, QTableWidget {
				background-color: #1E2127;
				color: #CCD2DC;
				border: 1px solid #30343E;
				border-radius: 3px;
				gridline-color: #2A2E35;
				outline: none;
			}
			QTableView::item:selected, QTableWidget::item:selected {
				background-color: rgba(86, 156, 214, 0.3);
				color: #FFFFFF;
			}
			QTableView::item:hover:!selected, QTableWidget::item:hover:!selected {
				background-color: rgba(86, 156, 214, 0.08);
			}
			QHeaderView::section {
				background-color: #252830;
				color: #9DA5B4;
				padding: 6px 8px;
				border: none;
				border-right: 1px solid #30343E;
				border-bottom: 1px solid #30343E;
				font-weight: normal;
			}
			QHeaderView::section:hover {
				background-color: #2D323C;
				color: #CCD2DC;
			}
			QTableCornerButton::section {
				background-color: #252830;
				border: none;
			}

			/* Progress Bar - Subtle animation */
			QProgressBar {
				background-color: #252830;
				border: none;
				border-radius: 3px;
				text-align: center;
				height: 6px;
			}
			QProgressBar::chunk {
				background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,
					stop:0 #569CD6, stop:1 #98C379);
				border-radius: 3px;
			}

			/* Splitter - Minimal handles */
			QSplitter::handle {
				background-color: #30343E;
			}
			QSplitter::handle:hover {
				background-color: #569CD6;
			}
			QSplitter::handle:horizontal {
				width: 1px;
			}
			QSplitter::handle:vertical {
				height: 1px;
			}

			/* Frame - Subtle borders */
			QFrame {
				background-color: transparent;
				border: none;
			}
			QFrame[frameShape="4"], QFrame[frameShape="5"] {
				background-color: #30343E;
			}

			/* Label */
			QLabel {
				color: #CCD2DC;
				background-color: transparent;
				border: none;
			}

			/* Status Bar - Integrated look */
			QStatusBar {
				background-color: #1C1E23;
				color: #808590;
				border-top: 1px solid #30343E;
			}
			QStatusBar::item {
				border: none;
			}

			/* Scroll Area */
			QScrollArea {
				background-color: transparent;
				border: none;
			}

			/* Tool Box */
			QToolBox {
				background-color: #1E2127;
				border: 1px solid #30343E;
				border-radius: 3px;
			}
			QToolBox::tab {
				background-color: #252830;
				color: #9DA5B4;
				border: none;
				padding: 8px 12px;
			}
			QToolBox::tab:selected {
				background-color: #1E2127;
				color: #CCD2DC;
				border-left: 2px solid #569CD6;
			}

			/* Dialog buttons */
			QDialogButtonBox QPushButton {
				min-width: 80px;
			}

			/* Message Box */
			QMessageBox {
				background-color: #1E2127;
			}

			/* Input Dialog */
			QInputDialog {
				background-color: #1E2127;
			}
		)";

		qApp->setStyleSheet( modernSheet );
	}

	defaults.is1stThemeApplication = false;
}

void theme_contruct_menu( class QMenu *menu ){
	auto *m = menu->addMenu( "GUI Theme" );
	m->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
	auto *group = s_theme_group = new QActionGroup( m );
	{
		auto *a = m->addAction( "Default" );
		a->setCheckable( true );
		group->addAction( a );
	}
	{
		auto *a = m->addAction( "Dark" );
		a->setCheckable( true );
		group->addAction( a );
	}
	{
		auto *a = m->addAction( "Darker" );
		a->setCheckable( true );
		group->addAction( a );
	}
	{
		auto *a = m->addAction( "Even Darker" );
		a->setCheckable( true );
		group->addAction( a );
	}
	{
		auto *a = m->addAction( "Modern" );
		a->setCheckable( true );
		group->addAction( a );
	}

	QObject::connect( s_theme_group, &QActionGroup::triggered, []( QAction *action ){
		theme_set( static_cast<ETheme>( s_theme_group->actions().indexOf( action ) ) );
	} );
}

void ThemeImport( int value ){
	s_theme = static_cast<ETheme>( value );
	if( s_theme_group != nullptr && 0 <= value && value < s_theme_group->actions().size() ){
		s_theme_group->actions().at( value )->setChecked( true );
	}
}
typedef FreeCaller1<int, ThemeImport> ThemeImportCaller;

void ThemeExport( const IntImportCallback& importer ){
	importer( static_cast<int>( s_theme ) );
}
typedef FreeCaller1<const IntImportCallback&, ThemeExport> ThemeExportCaller;


void theme_contruct(){
	GlobalPreferenceSystem().registerPreference( "GUITheme", makeIntStringImportCallback( ThemeImportCaller() ), makeIntStringExportCallback( ThemeExportCaller() ) );
	theme_set( s_theme ); // set theme here, not in importer, so it's set on the very 1st start too (when there is no preference to load)
}
