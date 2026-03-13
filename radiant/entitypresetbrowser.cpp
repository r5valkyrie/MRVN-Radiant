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

#include "entitypresetbrowser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QPushButton>
#include <QRandomGenerator>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "camwindow.h"
#include "commands.h"
#include "entity.h"
#include "eclasslib.h"
#include "grid.h"
#include "gtkutil/glwidget.h"
#include "gtkutil/guisettings.h"
#include "ifilesystem.h"
#include "igl.h"
#include "imap.h"
#include "imodel.h"
#include "ientity.h"
#include "iselection.h"
#include "irender.h"
#include "mainframe.h"
#include "map.h"
#include "qe3.h"
#include "render.h"
#include "renderable.h"
#include "renderer.h"
#include "signal/isignal.h"
#include "stream/stringstream.h"
#include "string/string.h"
#include "stringio.h"
#include "view.h"
#include "instancelib.h"
#include "selectionlib.h"
#include "traverselib.h"

#include "../plugins/entity/model.h"

namespace
{
constexpr int c_PresetIndexRole = Qt::ItemDataRole::UserRole;
constexpr const char* c_PresetMimeType = "application/x-radiant-entity-preset";

class PresetModelRotationOverrides
{
	std::unordered_map<std::string, Vector3> m_overrides;

	static bool hasPrefix( const std::string& str, const char* prefix ){
		const std::size_t prefixLen = std::strlen( prefix );
		return str.size() >= prefixLen && str.compare( 0, prefixLen, prefix ) == 0;
	}

	static std::string normalisePath( const char* path ){
		std::string result;
		if( path != nullptr ){
			result = path;
		}

		std::replace( result.begin(), result.end(), '\\', '/' );
		std::transform( result.begin(), result.end(), result.begin(), []( unsigned char c ){
			return static_cast<char>( std::tolower( c ) );
		} );

		while( hasPrefix( result, "./" ) ){
			result.erase( 0, 2 );
		}
		return result;
	}

	static bool parseQuotedString( const std::string& text, std::size_t& pos, std::string& out ){
		while( pos < text.size() && text[pos] != '"' ){
			++pos;
		}
		if( pos >= text.size() ){
			return false;
		}

		++pos;
		out.clear();
		while( pos < text.size() ){
			const char c = text[pos++];
			if( c == '"' ){
				return true;
			}

			if( c == '\\' && pos < text.size() ){
				const char escaped = text[pos];
				switch( escaped ){
				case '"': out.push_back( '"' ); ++pos; continue;
				case '\\': out.push_back( '\\' ); ++pos; continue;
				case '/': out.push_back( '/' ); ++pos; continue;
				case 'n': out.push_back( '\n' ); ++pos; continue;
				case 'r': out.push_back( '\r' ); ++pos; continue;
				case 't': out.push_back( '\t' ); ++pos; continue;
				default:
					out.push_back( '\\' );
					continue;
				}
			}

			out.push_back( c );
		}

		return false;
	}

	static bool parseRotation( const std::string& value, Vector3& rotation ){
		std::string cleaned = value;
		std::replace( cleaned.begin(), cleaned.end(), ',', ' ' );

		float x = 0.f;
		float y = 0.f;
		float z = 0.f;
		if( std::sscanf( cleaned.c_str(), "%f %f %f", &x, &y, &z ) == 3 ){
			rotation = Vector3( x, y, z );
			return true;
		}

		return false;
	}

	void reload(){
		m_overrides.clear();

		void* buffer = nullptr;
		const int size = vfsLoadFile( "models/rotation_overrides.json", &buffer );
		if( size <= 0 || buffer == nullptr ){
			return;
		}

		const std::string text( static_cast<const char*>( buffer ), static_cast<std::size_t>( size ) );
		vfsFreeFile( buffer );

		std::size_t pos = 0;
		std::string modelPath;
		std::string rotationText;
		while( parseQuotedString( text, pos, modelPath ) && parseQuotedString( text, pos, rotationText ) ){
			Vector3 rotation;
			if( !parseRotation( rotationText, rotation ) ){
				continue;
			}

			const std::string key = normalisePath( modelPath.c_str() );
			if( key.empty() ){
				continue;
			}

			m_overrides[key] = rotation;
		}
	}
public:
	static PresetModelRotationOverrides& instance(){
		static PresetModelRotationOverrides overrides;
		return overrides;
	}

	bool lookup( const char* modelPath, Vector3& rotation ){
		reload();

		const std::string key = normalisePath( modelPath );
		if( key.empty() ){
			return false;
		}

		const auto i = m_overrides.find( key );
		if( i == m_overrides.end() ){
			return false;
		}

		rotation = i->second;
		return true;
	}
};

class EntityPresetBrowser;

class EntityPresetTreeWidget : public QTreeWidget
{
	EntityPresetBrowser& m_browser;
	QPoint m_dragStartPos;
	int m_dragPresetIndex = -1;
public:
	EntityPresetTreeWidget( EntityPresetBrowser& browser ) : m_browser( browser ){
		setDragEnabled( true );
	}
protected:
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;
	void mouseReleaseEvent( QMouseEvent* event ) override;
};

struct EntityPresetKeyValue
{
	CopiedString key;
	CopiedString value;
};

struct EntityPresetEntity
{
	CopiedString name;
	CopiedString description;
	CopiedString placementOrigin;
	std::vector<EntityPresetKeyValue> keyValues;
	std::vector<CopiedString> brushes;

	const char* valueForKey( const char* key ) const {
		for( const EntityPresetKeyValue& keyValue : keyValues ){
			if( string_equal( keyValue.key.c_str(), key ) ){
				return keyValue.value.c_str();
			}
		}
		return "";
	}
};

struct EntityPreset
{
	CopiedString name;
	CopiedString category;
	CopiedString description;
	std::vector<EntityPresetEntity> entities;

	const char* valueForKey( const char* key ) const {
		return entities.empty() ? "" : entities.front().valueForKey( key );
	}
};

CopiedString qstring_to_copied_string( const QString& string ){
	const QByteArray utf8 = string.toUtf8();
	return utf8.constData();
}

QString json_to_string( const QJsonValue& value ){
	if( value.isString() ){
		return value.toString();
	}
	return value.toVariant().toString();
}

QString preset_default_name_for_entity( const EntityPresetEntity& presetEntity ){
	const QString scriptName = QString::fromUtf8( presetEntity.valueForKey( "script_name" ) ).trimmed();
	if( !scriptName.isEmpty() ){
		return scriptName;
	}

	const QString model = QString::fromUtf8( presetEntity.valueForKey( "model" ) ).trimmed();
	if( !model.isEmpty() ){
		const QString baseName = QFileInfo( model ).completeBaseName();
		if( !baseName.isEmpty() ){
			return baseName;
		}
		return model;
	}

	const QString classname = QString::fromUtf8( presetEntity.valueForKey( "classname" ) ).trimmed();
	if( !classname.isEmpty() ){
		return classname;
	}

	return "Preset";
}

void preset_normalize_key_order( EntityPresetEntity& presetEntity ){
	for( std::size_t i = 0; i < presetEntity.keyValues.size(); ++i ){
		if( string_equal( presetEntity.keyValues[i].key.c_str(), "classname" ) ){
			if( i != 0 ){
				std::swap( presetEntity.keyValues[0], presetEntity.keyValues[i] );
			}
			return;
		}
	}
}

bool preset_entity_from_json( const QJsonValue& value, EntityPresetEntity& presetEntity ){
	if( !value.isObject() ){
		return false;
	}

	const QJsonObject object = value.toObject();
	const QJsonObject entityObject = object.contains( "entity" ) && object.value( "entity" ).isObject()
		? object.value( "entity" ).toObject()
		: object;
	if( entityObject.isEmpty() ){
		return false;
	}

	presetEntity.name = qstring_to_copied_string( object.value( "name" ).toString() );
	presetEntity.description = qstring_to_copied_string( object.value( "description" ).toString() );
	presetEntity.placementOrigin = qstring_to_copied_string( object.value( "placement_origin" ).toString() );
	presetEntity.keyValues.clear();
	presetEntity.brushes.clear();

	for( auto i = entityObject.begin(); i != entityObject.end(); ++i ){
		EntityPresetKeyValue keyValue;
		keyValue.key = qstring_to_copied_string( i.key() );
		keyValue.value = qstring_to_copied_string( json_to_string( i.value() ) );
		presetEntity.keyValues.push_back( std::move( keyValue ) );
	}

	if( object.contains( "brushes" ) && object.value( "brushes" ).isArray() ){
		for( const QJsonValue& brushValue : object.value( "brushes" ).toArray() ){
			if( brushValue.isString() ){
				presetEntity.brushes.push_back( qstring_to_copied_string( brushValue.toString() ) );
			}
		}
	}

	preset_normalize_key_order( presetEntity );

	if( presetEntity.name.empty() ){
		presetEntity.name = qstring_to_copied_string( preset_default_name_for_entity( presetEntity ) );
	}

	return string_not_empty( presetEntity.valueForKey( "classname" ) );
}

bool preset_from_json( const QJsonObject& object, EntityPreset& preset ){
	preset.name = qstring_to_copied_string( object.value( "name" ).toString() );
	preset.category = qstring_to_copied_string( object.value( "category" ).toString() );
	preset.description = qstring_to_copied_string( object.value( "description" ).toString() );
	preset.entities.clear();

	if( object.contains( "entities" ) && object.value( "entities" ).isArray() ){
		const QJsonArray entitiesArray = object.value( "entities" ).toArray();
		for( const QJsonValue& value : entitiesArray ){
			EntityPresetEntity presetEntity;
			if( preset_entity_from_json( value, presetEntity ) ){
				preset.entities.push_back( std::move( presetEntity ) );
			}
		}
	}
	else{
		EntityPresetEntity presetEntity;
		if( preset_entity_from_json( object, presetEntity ) ){
			preset.entities.push_back( std::move( presetEntity ) );
		}
	}

	if( preset.entities.empty() ){
		return false;
	}

	if( preset.name.empty() ){
		preset.name = qstring_to_copied_string( preset_default_name_for_entity( preset.entities.front() ) );
	}
	return true;
}

QJsonObject preset_entity_to_json( const EntityPresetEntity& presetEntity ){
	QJsonObject object;
	if( string_not_empty( presetEntity.name.c_str() ) ){
		object.insert( "name", presetEntity.name.c_str() );
	}
	if( string_not_empty( presetEntity.description.c_str() ) ){
		object.insert( "description", presetEntity.description.c_str() );
	}
	if( string_not_empty( presetEntity.placementOrigin.c_str() ) ){
		object.insert( "placement_origin", presetEntity.placementOrigin.c_str() );
	}

	QJsonObject entityObject;
	for( const EntityPresetKeyValue& keyValue : presetEntity.keyValues ){
		entityObject.insert( keyValue.key.c_str(), keyValue.value.c_str() );
	}
	object.insert( "entity", entityObject );
	if( !presetEntity.brushes.empty() ){
		QJsonArray brushesArray;
		for( const CopiedString& brush : presetEntity.brushes ){
			brushesArray.append( brush.c_str() );
		}
		object.insert( "brushes", brushesArray );
	}
	return object;
}

QJsonObject preset_to_json( const EntityPreset& preset ){
	QJsonObject object;
	object.insert( "name", preset.name.c_str() );
	if( string_not_empty( preset.category.c_str() ) ){
		object.insert( "category", preset.category.c_str() );
	}
	if( string_not_empty( preset.description.c_str() ) ){
		object.insert( "description", preset.description.c_str() );
	}

	if( preset.entities.size() == 1 ){
		object.insert( "entity", preset_entity_to_json( preset.entities.front() ).value( "entity" ).toObject() );
	}
	else{
		QJsonArray entitiesArray;
		for( const EntityPresetEntity& presetEntity : preset.entities ){
			entitiesArray.append( preset_entity_to_json( presetEntity ) );
		}
		object.insert( "entities", entitiesArray );
	}
	return object;
}

Entity* selected_entity(){
	if( GlobalSelectionSystem().countSelected() == 0 ){
		return nullptr;
	}

	const scene::Path& path = GlobalSelectionSystem().ultimateSelected().path();
	Entity* entity = Node_getEntity( path.top() );
	if( entity == nullptr && path.size() > 1 ){
		entity = Node_getEntity( path.parent() );
	}
	return entity;
}

struct SelectedPresetEntity
{
	scene::Node* node = nullptr;
	Entity* entity = nullptr;
	Vector3 placementOrigin = g_vector3_identity;
};

std::vector<SelectedPresetEntity> selected_preset_entities(){
	class SelectedPresetEntitiesVisitor : public SelectionSystem::Visitor
	{
		std::vector<SelectedPresetEntity>& m_selected;
		mutable std::unordered_set<scene::Node*> m_seenNodes;
	public:
		SelectedPresetEntitiesVisitor( std::vector<SelectedPresetEntity>& selected ) : m_selected( selected ){
		}

		void visit( scene::Instance& instance ) const override {
			const scene::Path& path = instance.path();
			Entity* entity = Node_getEntity( path.top() );
			scene::Node* node = &path.top().get();
			scene::Path entityPath( path );
			if( entity == nullptr && path.size() > 1 ){
				entity = Node_getEntity( path.parent() );
				node = &path.parent().get();
				entityPath.pop();
			}

			if( entity == nullptr || node == nullptr || !m_seenNodes.insert( node ).second ){
				return;
			}

			Vector3 placementOrigin = g_vector3_identity;
			if( !string_parse_vector3( entity->getKeyValue( "origin" ), placementOrigin ) ){
				scene::Instance* entityInstance = &instance;
				if( entityPath.top().get_pointer() != path.top().get_pointer() ){
					entityInstance = &findInstance( entityPath );
				}
				placementOrigin = entityInstance->worldAABB().origin;
			}

			m_selected.push_back( { node, entity, placementOrigin } );
		}
	};

	std::vector<SelectedPresetEntity> selected;
	if( GlobalSelectionSystem().countSelected() == 0 ){
		return selected;
	}

	SelectedPresetEntitiesVisitor visitor( selected );
	GlobalSelectionSystem().foreachSelected( visitor );
	return selected;
}

inline MapExporter* Node_getMapExporter( scene::Node& node ){
	return NodeTypeCast<MapExporter>::cast( node );
}

CopiedString export_primitive_tokens( scene::Node& node ){
	MapExporter* exporter = Node_getMapExporter( node );
	if( exporter == nullptr ){
		return "";
	}

	StringOutputStream output( 2048 );
	TokenWriter& writer = GlobalScriptLibrary().m_pfnNewSimpleTokenWriter( output );
	exporter->exportTokens( writer );
	writer.release();
	return output.c_str();
}

void append_entity_brushes( scene::Node& node, std::vector<CopiedString>& brushes ){
	scene::Traversable* traversable = Node_getTraversable( node );
	if( traversable == nullptr ){
		return;
	}

	class CollectBrushesWalker : public scene::Traversable::Walker
	{
		std::vector<CopiedString>& m_brushes;
	public:
		CollectBrushesWalker( std::vector<CopiedString>& brushes ) : m_brushes( brushes ){
		}
		bool pre( scene::Node& node ) const override {
			if( Node_isPrimitive( node ) ){
				const CopiedString tokens = export_primitive_tokens( node );
				if( string_not_empty( tokens.c_str() ) ){
					m_brushes.push_back( tokens );
				}
			}
			return false;
		}
	} walker( brushes );

	traversable->traverse( walker );
}

QString default_name_for_entity( const Entity& entity ){
	const char* scriptName = entity.getKeyValue( "script_name" );
	if( string_not_empty( scriptName ) ){
		return scriptName;
	}

	const QString model = QString::fromUtf8( entity.getKeyValue( "model" ) ).trimmed();
	if( !model.isEmpty() ){
		const QString baseName = QFileInfo( model ).completeBaseName();
		if( !baseName.isEmpty() ){
			return baseName;
		}
	}

	const char* classname = entity.getClassName();
	if( string_not_empty( classname ) ){
		return classname;
	}

	return "Preset";
}

void normalize_preset_preview_angles( Vector3& angles ){
	angles[0] = static_cast<float>( float_mod( angles[0], 360 ) );
	angles[1] = static_cast<float>( float_mod( angles[1], 360 ) );
	angles[2] = static_cast<float>( float_mod( angles[2], 360 ) );
}

Vector3 preset_preview_angles( const EntityPresetEntity& presetEntity ){
	Vector3 angles( 0, 0, 0 );
	const char* anglesValue = presetEntity.valueForKey( "angles" );
	if( string_not_empty( anglesValue ) ){
		Vector3 sourceAngles;
		if( string_parse_vector3( anglesValue, sourceAngles ) ){
			angles = Vector3( sourceAngles[2], sourceAngles[0], sourceAngles[1] );
			normalize_preset_preview_angles( angles );
			return angles;
		}
	}

	const char* angleValue = presetEntity.valueForKey( "angle" );
	if( string_not_empty( angleValue ) ){
		if( string_parse_float( angleValue, angles[2] ) ){
			angles[0] = 0;
			angles[1] = 0;
			normalize_preset_preview_angles( angles );
		}
	}

	return angles;
}

EntityPresetEntity preset_entity_from_entity( const SelectedPresetEntity& selectedEntity, const bool includeOrigin, const Vector3& originOffset ){
	EntityPresetEntity presetEntity;
	const Entity& entity = *selectedEntity.entity;
	class CollectKeyValues : public Entity::Visitor
	{
		std::vector<EntityPresetKeyValue>& m_keyValues;
		bool m_includeOrigin;
		Vector3 m_originOffset;
	public:
		CollectKeyValues( std::vector<EntityPresetKeyValue>& keyValues, const bool includeOrigin, const Vector3& originOffset )
			: m_keyValues( keyValues ), m_includeOrigin( includeOrigin ), m_originOffset( originOffset ){
		}

		void visit( const char* key, const char* value ) override {
			if( string_equal( key, "origin" ) ){
				if( !m_includeOrigin ){
					return;
				}

				Vector3 origin;
				if( !string_parse_vector3( value, origin ) ){
					return;
				}

				EntityPresetKeyValue keyValue;
				keyValue.key = key;
				const Vector3 relativeOrigin = origin - m_originOffset;
				const auto originString = StringStream<96>( relativeOrigin[0], ' ', relativeOrigin[1], ' ', relativeOrigin[2] );
				keyValue.value = originString.c_str();
				m_keyValues.push_back( std::move( keyValue ) );
				return;
			}

			EntityPresetKeyValue keyValue;
			keyValue.key = key;
			keyValue.value = value;
			m_keyValues.push_back( std::move( keyValue ) );
		}
	} collector( presetEntity.keyValues, includeOrigin, originOffset );

	entity.forEachKeyValue( collector );
	presetEntity.name = qstring_to_copied_string( default_name_for_entity( entity ) );
	const Vector3 relativePlacementOrigin = selectedEntity.placementOrigin - originOffset;
	presetEntity.placementOrigin = StringStream<96>( relativePlacementOrigin[0], ' ', relativePlacementOrigin[1], ' ', relativePlacementOrigin[2] ).c_str();
	if( selectedEntity.node != nullptr ){
		append_entity_brushes( *selectedEntity.node, presetEntity.brushes );
	}
	preset_normalize_key_order( presetEntity );
	return presetEntity;
}

EntityPreset preset_from_entity( const Entity& entity, const QString& name, const QString& category, const QString& description ){
	EntityPreset preset;
	preset.name = qstring_to_copied_string( name );
	preset.category = qstring_to_copied_string( category );
	preset.description = qstring_to_copied_string( description );
	SelectedPresetEntity selectedEntity;
	selectedEntity.entity = const_cast<Entity*>( &entity );
	selectedEntity.placementOrigin = g_vector3_identity;
	preset.entities.push_back( preset_entity_from_entity( selectedEntity, false, g_vector3_identity ) );
	return preset;
}

EntityPreset preset_from_selection( const std::vector<SelectedPresetEntity>& selected, const QString& name, const QString& category, const QString& description ){
	EntityPreset preset;
	preset.name = qstring_to_copied_string( name );
	preset.category = qstring_to_copied_string( category );
	preset.description = qstring_to_copied_string( description );

	if( selected.empty() ){
		return preset;
	}

	bool captureAbsolute = false;
	for( const SelectedPresetEntity& selectedEntity : selected ){
		if( selectedEntity.node != nullptr ){
			scene::Traversable* traversable = Node_getTraversable( *selectedEntity.node );
			if( traversable != nullptr && !traversable->empty() ){
				captureAbsolute = true;
				break;
			}
		}
	}

	if( selected.size() == 1 ){
		preset.entities.push_back( preset_entity_from_entity( selected.front(), false, captureAbsolute ? g_vector3_identity : selected.front().placementOrigin ) );
		return preset;
	}

	const Vector3 anchorOrigin = captureAbsolute ? g_vector3_identity : selected.front().placementOrigin;

	for( const SelectedPresetEntity& selectedEntity : selected ){
		if( selectedEntity.entity == nullptr ){
			continue;
		}
		preset.entities.push_back( preset_entity_from_entity( selectedEntity, true, anchorOrigin ) );
	}

	return preset;
}

EntityPreset make_preset( const char* name, const char* category, std::initializer_list<std::pair<const char*, const char*>> keyValues ){
	EntityPresetEntity presetEntity;
	for( const auto& [key, value] : keyValues ){
		EntityPresetKeyValue keyValue;
		keyValue.key = key;
		keyValue.value = value;
		presetEntity.keyValues.push_back( std::move( keyValue ) );
	}
	presetEntity.name = qstring_to_copied_string( preset_default_name_for_entity( presetEntity ) );
	preset_normalize_key_order( presetEntity );

	EntityPreset preset;
	preset.name = name;
	preset.category = category;
	preset.entities.push_back( std::move( presetEntity ) );
	return preset;
}

EntityPresetEntity make_preset_entity(
	const char* name,
	std::initializer_list<std::pair<const char*, const char*>> keyValues ){
	EntityPresetEntity presetEntity;
	presetEntity.name = name;
	for( const auto& [key, value] : keyValues ){
		EntityPresetKeyValue keyValue;
		keyValue.key = key;
		keyValue.value = value;
		presetEntity.keyValues.push_back( std::move( keyValue ) );
	}
	if( presetEntity.name.empty() ){
		presetEntity.name = qstring_to_copied_string( preset_default_name_for_entity( presetEntity ) );
	}
	preset_normalize_key_order( presetEntity );
	return presetEntity;
}

EntityPreset make_multi_preset(
	const char* name,
	const char* category,
	std::initializer_list<EntityPresetEntity> entities ){
	EntityPreset preset;
	preset.name = name;
	preset.category = category;
	preset.entities.assign( entities.begin(), entities.end() );
	return preset;
}

std::vector<EntityPreset> default_presets_for_game(){
	if( !string_equal( basegame_get(), "platform" ) ){
		return {};
	}

	return {
		make_preset(
			"Survival Loot Bin",
			"Loot",
			{
				{ "classname", "prop_dynamic" },
				{ "script_name", "survival_lootbin" },
				{ "model", "mdl/props/loot_bin/loot_bin_01_animated.rmdl" },
				{ "angles", "0 -22.298 0" },
				{ "scale", "1" },
				{ "SuppressAnimSounds", "0" },
				{ "StartDisabled", "0" },
				{ "spawnflags", "0" },
				{ "solid", "6" },
				{ "skin", "0" },
				{ "SetBodyGroup", "0" },
				{ "rendermode", "0" },
				{ "renderfx", "0" },
				{ "rendercolor", "255 255 255" },
				{ "renderamt", "255" },
				{ "RandomAnimation", "0" },
				{ "pressuredelay", "0" },
				{ "PerformanceMode", "0" },
				{ "mingpulevel", "0" },
				{ "mincpulevel", "0" },
				{ "MinAnimTime", "5" },
				{ "maxgpulevel", "0" },
				{ "maxcpulevel", "0" },
				{ "MaxAnimTime", "10" },
				{ "HoldAnimation", "0" },
				{ "gamemode_tdm", "1" },
				{ "gamemode_sur", "1" },
				{ "gamemode_lts", "1" },
				{ "gamemode_lh", "1" },
				{ "gamemode_fd", "1" },
				{ "gamemode_ctf", "1" },
				{ "gamemode_cp", "1" },
				{ "fadedist", "-1" },
				{ "ExplodeRadius", "0" },
				{ "ExplodeDamage", "0" },
				{ "disableX360", "0" },
				{ "disableshadows", "0" },
				{ "disablereceiveshadows", "0" },
				{ "DisableBoneFollowers", "0" },
				{ "DefaultCycle", "0" },
				{ "collide_titan", "1" },
				{ "collide_ai", "1" },
				{ "ClientSide", "0" },
				{ "AnimateInStaticShadow", "0" }
			} ),
		make_preset(
			"Survival Plain Door",
			"Doors",
			{
				{ "classname", "prop_dynamic" },
				{ "script_name", "survival_door_plain" },
				{ "model", "mdl/door/door_256x256x8_elevatorstyle02_animated.rmdl" },
				{ "angles", "0 135 0" },
				{ "scale", "1" },
				{ "SuppressAnimSounds", "0" },
				{ "StartDisabled", "0" },
				{ "spawnflags", "0" },
				{ "solid", "6" },
				{ "skin", "0" },
				{ "SetBodyGroup", "0" },
				{ "rendermode", "0" },
				{ "renderfx", "0" },
				{ "rendercolor", "255 255 255" },
				{ "renderamt", "255" },
				{ "RandomAnimation", "0" },
				{ "pressuredelay", "0" },
				{ "PerformanceMode", "0" },
				{ "mingpulevel", "0" },
				{ "mincpulevel", "0" },
				{ "MinAnimTime", "5" },
				{ "maxgpulevel", "0" },
				{ "maxcpulevel", "0" },
				{ "MaxAnimTime", "10" },
				{ "HoldAnimation", "0" },
				{ "gamemode_tdm", "1" },
				{ "gamemode_sur", "1" },
				{ "gamemode_lts", "1" },
				{ "gamemode_lh", "1" },
				{ "gamemode_fd", "1" },
				{ "gamemode_ctf", "1" },
				{ "gamemode_cp", "1" },
				{ "fadedist", "-1" },
				{ "ExplodeRadius", "0" },
				{ "ExplodeDamage", "0" },
				{ "disableX360", "0" },
				{ "disableshadows", "0" },
				{ "disablereceiveshadows", "0" },
				{ "DisableBoneFollowers", "0" },
				{ "DefaultCycle", "0" },
				{ "collide_titan", "1" },
				{ "collide_ai", "1" },
				{ "ClientSide", "0" },
				{ "AnimateInStaticShadow", "0" }
			} ),
		make_preset(
			"Canyonlands Single Door",
			"Doors",
			{
				{ "classname", "prop_door" },
				{ "model", "mdl/door/canyonlands_door_single_02.rmdl" },
				{ "angles", "0 -45 0" },
				{ "scale", "1" },
				{ "only_spawn_in_freelance", "0" },
				{ "disableshadows", "0" }
			} ),
		make_multi_preset(
			"Jump Tower",
			"Movement",
			{
				make_preset_entity(
					"Balloon",
					{
						{ "classname", "prop_dynamic" },
						{ "SuppressAnimSounds", "0" },
						{ "StartDisabled", "0" },
						{ "spawnflags", "0" },
						{ "solid", "6" },
						{ "skin", "0" },
						{ "SetBodyGroup", "0" },
						{ "rendermode", "0" },
						{ "renderfx", "0" },
						{ "rendercolor", "255 255 255" },
						{ "renderamt", "255" },
						{ "RandomAnimation", "0" },
						{ "pressuredelay", "0" },
						{ "PerformanceMode", "0" },
						{ "mingpulevel", "0" },
						{ "mincpulevel", "0" },
						{ "MinAnimTime", "5" },
						{ "maxgpulevel", "0" },
						{ "maxcpulevel", "0" },
						{ "MaxAnimTime", "10" },
						{ "HoldAnimation", "0" },
						{ "gamemode_tdm", "1" },
						{ "gamemode_sur", "1" },
						{ "gamemode_lts", "1" },
						{ "gamemode_lh", "1" },
						{ "gamemode_fd", "1" },
						{ "gamemode_ctf", "1" },
						{ "gamemode_cp", "1" },
						{ "fadedist", "-1" },
						{ "ExplodeRadius", "0" },
						{ "ExplodeDamage", "0" },
						{ "disableX360", "0" },
						{ "disableshadows", "0" },
						{ "disablereceiveshadows", "0" },
						{ "DisableBoneFollowers", "0" },
						{ "DefaultCycle", "0" },
						{ "collide_titan", "1" },
						{ "collide_ai", "1" },
						{ "ClientSide", "0" },
						{ "AnimateInStaticShadow", "0" },
						{ "scale", "1" },
						{ "angles", "0 0 0" },
						{ "origin", "-1344 -2240 5178.01" },
						{ "script_name", "jump_tower" },
						{ "model", "mdl/props/zipline_balloon/zipline_balloon.rmdl" },
						{ "link_guid", "5676bbe95e3b805d" }
					} ),
				make_preset_entity(
					"Zipline Bottom",
					{
						{ "classname", "zipline" },
						{ "_zipline_rest_point_15", "-1344 -2240 5178.01" },
						{ "_zipline_rest_point_14", "-1344 -2240 4836.400197" },
						{ "_zipline_rest_point_13", "-1344 -2240 4494.814404" },
						{ "_zipline_rest_point_12", "-1344 -2240 4153.291792" },
						{ "_zipline_rest_point_11", "-1344 -2240 3811.811549" },
						{ "_zipline_rest_point_10", "-1344 -2240 3470.384695" },
						{ "_zipline_rest_point_9", "-1344 -2240 3129.005109" },
						{ "_zipline_rest_point_8", "-1344 -2240 2787.676218" },
						{ "_zipline_rest_point_7", "-1344 -2240 2446.396061" },
						{ "_zipline_rest_point_6", "-1344 -2240 2105.165876" },
						{ "_zipline_rest_point_5", "-1344 -2240 1763.984664" },
						{ "_zipline_rest_point_4", "-1344 -2240 1422.853287" },
						{ "_zipline_rest_point_3", "-1344 -2240 1081.771265" },
						{ "_zipline_rest_point_2", "-1344 -2240 740.738829" },
						{ "_zipline_rest_point_1", "-1344 -2240 399.755937" },
						{ "_zipline_rest_point_0", "-1344 -2240 64.01" },
						{ "ZiplineSpeedScale", "1" },
						{ "ZiplinePushOffInDirectionX", "0" },
						{ "ZiplineLengthScale", "1" },
						{ "ZiplineDropToBottom", "1" },
						{ "ZiplineAutoDetachDistance", "350" },
						{ "Width", "2" },
						{ "Material", "cable/zipline.vmt" },
						{ "DetachEndOnUse", "0" },
						{ "DetachEndOnSpawn", "0" },
						{ "scale", "1" },
						{ "angles", "0 0 0" },
						{ "origin", "-1344 -2240 64.01" },
						{ "script_name", "skydive_tower" },
						{ "link_to_guid_0", "8cef16bb" },
						{ "link_guid", "2576c039" },
						{ "ZiplineVertical", "1" },
						{ "ZiplineVersion", "2" },
						{ "ZiplinePreserveVelocity", "1" },
						{ "ZiplineFadeDistance", "20000" }
					} ),
				make_preset_entity(
					"Zipline Top",
					{
						{ "classname", "zipline" },
						{ "ZiplineSpeedScale", "1" },
						{ "ZiplinePushOffInDirectionX", "0" },
						{ "ZiplineLengthScale", "1" },
						{ "ZiplineDropToBottom", "1" },
						{ "Width", "2" },
						{ "Material", "cable/zipline.vmt" },
						{ "DetachEndOnUse", "0" },
						{ "DetachEndOnSpawn", "0" },
						{ "scale", "1" },
						{ "angles", "0 0 0" },
						{ "origin", "-1344 -2240 5178.01" },
						{ "script_name", "skydive_tower" },
						{ "link_guid", "8cef16bb" },
						{ "ZiplineVertical", "1" },
						{ "ZiplinePreserveVelocity", "1" },
						{ "ZiplineFadeDistance", "20000" },
						{ "ZiplineAutoDetachDistance", "100" }
					} ),
				make_preset_entity(
					"Base",
					{
						{ "classname", "prop_dynamic" },
						{ "SuppressAnimSounds", "0" },
						{ "StartDisabled", "0" },
						{ "spawnflags", "0" },
						{ "solid", "6" },
						{ "skin", "0" },
						{ "SetBodyGroup", "0" },
						{ "rendermode", "0" },
						{ "renderfx", "0" },
						{ "rendercolor", "255 255 255" },
						{ "renderamt", "255" },
						{ "RandomAnimation", "0" },
						{ "pressuredelay", "0" },
						{ "PerformanceMode", "0" },
						{ "mingpulevel", "0" },
						{ "mincpulevel", "0" },
						{ "MinAnimTime", "5" },
						{ "maxgpulevel", "0" },
						{ "maxcpulevel", "0" },
						{ "MaxAnimTime", "10" },
						{ "HoldAnimation", "0" },
						{ "gamemode_tdm", "1" },
						{ "gamemode_sur", "1" },
						{ "gamemode_lts", "1" },
						{ "gamemode_lh", "1" },
						{ "gamemode_fd", "1" },
						{ "gamemode_ctf", "1" },
						{ "gamemode_cp", "1" },
						{ "fadedist", "-1" },
						{ "ExplodeRadius", "0" },
						{ "ExplodeDamage", "0" },
						{ "disableX360", "0" },
						{ "disableshadows", "0" },
						{ "disablereceiveshadows", "0" },
						{ "DisableBoneFollowers", "0" },
						{ "DefaultCycle", "0" },
						{ "collide_titan", "1" },
						{ "collide_ai", "1" },
						{ "ClientSide", "0" },
						{ "AnimateInStaticShadow", "0" },
						{ "scale", "1" },
						{ "angles", "0 0 0" },
						{ "origin", "-1344 -2240 0" },
						{ "script_name", "jump_tower" },
						{ "model", "mdl/props/zipline_balloon/zipline_balloon_base.rmdl" },
						{ "link_to_guid_0", "5676bbe95e3b805d" },
						{ "link_guid", "524833eb88d56a88" }
					} )
			} )
	};
}

bool is_translated_vector_key( const char* key ){
	return string_equal( key, "origin" )
		|| string_equal_prefix( key, "_zipline_rest_point_" );
}

CopiedString format_vector3( const Vector3& value ){
	return StringStream<96>( value[0], ' ', value[1], ' ', value[2] );
}

	CopiedString generate_link_guid(){
		return StringStream<32>( QString::number( static_cast<qulonglong>( QRandomGenerator::global()->generate64() ), 16 ).toLatin1().constData() );
	}

	class PresetStringInputStream : public TextInputStream
	{
		const char* m_data;
		std::size_t m_length;
		std::size_t m_offset;
	public:
		explicit PresetStringInputStream( const char* data ) :
			m_data( data != nullptr ? data : "" ),
			m_length( string_length( m_data ) ),
			m_offset( 0 ){
		}

		std::size_t read( char* buffer, std::size_t length ) override {
			if( buffer == nullptr || length == 0 || m_offset >= m_length ){
				return 0;
			}

			const std::size_t remaining = m_length - m_offset;
			const std::size_t count = remaining < length ? remaining : length;
			std::memcpy( buffer, m_data + m_offset, count );
			m_offset += count;
			return count;
		}
	};

class PreviewModelGraph final : public scene::Graph, public scene::Instantiable::Observer
{
	typedef std::map<PathConstReference, scene::Instance*> InstanceMap;

	InstanceMap m_instances;
	scene::Path m_rootpath;
	scene::Instantiable::Observer& m_observer;

public:
	PreviewModelGraph( scene::Instantiable::Observer& observer ) : m_observer( observer ){
	}

	void addSceneChangedCallback( const SignalHandler& ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: addSceneChangedCallback()" );
	}
	void sceneChanged() override {
		ASSERT_MESSAGE( 0, "Reached unreachable: sceneChanged()" );
	}
	scene::Node& root() override {
		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );
		return m_rootpath.top();
	}
	void insert_root( scene::Node& root ) override {
		ASSERT_MESSAGE( m_rootpath.empty(), "scenegraph root already exists" );
		root.IncRef();
		Node_traverseSubgraph( root, InstanceSubgraphWalker( this, scene::Path(), 0 ) );
		m_rootpath.push( makeReference( root ) );
	}
	void erase_root() override {
		ASSERT_MESSAGE( !m_rootpath.empty(), "scenegraph root does not exist" );
		scene::Node& root = m_rootpath.top();
		m_rootpath.pop();
		Node_traverseSubgraph( root, UninstanceSubgraphWalker( this, scene::Path() ) );
		root.DecRef();
	}
	void boundsChanged() override {
		ASSERT_MESSAGE( 0, "Reached unreachable: boundsChanged()" );
	}
	void traverse( const Walker& ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: traverse()" );
	}
	void traverse_subgraph( const Walker&, const scene::Path& ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: traverse_subgraph()" );
	}
	scene::Instance* find( const scene::Path& ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: find()" );
		return nullptr;
	}
	void insert( scene::Instance* instance ) override {
		m_instances.insert( InstanceMap::value_type( PathConstReference( instance->path() ), instance ) );
		m_observer.insert( instance );
	}
	void erase( scene::Instance* instance ) override {
		m_instances.erase( PathConstReference( instance->path() ) );
		m_observer.erase( instance );
	}
	SignalHandlerId addBoundsChangedCallback( const SignalHandler& ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: addBoundsChangedCallback()" );
		return Handle<Opaque<SignalHandler>>( nullptr );
	}
	void removeBoundsChangedCallback( SignalHandlerId ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: removeBoundsChangedCallback()" );
	}
	TypeId getNodeTypeId( const char* ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: getNodeTypeId()" );
		return 0;
	}
	TypeId getInstanceTypeId( const char* ) override {
		ASSERT_MESSAGE( 0, "Reached unreachable: getInstanceTypeId()" );
		return 0;
	}
	void clear(){
		DeleteSubgraph( root() );
	}
};

class PreviewTraversableModelNodeSet : public scene::Traversable
{
	UnsortedNodeSet m_children;
	Observer* m_observer{};

	void copy( const PreviewTraversableModelNodeSet& other ){
		m_children = other.m_children;
	}
	void notifyInsertAll(){
		if( m_observer != nullptr ){
			for( UnsortedNodeSet::iterator i = m_children.begin(); i != m_children.end(); ++i ){
				m_observer->insert( *i );
			}
		}
	}
	void notifyEraseAll(){
		if( m_observer != nullptr ){
			for( UnsortedNodeSet::iterator i = m_children.begin(); i != m_children.end(); ++i ){
				m_observer->erase( *i );
			}
		}
	}
public:
	PreviewTraversableModelNodeSet() = default;
	PreviewTraversableModelNodeSet( const PreviewTraversableModelNodeSet& other ) :
		scene::Traversable( other ){
		copy( other );
		notifyInsertAll();
	}
	~PreviewTraversableModelNodeSet(){
		notifyEraseAll();
	}
	PreviewTraversableModelNodeSet& operator=( const PreviewTraversableModelNodeSet& other ){
		if( m_observer != nullptr ){
			nodeset_diff( m_children, other.m_children, m_observer );
		}
		copy( other );
		return *this;
	}
	void attach( Observer* observer ) {
		ASSERT_MESSAGE( m_observer == nullptr, "observer cannot be attached" );
		m_observer = observer;
		notifyInsertAll();
	}
	void detach( Observer* observer ) {
		ASSERT_MESSAGE( m_observer == observer, "observer cannot be detached" );
		notifyEraseAll();
		m_observer = nullptr;
	}
	void insert( scene::Node& node ) override {
		m_children.insert( NodeSmartReference( node ) );
		if( m_observer != nullptr ){
			m_observer->insert( node );
		}
	}
	void erase( scene::Node& node ) override {
		if( m_observer != nullptr ){
			m_observer->erase( node );
		}
		m_children.erase( NodeSmartReference( node ) );
	}
	void traverse( const Walker& walker ) override {
		UnsortedNodeSet::iterator i = m_children.begin();
		while( i != m_children.end() ){
			Node_traverseSubgraph( *i++, walker );
		}
	}
	bool empty() const override {
		return m_children.empty();
	}
};

class PreviewModelGraphRoot : public scene::Node::Symbiot, public scene::Instantiable, public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<PreviewModelGraphRoot, scene::Instantiable>::install( m_casts );
			NodeContainedCast<PreviewModelGraphRoot, scene::Traversable>::install( m_casts );
			NodeContainedCast<PreviewModelGraphRoot, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	IdentityTransform m_transform;
	PreviewTraversableModelNodeSet m_traverse;
	InstanceSet m_instances;
public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_traverse;
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
	}

	PreviewModelGraphRoot() : m_node( this, this, StaticTypeCasts::instance().get() ){
		m_node.m_isRoot = true;
		m_traverse.attach( this );
	}
	~PreviewModelGraphRoot() = default;
	void release() override {
		m_traverse.detach( this );
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}
	void insert( scene::Node& child ) override {
		m_instances.insert( child );
	}
	void erase( scene::Node& child ) override {
		m_instances.erase( child );
	}
	scene::Node& clone() const {
		return ( new PreviewModelGraphRoot( *this ) )->node();
	}
	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new SelectableInstance( path, parent );
	}
	void forEachInstance( const scene::Instantiable::Visitor& visitor ) override {
		m_instances.forEachInstance( visitor );
	}
	void insert( scene::Instantiable::Observer* observer, const scene::Path& path, scene::Instance* instance ) override {
		m_instances.insert( observer, path, instance );
	}
	scene::Instance* erase( scene::Instantiable::Observer* observer, const scene::Path& path ) override {
		return m_instances.erase( observer, path );
	}
};

class PreviewModelNode :
	public scene::Node::Symbiot,
	public scene::Instantiable,
	public scene::Traversable::Observer
{
	class TypeCasts
	{
		NodeTypeCastTable m_casts;
	public:
		TypeCasts(){
			NodeStaticCast<PreviewModelNode, scene::Instantiable>::install( m_casts );
			NodeContainedCast<PreviewModelNode, scene::Traversable>::install( m_casts );
			NodeContainedCast<PreviewModelNode, TransformNode>::install( m_casts );
		}
		NodeTypeCastTable& get(){
			return m_casts;
		}
	};

	scene::Node m_node;
	InstanceSet m_instances;
	SingletonModel m_model;
	MatrixTransform m_transform;

	void construct(){
		m_model.attach( this );
	}
	void destroy(){
		m_model.detach( this );
	}

public:
	typedef LazyStatic<TypeCasts> StaticTypeCasts;

	scene::Traversable& get( NullType<scene::Traversable> ){
		return m_model.getTraversable();
	}
	TransformNode& get( NullType<TransformNode> ){
		return m_transform;
	}

	PreviewModelNode() :
		m_node( this, this, StaticTypeCasts::instance().get() ){
		construct();
	}
	~PreviewModelNode(){
		destroy();
	}
	void release() override {
		delete this;
	}
	scene::Node& node(){
		return m_node;
	}
	void insert( scene::Node& child ) override {
		m_instances.insert( child );
	}
	void erase( scene::Node& child ) override {
		m_instances.erase( child );
	}
	scene::Instance* create( const scene::Path& path, scene::Instance* parent ) override {
		return new SelectableInstance( path, parent );
	}
	void forEachInstance( const scene::Instantiable::Visitor& visitor ) override {
		m_instances.forEachInstance( visitor );
	}
	void insert( scene::Instantiable::Observer* observer, const scene::Path& path, scene::Instance* instance ) override {
		m_instances.insert( observer, path, instance );
	}
	scene::Instance* erase( scene::Instantiable::Observer* observer, const scene::Path& path ) override {
		return m_instances.erase( observer, path );
	}
	void setModel( const char* modelname ){
		m_model.modelChanged( modelname );
	}
};

class PreviewModelRenderer : public Renderer
{
	struct state_type
	{
		Shader* m_state{};
	};

	std::vector<state_type> m_state_stack;
	RenderStateFlags m_globalstate;
public:
	PreviewModelRenderer( RenderStateFlags globalstate ) : m_globalstate( globalstate ){
		m_state_stack.push_back( state_type() );
	}
	void SetState( Shader* state, EStyle style ) override {
		ASSERT_NOTNULL( state );
		if( style == eFullMaterials ){
			m_state_stack.back().m_state = state;
		}
	}
	EStyle getStyle() const override {
		return eFullMaterials;
	}
	void PushState() override {
		m_state_stack.push_back( m_state_stack.back() );
	}
	void PopState() override {
		m_state_stack.pop_back();
	}
	void Highlight( EHighlightMode, bool = true ) override {
	}
	void addRenderable( const OpenGLRenderable& renderable, const Matrix4& localToWorld ) override {
		m_state_stack.back().m_state->addRenderable( renderable, localToWorld );
	}
	void render( const Matrix4& modelview, const Matrix4& projection ){
		GlobalShaderCache().render( m_globalstate, modelview, projection );
	}
};

class PresetModelPreview;

struct PresetPreviewModel
{
	CopiedString modelPath;
	Vector3 origin;
	Vector3 angles;
	Vector3 modelRotation;
};

class PresetModelPreviewWidget : public QOpenGLWidget
{
	PresetModelPreview& m_preview;
	QPoint m_lastPos;
	bool m_dragging = false;
public:
	PresetModelPreviewWidget( PresetModelPreview& preview ) : m_preview( preview ){
	}
	~PresetModelPreviewWidget() override;
protected:
	void initializeGL() override;
	void resizeGL( int w, int h ) override;
	void paintGL() override;
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;
	void mouseReleaseEvent( QMouseEvent* event ) override;
	void wheelEvent( QWheelEvent* event ) override;
};

class PresetModelPreview : public scene::Instantiable::Observer
{
	PreviewModelGraph m_graph;
	std::vector<PresetPreviewModel> m_models;
	std::vector<scene::Instance*> m_instances;
	PresetModelPreviewWidget* m_widget = nullptr;
	int m_width = 1;
	int m_height = 1;
	float m_yaw = 25.f;
	float m_pitch = -15.f;
	float m_zoom = 1.f;
	bool m_glReady = false;

	void clearScene(){
		m_graph.clear();
		m_instances.clear();
	}
	void populateScene(){
		clearScene();
		if( !m_glReady || m_models.empty() ){
			return;
		}

		for( const PresetPreviewModel& model : m_models ){
			if( string_empty( model.modelPath.c_str() ) ){
				continue;
			}

			auto* modelNode = new PreviewModelNode;
			modelNode->setModel( model.modelPath.c_str() );
			NodeSmartReference modelNodeReference( modelNode->node() );
			Node_getTraversable( m_graph.root() )->insert( modelNodeReference );
		}
		updateTransforms();
	}
public:
	PresetModelPreview() : m_graph( *this ){
		m_graph.insert_root( ( new PreviewModelGraphRoot )->node() );
	}
	~PresetModelPreview(){
		m_graph.erase_root();
	}

	void attachWidget( PresetModelPreviewWidget* widget ){
		m_widget = widget;
	}
	void detachWidget(){
		m_widget = nullptr;
	}
	void onContextCreated(){
		m_glReady = true;
		populateScene();
		queueDraw();
	}
	void onContextDestroyed(){
		if( !m_glReady ){
			return;
		}
		clearScene();
		m_glReady = false;
	}
	void shutdown(){
		clearScene();
		m_models.clear();
		m_widget = nullptr;
		m_glReady = false;
	}
	void insert( scene::Instance* instance ) override {
		if( instance->path().size() == 3 ){
			m_instances.push_back( instance );
			updateTransforms();
		}
	}
	void erase( scene::Instance* instance ) override {
		(void)instance;
		m_instances.clear();
	}
	void resize( int width, int height ){
		m_width = std::max( 1, width );
		m_height = std::max( 1, height );
		updateTransforms();
	}
	void queueDraw() const {
		if( m_widget != nullptr ){
			widget_queue_draw( *m_widget );
		}
	}
	void rotateBy( float deltaYaw, float deltaPitch ){
		m_yaw += deltaYaw;
		m_pitch = std::max( -89.f, std::min( 89.f, m_pitch + deltaPitch ) );
		updateTransforms();
		queueDraw();
	}
	void zoomBy( float factor ){
		m_zoom = std::max( 0.1f, std::min( 8.f, m_zoom * factor ) );
		updateTransforms();
		queueDraw();
	}
	void setModels( const std::vector<PresetPreviewModel>& models ){
		m_models = models;
		m_zoom = 1.f;
		populateScene();
		queueDraw();
	}
	void updateTransforms(){
		if( m_instances.empty() || m_models.empty() ){
			return;
		}

		AABB combinedAabb;
		const std::size_t count = std::min( m_instances.size(), m_models.size() );
		for( std::size_t i = 0; i < count; ++i ){
			Bounded* bounded = Instance_getBounded( *m_instances[i] );
			if( bounded == nullptr ){
				continue;
			}

			AABB aabb = bounded->localAABB();
			aabb.origin += m_models[i].origin;
			aabb_extend_by_aabb_safe( combinedAabb, aabb );
		}
		if( !aabb_valid( combinedAabb ) ){
			return;
		}

		AABB rotatedAabb = combinedAabb;
		const float maxExtent = rotatedAabb.extents[ vector3_max_abs_component_index( rotatedAabb.extents ) ];
		const float scale = ( maxExtent > 0.f ? std::max( 48.f, std::min( m_width, m_height ) * 0.32f ) / maxExtent : 1.f ) * m_zoom;
		rotatedAabb.extents.z() *= 2;

		const Matrix4 autoRotation = matrix4_rotation_for_euler_xyz_degrees(
			vector3_min_abs_component_index( rotatedAabb.extents ) == 0 ? Vector3( 0, 0, -90 )
			: vector3_min_abs_component_index( rotatedAabb.extents ) == 2 ? Vector3( 90, 0, 0 )
			: g_vector3_identity );
		const Matrix4 userRotation = matrix4_rotation_for_euler_xyz_degrees( Vector3( m_pitch, 0, m_yaw ) );
		const Matrix4 globalTransform =
			matrix4_multiplied_by_matrix4(
				matrix4_translation_for_vec3( Vector3( m_width / 2.f, 0, -m_height / 2.f ) ),
				matrix4_multiplied_by_matrix4(
					userRotation,
					matrix4_multiplied_by_matrix4(
						autoRotation,
						matrix4_multiplied_by_matrix4(
							matrix4_scale_for_vec3( Vector3( scale, scale, scale ) ),
							matrix4_translation_for_vec3( -combinedAabb.origin ) ) ) ) );

		for( std::size_t i = 0; i < count; ++i ){
			TransformNode* transformNode = Node_getTransformNode( m_instances[i]->path().parent() );
			if( transformNode == nullptr ){
				continue;
			}

			const_cast<Matrix4&>( transformNode->localToParent() ) =
				matrix4_multiplied_by_matrix4(
					globalTransform,
					matrix4_multiplied_by_matrix4(
						matrix4_translation_for_vec3( m_models[i].origin ),
						matrix4_multiplied_by_matrix4(
							matrix4_rotation_for_euler_xyz_degrees( m_models[i].angles ),
							matrix4_rotation_for_euler_xyz_degrees( m_models[i].modelRotation ) ) ) );
			m_instances[i]->parent()->transformChangedLocal();
			m_instances[i]->transformChangedLocal();
		}
	}
	void render(){
		gl().glViewport( 0, 0, m_width, m_height );
		gl().glDepthMask( GL_TRUE );
		gl().glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		gl().glClearColor( 0.19f, 0.19f, 0.19f, 0.f );
		gl().glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

		if( m_instances.empty() ){
			return;
		}

		const unsigned int globalstate = RENDER_DEPTHTEST
			| RENDER_COLOURWRITE
			| RENDER_DEPTHWRITE
			| RENDER_ALPHATEST
			| RENDER_BLEND
			| RENDER_CULLFACE
			| RENDER_COLOURARRAY
			| RENDER_FOG
			| RENDER_COLOURCHANGE
			| RENDER_FILL
			| RENDER_LIGHTING
			| RENDER_TEXTURE
			| RENDER_SMOOTH
			| RENDER_SCALED;

		Matrix4 projection;
		projection[0] = 1.0f / static_cast<float>( m_width / 2.f );
		projection[5] = 1.0f / static_cast<float>( m_height / 2.f );
		projection[10] = 1.0f / 9999.f;
		projection[12] = 0.0f;
		projection[13] = 0.0f;
		projection[14] = -1.0f;
		projection[1] = projection[2] = projection[3] =
		projection[4] = projection[6] = projection[7] =
		projection[8] = projection[9] = projection[11] = 0.0f;
		projection[15] = 1.0f;

		Matrix4 modelview;
		modelview[12] = -m_width / 2.f;
		modelview[13] = m_height / 2.f;
		modelview[14] = 9999.f;
		modelview[0] = 1;
		modelview[1] = 0;
		modelview[2] = 0;
		modelview[4] = 0;
		modelview[5] = 0;
		modelview[6] = 1;
		modelview[8] = 0;
		modelview[9] = 1;
		modelview[10] = 0;
		modelview[3] = modelview[7] = modelview[11] = 0;
		modelview[15] = 1;

		View view( true );
		view.Construct( projection, modelview, m_width, m_height );

		gl().glMatrixMode( GL_PROJECTION );
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &projection ) );
		gl().glMatrixMode( GL_MODELVIEW );
		gl().glLoadMatrixf( reinterpret_cast<const float*>( &modelview ) );

		GLfloat inverse_cam_dir[4], ambient[4], diffuse[4];
		ambient[0] = ambient[1] = ambient[2] = 0.45f;
		ambient[3] = 1.0f;
		diffuse[0] = diffuse[1] = diffuse[2] = 0.45f;
		diffuse[3] = 1.0f;
		inverse_cam_dir[0] = -view.getViewDir()[0];
		inverse_cam_dir[1] = -view.getViewDir()[1];
		inverse_cam_dir[2] = -view.getViewDir()[2];
		inverse_cam_dir[3] = 0;
		gl().glLightfv( GL_LIGHT0, GL_POSITION, inverse_cam_dir );
		gl().glLightfv( GL_LIGHT0, GL_AMBIENT, ambient );
		gl().glLightfv( GL_LIGHT0, GL_DIFFUSE, diffuse );
		gl().glEnable( GL_LIGHT0 );

		PreviewModelRenderer renderer( globalstate );
		for( scene::Instance* instance : m_instances ){
			if( Renderable* renderable = Instance_getRenderable( *instance ) ){
				renderable->renderSolid( renderer, view );
			}
		}
		renderer.render( modelview, projection );
		gl().glBindTexture( GL_TEXTURE_2D, 0 );
	}
};

PresetModelPreviewWidget::~PresetModelPreviewWidget(){
	m_preview.detachWidget();
	m_preview.onContextDestroyed();
	glwidget_context_destroyed();
}

void PresetModelPreviewWidget::initializeGL(){
	glwidget_context_created( *this );
	m_preview.onContextCreated();
}

void PresetModelPreviewWidget::resizeGL( int w, int h ){
	m_preview.resize( w, h );
}

void PresetModelPreviewWidget::paintGL(){
	m_preview.render();
}

void PresetModelPreviewWidget::mousePressEvent( QMouseEvent* event ){
	if( event->button() == Qt::MouseButton::LeftButton ){
		m_dragging = true;
		m_lastPos = event->pos();
	}
}

void PresetModelPreviewWidget::mouseMoveEvent( QMouseEvent* event ){
	if( m_dragging ){
		const QPoint delta = event->pos() - m_lastPos;
		m_lastPos = event->pos();
		m_preview.rotateBy( delta.x(), delta.y() );
	}
}

void PresetModelPreviewWidget::mouseReleaseEvent( QMouseEvent* event ){
	if( event->button() == Qt::MouseButton::LeftButton ){
		m_dragging = false;
	}
}

void PresetModelPreviewWidget::wheelEvent( QWheelEvent* event ){
	const int delta = event->angleDelta().y();
	if( delta != 0 ){
		m_preview.zoomBy( delta > 0 ? 1.1f : 1.f / 1.1f );
	}
	event->accept();
}

class EntityPresetBrowser
{
	friend class EntityPresetTreeWidget;
	QWidget* m_parent{};
	QTreeWidget* m_tree{};
	QTableWidget* m_table{};
	QLineEdit* m_filterEntry{};
	std::unique_ptr<PresetModelPreview> m_modelPreview;
	PresetModelPreviewWidget* m_previewWidget{};
	QLabel* m_summaryLabel{};
	QLabel* m_statusLabel{};
	QPushButton* m_useButton{};
	QPushButton* m_updateButton{};
	QPushButton* m_deleteButton{};
	CopiedString m_presetPath;
	std::vector<EntityPreset> m_presets;
	bool m_mapValidCallbackRegistered = false;

	QString presetPathQ() const {
		return QString::fromUtf8( m_presetPath.c_str() );
	}
	void clearPreview(){
		if( m_modelPreview != nullptr ){
			m_modelPreview->setModels( {} );
		}
	}
	void onMapValidChanged(){
		if( !Map_Valid( g_map ) ){
			clearPreview();
		}
	}
	bool presetIndexFromMimeData( const QMimeData* mimeData, int& presetIndex ) const {
		if( mimeData == nullptr || !mimeData->hasFormat( c_PresetMimeType ) ){
			return false;
		}

		bool ok = false;
		const int index = QString::fromUtf8( mimeData->data( c_PresetMimeType ) ).toInt( &ok );
		if( !ok || index < 0 || index >= static_cast<int>( m_presets.size() ) ){
			return false;
		}

		presetIndex = index;
		return true;
	}

	QString currentFilter() const {
		return m_filterEntry != nullptr ? m_filterEntry->text().trimmed() : QString();
	}

	int currentPresetIndex() const {
		if( m_tree == nullptr || m_tree->currentItem() == nullptr ){
			return -1;
		}
		const QVariant data = m_tree->currentItem()->data( 0, c_PresetIndexRole );
		return data.isValid() ? data.toInt() : -1;
	}

	EntityPreset* currentPreset(){
		const int index = currentPresetIndex();
		return index >= 0 && index < static_cast<int>( m_presets.size() ) ? &m_presets[index] : nullptr;
	}

	const EntityPreset* currentPreset() const {
		const int index = currentPresetIndex();
		return index >= 0 && index < static_cast<int>( m_presets.size() ) ? &m_presets[index] : nullptr;
	}

	QTreeWidgetItem* findPresetItem( int presetIndex ) const {
		if( m_tree == nullptr ){
			return nullptr;
		}

		for( QTreeWidgetItemIterator iterator( m_tree ); *iterator != nullptr; ++iterator ){
			QTreeWidgetItem* item = *iterator;
			const QVariant data = item->data( 0, c_PresetIndexRole );
			if( data.isValid() && data.toInt() == presetIndex ){
				return item;
			}
		}
		return nullptr;
	}

	QTreeWidgetItem* firstPresetItem() const {
		if( m_tree == nullptr ){
			return nullptr;
		}

		for( QTreeWidgetItemIterator iterator( m_tree ); *iterator != nullptr; ++iterator ){
			QTreeWidgetItem* item = *iterator;
			if( item->data( 0, c_PresetIndexRole ).isValid() ){
				return item;
			}
		}
		return nullptr;
	}

	void refreshPresetPath(){
		const QString enginePath = QString::fromUtf8( EnginePath_get() ).trimmed();
		if( enginePath.isEmpty() ){
			m_presetPath = "";
			return;
		}

		const QString presetPath = QDir::cleanPath(
			QDir( enginePath ).filePath( QString( "%1/entities/entity_presets.json" ).arg( basegame_get() ) ) );
		m_presetPath = qstring_to_copied_string( QDir::toNativeSeparators( presetPath ) );
	}

	bool matchesFilter( const EntityPreset& preset, const QString& filter ) const {
		if( filter.isEmpty() ){
			return true;
		}

		const Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
		QStringList candidates;
		candidates << QString::fromUtf8( preset.name.c_str() )
			<< QString::fromUtf8( preset.category.c_str() )
			<< QString::fromUtf8( preset.description.c_str() );

		for( const EntityPresetEntity& presetEntity : preset.entities ){
			candidates << QString::fromUtf8( presetEntity.name.c_str() )
				<< QString::fromUtf8( presetEntity.description.c_str() );
			for( const EntityPresetKeyValue& keyValue : presetEntity.keyValues ){
				candidates << QString::fromUtf8( keyValue.key.c_str() )
					<< QString::fromUtf8( keyValue.value.c_str() );
			}
		}

		for( const QString& candidate : candidates ){
			if( candidate.contains( filter, caseSensitivity ) ){
				return true;
			}
		}
		return false;
	}

	void updateStatus( const QString& extra = QString() ){
		if( m_statusLabel == nullptr ){
			return;
		}

		QString status = QString( "%1 presets | %2" )
			.arg( static_cast<int>( m_presets.size() ) )
			.arg( presetPathQ() );
		if( !extra.isEmpty() ){
			status += QString( " | %1" ).arg( extra );
		}
		m_statusLabel->setText( status );
	}

	void rebuildTree( int selectPresetIndex = -1 ){
		if( m_tree == nullptr ){
			return;
		}

		if( selectPresetIndex < 0 ){
			selectPresetIndex = currentPresetIndex();
		}

		const QString filter = currentFilter();
		const QSignalBlocker blocker( m_tree );
		m_tree->clear();

		QMap<QString, QTreeWidgetItem*> categoryItems;
		for( std::size_t i = 0; i < m_presets.size(); ++i ){
			const EntityPreset& preset = m_presets[i];
			if( !matchesFilter( preset, filter ) ){
				continue;
			}

			QTreeWidgetItem* parent = nullptr;
			QString categoryPath;
			QString normalizedCategory = QString::fromUtf8( preset.category.c_str() );
			normalizedCategory.replace( '\\', '/' );
			const QStringList categories = normalizedCategory.split( '/', Qt::SkipEmptyParts );
			for( const QString& category : categories ){
				categoryPath += '/' + category;
				QTreeWidgetItem* item = categoryItems.value( categoryPath, nullptr );
				if( item == nullptr ){
					item = new QTreeWidgetItem( QStringList( category ) );
					if( parent != nullptr ){
						parent->addChild( item );
					}
					else{
						m_tree->addTopLevelItem( item );
					}
					categoryItems.insert( categoryPath, item );
				}
				parent = item;
			}

			QTreeWidgetItem* presetItem = new QTreeWidgetItem( QStringList( preset.name.c_str() ) );
			presetItem->setData( 0, c_PresetIndexRole, static_cast<int>( i ) );
			presetItem->setToolTip( 0, QString::fromUtf8( preset.valueForKey( "classname" ) ) );
			if( parent != nullptr ){
				parent->addChild( presetItem );
			}
			else{
				m_tree->addTopLevelItem( presetItem );
			}
		}

		m_tree->expandAll();

		QTreeWidgetItem* currentItem = findPresetItem( selectPresetIndex );
		if( currentItem == nullptr ){
			currentItem = firstPresetItem();
		}
		if( currentItem != nullptr ){
			m_tree->setCurrentItem( currentItem );
		}

		updateDetails();
	}

	void updateDetails(){
		if( m_table == nullptr || m_summaryLabel == nullptr ){
			return;
		}

		const EntityPreset* preset = currentPreset();
		if( preset == nullptr ){
			clearPreview();
			m_summaryLabel->setText( "Select a preset to inspect or spawn it." );
			m_table->clearContents();
			m_table->setRowCount( 0 );
			if( m_useButton != nullptr ) m_useButton->setEnabled( false );
			if( m_updateButton != nullptr ) m_updateButton->setEnabled( false );
			if( m_deleteButton != nullptr ) m_deleteButton->setEnabled( false );
			return;
		}

		if( m_modelPreview != nullptr ){
			Vector3 anchorOrigin( 0, 0, 0 );
			bool anchorOriginFound = false;
			for( const EntityPresetEntity& presetEntity : preset->entities ){
				if( string_parse_vector3( presetEntity.valueForKey( "origin" ), anchorOrigin ) ){
					anchorOriginFound = true;
					break;
				}
			}
			if( !anchorOriginFound ){
				anchorOrigin = g_vector3_identity;
			}

			std::vector<PresetPreviewModel> previewModels;
			for( const EntityPresetEntity& presetEntity : preset->entities ){
				const char* modelPath = presetEntity.valueForKey( "model" );
				if( string_empty( modelPath ) ){
					continue;
				}

				Vector3 origin = anchorOrigin;
				if( !string_parse_vector3( presetEntity.valueForKey( "origin" ), origin ) ){
					origin = anchorOrigin;
				}

				PresetPreviewModel previewModel;
				previewModel.modelPath = modelPath;
				previewModel.origin = vector3_subtracted( origin, anchorOrigin );
				previewModel.angles = preset_preview_angles( presetEntity );
				previewModel.modelRotation = g_vector3_identity;
				PresetModelRotationOverrides::instance().lookup( modelPath, previewModel.modelRotation );
				previewModels.push_back( std::move( previewModel ) );
			}

			m_modelPreview->setModels( previewModels );
		}

		QStringList lines;
		lines << QString::fromUtf8( preset->name.c_str() );
		lines << QString( "Entities: %1" ).arg( static_cast<int>( preset->entities.size() ) );
		lines << QString( "Primary Class: %1" ).arg( preset->valueForKey( "classname" ) );

		const char* scriptName = preset->valueForKey( "script_name" );
		if( string_not_empty( scriptName ) ){
			lines << QString( "Script: %1" ).arg( scriptName );
		}

		const char* model = preset->valueForKey( "model" );
		if( string_not_empty( model ) ){
			lines << QString( "Model: %1" ).arg( model );
		}

		if( string_not_empty( preset->description.c_str() ) ){
			lines << QString::fromUtf8( preset->description.c_str() );
		}

		m_summaryLabel->setText( lines.join( '\n' ) );

		int rowCount = 0;
		for( const EntityPresetEntity& presetEntity : preset->entities ){
			rowCount += static_cast<int>( presetEntity.keyValues.size() );
		}

		m_table->clearContents();
		m_table->setRowCount( rowCount );
		int row = 0;
		for( std::size_t entityIndex = 0; entityIndex < preset->entities.size(); ++entityIndex ){
			const EntityPresetEntity& presetEntity = preset->entities[entityIndex];
			const QString entityLabel = string_not_empty( presetEntity.name.c_str() )
				? QString::fromUtf8( presetEntity.name.c_str() )
				: QString( "Entity %1" ).arg( static_cast<int>( entityIndex ) + 1 );

			for( const EntityPresetKeyValue& keyValue : presetEntity.keyValues ){
				m_table->setItem( row, 0, new QTableWidgetItem( QString( "[%1] %2" ).arg( entityLabel, keyValue.key.c_str() ) ) );
				m_table->setItem( row, 1, new QTableWidgetItem( keyValue.value.c_str() ) );
				++row;
			}
		}

		if( m_useButton != nullptr ) m_useButton->setEnabled( true );
		if( m_updateButton != nullptr ) m_updateButton->setEnabled( true );
		if( m_deleteButton != nullptr ) m_deleteButton->setEnabled( true );
	}

	void replaceLinkGuidReferences( CopiedString& value, const QMap<QString, QString>& guidRemap ) const {
		const QString oldGuid = QString::fromUtf8( value.c_str() );
		const QString newGuid = guidRemap.value( oldGuid );
		if( !newGuid.isEmpty() ){
			value = qstring_to_copied_string( newGuid );
		}
	}

	Vector3 preset_entity_anchor_origin( const EntityPresetEntity& presetEntity ) const {
		Vector3 origin;
		if( string_parse_vector3( presetEntity.placementOrigin.c_str(), origin ) ){
			return origin;
		}
		if( string_parse_vector3( presetEntity.valueForKey( "origin" ), origin ) ){
			return origin;
		}
		return g_vector3_identity;
	}

	CopiedString buildPresetImportText( const EntityPreset& preset, const QMap<QString, QString>& guidRemap ) const {
		StringOutputStream output( 8192 );
		TokenWriter& writer = GlobalScriptLibrary().m_pfnNewSimpleTokenWriter( output );

		for( const EntityPresetEntity& presetEntity : preset.entities ){
			writer.writeToken( "{" );
			writer.nextLine();

			for( const EntityPresetKeyValue& sourceKeyValue : presetEntity.keyValues ){
				CopiedString value = sourceKeyValue.value;
				const char* key = sourceKeyValue.key.c_str();
				if( string_equal( key, "link_guid" ) || string_equal_prefix( key, "link_to_guid_" ) ){
					replaceLinkGuidReferences( value, guidRemap );
				}
				writer.writeString( key );
				writer.writeString( value.c_str() );
				writer.nextLine();
			}

			for( const CopiedString& brush : presetEntity.brushes ){
				output << brush.c_str();
				if( brush.c_str()[0] != '\0' && brush.c_str()[string_length( brush.c_str() ) - 1] != '\n' ){
					output << '\n';
				}
			}

			writer.writeToken( "}" );
			writer.nextLine();
		}

		writer.release();
		return output.c_str();
	}

	void translateImportedPresetEntities( const Vector3& translation ) const {
		if( translation == g_vector3_identity ){
			return;
		}

		GlobalSelectionSystem().translateSelected( translation );

		const auto selected = selected_preset_entities();
		for( const SelectedPresetEntity& selectedEntity : selected ){
			if( selectedEntity.entity == nullptr ){
				continue;
			}

			class CollectKeyValues final : public Entity::Visitor
			{
				std::vector<CopiedString>& m_keys;
			public:
				explicit CollectKeyValues( std::vector<CopiedString>& keys ) : m_keys( keys ){
				}
				void visit( const char* key, const char* value ) override {
					(void)value;
					if( !string_equal( key, "origin" ) && is_translated_vector_key( key ) ){
						m_keys.push_back( key );
					}
				}
			};

			std::vector<CopiedString> translatedKeys;
			CollectKeyValues collector( translatedKeys );
			selectedEntity.entity->forEachKeyValue( collector );

			for( const CopiedString& key : translatedKeys ){
				Vector3 value;
				if( string_parse_vector3( selectedEntity.entity->getKeyValue( key.c_str() ), value ) ){
					selectedEntity.entity->setKeyValue( key.c_str(), format_vector3( value + translation ).c_str() );
				}
			}
		}
	}

	bool usePresetAtOrigin( const EntityPreset& preset, const Vector3& insertOrigin ){
		if( preset.entities.empty() ){
			return false;
		}

		const Vector3 anchorOrigin = preset_entity_anchor_origin( preset.entities.front() );

		const Vector3 translationDelta = insertOrigin - anchorOrigin;

		QMap<QString, QString> guidRemap;
		for( const EntityPresetEntity& presetEntity : preset.entities ){
			const char* linkGuid = presetEntity.valueForKey( "link_guid" );
			if( string_not_empty( linkGuid ) ){
				const QString oldGuid = QString::fromUtf8( linkGuid );
				if( !guidRemap.contains( oldGuid ) ){
					guidRemap.insert( oldGuid, QString::fromUtf8( generate_link_guid().c_str() ) );
				}
			}
		}

		const auto command = StringStream<128>( "entityPresetCreate ", preset.name.c_str() );
		UndoableCommand undo( command );

		const bool previousSuppressModelPrompt = Entity_setSuppressModelPrompt( true );
		GlobalSelectionSystem().setSelectedAll( false );
		const CopiedString importText = buildPresetImportText( preset, guidRemap );
		if( string_not_empty( importText.c_str() ) ){
			PresetStringInputStream input( importText.c_str() );
			Map_ImportSelected( input, Map_getFormat( g_map ) );
			translateImportedPresetEntities( translationDelta );
		}
		Entity_setSuppressModelPrompt( previousSuppressModelPrompt );

		if( GlobalSelectionSystem().countSelected() == 0 ){
			QMessageBox::warning( m_parent, "Entity Presets",
				"The preset did not create any entities." );
			return false;
		}

		return true;
	}
public:
	void startPresetDrag( int presetIndex, QWidget* source ){
		if( presetIndex < 0 || presetIndex >= static_cast<int>( m_presets.size() ) ){
			return;
		}

		auto* mimeData = new QMimeData;
		mimeData->setData( c_PresetMimeType, QByteArray::number( presetIndex ) );

		QDrag* drag = new QDrag( source );
		drag->setMimeData( mimeData );
		drag->setHotSpot( QPoint( 12, 12 ) );
		drag->exec( Qt::DropAction::CopyAction );
	}
	bool hasPresetMimeData( const QMimeData* mimeData ) const {
		int presetIndex = -1;
		return presetIndexFromMimeData( mimeData, presetIndex );
	}
	bool dropPresetMimeData( const QMimeData* mimeData, const Vector3& origin ){
		int presetIndex = -1;
		if( !presetIndexFromMimeData( mimeData, presetIndex ) ){
			return false;
		}
		return usePresetAtOrigin( m_presets[presetIndex], origin );
	}

	bool savePresetsToDisk(){
		refreshPresetPath();
		if( m_presetPath.empty() ){
			updateStatus();
			return false;
		}

		const QFileInfo fileInfo( presetPathQ() );
		if( !QDir().mkpath( fileInfo.absolutePath() ) ){
			updateStatus();
			return false;
		}

		QJsonArray presetsArray;
		for( const EntityPreset& preset : m_presets ){
			presetsArray.append( preset_to_json( preset ) );
		}

		QJsonObject root;
		root.insert( "version", 1 );
		root.insert( "presets", presetsArray );

		QFile file( presetPathQ() );
		if( !file.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) ){
			updateStatus();
			return false;
		}

		file.write( QJsonDocument( root ).toJson( QJsonDocument::Indented ) );
		updateStatus( "saved" );
		return true;
	}

	void loadPresetsFromDisk(){
		refreshPresetPath();
		m_presets.clear();
		if( m_presetPath.empty() ){
			updateStatus();
			rebuildTree( -1 );
			return;
		}

		QFile file( presetPathQ() );
		if( !file.exists() ){
			m_presets = default_presets_for_game();
			if( !m_presets.empty() ){
				if( savePresetsToDisk() ){
					updateStatus( "seeded defaults" );
				}
			}
			else{
				updateStatus( "new file" );
			}
			rebuildTree( -1 );
			return;
		}
		if( !file.open( QIODevice::ReadOnly | QIODevice::Text ) ){
			updateStatus( "read failed" );
			rebuildTree( -1 );
			return;
		}

		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &parseError );
		if( parseError.error != QJsonParseError::NoError ){
			updateStatus();
			rebuildTree( -1 );
			return;
		}

		QJsonArray presetsArray;
		if( document.isArray() ){
			presetsArray = document.array();
		}
		else if( document.isObject() ){
			const QJsonObject root = document.object();
			if( root.contains( "presets" ) && root.value( "presets" ).isArray() ){
				presetsArray = root.value( "presets" ).toArray();
			}
			else{
				presetsArray.append( root );
			}
		}

		for( const QJsonValue& value : presetsArray ){
			if( !value.isObject() ){
				continue;
			}

			EntityPreset preset;
			if( preset_from_json( value.toObject(), preset ) ){
				m_presets.push_back( std::move( preset ) );
			}
		}

		updateStatus();
		rebuildTree( -1 );
	}

	void useCurrentPreset(){
		const EntityPreset* preset = currentPreset();
		if( preset == nullptr ){
			return;
		}

		const Vector3 insertOrigin = vector3_snapped(
			Camera_getOrigin( *g_pParentWnd->GetCamWnd() ) - Camera_getViewVector( *g_pParentWnd->GetCamWnd() ) * 128.f,
			GetSnapGridSize() );
		usePresetAtOrigin( *preset, insertOrigin );
	}

	void addSelectedEntityPreset(){
		const auto selected = selected_preset_entities();
		if( selected.empty() ){
			QMessageBox::information( m_parent, "Entity Presets",
				"Select one or more entities first, then use Add Selected to capture them as a preset." );
			return;
		}

		Entity* entity = selected.front().entity;
		const QString defaultName = default_name_for_entity( *entity );
		bool ok = false;
		const QString name = QInputDialog::getText( m_parent, "New Entity Preset",
			"Preset name:", QLineEdit::Normal, defaultName, &ok ).trimmed();
		if( !ok || name.isEmpty() ){
			return;
		}

		const QString defaultCategory = entity->getClassName();
		const QString category = QInputDialog::getText( m_parent, "New Entity Preset",
			"Category:", QLineEdit::Normal, defaultCategory, &ok ).trimmed();
		if( !ok ){
			return;
		}

		int replaceIndex = -1;
		for( std::size_t i = 0; i < m_presets.size(); ++i ){
			if( QString::compare( QString::fromUtf8( m_presets[i].name.c_str() ), name, Qt::CaseInsensitive ) == 0
			 && QString::compare( QString::fromUtf8( m_presets[i].category.c_str() ), category, Qt::CaseInsensitive ) == 0 ){
				replaceIndex = static_cast<int>( i );
				break;
			}
		}

		EntityPreset preset = preset_from_selection( selected, name, category, QString() );
		if( preset.entities.empty() ){
			return;
		}
		if( replaceIndex >= 0 ){
			if( QMessageBox::question( m_parent, "Overwrite Entity Preset",
				QString( "Replace preset \"%1\" in \"%2\"?" ).arg( name, category ) ) != QMessageBox::StandardButton::Yes ){
				return;
			}
			m_presets[replaceIndex] = std::move( preset );
			if( savePresetsToDisk() ){
				rebuildTree( replaceIndex );
			}
			return;
		}

		m_presets.push_back( std::move( preset ) );
		if( savePresetsToDisk() ){
			rebuildTree( static_cast<int>( m_presets.size() - 1 ) );
		}
	}

	void updateCurrentPresetFromSelection(){
		EntityPreset* preset = currentPreset();
		if( preset == nullptr ){
			return;
		}

		const auto selected = selected_preset_entities();
		if( selected.empty() ){
			QMessageBox::information( m_parent, "Entity Presets",
				"Select one or more entities first, then use Update to overwrite the current preset." );
			return;
		}

		const int presetIndex = currentPresetIndex();
		EntityPreset updated = preset_from_selection(
			selected,
			QString::fromUtf8( preset->name.c_str() ),
			QString::fromUtf8( preset->category.c_str() ),
			QString::fromUtf8( preset->description.c_str() ) );
		if( updated.entities.empty() ){
			return;
		}
		*preset = std::move( updated );
		if( savePresetsToDisk() ){
			rebuildTree( presetIndex );
		}
	}

	void deleteCurrentPreset(){
		const int presetIndex = currentPresetIndex();
		if( presetIndex < 0 || presetIndex >= static_cast<int>( m_presets.size() ) ){
			return;
		}

		const EntityPreset& preset = m_presets[presetIndex];
		if( QMessageBox::question( m_parent, "Delete Entity Preset",
			QString( "Delete preset \"%1\"?" ).arg( preset.name.c_str() ) ) != QMessageBox::StandardButton::Yes ){
			return;
		}

		m_presets.erase( m_presets.begin() + presetIndex );
		if( savePresetsToDisk() ){
			rebuildTree( presetIndex < static_cast<int>( m_presets.size() ) ? presetIndex : presetIndex - 1 );
		}
	}

public:
	QWidget* constructWindow( QWidget* toplevel ){
		m_parent = toplevel;
		if( !m_mapValidCallbackRegistered ){
			Map_addValidCallback( g_map, makeSignalHandler( MemberCaller<EntityPresetBrowser, &EntityPresetBrowser::onMapValidChanged>( *this ) ) );
			m_mapValidCallbackRegistered = true;
		}
		if( m_modelPreview == nullptr ){
			m_modelPreview = std::make_unique<PresetModelPreview>();
		}

		auto* splitter = new QSplitter;
		auto* left = new QWidget;
		auto* right = new QWidget;
		splitter->addWidget( left );
		splitter->addWidget( right );

		auto* leftLayout = new QVBoxLayout( left );
		auto* rightLayout = new QVBoxLayout( right );
		leftLayout->setContentsMargins( 0, 0, 0, 0 );
		rightLayout->setContentsMargins( 0, 0, 0, 0 );
		leftLayout->setSpacing( 0 );
		rightLayout->setSpacing( 4 );

		{
			auto* controls = new QWidget;
			auto* controlsLayout = new QVBoxLayout( controls );
			controlsLayout->setContentsMargins( 0, 0, 0, 0 );
			controlsLayout->setSpacing( 4 );
			leftLayout->addWidget( controls );

			auto* buttonRow = new QHBoxLayout;
			buttonRow->setContentsMargins( 0, 0, 0, 0 );
			buttonRow->setSpacing( 4 );
			controlsLayout->addLayout( buttonRow );

			auto* filterRow = new QHBoxLayout;
			filterRow->setContentsMargins( 0, 0, 0, 0 );
			filterRow->setSpacing( 0 );
			controlsLayout->addLayout( filterRow );

			auto* reloadButton = new QPushButton( "Refresh JSON" );
			reloadButton->setFlat( true );
			reloadButton->setToolTip( "Re-read entity_presets.json from disk" );
			buttonRow->addWidget( reloadButton );

			auto* addButton = new QPushButton( "Add Selected" );
			addButton->setFlat( true );
			buttonRow->addWidget( addButton );

			m_updateButton = new QPushButton( "Update" );
			m_updateButton->setFlat( true );
			buttonRow->addWidget( m_updateButton );

			m_deleteButton = new QPushButton( "Delete" );
			m_deleteButton->setFlat( true );
			buttonRow->addWidget( m_deleteButton );

			buttonRow->addStretch( 1 );

			m_filterEntry = new QLineEdit;
			m_filterEntry->setPlaceholderText( "Filter presets..." );
			m_filterEntry->setClearButtonEnabled( true );
			m_filterEntry->setMinimumWidth( 160 );
			filterRow->addWidget( m_filterEntry );

			QObject::connect( m_filterEntry, &QLineEdit::textChanged, [this]( const QString& ){
				rebuildTree();
			} );
			QObject::connect( reloadButton, &QPushButton::clicked, [this](){
				loadPresetsFromDisk();
			} );
			QObject::connect( addButton, &QPushButton::clicked, [this](){
				addSelectedEntityPreset();
			} );
			QObject::connect( m_updateButton, &QPushButton::clicked, [this](){
				updateCurrentPresetFromSelection();
			} );
			QObject::connect( m_deleteButton, &QPushButton::clicked, [this](){
				deleteCurrentPreset();
			} );
		}

		{
			m_tree = new EntityPresetTreeWidget( *this );
			m_tree->setHeaderHidden( true );
			m_tree->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
			m_tree->setUniformRowHeights( true );
			leftLayout->addWidget( m_tree );

			QObject::connect( m_tree, &QTreeWidget::currentItemChanged,
				[this]( QTreeWidgetItem*, QTreeWidgetItem* ){
					updateDetails();
				} );
			QObject::connect( m_tree, &QTreeWidget::itemActivated,
				[this]( QTreeWidgetItem* item, int ){
					if( item != nullptr && item->data( 0, c_PresetIndexRole ).isValid() ){
						useCurrentPreset();
					}
				} );
		}

		{
			m_previewWidget = new PresetModelPreviewWidget( *m_modelPreview );
			m_modelPreview->attachWidget( m_previewWidget );
			m_previewWidget->setMinimumHeight( 220 );
			rightLayout->addWidget( m_previewWidget );

			m_summaryLabel = new QLabel( "Select a preset to inspect or spawn it." );
			m_summaryLabel->setWordWrap( true );
			m_summaryLabel->setTextInteractionFlags( Qt::TextInteractionFlag::TextSelectableByMouse );
			rightLayout->addWidget( m_summaryLabel );

			m_useButton = new QPushButton( "Use Preset" );
			rightLayout->addWidget( m_useButton );
			QObject::connect( m_useButton, &QPushButton::clicked, [this](){
				useCurrentPreset();
			} );

			m_table = new QTableWidget;
			m_table->setColumnCount( 2 );
			m_table->setHorizontalHeaderLabels( { "Key", "Value" } );
			m_table->setEditTriggers( QAbstractItemView::EditTrigger::NoEditTriggers );
			m_table->setSelectionBehavior( QAbstractItemView::SelectionBehavior::SelectRows );
			m_table->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
			m_table->verticalHeader()->setVisible( false );
			m_table->horizontalHeader()->setStretchLastSection( true );
			m_table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeMode::ResizeToContents );
			rightLayout->addWidget( m_table, 1 );

			m_statusLabel = new QLabel;
			m_statusLabel->setTextInteractionFlags( Qt::TextInteractionFlag::TextSelectableByMouse );
			rightLayout->addWidget( m_statusLabel );
		}

		splitter->setStretchFactor( 0, 0 );
		splitter->setStretchFactor( 1, 1 );
		g_guiSettings.addSplitter( splitter, "EntityPresets/splitter", { 220, 580 } );
		auto* refreshShortcut = new QShortcut( QKeySequence( "F5" ), splitter );
		QObject::connect( refreshShortcut, &QShortcut::activated, [this](){
			loadPresetsFromDisk();
		} );

		loadPresetsFromDisk();
		updateDetails();
		return splitter;
	}

	void destroyWindow(){
		if( m_modelPreview != nullptr ){
			m_modelPreview->shutdown();
		}
		m_parent = nullptr;
		m_tree = nullptr;
		m_table = nullptr;
		m_filterEntry = nullptr;
		m_previewWidget = nullptr;
		m_summaryLabel = nullptr;
		m_statusLabel = nullptr;
		m_useButton = nullptr;
		m_updateButton = nullptr;
		m_deleteButton = nullptr;
	}
};

EntityPresetBrowser g_EntityPresetBrowser;

void EntityPresetTreeWidget::mousePressEvent( QMouseEvent* event ){
	m_dragPresetIndex = -1;
	if( event->button() == Qt::MouseButton::LeftButton ){
		if( QTreeWidgetItem* item = itemAt( event->pos() ) ){
			const QVariant data = item->data( 0, c_PresetIndexRole );
			if( data.isValid() ){
				m_dragPresetIndex = data.toInt();
				m_dragStartPos = event->pos();
			}
		}
	}
	QTreeWidget::mousePressEvent( event );
}

void EntityPresetTreeWidget::mouseMoveEvent( QMouseEvent* event ){
	if( ( event->buttons() & Qt::MouseButton::LeftButton ) != 0
	 && m_dragPresetIndex >= 0
	 && ( event->pos() - m_dragStartPos ).manhattanLength() >= QApplication::startDragDistance() ){
		m_browser.startPresetDrag( m_dragPresetIndex, this );
		m_dragPresetIndex = -1;
		return;
	}
	QTreeWidget::mouseMoveEvent( event );
}

void EntityPresetTreeWidget::mouseReleaseEvent( QMouseEvent* event ){
	m_dragPresetIndex = -1;
	QTreeWidget::mouseReleaseEvent( event );
}
}

void EntityPresetBrowser_Construct(){
}

void EntityPresetBrowser_Destroy(){
}

QWidget* EntityPresetBrowser_constructWindow( QWidget* toplevel ){
	return g_EntityPresetBrowser.constructWindow( toplevel );
}

void EntityPresetBrowser_destroyWindow(){
	g_EntityPresetBrowser.destroyWindow();
}

bool EntityPresetBrowser_hasMimeData( const QMimeData* mimeData ){
	return g_EntityPresetBrowser.hasPresetMimeData( mimeData );
}

bool EntityPresetBrowser_dropMimeData( const QMimeData* mimeData, const Vector3& origin ){
	return g_EntityPresetBrowser.dropPresetMimeData( mimeData, origin );
}
