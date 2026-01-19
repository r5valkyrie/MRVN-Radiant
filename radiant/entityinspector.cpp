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

#include "entityinspector.h"

#include "debugging/debugging.h"

#include "ientity.h"
#include "ifilesystem.h"
#include "imodel.h"
#include "iscenegraph.h"
#include "iselection.h"
#include "iundo.h"

#include <map>
#include <set>
#include <variant>
#include <vector>

#include <gtkutil/guisettings.h>
#include <QSplitter>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QCheckBox>
#include "gtkutil/lineedit.h"
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QKeyEvent>
#include <QApplication>
#include <QButtonGroup>
#include <QGroupBox>
#include <QFrame>
#include <QStackedWidget>
#include <QTabWidget>
#include <QLineEdit>
#include <QTimer>
#include <QRegularExpression>
#include <QPalette>
#include "gtkutil/combobox.h"

#include "os/path.h"
#include "eclasslib.h"
#include "scenelib.h"
#include "generic/callback.h"
#include "os/file.h"
#include "stream/stringstream.h"
#include "moduleobserver.h"
#include "stringio.h"

#include "gtkutil/accelerator.h"
#include "gtkutil/dialog.h"
#include "gtkutil/filechooser.h"
#include "gtkutil/messagebox.h"
#include "gtkutil/nonmodal.h"
#include "gtkutil/entry.h"

#include "qe3.h"
#include "gtkmisc.h"
#include "gtkdlgs.h"
#include "entity.h"
#include "mainframe.h"
#include "textureentry.h"
#include "groupdialog.h"

#include "select.h"

namespace
{
typedef std::map<CopiedString, CopiedString> KeyValues;
KeyValues g_selectedKeyValues;
KeyValues g_selectedDefaultKeyValues;
}

const char* SelectedEntity_getValueForKey( const char* key ){
	{
		KeyValues::const_iterator i = g_selectedKeyValues.find( key );
		if ( i != g_selectedKeyValues.end() ) {
			return ( *i ).second.c_str();
		}
	}
	{
		KeyValues::const_iterator i = g_selectedDefaultKeyValues.find( key );
		if ( i != g_selectedDefaultKeyValues.end() ) {
			return ( *i ).second.c_str();
		}
	}
	return "";
}

void Scene_EntitySetKeyValue_Selected_Undoable( const char* key, const char* value ){
	const auto command = StringStream( "entitySetKeyValue -key ", makeQuoted( key ), " -value ", makeQuoted( value ) );
	UndoableCommand undo( command );
	Scene_EntitySetKeyValue_Selected( key, value );
}

class EntityAttribute
{
public:
	virtual QWidget* getWidget() const = 0;
	virtual void update() = 0;
	virtual void release() = 0;
};

class BooleanAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	QCheckBox* m_check;
public:
	BooleanAttribute( const char* key ) :
		m_key( key ),
		m_check( new QCheckBox )
	{
		QObject::connect( m_check, &QAbstractButton::clicked, [this](){ apply(); } );
		update();
	}
	QWidget* getWidget() const override {
		return m_check;
	}
	void release() override {
		delete this;
	}
	void apply(){
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), m_check->isChecked() ? "1" : "" );
	}
	typedef MemberCaller<BooleanAttribute, &BooleanAttribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		m_check->setChecked( atoi( value ) != 0 ); // atoi( empty ) is also 0
	}
	typedef MemberCaller<BooleanAttribute, &BooleanAttribute::update> UpdateCaller;
};


class StringAttribute : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry *m_entry;
public:
	StringAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ){
	}
	virtual ~StringAttribute() = default;
	QWidget* getWidget() const override {
		return m_entry;
	}
	QLineEdit* getEntry() const {
		return m_entry;
	}

	void release() override {
		delete this;
	}
	void apply(){
		const auto value = m_entry->text().toLatin1();
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), value.constData() );
	}
	typedef MemberCaller<StringAttribute, &StringAttribute::apply> ApplyCaller;

	void update() override {
		m_entry->setText( SelectedEntity_getValueForKey( m_key.c_str() ) );
	}
	typedef MemberCaller<StringAttribute, &StringAttribute::update> UpdateCaller;
};

class ShaderAttribute : public StringAttribute
{
public:
	ShaderAttribute( const char* key ) : StringAttribute( key ){
		GlobalShaderEntryCompletion::instance().connect( StringAttribute::getEntry() );
	}
};

class TextureAttribute : public StringAttribute
{
public:
	TextureAttribute( const char* key ) : StringAttribute( key ){
		if( string_empty( GlobalRadiant().getGameDescriptionKeyValue( "show_wads" ) ) )
			GlobalAllShadersEntryCompletion::instance().connect( StringAttribute::getEntry() );	// with textures/
		else
			GlobalTextureEntryCompletion::instance().connect( StringAttribute::getEntry() );	// w/o
	}
};


class ColorAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry *m_entry;
public:
	ColorAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ){
		auto button = m_entry->addAction( QApplication::style()->standardIcon( QStyle::SP_ArrowRight ), QLineEdit::ActionPosition::TrailingPosition );
		QObject::connect( button, &QAction::triggered, [this](){ browse(); } );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_entry;
	}
	void apply(){
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), m_entry->text().toLatin1().constData() );
	}
	typedef MemberCaller<ColorAttribute, &ColorAttribute::apply> ApplyCaller;
	void update() override {
		m_entry->setText( SelectedEntity_getValueForKey( m_key.c_str() ) );
	}
	typedef MemberCaller<ColorAttribute, &ColorAttribute::update> UpdateCaller;
	void browse(){
		Vector3 color( 1, 1, 1 );
		string_parse_vector3( m_entry->text().toLatin1().constData(), color );
		if( color_dialog( m_entry->window(), color ) ){
			char buffer[64];
			sprintf( buffer, "%g %g %g", color[0], color[1], color[2] );
			m_entry->setText( buffer );
			apply();
		}
	}
};


class ModelAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry *m_entry;
public:
	ModelAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ){
		auto button = m_entry->addAction( QApplication::style()->standardIcon( QStyle::SP_FileDialogStart ), QLineEdit::ActionPosition::TrailingPosition );
		QObject::connect( button, &QAction::triggered, [this](){ browse(); } );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_entry;
	}
	void apply(){
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), m_entry->text().toLatin1().constData() );
	}
	typedef MemberCaller<ModelAttribute, &ModelAttribute::apply> ApplyCaller;
	void update() override {
		m_entry->setText( SelectedEntity_getValueForKey( m_key.c_str() ) );
	}
	typedef MemberCaller<ModelAttribute, &ModelAttribute::update> UpdateCaller;
	void browse(){
		const char *filename = misc_model_dialog( m_entry->window(), m_entry->text().toLatin1().constData() );

		if ( filename != 0 ) {
			m_entry->setText( filename );
			apply();
		}
	}
};

const char* browse_sound( QWidget* parent, const char* filepath ){
	StringOutputStream buffer( 256 );

	if( !string_empty( filepath ) ){
		const char* root = GlobalFileSystem().findFile( filepath );
		if( !string_empty( root ) && file_is_directory( root ) )
			buffer << root << filepath;
	}
	if( buffer.empty() ){
		buffer << g_qeglobals.m_userGamePath << "sound/";

		if ( !file_readable( buffer ) ) {
			// just go to fsmain
			buffer( g_qeglobals.m_userGamePath );
		}
	}

	const char* filename = file_dialog( parent, true, "Open Sound File", buffer, "sound" );
	if ( filename != 0 ) {
		const char* relative = path_make_relative( filename, GlobalFileSystem().findRoot( filename ) );
		if ( relative == filename ) {
			globalWarningStream() << "WARNING: could not extract the relative path, using full path instead\n";
		}
		return relative;
	}
	return filename;
}

class SoundAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry *m_entry;
public:
	SoundAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ){
		auto button = m_entry->addAction( QApplication::style()->standardIcon( QStyle::SP_MediaVolume ), QLineEdit::ActionPosition::TrailingPosition );
		QObject::connect( button, &QAction::triggered, [this](){ browse(); } );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_entry;
	}
	void apply(){
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), m_entry->text().toLatin1().constData() );
	}
	typedef MemberCaller<SoundAttribute, &SoundAttribute::apply> ApplyCaller;
	void update() override {
		m_entry->setText( SelectedEntity_getValueForKey( m_key.c_str() ) );
	}
	typedef MemberCaller<SoundAttribute, &SoundAttribute::update> UpdateCaller;
	void browse(){
		const char *filename = browse_sound( m_entry->window(), m_entry->text().toLatin1().constData() );

		if ( filename != 0 ) {
			m_entry->setText( filename );
			apply();
		}
	}
};


inline double angle_normalised( double angle ){
	return float_mod( angle, 360.0 );
}
#include "camwindow.h"
class CamAnglesButton
{
	typedef Callback1<const Vector3&> ApplyCallback;
	ApplyCallback m_apply;
public:
	QPushButton* m_button;
	CamAnglesButton( const ApplyCallback& apply ) : m_apply( apply ), m_button( new QPushButton( "<-cam" ) ){
		QObject::connect( m_button, &QAbstractButton::clicked, [this](){
			Vector3 angles( Camera_getAngles( *g_pParentWnd->GetCamWnd() ) );
			if( !string_equal( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entities" ), "quake" ) ) /* stupid quake bug */
				angles[0] = -angles[0];
			m_apply( angles );
		} );
	}
};

inline QWidget *new_container_widget(){
	QWidget *w = new QWidget;
	auto l = new QHBoxLayout( w );
	l->setContentsMargins( 0, 0, 0, 0 );
	return w;
}

class AngleAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry* m_entry;
	CamAnglesButton m_butt;
	QWidget *m_hbox;
public:
	AngleAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ),
		m_butt( ApplyVecCaller( *this ) ),
		m_hbox( new_container_widget() ){
		m_hbox->layout()->addWidget( m_entry );
		m_hbox->layout()->addWidget( m_butt.m_button );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_hbox;
	}
	void apply(){
		const auto angle = StringStream<32>( angle_normalised( entry_get_float( m_entry ) ) );
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), angle );
	}
	typedef MemberCaller<AngleAttribute, &AngleAttribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		if ( !string_empty( value ) ) {
			const auto angle = StringStream<32>( angle_normalised( atof( value ) ) );
			m_entry->setText( angle.c_str() );
		}
		else
		{
			m_entry->setText( "0" );
		}
	}
	typedef MemberCaller<AngleAttribute, &AngleAttribute::update> UpdateCaller;

	void apply( const Vector3& angles ){
		entry_set_float( m_entry, angles[1] );
		apply();
	}
	typedef MemberCaller1<AngleAttribute, const Vector3&, &AngleAttribute::apply> ApplyVecCaller;
};

class DirectionAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	NonModalEntry* m_entry;
	RadioHBox m_radio;
	CamAnglesButton m_butt;
	QWidget* m_hbox;
	static constexpr const char *const buttons[] = { "up", "down", "yaw" };
public:
	DirectionAttribute( const char* key ) :
		m_key( key ),
		m_entry( new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) ),
		m_radio( RadioHBox_new( StringArrayRange( buttons ) ) ),
		m_butt( ApplyVecCaller( *this ) ),
		m_hbox( new_container_widget() ){
		static_cast<QHBoxLayout*>( m_hbox->layout() )->addLayout( m_radio.m_hbox );
		m_hbox->layout()->addWidget( m_entry );
		m_hbox->layout()->addWidget( m_butt.m_button );
		QObject::connect( m_radio.m_radio, &QButtonGroup::idClicked, ApplyRadioCaller( *this ) );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_hbox;
	}
	void apply(){
		const auto angle = StringStream<32>( angle_normalised( entry_get_float( m_entry ) ) );
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), angle );
	}
	typedef MemberCaller<DirectionAttribute, &DirectionAttribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		if ( !string_empty( value ) ) {
			const float f = atof( value );
			if ( f == -1 ) {
				m_entry->setEnabled( false );
				m_radio.m_radio->button( 0 )->setChecked( true );
				m_entry->clear();
			}
			else if ( f == -2 ) {
				m_entry->setEnabled( false );
				m_radio.m_radio->button( 1 )->setChecked( true );
				m_entry->clear();
			}
			else
			{
				m_entry->setEnabled( true );
				m_radio.m_radio->button( 2 )->setChecked( true );
				const auto angle = StringStream<32>( angle_normalised( f ) );
				m_entry->setText( angle.c_str() );
			}
		}
		else
		{
			m_radio.m_radio->button( 2 )->setChecked( true );
			m_entry->setText( "0" );
		}
	}
	typedef MemberCaller<DirectionAttribute, &DirectionAttribute::update> UpdateCaller;

	void applyRadio( int id ){
		if ( id == 0 ) {
			Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), "-1" );
		}
		else if ( id == 1 ) {
			Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), "-2" );
		}
		else if ( id == 2 ) {
			apply();
		}
	}
	typedef MemberCaller1<DirectionAttribute, int, &DirectionAttribute::applyRadio> ApplyRadioCaller;

	void apply( const Vector3& angles ){
		entry_set_float( m_entry, angles[1] );
		apply();
	}
	typedef MemberCaller1<DirectionAttribute, const Vector3&, &DirectionAttribute::apply> ApplyVecCaller;
};


class AnglesEntry
{
public:
	QLineEdit* m_roll;
	QLineEdit* m_pitch;
	QLineEdit* m_yaw;
	AnglesEntry() : m_roll( 0 ), m_pitch( 0 ), m_yaw( 0 ){
	}
};

class AnglesAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	AnglesEntry m_angles;
	CamAnglesButton m_butt;
	QWidget* m_hbox;
public:
	AnglesAttribute( const char* key ) :
		m_key( key ),
		m_butt( ApplyVecCaller( *this ) ),
		m_hbox( new_container_widget() ){
		m_hbox->layout()->addWidget( m_angles.m_pitch = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
		m_hbox->layout()->addWidget( m_angles.m_yaw = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
		m_hbox->layout()->addWidget( m_angles.m_roll = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
		m_hbox->layout()->addWidget( m_butt.m_button );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_hbox;
	}
	void apply(){
		const auto angles = StringStream<64>( angle_normalised( entry_get_float( m_angles.m_pitch ) ),
		                                 ' ', angle_normalised( entry_get_float( m_angles.m_yaw ) ),
		                                 ' ', angle_normalised( entry_get_float( m_angles.m_roll ) ) );
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), angles );
	}
	typedef MemberCaller<AnglesAttribute, &AnglesAttribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		if ( !string_empty( value ) ) {
			DoubleVector3 pitch_yaw_roll;
			if ( !string_parse_vector3( value, pitch_yaw_roll ) ) {
				pitch_yaw_roll = DoubleVector3( 0, 0, 0 );
			}
			StringOutputStream angle( 32 );

			angle( angle_normalised( pitch_yaw_roll.x() ) );
			m_angles.m_pitch->setText( angle.c_str() );

			angle( angle_normalised( pitch_yaw_roll.y() ) );
			m_angles.m_yaw->setText( angle.c_str() );

			angle( angle_normalised( pitch_yaw_roll.z() ) );
			m_angles.m_roll->setText( angle.c_str() );
		}
		else
		{
			m_angles.m_pitch->setText( "0" );
			m_angles.m_yaw->setText( "0" );
			m_angles.m_roll->setText( "0" );
		}
	}
	typedef MemberCaller<AnglesAttribute, &AnglesAttribute::update> UpdateCaller;

	void apply( const Vector3& angles ){
		entry_set_float( m_angles.m_pitch, angles[0] );
		entry_set_float( m_angles.m_yaw, angles[1] );
		entry_set_float( m_angles.m_roll, 0 );
		apply();
	}
	typedef MemberCaller1<AnglesAttribute, const Vector3&, &AnglesAttribute::apply> ApplyVecCaller;
};

class Vector3Entry
{
public:
	QLineEdit* m_x;
	QLineEdit* m_y;
	QLineEdit* m_z;
	Vector3Entry() : m_x( 0 ), m_y( 0 ), m_z( 0 ){
	}
};

class Vector3Attribute final : public EntityAttribute
{
	const CopiedString m_key;
	Vector3Entry m_vector3;
	QWidget* m_hbox;
public:
	Vector3Attribute( const char* key ) :
		m_key( key ),
		m_hbox( new_container_widget() ){
		m_hbox->layout()->addWidget( m_vector3.m_x = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
		m_hbox->layout()->addWidget( m_vector3.m_y = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
		m_hbox->layout()->addWidget( m_vector3.m_z = new NonModalEntry( ApplyCaller( *this ), UpdateCaller( *this ) ) );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_hbox;
	}
	void apply(){
		const auto vector3 = StringStream<64>( entry_get_float( m_vector3.m_x ),
		                                  ' ', entry_get_float( m_vector3.m_y ),
		                                  ' ', entry_get_float( m_vector3.m_z ) );
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), vector3 );
	}
	typedef MemberCaller<Vector3Attribute, &Vector3Attribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		if ( !string_empty( value ) ) {
			DoubleVector3 x_y_z;
			if ( !string_parse_vector3( value, x_y_z ) ) {
				x_y_z = DoubleVector3( 0, 0, 0 );
			}
			StringOutputStream buffer(32);

			buffer( x_y_z.x() );
			m_vector3.m_x->setText( buffer.c_str() );

			buffer( x_y_z.y() );
			m_vector3.m_y->setText( buffer.c_str() );

			buffer( x_y_z.z() );
			m_vector3.m_z->setText( buffer.c_str() );
		}
		else
		{
			m_vector3.m_x->setText( "0" );
			m_vector3.m_y->setText( "0" );
			m_vector3.m_z->setText( "0" );
		}
	}
	typedef MemberCaller<Vector3Attribute, &Vector3Attribute::update> UpdateCaller;
};

class ListAttribute final : public EntityAttribute
{
	const CopiedString m_key;
	QComboBox* m_combo;
	const ListAttributeType& m_type;
public:
	ListAttribute( const char* key, const ListAttributeType& type ) :
		m_key( key ),
		m_combo( new ComboBox ),
		m_type( type ){
		for ( const auto&[ name, value ] : type )
		{
			m_combo->addItem( name.c_str() );
		}
		QObject::connect( m_combo, QOverload<int>::of( &QComboBox::activated ), ApplyCaller( *this ) );
	}
	void release() override {
		delete this;
	}
	QWidget* getWidget() const override {
		return m_combo;
	}
	void apply(){
		// looks safe to assume that user actions wont make m_combo->currentIndex() -1
		Scene_EntitySetKeyValue_Selected_Undoable( m_key.c_str(), m_type[m_combo->currentIndex()].second.c_str() );
	}
	typedef MemberCaller<ListAttribute, &ListAttribute::apply> ApplyCaller;

	void update() override {
		const char* value = SelectedEntity_getValueForKey( m_key.c_str() );
		ListAttributeType::const_iterator i = m_type.findValue( value );
		if ( i != m_type.end() ) {
			m_combo->setCurrentIndex( static_cast<int>( std::distance( m_type.begin(), i ) ) );
		}
		else
		{
			m_combo->setCurrentIndex( 0 );
		}
	}
	typedef MemberCaller<ListAttribute, &ListAttribute::update> UpdateCaller;
};


namespace
{
bool g_entityInspector_windowConstructed = false;

// Map entities list - shows all entities currently in the map
QTreeWidget* g_mapEntitiesList;
QLineEdit* g_mapEntitiesFilter;

// Entity class list kept for internal use but hidden from UI
QTreeWidget* g_entityClassList;

QCheckBox* g_entitySpawnflagsCheck[MAX_FLAGS];
QGroupBox* g_spawnflagsGroup;

QLineEdit* g_entityKeyEntry;
QLineEdit* g_entityValueEntry;

QToolButton* g_focusToggleButton;

QTreeWidget* g_entprops_store;
QLineEdit* g_inlineEditor = nullptr;
QTreeWidgetItem* g_editingItem = nullptr;
int g_editingColumn = -1;
const EntityClass* g_current_flags = 0;
const EntityClass* g_current_attributes = 0;

// the number of active spawnflags
int g_spawnflag_count;
// table: index, match spawnflag item to the spawnflag index (i.e. which bit)
int spawn_table[MAX_FLAGS];
// we change the layout depending on how many spawn flags we need to display
QGridLayout* g_spawnflagsTable;

QGridLayout* g_attributeBox = nullptr;
typedef std::vector<EntityAttribute*> EntityAttributes;
EntityAttributes g_entityAttributes;

// Styling constants
const char* const g_inspectorStyle = R"(
QGroupBox {
    font-weight: bold;
    border: 1px solid palette(mid);
    border-radius: 4px;
    margin-top: 8px;
    padding-top: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 8px;
    padding: 0 4px;
}
QTreeWidget {
    border: 1px solid palette(mid);
    border-radius: 3px;
    background: palette(base);
    alternate-background-color: palette(alternateBase);
}
QTreeWidget::item {
    padding: 2px 0;
}
QTreeWidget::item:selected {
    background: palette(highlight);
    color: palette(highlightedText);
}
QLineEdit {
    border: 1px solid palette(mid);
    border-radius: 3px;
    padding: 4px 6px;
}
QLineEdit:focus {
    border-color: palette(highlight);
}
QPushButton, QToolButton {
    border: 1px solid palette(mid);
    border-radius: 3px;
    padding: 4px 12px;
    background: palette(button);
}
QPushButton:hover, QToolButton:hover {
    background: palette(light);
}
QPushButton:pressed, QToolButton:pressed {
    background: palette(midlight);
}
QCheckBox {
    spacing: 4px;
}
QScrollArea {
    border: none;
}
)";
}

void GlobalEntityAttributes_clear(){
	for ( EntityAttribute* attr : g_entityAttributes )
	{
		attr->release();
	}
	g_entityAttributes.clear();
}

class GetKeyValueVisitor : public Entity::Visitor
{
	KeyValues& m_keyvalues;
public:
	GetKeyValueVisitor( KeyValues& keyvalues )
		: m_keyvalues( keyvalues ){
	}

	void visit( const char* key, const char* value ){
		m_keyvalues.insert( KeyValues::value_type( CopiedString( key ), CopiedString( value ) ) );
	}

};

void Entity_GetKeyValues( const Entity& entity, KeyValues& keyvalues, KeyValues& defaultValues ){
	GetKeyValueVisitor visitor( keyvalues );

	entity.forEachKeyValue( visitor );

	const EntityClassAttributes& attributes = entity.getEntityClass().m_attributes;

	for ( EntityClassAttributes::const_iterator i = attributes.begin(); i != attributes.end(); ++i )
	{
		defaultValues.insert( KeyValues::value_type( ( *i ).first, ( *i ).second.m_value ) );
	}
}

void Entity_GetKeyValues_Selected( KeyValues& keyvalues, KeyValues& defaultValues ){
	class EntityGetKeyValues : public SelectionSystem::Visitor
	{
		KeyValues& m_keyvalues;
		KeyValues& m_defaultValues;
		mutable std::set<Entity*> m_visited;
	public:
		EntityGetKeyValues( KeyValues& keyvalues, KeyValues& defaultValues )
			: m_keyvalues( keyvalues ), m_defaultValues( defaultValues ){
		}
		void visit( scene::Instance& instance ) const {
			Entity* entity = Node_getEntity( instance.path().top() );
			if ( entity == 0 && instance.path().size() != 1 ) {
				entity = Node_getEntity( instance.path().parent() );
			}
			if ( entity != 0 && m_visited.insert( entity ).second ) {
				Entity_GetKeyValues( *entity, m_keyvalues, m_defaultValues );
			}
		}
	} visitor( keyvalues, defaultValues );
	GlobalSelectionSystem().foreachSelected( visitor );
}

const char* keyvalues_valueforkey( KeyValues& keyvalues, const char* key ){
	KeyValues::iterator i = keyvalues.find( CopiedString( key ) );
	if ( i != keyvalues.end() ) {
		return ( *i ).second.c_str();
	}
	return "";
}

// required to store EntityClass* in QVariant
Q_DECLARE_METATYPE( EntityClass* )
class EntityClassListStoreAppend : public EntityClassVisitor
{
	QTreeWidget* tree;
public:
	EntityClassListStoreAppend( QTreeWidget* tree_ ) : tree( tree_ ){
	}
	void visit( EntityClass* e ){
		auto item = new QTreeWidgetItem( tree );
		item->setData( 0, Qt::ItemDataRole::DisplayRole, e->name() );
		item->setData( 0, Qt::ItemDataRole::UserRole, QVariant::fromValue( e ) );
	}
};

void EntityClassList_fill(){
	if( !g_entityClassList ) return;
	EntityClassListStoreAppend append( g_entityClassList );
	GlobalEntityClassManager().forEach( append );
}

void EntityClassList_clear(){
	if( g_entityClassList )
		g_entityClassList->clear();
}

// Map Entities List - collects all entities in the current map
struct MapEntityInfo {
	scene::Node* node;
	CopiedString classname;
	CopiedString targetname;
	Vector3 origin;
};

std::vector<MapEntityInfo> g_mapEntities;

class MapEntityCollector : public scene::Graph::Walker
{
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const {
		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const {
		Entity* entity = Node_getEntity( path.top() );
		if ( entity != 0 ) {
			MapEntityInfo info;
			info.node = &path.top().get();
			info.classname = entity->getKeyValue( "classname" );
			info.targetname = entity->getKeyValue( "targetname" );
			Vector3 origin( 0, 0, 0 );
			string_parse_vector3( entity->getKeyValue( "origin" ), origin );
			info.origin = origin;
			g_mapEntities.push_back( info );
		}
	}
};

void MapEntitiesList_fill(){
	if( !g_mapEntitiesList ) return;
	
	g_mapEntities.clear();
	g_mapEntitiesList->clear();
	
	GlobalSceneGraph().traverse( MapEntityCollector() );
	
	const QString filter = g_mapEntitiesFilter ? g_mapEntitiesFilter->text().toLower() : QString();
	
	for ( const auto& info : g_mapEntities )
	{
		QString displayName = QString( "%1" ).arg( info.classname.c_str() );
		if ( !info.targetname.empty() ) {
			displayName += QString( " (%1)" ).arg( info.targetname.c_str() );
		}
		
		// Apply filter
		if ( !filter.isEmpty() && !displayName.toLower().contains( filter ) ) {
			continue;
		}
		
		auto item = new QTreeWidgetItem( g_mapEntitiesList );
		item->setText( 0, displayName );
		item->setData( 0, Qt::ItemDataRole::UserRole, QVariant::fromValue( reinterpret_cast<quintptr>( info.node ) ) );
		
		// Color-code by entity type
		if ( info.classname.c_str()[0] == 't' && strncmp( info.classname.c_str(), "trigger_", 8 ) == 0 ) {
			item->setForeground( 0, QColor( 255, 165, 0 ) ); // Orange for triggers
		} else if ( strncmp( info.classname.c_str(), "info_", 5 ) == 0 ) {
			item->setForeground( 0, QColor( 100, 180, 255 ) ); // Blue for info entities
		} else if ( strncmp( info.classname.c_str(), "func_", 5 ) == 0 ) {
			item->setForeground( 0, QColor( 100, 255, 100 ) ); // Green for func entities
		} else if ( strncmp( info.classname.c_str(), "light", 5 ) == 0 ) {
			item->setForeground( 0, QColor( 255, 255, 100 ) ); // Yellow for lights
		}
	}
}

void MapEntitiesList_clear(){
	if( g_mapEntitiesList ){
		g_mapEntitiesList->clear();
	}
	g_mapEntities.clear();
}

void EntityAttribute_setTooltip( QWidget* widget, const char* name, const char* description ){
	StringOutputStream stream( 256 );
	if( string_not_empty( name ) )
		stream << "<b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" << name << "</b>&nbsp;&nbsp;&nbsp;&nbsp;";
	if( string_not_empty( description ) ){
		stream << "<br>" << description;
	}
	if( !stream.empty() )
		widget->setToolTip( stream.c_str() );
}

void SpawnFlags_setEntityClass( EntityClass* eclass ){
	if ( eclass == g_current_flags ) {
		return;
	}

	g_current_flags = eclass;

	g_spawnflag_count = 0;

	// do a first pass to count the spawn flags, don't touch the widgets, we don't know in what state they are
	for ( int i = 0; i < MAX_FLAGS; i++ )
	{
		if ( eclass->flagnames[i][0] != 0 && strcmp( eclass->flagnames[i], "-" ) ) {
			spawn_table[g_spawnflag_count++] = i;
		}
		// hide all boxes
		g_entitySpawnflagsCheck[i]->hide();
	}

	// Show or hide the spawnflags group based on whether there are any flags
	if( g_spawnflagsGroup ){
		g_spawnflagsGroup->setVisible( g_spawnflag_count > 0 );
	}

	for ( int i = 0; i < g_spawnflag_count; ++i )
	{
		const auto str = StringStream<16>( LowerCase( eclass->flagnames[spawn_table[i]] ) );

		QCheckBox *check = g_entitySpawnflagsCheck[i];
		check->setText( str.c_str() );
		check->show();

		if( const EntityClassAttribute* attribute = eclass->flagAttributes[spawn_table[i]] ){
			EntityAttribute_setTooltip( check, attribute->m_name.c_str(), attribute->m_description.c_str() );
		}
	}
}

void EntityClassList_selectEntityClass( EntityClass* eclass ){
	if( g_entityClassList ){
		auto list = g_entityClassList->findItems( eclass->name(), Qt::MatchFlag::MatchFixedString );
		if( !list.isEmpty() ){
			g_entityClassList->setCurrentItem( list.first() );
		}
	}
}

void EntityInspector_appendAttribute( const EntityClassAttributePair& attributePair, EntityAttribute& attribute ){
	const char* keyname = attributePair.first.c_str(); //EntityClassAttributePair_getName( attributePair );
	auto label = new QLabel( keyname );
	EntityAttribute_setTooltip( label, attributePair.second.m_name.c_str(), attributePair.second.m_description.c_str() );
	DialogGrid_packRow( g_attributeBox, attribute.getWidget(), label );
}


template<typename Attribute>
class StatelessAttributeCreator
{
public:
	static EntityAttribute* create( const char* name ){
		return new Attribute( name );
	}
};

class EntityAttributeFactory
{
	typedef EntityAttribute* ( *CreateFunc )( const char* name );
	typedef std::map<const char*, CreateFunc, RawStringLess> Creators;
	Creators m_creators;
public:
	EntityAttributeFactory(){
		m_creators.insert( Creators::value_type( "string", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "array", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "integer", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "boolean", &StatelessAttributeCreator<BooleanAttribute>::create ) );
		m_creators.insert( Creators::value_type( "real", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "angle", &StatelessAttributeCreator<AngleAttribute>::create ) );
		m_creators.insert( Creators::value_type( "direction", &StatelessAttributeCreator<DirectionAttribute>::create ) );
		m_creators.insert( Creators::value_type( "vector3", &StatelessAttributeCreator<Vector3Attribute>::create ) );
		m_creators.insert( Creators::value_type( "real3", &StatelessAttributeCreator<Vector3Attribute>::create ) );
		m_creators.insert( Creators::value_type( "angles", &StatelessAttributeCreator<AnglesAttribute>::create ) );
		m_creators.insert( Creators::value_type( "color", &StatelessAttributeCreator<ColorAttribute>::create ) );
		m_creators.insert( Creators::value_type( "target", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "targetname", &StatelessAttributeCreator<StringAttribute>::create ) );
		m_creators.insert( Creators::value_type( "sound", &StatelessAttributeCreator<SoundAttribute>::create ) );
		m_creators.insert( Creators::value_type( "shader", &StatelessAttributeCreator<ShaderAttribute>::create ) );
		m_creators.insert( Creators::value_type( "texture", &StatelessAttributeCreator<TextureAttribute>::create ) );
		m_creators.insert( Creators::value_type( "model", &StatelessAttributeCreator<ModelAttribute>::create ) );
		m_creators.insert( Creators::value_type( "skin", &StatelessAttributeCreator<StringAttribute>::create ) );
	}
	EntityAttribute* create( const char* type, const char* name ){
		Creators::iterator i = m_creators.find( type );
		if ( i != m_creators.end() ) {
			return ( *i ).second( name );
		}
		const ListAttributeType* listType = GlobalEntityClassManager().findListType( type );
		if ( listType != 0 ) {
			return new ListAttribute( name, *listType );
		}
		return 0;
	}
};

typedef Static<EntityAttributeFactory> GlobalEntityAttributeFactory;

void EntityInspector_setEntityClass( EntityClass *eclass ){
	SpawnFlags_setEntityClass( eclass );

	if ( eclass != g_current_attributes ) {
		g_current_attributes = eclass;

		while( QLayoutItem *item = g_attributeBox->takeAt( 0 ) ){
			delete item->widget();
			delete item;
		}
		g_attributeBox->update(); // trigger scrollbar update
		GlobalEntityAttributes_clear();

		for ( const EntityClassAttributePair &pair : eclass->m_attributes )
		{
			EntityAttribute* attribute = GlobalEntityAttributeFactory::instance().create( pair.second.m_type.c_str(), pair.first.c_str() );
			if ( attribute != 0 ) {
				g_entityAttributes.push_back( attribute );
				EntityInspector_appendAttribute( pair, *g_entityAttributes.back() );
			}
		}
	}
}

void EntityInspector_updateSpawnflags(){
	{
		const int f = atoi( SelectedEntity_getValueForKey( "spawnflags" ) );
		for ( int i = 0; i < g_spawnflag_count; ++i )
		{
			const bool v = !!( f & ( 1 << spawn_table[i] ) );

			g_entitySpawnflagsCheck[i]->setChecked( v );
		}
	}
}

void EntityInspector_applySpawnflags(){
	int f = 0;

	for ( int i = 0; i < g_spawnflag_count; ++i )
	{
		const int v = g_entitySpawnflagsCheck[i]->isChecked();
		f |= v << spawn_table[i];
	}

	char value[32] = {};
	if( f != 0 )
		sprintf( value, "%i", f );

	{
		const auto command = StringStream<64>( "entitySetSpawnflags -flags ", f );
		UndoableCommand undo( command );

		Scene_EntitySetKeyValue_Selected( "spawnflags", value );
	}
}


// Finish any active inline edit
void EntityInspector_finishInlineEdit(){
	if( g_inlineEditor && g_editingItem && g_editingColumn == 1 ){
		QString newValue = g_inlineEditor->text();
		QString key = g_editingItem->text( 0 );
		
		// Apply the change
		Scene_EntitySetKeyValue_Selected_Undoable( key.toLatin1().constData(), newValue.toLatin1().constData() );
		
		g_inlineEditor->hide();
		g_editingItem = nullptr;
		g_editingColumn = -1;
	}
}

// Cancel inline editing
void EntityInspector_cancelInlineEdit(){
	if( g_inlineEditor ){
		g_inlineEditor->hide();
		g_editingItem = nullptr;
		g_editingColumn = -1;
	}
}

// Event filter for inline editor to handle Escape key and focus loss
class InlineEditEventFilter : public QObject
{
public:
	bool eventFilter( QObject* obj, QEvent* event ) override {
		if( event->type() == QEvent::KeyPress ){
			QKeyEvent* keyEvent = static_cast<QKeyEvent*>( event );
			if( keyEvent->key() == Qt::Key_Escape ){
				EntityInspector_cancelInlineEdit();
				return true;
			}
		}
		else if( event->type() == QEvent::FocusOut ){
			// Finish edit when focus is lost
			EntityInspector_finishInlineEdit();
			return false;
		}
		return QObject::eventFilter( obj, event );
	}
};

static InlineEditEventFilter* g_inlineEditFilter = nullptr;

// Start inline editing on a tree item
void EntityInspector_startInlineEdit( QTreeWidgetItem* item, int column ){
	if( !item || column != 1 ) return; // Only edit value column
	
	// Don't allow editing classname through double-click (use the proper mechanism)
	QString key = item->text( 0 );
	if( key == "classname" ) return;
	
	// Finish any previous edit
	EntityInspector_finishInlineEdit();
	
	g_editingItem = item;
	g_editingColumn = column;
	
	// Position the editor over the item
	QRect rect = g_entprops_store->visualItemRect( item );
	int headerWidth = g_entprops_store->columnWidth( 0 );
	rect.setLeft( headerWidth );
	
	if( !g_inlineEditor ){
		g_inlineEditor = new QLineEdit( g_entprops_store->viewport() );
		QObject::connect( g_inlineEditor, &QLineEdit::returnPressed, [](){ EntityInspector_finishInlineEdit(); } );
		
		if( !g_inlineEditFilter ){
			g_inlineEditFilter = new InlineEditEventFilter;
		}
		g_inlineEditor->installEventFilter( g_inlineEditFilter );
	}
	
	g_inlineEditor->setText( item->text( 1 ) );
	g_inlineEditor->setGeometry( rect );
	g_inlineEditor->show();
	g_inlineEditor->setFocus();
	g_inlineEditor->selectAll();
}

void EntityInspector_updateKeyValues(){
	// Cancel any active inline edit when updating
	EntityInspector_cancelInlineEdit();
	
	g_selectedKeyValues.clear();
	g_selectedDefaultKeyValues.clear();
	Entity_GetKeyValues_Selected( g_selectedKeyValues, g_selectedDefaultKeyValues );

	EntityClass* eclass = GlobalEntityClassManager().findOrInsert( keyvalues_valueforkey( g_selectedKeyValues, "classname" ), false );
	EntityInspector_setEntityClass( eclass );

	EntityInspector_updateSpawnflags();

	g_entprops_store->clear();
	
	// Collect all keys: both current values AND default attributes from entity class
	std::set<CopiedString> allKeys;
	
	// Add current key-values
	for ( const auto&[ key, value ] : g_selectedKeyValues ){
		allKeys.insert( key );
	}
	
	// Add entity class attributes (these define the expected properties)
	if( eclass ){
		for ( const EntityClassAttributePair &pair : eclass->m_attributes ){
			allKeys.insert( pair.first );
		}
	}
	
	// Add default values from entity class
	for ( const auto&[ key, value ] : g_selectedDefaultKeyValues ){
		allKeys.insert( key );
	}
	
	// Build unified table
	for ( const CopiedString& key : allKeys ){
		// Get current value (or empty if not set)
		QString currentValue;
		QString defaultValue;
		bool hasValue = false;
		bool isDefault = false;
		
		// Check for actual value
		auto it = g_selectedKeyValues.find( key );
		if( it != g_selectedKeyValues.end() ){
			currentValue = it->second.c_str();
			hasValue = true;
		}
		
		// Check for default value
		auto dit = g_selectedDefaultKeyValues.find( key );
		if( dit != g_selectedDefaultKeyValues.end() ){
			defaultValue = dit->second.c_str();
			if( !hasValue ){
				currentValue = defaultValue;
				isDefault = true;
			}
		}
		
		auto item = new QTreeWidgetItem( { key.c_str(), currentValue } );
		
		// Style based on state
		if ( string_equal( key.c_str(), "classname" ) || string_equal( key.c_str(), "targetname" ) ) {
			// Important keys: bold
			QFont font = item->font( 0 );
			font.setBold( true );
			item->setFont( 0, font );
			item->setFont( 1, font );
		}
		else if( isDefault || !hasValue ){
			// Default/unset values: gray italic
			item->setForeground( 1, QColor( 128, 128, 128 ) );
			QFont font = item->font( 1 );
			font.setItalic( true );
			item->setFont( 1, font );
		}
		
		// Store tooltip with description if available from entity class
		if( eclass ){
			for ( const EntityClassAttributePair &pair : eclass->m_attributes ){
				if( string_equal( pair.first.c_str(), key.c_str() ) ){
					QString tooltip = QString( "<b>%1</b>" ).arg( pair.second.m_name.c_str() );
					if( !pair.second.m_description.empty() ){
						tooltip += QString( "<br>%1" ).arg( pair.second.m_description.c_str() );
					}
					if( !defaultValue.isEmpty() ){
						tooltip += QString( "<br><i>Default: %1</i>" ).arg( defaultValue );
					}
					item->setToolTip( 0, tooltip );
					item->setToolTip( 1, tooltip );
					break;
				}
			}
		}
		
		g_entprops_store->addTopLevelItem( item );
	}

	for ( EntityAttribute *attr : g_entityAttributes ){
		attr->update();
	}
}

class EntityInspectorDraw
{
	IdleDraw m_idleDraw;
public:
	EntityInspectorDraw() : m_idleDraw( FreeCaller<EntityInspector_updateKeyValues>( ) ){
	}
	void queueDraw(){
		m_idleDraw.queueDraw();
	}
};

EntityInspectorDraw g_EntityInspectorDraw;


void EntityInspector_keyValueChanged(){
	g_EntityInspectorDraw.queueDraw();
}
void EntityInspector_selectionChanged( const Selectable& ){
	EntityInspector_keyValueChanged();
}

void EntityInspector_applyKeyValue(){
	// Get current selection text
	const auto key = g_entityKeyEntry->text().toLatin1();
	const auto value = g_entityValueEntry->text().toLatin1();

	// TTimo: if you change the classname to worldspawn you won't merge back in the structural brushes but create a parasite entity
//	if ( !strcmp( key.c_str(), "classname" ) && !strcmp( value.c_str(), "worldspawn" ) ) {
//		qt_MessageBox( g_entityKeyEntry->window(), "Cannot change \"classname\" key back to worldspawn." );
//		return;
//	}

	// RR2DO2: we don't want spaces and special symbols in entity keys
	if ( std::any_of( key.cbegin(), key.cend(), []( const char c ) { return strchr( " \n\r\t\v\"", c ) != nullptr; } ) ) {
		qt_MessageBox( g_entityKeyEntry->window(), "No spaces, newlines, tabs, quotes are allowed in entity key names." );
		return;
	}
	if ( std::any_of( value.cbegin(), value.cend(), []( const char c ){ return strchr( "\n\r\"", c ) != nullptr; } ) ) {
		qt_MessageBox( g_entityKeyEntry->window(), "No newlines & quotes are allowed in entity key values." );
		return;
	}
	// avoid empty key name; empty value is okay: deletes key
	if( key.isEmpty() )
		return;

	if ( string_equal( key.constData(), "classname" ) ) {
		Scene_EntitySetClassname_Selected( value.constData() );
	}
	else
	{
		Scene_EntitySetKeyValue_Selected_Undoable( key.constData(), value.constData() );
	}
}

void EntityInspector_clearKeyValue(){
	// Get current selection text
	if( const auto item = g_entprops_store->currentItem() ){
		const auto key = item->text( 0 ).toLatin1();

		if ( !string_equal( key.constData(), "classname" ) ) {
			const auto command = StringStream<64>( "entityDeleteKey -key ", key.constData() );
			UndoableCommand undo( command );
			Scene_EntitySetKeyValue_Selected( key.constData(), "" );
		}
	}
}

class : public QObject
{
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ) {
			QKeyEvent *keyEvent = static_cast<QKeyEvent *>( event );
			if( keyEvent->key() == Qt::Key_Delete ){
				EntityInspector_clearKeyValue();
				event->accept();
			}
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
}
g_EntityProperties_keypress;

void EntityInspector_clearAllKeyValues(){
	UndoableCommand undo( "entityClear" );

	// remove all keys except classname and origin
	for ( const auto&[ key, value ] : g_selectedKeyValues )
	{
		if ( !string_equal( key.c_str(), "classname" ) && !string_equal( key.c_str(), "origin" ) ) {
			Scene_EntitySetKeyValue_Selected( key.c_str(), "" );
		}
	}
}

// =============================================================================
// callbacks

static void EntityProperties_selection_changed( QTreeWidgetItem *item, int column ){
	if( item != nullptr ){
		g_entityKeyEntry->setText( item->text( 0 ) );
		g_entityValueEntry->setText( item->text( 1 ) );
	}
}


class : public QObject
{
protected:
	bool eventFilter( QObject *obj, QEvent *event ) override {
		if( event->type() == QEvent::ShortcutOverride ) {
			QKeyEvent *keyEvent = static_cast<QKeyEvent *>( event );
			if( keyEvent->key() == Qt::Key_Return
			 || keyEvent->key() == Qt::Key_Enter
			 || keyEvent->key() == Qt::Key_Tab
			 || keyEvent->key() == Qt::Key_Up
			 || keyEvent->key() == Qt::Key_Down
			 || keyEvent->key() == Qt::Key_PageUp
			 || keyEvent->key() == Qt::Key_PageDown ){
				event->accept();
			}
		}
		// clear focus widget while showing to keep global shortcuts working
		else if( event->type() == QEvent::Show ) {
			QTimer::singleShot( 0, [obj]() {
				if( static_cast<QWidget*>( obj )->focusWidget() != nullptr )
					static_cast<QWidget*>( obj )->focusWidget()->clearFocus();
			} );
		}
		return QObject::eventFilter( obj, event ); // standard event processing
	}
}
g_pressedKeysFilter;


void EntityInspector_destroyWindow(){
	g_entityInspector_windowConstructed = false;
	GlobalEntityAttributes_clear();
	MapEntitiesList_clear();
}

// Helper to create a styled group box
static QGroupBox* createGroupBox( const QString& title ){
	auto group = new QGroupBox( title );
	return group;
}

// Helper to create section header label
static QLabel* createSectionLabel( const QString& text ){
	auto label = new QLabel( text );
	QFont font = label->font();
	font.setBold( true );
	font.setPointSizeF( font.pointSizeF() * 1.1 );
	label->setFont( font );
	label->setContentsMargins( 0, 4, 0, 4 );
	return label;
}

// Select entity in the scene from map entities list
static void MapEntitiesList_selectEntity( QTreeWidgetItem *item ){
	if( !item ) return;
	
	quintptr nodePtr = item->data( 0, Qt::ItemDataRole::UserRole ).value<quintptr>();
	scene::Node* node = reinterpret_cast<scene::Node*>( nodePtr );
	if( !node ) return;
	
	// Find the instance for this node and select it
	class EntitySelector : public scene::Graph::Walker
	{
		scene::Node* m_targetNode;
	public:
		EntitySelector( scene::Node* target ) : m_targetNode( target ) {}
		bool pre( const scene::Path& path, scene::Instance& instance ) const {
			if( &path.top().get() == m_targetNode ){
				Instance_setSelected( instance, true );
				return false;
			}
			return true;
		}
	};
	
	GlobalSelectionSystem().setSelectedAll( false );
	GlobalSceneGraph().traverse( EntitySelector( node ) );
}

QWidget* EntityInspector_constructWindow( QWidget* toplevel ){
	auto mainWidget = new QWidget;
	mainWidget->setStyleSheet( g_inspectorStyle );
	auto mainLayout = new QVBoxLayout( mainWidget );
	mainLayout->setContentsMargins( 4, 4, 4, 4 );
	mainLayout->setSpacing( 4 );

	QObject::connect( mainWidget, &QObject::destroyed, EntityInspector_destroyWindow );
	mainWidget->installEventFilter( &g_pressedKeysFilter );

	// Create main vertical splitter
	auto mainSplitter = new QSplitter( Qt::Vertical );
	mainLayout->addWidget( mainSplitter );

	// ===== TOP SECTION: Map Entities =====
	{
		auto mapEntitiesWidget = new QWidget;
		auto mapEntitiesLayout = new QVBoxLayout( mapEntitiesWidget );
		mapEntitiesLayout->setContentsMargins( 0, 0, 0, 0 );
		mapEntitiesLayout->setSpacing( 4 );

		// Header with refresh button
		auto headerLayout = new QHBoxLayout;
		headerLayout->addWidget( createSectionLabel( "📍 Map Entities" ) );
		headerLayout->addStretch();
		{
			auto refreshBtn = new QToolButton;
			refreshBtn->setText( "⟳" );
			refreshBtn->setToolTip( "Refresh entity list" );
			QObject::connect( refreshBtn, &QAbstractButton::clicked, [](){ MapEntitiesList_fill(); } );
			headerLayout->addWidget( refreshBtn );
		}
		mapEntitiesLayout->addLayout( headerLayout );

		// Filter
		{
			g_mapEntitiesFilter = new QLineEdit;
			g_mapEntitiesFilter->setPlaceholderText( "Filter entities..." );
			g_mapEntitiesFilter->setClearButtonEnabled( true );
			QObject::connect( g_mapEntitiesFilter, &QLineEdit::textChanged, [](){ MapEntitiesList_fill(); } );
			mapEntitiesLayout->addWidget( g_mapEntitiesFilter );
		}

		// Entity list
		{
			auto tree = g_mapEntitiesList = new QTreeWidget;
			tree->setColumnCount( 1 );
			tree->setHeaderHidden( true );
			tree->setRootIsDecorated( false );
			tree->setAlternatingRowColors( true );
			tree->setUniformRowHeights( true );
			tree->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
			
			// Single click selects entity and shows its properties
			QObject::connect( tree, &QTreeWidget::itemClicked, []( QTreeWidgetItem *item, int ){
				MapEntitiesList_selectEntity( item );
			} );
			
			// Double click also focuses the view
			QObject::connect( tree, &QTreeWidget::itemDoubleClicked, []( QTreeWidgetItem *item, int ){
				MapEntitiesList_selectEntity( item );
				if( g_focusToggleButton->isChecked() || true ){
					FocusAllViews();
				}
			} );
			
			mapEntitiesLayout->addWidget( tree, 1 );
		}

		mainSplitter->addWidget( mapEntitiesWidget );
	}

	// ===== BOTTOM SECTION: Entity Properties (merged Properties + Attributes) =====
	{
		auto propsWidget = new QWidget;
		auto propsLayout = new QVBoxLayout( propsWidget );
		propsLayout->setContentsMargins( 0, 0, 0, 0 );
		propsLayout->setSpacing( 4 );

		propsLayout->addWidget( createSectionLabel( "⚙ Entity Properties" ) );

		// Scrollable area for all properties
		auto scroll = new QScrollArea;
		scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarPolicy::ScrollBarAlwaysOff );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::Shape::NoFrame );

		auto scrollContent = new QWidget;
		auto scrollLayout = new QVBoxLayout( scrollContent );
		scrollLayout->setContentsMargins( 0, 0, 4, 0 );
		scrollLayout->setSpacing( 4 );

		// Spawnflags group
		{
			g_spawnflagsGroup = createGroupBox( "⚑ Spawnflags" );
			auto flagsLayout = g_spawnflagsTable = new QGridLayout( g_spawnflagsGroup );
			flagsLayout->setSpacing( 2 );
			flagsLayout->setContentsMargins( 6, 6, 6, 6 );

			for ( int i = 0; i < MAX_FLAGS; i++ )
			{
				auto check = g_entitySpawnflagsCheck[i] = new QCheckBox;
				flagsLayout->addWidget( check, i / 4, i % 4 );
				check->hide();
				QObject::connect( check, &QAbstractButton::clicked, EntityInspector_applySpawnflags );
			}

			g_spawnflagsGroup->setVisible( false );
			scrollLayout->addWidget( g_spawnflagsGroup );
		}

		// Unified Key/Value table (shows all attributes with current values)
		{
			auto propsGroup = createGroupBox( "🔑 Properties" );
			auto propsGroupLayout = new QVBoxLayout( propsGroup );
			propsGroupLayout->setContentsMargins( 6, 6, 6, 6 );
			propsGroupLayout->setSpacing( 4 );
			
			// Help text
			auto helpLabel = new QLabel( "<i>Double-click value to edit. Gray italic = default/unset.</i>" );
			helpLabel->setStyleSheet( "color: gray; font-size: 10px;" );
			propsGroupLayout->addWidget( helpLabel );

			auto tree = g_entprops_store = new QTreeWidget;
			tree->setColumnCount( 2 );
			tree->setUniformRowHeights( true );
			tree->setAlternatingRowColors( true );
			tree->setHeaderLabels( { "Key", "Value" } );
			tree->header()->setVisible( true );
			tree->header()->setSectionResizeMode( 0, QHeaderView::ResizeMode::ResizeToContents );
			tree->header()->setSectionResizeMode( 1, QHeaderView::ResizeMode::Stretch );
			tree->setRootIsDecorated( false );
			tree->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
			tree->setMinimumHeight( 200 );

			// Single click selects and populates edit fields
			QObject::connect( tree, &QTreeWidget::itemPressed, EntityProperties_selection_changed );
			
			// Double click enables inline editing
			QObject::connect( tree, &QTreeWidget::itemDoubleClicked, []( QTreeWidgetItem *item, int column ){
				EntityInspector_startInlineEdit( item, column );
			});
			
			tree->installEventFilter( &g_EntityProperties_keypress );

			propsGroupLayout->addWidget( tree, 1 );
			scrollLayout->addWidget( propsGroup, 1 );
		}

		// Hidden attribute widgets container (for special attribute types like color pickers)
		{
			auto attrWidget = new QWidget;
			attrWidget->hide();  // Hidden - attributes shown in table above
			g_attributeBox = new QGridLayout( attrWidget );
			g_attributeBox->setAlignment( Qt::AlignmentFlag::AlignTop );
			g_attributeBox->setColumnStretch( 0, 1 );
			g_attributeBox->setColumnStretch( 1, 2 );
			g_attributeBox->setSpacing( 4 );
			g_attributeBox->setContentsMargins( 0, 0, 0, 0 );
			scrollLayout->addWidget( attrWidget );
		}

		scrollLayout->addStretch();
		scroll->setWidget( scrollContent );
		propsLayout->addWidget( scroll, 1 );

		// key/value entry at the bottom (outside scroll)
		{
			auto entryGroup = createGroupBox( "✏ Edit / Add Property" );
			auto grid = new QGridLayout( entryGroup );
			grid->setSpacing( 4 );
			grid->setContentsMargins( 6, 6, 6, 6 );
			{
				grid->addWidget( new QLabel( "Key:" ), 0, 0 );
				auto line = g_entityKeyEntry = new LineEdit;
				grid->addWidget( line, 0, 1 );
				QObject::connect( line, &QLineEdit::returnPressed, [](){ g_entityValueEntry->setFocus(); g_entityValueEntry->selectAll(); } );
			}
			{
				grid->addWidget( new QLabel( "Value:" ), 1, 0 );
				auto line = g_entityValueEntry = new LineEdit;
				grid->addWidget( line, 1, 1 );
				QObject::connect( line, &QLineEdit::returnPressed, [](){ EntityInspector_applyKeyValue(); } );
			}
			// Apply button
			{
				auto applyBtn = new QPushButton( "Apply" );
				applyBtn->setToolTip( "Apply key/value change (Enter)" );
				QObject::connect( applyBtn, &QAbstractButton::clicked, [](){ EntityInspector_applyKeyValue(); } );
				grid->addWidget( applyBtn, 0, 2, 2, 1 );
			}
			propsLayout->addWidget( entryGroup );
		}

		// Action buttons
		{
			auto btnLayout = new QHBoxLayout;
			btnLayout->setSpacing( 4 );

			{
				auto b = new QPushButton( "🗑 Clear All" );
				b->setToolTip( "Remove all custom properties" );
				QObject::connect( b, &QAbstractButton::clicked, EntityInspector_clearAllKeyValues );
				btnLayout->addWidget( b );
			}
			{
				auto b = new QPushButton( "✕ Delete" );
				b->setToolTip( "Delete selected property" );
				QObject::connect( b, &QAbstractButton::clicked, EntityInspector_clearKeyValue );
				btnLayout->addWidget( b );
			}

			btnLayout->addStretch();

			// Connection buttons
			{
				auto b = new QToolButton;
				b->setText( "◀" );
				b->setToolTip( "Select targeting entities" );
				QObject::connect( b, &QAbstractButton::clicked, [](){ Select_ConnectedEntities( true, false, g_focusToggleButton->isChecked() ); } );
				btnLayout->addWidget( b );
			}
			{
				auto b = new QToolButton;
				b->setText( "▶" );
				b->setToolTip( "Select targets" );
				QObject::connect( b, &QAbstractButton::clicked, [](){ Select_ConnectedEntities( false, true, g_focusToggleButton->isChecked() ); } );
				btnLayout->addWidget( b );
			}
			{
				auto b = new QToolButton;
				b->setText( "◀▶" );
				b->setToolTip( "Select all connected entities" );
				QObject::connect( b, &QAbstractButton::clicked, [](){ Select_ConnectedEntities( true, true, g_focusToggleButton->isChecked() ); } );
				btnLayout->addWidget( b );
			}
			{
				auto b = g_focusToggleButton = new QToolButton;
				b->setText( "👁" );
				b->setToolTip( "Auto-focus on selection" );
				b->setCheckable( true );
				QObject::connect( b, &QAbstractButton::clicked, []( bool checked ){ if( checked ) FocusAllViews(); } );
				btnLayout->addWidget( b );
			}

			propsLayout->addLayout( btnLayout );
		}

		mainSplitter->addWidget( propsWidget );
	}

	// Hidden entity class list for internal lookups
	g_entityClassList = new QTreeWidget;
	g_entityClassList->hide();

	g_entityInspector_windowConstructed = true;
	EntityClassList_fill();
	
	// Auto-refresh map entities when inspector becomes visible
	// Use event filter to detect show events (works with tab switching)
	class ShowEventFilter : public QObject {
	public:
		bool eventFilter( QObject* obj, QEvent* event ) override {
			if( event->type() == QEvent::Show || event->type() == QEvent::WindowActivate ){
				QTimer::singleShot( 0, [](){ MapEntitiesList_fill(); } );
			}
			return QObject::eventFilter( obj, event );
		}
	};
	static ShowEventFilter* showFilter = new ShowEventFilter;
	mainWidget->installEventFilter( showFilter );
	
	// Initial fill
	QTimer::singleShot( 100, [](){ MapEntitiesList_fill(); } );

	typedef FreeCaller1<const Selectable&, EntityInspector_selectionChanged> EntityInspectorSelectionChangedCaller;
	GlobalSelectionSystem().addSelectionChangeCallback( EntityInspectorSelectionChangedCaller() );
	GlobalEntityCreator().setKeyValueChangedFunc( EntityInspector_keyValueChanged );

	g_guiSettings.addSplitter( mainSplitter, "EntityInspector/mainSplitter", { 200, 400 } );

	return mainWidget;
}

class EntityInspector : public ModuleObserver
{
	std::size_t m_unrealised;
public:
	EntityInspector() : m_unrealised( 1 ){
	}
	void realise(){
		if ( --m_unrealised == 0 ) {
			if ( g_entityInspector_windowConstructed ) {
				//globalOutputStream() << "Entity Inspector: realise\n";
				EntityClassList_fill();
				MapEntitiesList_fill();
			}
		}
	}
	void unrealise(){
		if ( ++m_unrealised == 1 ) {
			if ( g_entityInspector_windowConstructed ) {
				//globalOutputStream() << "Entity Inspector: unrealise\n";
				EntityClassList_clear();
				MapEntitiesList_clear();
			}
		}
	}
};

EntityInspector g_EntityInspector;

#include "preferencesystem.h"
#include "stringio.h"

void EntityInspector_construct(){
	GlobalEntityClassManager().attach( g_EntityInspector );
}

void EntityInspector_destroy(){
	GlobalEntityClassManager().detach( g_EntityInspector );
}
