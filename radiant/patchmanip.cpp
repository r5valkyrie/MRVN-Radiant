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

#include "patchmanip.h"

#include "debugging/debugging.h"


#include "iselection.h"
#include "ipatch.h"

#include "math/vector.h"
#include "math/aabb.h"
#include "generic/callback.h"

#include "gtkutil/menu.h"
#include "gtkutil/image.h"
#include "gtkutil/widget.h"
#include "map.h"
#include "mainframe.h"
#include "commands.h"
#include "gtkmisc.h"
#include "gtkdlgs.h"
#include "texwindow.h"
#include "xywindow.h"
#include "select.h"
#include "patch.h"
#include "grid.h"
#include "patchdialog.h"
#include "iundo.h"

PatchCreator* g_patchCreator = 0;

static void Scene_PatchConstructPrefab_Append( scene::Graph& graph, const AABB aabb, const char* shader, EPatchPrefab eType, int axis, std::size_t width, std::size_t height, bool redisperse ){
	NodeSmartReference node( g_patchCreator->createPatch() );
	Node_getTraversable( Map_FindOrInsertWorldspawn( g_map ) )->insert( node );

	Patch* patch = Node_getPatch( node );
	patch->SetShader( shader );

	patch->ConstructPrefab( aabb, eType, axis, width, height );
	if( redisperse ){
		patch->Redisperse( COL );
		patch->Redisperse( ROW );
	}
	if( eType == EPatchPrefab::Plane ){
		patch->NaturalTexture();
	}
	patch->controlPointsChanged();

	{
		scene::Path patchpath( makeReference( GlobalSceneGraph().root() ) );
		patchpath.push( makeReference( *Map_GetWorldspawn( g_map ) ) );
		patchpath.push( makeReference( node.get() ) );
		Instance_getSelectable( *graph.find( patchpath ) )->setSelected( true );
	}
}

void Scene_PatchConstructPrefab( scene::Graph& graph, const AABB aabb, const char* shader, EPatchPrefab eType, int axis, std::size_t width = 3, std::size_t height = 3, bool redisperse = false ){
	Select_Delete();
	GlobalSelectionSystem().setSelectedAll( false );
	Scene_PatchConstructPrefab_Append( graph, aabb, shader, eType, axis, width, height, redisperse );
}


void Patch_makeCaps( Patch& patch, scene::Instance& instance, EPatchCap type, const char* shader ){
	if ( ( type == EPatchCap::EndCap || type == EPatchCap::IEndCap )
	     && patch.getWidth() != 5 ) {
		globalErrorStream() << "cannot create end-cap - patch width != 5\n";
		return;
	}
	if ( ( type == EPatchCap::Bevel || type == EPatchCap::IBevel )
	     && patch.getWidth() != 3 && patch.getWidth() != 5 ) {
		globalErrorStream() << "cannot create bevel-cap - patch width != 3\n";
		return;
	}
	if ( type == EPatchCap::Cylinder
	     && patch.getWidth() != 9 ) {
		globalErrorStream() << "cannot create cylinder-cap - patch width != 9\n";
		return;
	}

	{
		NodeSmartReference cap( g_patchCreator->createPatch() );
		Node_getTraversable( instance.path().parent() )->insert( cap );

		patch.MakeCap( Node_getPatch( cap ), type, ROW, true );
		Node_getPatch( cap )->SetShader( shader );

		scene::Path path( instance.path() );
		path.pop();
		path.push( makeReference( cap.get() ) );
		selectPath( path, true );
	}

	{
		NodeSmartReference cap( g_patchCreator->createPatch() );
		Node_getTraversable( instance.path().parent() )->insert( cap );

		patch.MakeCap( Node_getPatch( cap ), type, ROW, false );
		Node_getPatch( cap )->SetShader( shader );

		scene::Path path( instance.path() );
		path.pop();
		path.push( makeReference( cap.get() ) );
		selectPath( path, true );
	}
}


typedef std::vector<scene::Instance*> InstanceVector;

class PatchStoreInstance
{
	InstanceVector& m_instances;
public:
	PatchStoreInstance( InstanceVector& instances ) : m_instances( instances ){
	}
	void operator()( PatchInstance& patch ) const {
		m_instances.push_back( &patch );
	}
};

void Scene_PatchDoCap_Selected( scene::Graph& graph, const char* shader, EPatchCap type ){
	InstanceVector instances;
	Scene_forEachVisibleSelectedPatchInstance( PatchStoreInstance( instances ) );
	for ( auto i : instances )
	{
		Patch_makeCaps( *Node_getPatch( i->path().top() ), *i, type, shader );
	}
}

void Patch_deform( Patch& patch, scene::Instance& instance, const int deform, const int axis ){
	patch.undoSave();

	for ( PatchControlIter i = patch.begin(); i != patch.end(); ++i ){
		PatchControl& control = *i;
		int randomNumber = int( deform * ( float( std::rand() ) / float( RAND_MAX ) ) );
		control.m_vertex[ axis ] += randomNumber;
	}

	patch.controlPointsChanged();
}

void Scene_PatchDeform( scene::Graph& graph, const int deform, const int axis )
{
	InstanceVector instances;
	Scene_forEachVisibleSelectedPatchInstance( PatchStoreInstance( instances ) );
	for ( auto i : instances )
	{
		Patch_deform( *Node_getPatch( i->path().top() ), *i, deform, axis );
	}

}

void Patch_thicken( Patch& patch, scene::Instance& instance, const float thickness, bool seams, const int axis ){

	const auto aabb_small = []( const AABB& aabb ){
		return ( aabb.extents[0] < 0.01 && aabb.extents[1] < 0.01 ) ||
		       ( aabb.extents[1] < 0.01 && aabb.extents[2] < 0.01 ) ||
		       ( aabb.extents[0] < 0.01 && aabb.extents[2] < 0.01 );
	};

	// Create a new patch node
	NodeSmartReference node( g_patchCreator->createPatch() );
	// Insert the node into original's entity
	Node_getTraversable( instance.path().parent() )->insert( node );

	// Retrieve the contained patch from the node
	Patch* targetPatch = Node_getPatch( node );

	// Create the opposite patch with the given thickness = distance
	bool no12 = true;
	bool no34 = true;
	targetPatch->createThickenedOpposite( patch, thickness, axis, no12, no34 );

	{ // Now select the newly created patch
		scene::Path path( instance.parent()->path() );
		path.push( makeReference( node.get() ) );
		selectPath( path, true );
	}

	if( seams && thickness != 0.0f ){
		int i = no12? 2 : 0;
		int iend = no34? 2 : 4;
		// Now create the four walls
		for ( ; i < iend; ++i ){
			// Allocate new patch
			NodeSmartReference node = NodeSmartReference( g_patchCreator->createPatch() );
			// Insert each node into worldspawn
			Node_getTraversable( instance.path().parent() )->insert( node );

			// Retrieve the contained patch from the node
			Patch* wallPatch = Node_getPatch( node );

			// Create the wall patch by passing i as wallIndex
			wallPatch->createThickenedWall( patch, *targetPatch, i );

			if( aabb_small( wallPatch->localAABB() ) ){
				//globalOutputStream() << "Thicken: Discarding degenerate patch.\n";
				Node_getTraversable( instance.path().parent() )->erase( node );
			}
			else { // Now select the newly created patch
				scene::Path path( instance.parent()->path() );
				path.push( makeReference( node.get() ) );
				selectPath( path, true );
			}
		}
	}

	// Invert the target patch so that it faces the opposite direction
	targetPatch->InvertMatrix();

	if( aabb_small( targetPatch->localAABB() ) ){
		//globalOutputStream() << "Thicken: Discarding degenerate patch.\n";
		Node_getTraversable( instance.path().parent() )->erase( node );
	}
}

void Scene_PatchThicken( scene::Graph& graph, const int thickness, bool seams, const int axis )
{
	InstanceVector instances;
	Scene_forEachVisibleSelectedPatchInstance( PatchStoreInstance( instances ) );
	for ( auto i : instances )
	{
		Patch_thicken( *Node_getPatch( i->path().top() ), *i, thickness, seams, axis );
	}

}

Patch* Scene_GetUltimateSelectedVisiblePatch(){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		scene::Node& node = GlobalSelectionSystem().ultimateSelected().path().top();
		if ( node.visible() ) {
			return Node_getPatch( node );
		}
	}
	return 0;
}


void Scene_PatchCapTexture_Selected( scene::Graph& graph ){
	Scene_forEachVisibleSelectedPatch( []( Patch& patch ){ patch.CapTexture(); } );
	SceneChangeNotify();
}

void Scene_PatchProjectTexture_Selected( scene::Graph& graph, const texdef_t& texdef, const Vector3* direction ){
	Scene_forEachVisibleSelectedPatch( [texdef, direction]( Patch& patch ){ patch.ProjectTexture( texdef, direction ); } );
	SceneChangeNotify();
}

void Scene_PatchProjectTexture_Selected( scene::Graph& graph, const TextureProjection& projection, const Vector3& normal ){
	Scene_forEachVisibleSelectedPatch( [projection, normal]( Patch& patch ){ patch.ProjectTexture( projection, normal ); } );
	SceneChangeNotify();
}

void Scene_PatchFlipTexture_Selected( scene::Graph& graph, int axis ){
	Scene_forEachVisibleSelectedPatch( [axis]( Patch& patch ){ patch.FlipTexture( axis ); } );
}

void Scene_PatchNaturalTexture_Selected( scene::Graph& graph ){
	Scene_forEachVisibleSelectedPatch( []( Patch& patch ){ patch.NaturalTexture(); } );
	SceneChangeNotify();
}

void Scene_PatchTileTexture_Selected( scene::Graph& graph, float s, float t ){
	Scene_forEachVisibleSelectedPatch( [s, t]( Patch& patch ){ patch.SetTextureRepeat( s, t ); } );
	SceneChangeNotify();
}


void Scene_PatchInsertRemove_Selected( scene::Graph& graph, bool bInsert, bool bColumn, bool bFirst ){
	Scene_forEachVisibleSelectedPatch( [bInsert, bColumn, bFirst]( Patch& patch ){ patch.InsertRemove( bInsert, bColumn, bFirst ); } );
}

void Scene_PatchInvert_Selected( scene::Graph& graph ){
	Scene_forEachVisibleSelectedPatch( []( Patch& patch ){ patch.InvertMatrix(); } );
}

void Scene_PatchRedisperse_Selected( scene::Graph& graph, EMatrixMajor major ){
	Scene_forEachVisibleSelectedPatch( [major]( Patch& patch ){ patch.Redisperse( major ); } );
}

void Scene_PatchSmooth_Selected( scene::Graph& graph, EMatrixMajor major ){
	Scene_forEachVisibleSelectedPatch( [major]( Patch& patch ){ patch.Smooth( major ); } );
}

void Scene_PatchTranspose_Selected( scene::Graph& graph ){
	Scene_forEachVisibleSelectedPatch( []( Patch& patch ){ patch.TransposeMatrix(); } );
}

void Scene_PatchSetShader_Selected( scene::Graph& graph, const char* name ){
	Scene_forEachVisibleSelectedPatch( [name]( Patch& patch ){ patch.SetShader( name ); } );
	SceneChangeNotify();
}

void Scene_PatchGetShader_Selected( scene::Graph& graph, CopiedString& name ){
	if ( Patch* patch = Scene_GetUltimateSelectedVisiblePatch() )
		name = patch->GetShader();
}

class PatchSelectByShader
{
	const char* m_name;
public:
	inline PatchSelectByShader( const char* name )
		: m_name( name ){
	}
	void operator()( PatchInstance& patch ) const {
		if ( shader_equal( patch.getPatch().GetShader(), m_name ) ) {
			patch.setSelected( true );
		}
	}
};

void Scene_PatchSelectByShader( scene::Graph& graph, const char* name ){
	Scene_forEachVisiblePatchInstance( PatchSelectByShader( name ) );
}


class PatchFindReplaceShader
{
	const char* m_find;
	const char* m_replace;
public:
	PatchFindReplaceShader( const char* find, const char* replace ) : m_find( find ), m_replace( replace ){
	}
	void operator()( Patch& patch ) const {
		if ( shader_equal( patch.GetShader(), m_find ) ) {
			patch.SetShader( m_replace );
		}
	}
};

void Scene_PatchFindReplaceShader( scene::Graph& graph, const char* find, const char* replace ){
	if( !replace ){
		Scene_forEachVisiblePatchInstance( PatchSelectByShader( find ) );
	}
	else{
		Scene_forEachVisiblePatch( PatchFindReplaceShader( find, replace ) );
	}
}

void Scene_PatchFindReplaceShader_Selected( scene::Graph& graph, const char* find, const char* replace ){
	if( !replace ){
		//do nothing, because alternative is replacing to notex
		//perhaps deselect ones with not matching shaders here?
	}
	else{
		Scene_forEachVisibleSelectedPatch( PatchFindReplaceShader( find, replace ) );
	}
}


AABB PatchCreator_getBounds(){
	AABB aabb( aabb_for_minmax( Select_getWorkZone().d_work_min, Select_getWorkZone().d_work_max ) );

	float gridSize = GetGridSize();

	if ( aabb.extents[0] == 0 ) {
		aabb.extents[0] = gridSize;
	}
	if ( aabb.extents[1] == 0 ) {
		aabb.extents[1] = gridSize;
	}
	if ( aabb.extents[2] == 0 ) {
		aabb.extents[2] = gridSize;
	}

	if ( aabb_valid( aabb ) ) {
		return aabb;
	}
	return AABB( Vector3( 0, 0, 0 ), Vector3( 64, 64, 64 ) );
}

void DoNewPatchDlg( EPatchPrefab prefab, int minrows, int mincols, int defrows, int defcols, int maxrows, int maxcols );

void Patch_XactCylinder(){
	UndoableCommand undo( "patchCreateXactCylinder" );

	DoNewPatchDlg( EPatchPrefab::ExactCylinder, 3, 7, 3, 13, 0, 0 );
}

void Patch_XactSphere(){
	UndoableCommand undo( "patchCreateXactSphere" );

	DoNewPatchDlg( EPatchPrefab::ExactSphere, 5, 7, 7, 13, 0, 0 );
}

void Patch_XactCone(){
	UndoableCommand undo( "patchCreateXactCone" );

	DoNewPatchDlg( EPatchPrefab::ExactCone, 3, 7, 3, 13, 0, 0 );
}

void Patch_Cylinder(){
	UndoableCommand undo( "patchCreateCylinder" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::Cylinder, GlobalXYWnd_getCurrentViewType() );
}

void Patch_DenseCylinder(){
	UndoableCommand undo( "patchCreateDenseCylinder" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::DenseCylinder, GlobalXYWnd_getCurrentViewType() );
}

void Patch_VeryDenseCylinder(){
	UndoableCommand undo( "patchCreateVeryDenseCylinder" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::VeryDenseCylinder, GlobalXYWnd_getCurrentViewType() );
}

void Patch_SquareCylinder(){
	UndoableCommand undo( "patchCreateSquareCylinder" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::SqCylinder, GlobalXYWnd_getCurrentViewType() );
}

void Patch_Endcap(){
	UndoableCommand undo( "patchCreateEndCap" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::EndCap, GlobalXYWnd_getCurrentViewType() );
}

void Patch_Bevel(){
	UndoableCommand undo( "patchCreateBevel" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::Bevel, GlobalXYWnd_getCurrentViewType() );
}

void Patch_Sphere(){
	UndoableCommand undo( "patchCreateSphere" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::Sphere, GlobalXYWnd_getCurrentViewType() );
}

void Patch_SquareBevel(){
}

void Patch_SquareEndcap(){
}

void Patch_Cone(){
	UndoableCommand undo( "patchCreateCone" );

	Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), EPatchPrefab::Cone, GlobalXYWnd_getCurrentViewType() );
}

void Patch_Plane(){
	UndoableCommand undo( "patchCreatePlane" );

	DoNewPatchDlg( EPatchPrefab::Plane, 3, 3, 3, 3, 0, 0 );
}

void Patch_InsertFirstColumn(){
	UndoableCommand undo( "patchInsertFirstColumns" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), true, true, true );
}

void Patch_InsertLastColumn(){
	UndoableCommand undo( "patchInsertLastColumns" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), true, true, false );
}

void Patch_InsertFirstRow(){
	UndoableCommand undo( "patchInsertFirstRows" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), true, false, true );
}

void Patch_InsertLastRow(){
	UndoableCommand undo( "patchInsertLastRows" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), true, false, false );
}

void Patch_DeleteFirstColumn(){
	UndoableCommand undo( "patchDeleteFirstColumns" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), false, true, true );
}

void Patch_DeleteLastColumn(){
	UndoableCommand undo( "patchDeleteLastColumns" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), false, true, false );
}

void Patch_DeleteFirstRow(){
	UndoableCommand undo( "patchDeleteFirstRows" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), false, false, true );
}

void Patch_DeleteLastRow(){
	UndoableCommand undo( "patchDeleteLastRows" );

	Scene_PatchInsertRemove_Selected( GlobalSceneGraph(), false, false, false );
}

void Patch_Invert(){
	UndoableCommand undo( "patchInvert" );

	Scene_PatchInvert_Selected( GlobalSceneGraph() );
}

void Patch_RedisperseRows(){
	UndoableCommand undo( "patchRedisperseRows" );

	Scene_PatchRedisperse_Selected( GlobalSceneGraph(), ROW );
}

void Patch_RedisperseCols(){
	UndoableCommand undo( "patchRedisperseColumns" );

	Scene_PatchRedisperse_Selected( GlobalSceneGraph(), COL );
}

void Patch_SmoothRows(){
	UndoableCommand undo( "patchSmoothRows" );

	Scene_PatchSmooth_Selected( GlobalSceneGraph(), ROW );
}

void Patch_SmoothCols(){
	UndoableCommand undo( "patchSmoothColumns" );

	Scene_PatchSmooth_Selected( GlobalSceneGraph(), COL );
}

void Patch_Transpose(){
	UndoableCommand undo( "patchTranspose" );

	Scene_PatchTranspose_Selected( GlobalSceneGraph() );
}

void DoCapDlg();

void Patch_Cap(){
	// FIXME: add support for patch cap creation
	// Patch_CapCurrent();
	UndoableCommand undo( "patchPutCaps" );

	DoCapDlg();
}

///\todo Unfinished.
void Patch_OverlayOn(){
}

///\todo Unfinished.
void Patch_OverlayOff(){
}

void Patch_FlipTextureX(){
	UndoableCommand undo( "patchFlipTextureU" );

	Scene_PatchFlipTexture_Selected( GlobalSceneGraph(), 0 );
}

void Patch_FlipTextureY(){
	UndoableCommand undo( "patchFlipTextureV" );

	Scene_PatchFlipTexture_Selected( GlobalSceneGraph(), 1 );
}

void Patch_NaturalTexture(){
	UndoableCommand undo( "patchNaturalTexture" );

	Scene_PatchNaturalTexture_Selected( GlobalSceneGraph() );
}

void Patch_CapTexture(){
	UndoableCommand command( "patchCapTexture" );
	Scene_PatchCapTexture_Selected( GlobalSceneGraph() );
}

void Patch_FitTexture(){
}

void DoPatchDeformDlg();

void Patch_Deform(){
	UndoableCommand undo( "patchDeform" );

	DoPatchDeformDlg();
}

void DoPatchThickenDlg();

void Patch_Thicken(){
	UndoableCommand undo( "patchThicken" );

	DoPatchThickenDlg();
}



#include "ifilter.h"


class filter_patch_all : public PatchFilter
{
public:
	bool filter( const Patch& patch ) const {
		return true;
	}
};

class filter_patch_shader : public PatchFilter
{
	const char* m_shader;
public:
	filter_patch_shader( const char* shader ) : m_shader( shader ){
	}
	bool filter( const Patch& patch ) const {
		return shader_equal( patch.GetShader(), m_shader );
	}
};

class filter_patch_flags : public PatchFilter
{
	int m_flags;
public:
	filter_patch_flags( int flags ) : m_flags( flags ){
	}
	bool filter( const Patch& patch ) const {
		return ( patch.getShaderFlags() & m_flags ) != 0;
	}
};


filter_patch_all g_filter_patch_all;
filter_patch_flags g_filter_patch_clip( QER_CLIP );
filter_patch_shader g_filter_patch_commonclip( "textures/common/clip" );
filter_patch_shader g_filter_patch_weapclip( "textures/common/weapclip" );
filter_patch_flags g_filter_patch_translucent( QER_TRANS | QER_ALPHATEST );

void PatchFilters_construct(){
	add_patch_filter( g_filter_patch_all, EXCLUDE_CURVES );
	add_patch_filter( g_filter_patch_clip, EXCLUDE_CLIP );
	add_patch_filter( g_filter_patch_commonclip, EXCLUDE_CLIP );
	add_patch_filter( g_filter_patch_weapclip, EXCLUDE_CLIP );
	add_patch_filter( g_filter_patch_translucent, EXCLUDE_TRANSLUCENT );
}


#include "preferences.h"

void Patch_constructPreferences( PreferencesPage& page ){
	page.appendSpinner( "Patch Subdivide Threshold", g_PatchSubdivideThreshold, 0, 128 );
}
void Patch_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Patches", "Patch Display Preferences" ) );
	Patch_constructPreferences( page );
}
void Patch_registerPreferencesPage(){
	PreferencesDialog_addDisplayPage( FreeCaller1<PreferenceGroup&, Patch_constructPage>() );
}


#include "preferencesystem.h"

void PatchPreferences_construct(){
	GlobalPreferenceSystem().registerPreference( "Subdivisions", IntImportStringCaller( g_PatchSubdivideThreshold ), IntExportStringCaller( g_PatchSubdivideThreshold ) );
}


#include "generic/callback.h"

static void Patch_TerrainCreate();
static void Patch_TerrainSettings();
static void Patch_TerrainRaise();
static void Patch_TerrainLower();
static void Patch_TerrainFlatten();
static void Patch_TerrainSmooth();
static void Patch_TerrainNoise();
static void Patch_TerrainErode();
static void Patch_TerrainPaintHeight();
static void Patch_TerrainFillSelection();
static void Patch_TerrainStitch();
static void Patch_TerrainToolOff();
static void Patch_TerrainBrushSizeIncrease();
static void Patch_TerrainBrushSizeDecrease();
static void Patch_TerrainBrushStrengthIncrease();
static void Patch_TerrainBrushStrengthDecrease();
extern ToggleItem g_terrainRaise_button;
extern ToggleItem g_terrainLower_button;
extern ToggleItem g_terrainFlatten_button;
extern ToggleItem g_terrainSmooth_button;
extern ToggleItem g_terrainNoise_button;
extern ToggleItem g_terrainErode_button;
extern ToggleItem g_terrainPaintHeight_button;

void Patch_registerCommands(){
	GlobalCommands_insert( "InvertCurveTextureX", FreeCaller<Patch_FlipTextureX>(), QKeySequence( "Ctrl+Shift+I" ) );
	GlobalCommands_insert( "InvertCurveTextureY", FreeCaller<Patch_FlipTextureY>(), QKeySequence( "Shift+I" ) );
	GlobalCommands_insert( "NaturalizePatch", FreeCaller<Patch_NaturalTexture>(), QKeySequence( "Ctrl+N" ) );
	GlobalCommands_insert( "PatchCylinder", FreeCaller<Patch_Cylinder>() );
//	GlobalCommands_insert( "PatchDenseCylinder", FreeCaller<Patch_DenseCylinder>() );
//	GlobalCommands_insert( "PatchVeryDenseCylinder", FreeCaller<Patch_VeryDenseCylinder>() );
	GlobalCommands_insert( "PatchSquareCylinder", FreeCaller<Patch_SquareCylinder>() );
	GlobalCommands_insert( "PatchXactCylinder", FreeCaller<Patch_XactCylinder>() );
	GlobalCommands_insert( "PatchXactSphere", FreeCaller<Patch_XactSphere>() );
	GlobalCommands_insert( "PatchXactCone", FreeCaller<Patch_XactCone>() );
	GlobalCommands_insert( "PatchEndCap", FreeCaller<Patch_Endcap>() );
	GlobalCommands_insert( "PatchBevel", FreeCaller<Patch_Bevel>() );
//	GlobalCommands_insert( "PatchSquareBevel", FreeCaller<Patch_SquareBevel>() );
//	GlobalCommands_insert( "PatchSquareEndcap", FreeCaller<Patch_SquareEndcap>() );
	GlobalCommands_insert( "PatchCone", FreeCaller<Patch_Cone>() );
	GlobalCommands_insert( "PatchSphere", FreeCaller<Patch_Sphere>() );
	GlobalCommands_insert( "SimplePatchMesh", FreeCaller<Patch_Plane>(), QKeySequence( "Shift+P" ) );
	GlobalCommands_insert( "PatchInsertFirstColumn", FreeCaller<Patch_InsertFirstColumn>(), QKeySequence( Qt::CTRL + Qt::SHIFT + Qt::Key_Plus + Qt::KeypadModifier ) );
	GlobalCommands_insert( "PatchInsertLastColumn", FreeCaller<Patch_InsertLastColumn>() );
	GlobalCommands_insert( "PatchInsertFirstRow", FreeCaller<Patch_InsertFirstRow>(), QKeySequence( Qt::CTRL + Qt::Key_Plus + Qt::KeypadModifier ) );
	GlobalCommands_insert( "PatchInsertLastRow", FreeCaller<Patch_InsertLastRow>() );
	GlobalCommands_insert( "PatchDeleteFirstColumn", FreeCaller<Patch_DeleteFirstColumn>() );
	GlobalCommands_insert( "PatchDeleteLastColumn", FreeCaller<Patch_DeleteLastColumn>(), QKeySequence( Qt::CTRL + Qt::SHIFT + Qt::Key_Minus + Qt::KeypadModifier ) );
	GlobalCommands_insert( "PatchDeleteFirstRow", FreeCaller<Patch_DeleteFirstRow>() );
	GlobalCommands_insert( "PatchDeleteLastRow", FreeCaller<Patch_DeleteLastRow>(), QKeySequence( Qt::CTRL + Qt::Key_Minus + Qt::KeypadModifier ) );
	GlobalCommands_insert( "InvertCurve", FreeCaller<Patch_Invert>(), QKeySequence( "Ctrl+I" ) );
	//GlobalCommands_insert( "RedisperseRows", FreeCaller<Patch_RedisperseRows>(), QKeySequence( "Ctrl+E" ) );
	GlobalCommands_insert( "RedisperseRows", FreeCaller<Patch_RedisperseRows>() );
	//GlobalCommands_insert( "RedisperseCols", FreeCaller<Patch_RedisperseCols>(), QKeySequence( "Ctrl+Shift+E" ) );
	GlobalCommands_insert( "RedisperseCols", FreeCaller<Patch_RedisperseCols>() );
	GlobalCommands_insert( "SmoothRows", FreeCaller<Patch_SmoothRows>(), QKeySequence( "Ctrl+W" ) );
	GlobalCommands_insert( "SmoothCols", FreeCaller<Patch_SmoothCols>(), QKeySequence( "Ctrl+Shift+W" ) );
	GlobalCommands_insert( "MatrixTranspose", FreeCaller<Patch_Transpose>(), QKeySequence( "Ctrl+Shift+M" ) );
	GlobalCommands_insert( "CapCurrentCurve", FreeCaller<Patch_Cap>(), QKeySequence( "Shift+C" ) );
//	GlobalCommands_insert( "MakeOverlayPatch", FreeCaller<Patch_OverlayOn>(), QKeySequence( "Y" ) );
//	GlobalCommands_insert( "ClearPatchOverlays", FreeCaller<Patch_OverlayOff>(), QKeySequence( "Ctrl+L" ) );
	GlobalCommands_insert( "PatchDeform", FreeCaller<Patch_Deform>() );
	GlobalCommands_insert( "PatchThicken", FreeCaller<Patch_Thicken>(), QKeySequence( "Ctrl+T" ) );
	GlobalCommands_insert( "TerrainPatchCreate", FreeCaller<Patch_TerrainCreate>() );
	GlobalCommands_insert( "TerrainToolSettings", FreeCaller<Patch_TerrainSettings>() );
	GlobalToggles_insert( "TerrainRaise", FreeCaller<Patch_TerrainRaise>(), ToggleItem::AddCallbackCaller( g_terrainRaise_button ) );
	GlobalToggles_insert( "TerrainLower", FreeCaller<Patch_TerrainLower>(), ToggleItem::AddCallbackCaller( g_terrainLower_button ) );
	GlobalToggles_insert( "TerrainFlatten", FreeCaller<Patch_TerrainFlatten>(), ToggleItem::AddCallbackCaller( g_terrainFlatten_button ) );
	GlobalToggles_insert( "TerrainSmooth", FreeCaller<Patch_TerrainSmooth>(), ToggleItem::AddCallbackCaller( g_terrainSmooth_button ) );
	GlobalToggles_insert( "TerrainNoise", FreeCaller<Patch_TerrainNoise>(), ToggleItem::AddCallbackCaller( g_terrainNoise_button ) );
	GlobalToggles_insert( "TerrainErode", FreeCaller<Patch_TerrainErode>(), ToggleItem::AddCallbackCaller( g_terrainErode_button ) );
	GlobalToggles_insert( "TerrainPaintHeight", FreeCaller<Patch_TerrainPaintHeight>(), ToggleItem::AddCallbackCaller( g_terrainPaintHeight_button ) );
	GlobalCommands_insert( "TerrainFillSelection", FreeCaller<Patch_TerrainFillSelection>() );
	GlobalCommands_insert( "TerrainStitch", FreeCaller<Patch_TerrainStitch>() );
	GlobalCommands_insert( "TerrainToolOff", FreeCaller<Patch_TerrainToolOff>() );
	GlobalCommands_insert( "TerrainBrushSizeIncrease", FreeCaller<Patch_TerrainBrushSizeIncrease>(), QKeySequence( "]" ) );
	GlobalCommands_insert( "TerrainBrushSizeDecrease", FreeCaller<Patch_TerrainBrushSizeDecrease>(), QKeySequence( "[" ) );
	GlobalCommands_insert( "TerrainBrushStrengthIncrease", FreeCaller<Patch_TerrainBrushStrengthIncrease>(), QKeySequence( "Shift+]" ) );
	GlobalCommands_insert( "TerrainBrushStrengthDecrease", FreeCaller<Patch_TerrainBrushStrengthDecrease>(), QKeySequence( "Shift+[" ) );
}

void Patch_constructToolbar( QToolBar* toolbar ){
	toolbar_append_button( toolbar, "Put caps on the current patch", "curve_cap.png", "CapCurrentCurve" );
	toolbar_append_button( toolbar, "Create terrain patch", "patch_wireframe.png", "TerrainPatchCreate" );
	toolbar_append_toggle_button( toolbar, "Terrain raise tool", "terrain_raise.png", "TerrainRaise" );
	toolbar_append_toggle_button( toolbar, "Terrain lower tool", "terrain_lower.png", "TerrainLower" );
	toolbar_append_toggle_button( toolbar, "Terrain flatten tool", "terrain_flatten.png", "TerrainFlatten" );
	toolbar_append_toggle_button( toolbar, "Terrain smooth tool", "terrain_smooth.png", "TerrainSmooth" );
	toolbar_append_toggle_button( toolbar, "Terrain noise tool", "terrain_noise.png", "TerrainNoise" );
	toolbar_append_toggle_button( toolbar, "Terrain erode tool", "terrain_erode.png", "TerrainErode" );
	toolbar_append_toggle_button( toolbar, "Terrain paint height tool", "terrain_paintheight.png", "TerrainPaintHeight" );
	toolbar_append_button( toolbar, "Fill terrain between selected verts", "terrain_fill.png", "TerrainFillSelection" );
	toolbar_append_button( toolbar, "Stitch adjacent terrain patches", "terrain_stitch.png", "TerrainStitch" );
	toolbar_append_button( toolbar, "Disable terrain tool", "terrain_off.png", "TerrainToolOff" );
	toolbar_append_button( toolbar, "Terrain brush size -", "terrain_brush_smaller.png", "TerrainBrushSizeDecrease" );
	toolbar_append_button( toolbar, "Terrain brush size +", "terrain_brush_larger.png", "TerrainBrushSizeIncrease" );
	toolbar_append_button( toolbar, "Terrain tool settings", "terrain_settings.png", "TerrainToolSettings" );
}

void Patch_constructMenu( QMenu* menu ){
	create_menu_item_with_mnemonic( menu, "Simple Patch Mesh...", "SimplePatchMesh" );
	{
		QMenu* submenu = menu->addMenu( "Terrain" );
		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );
		create_menu_item_with_mnemonic( submenu, "Create Terrain Patch...", "TerrainPatchCreate" );
		submenu->addSeparator();
		create_check_menu_item_with_mnemonic( submenu, "Raise Tool", "TerrainRaise" );
		create_check_menu_item_with_mnemonic( submenu, "Lower Tool", "TerrainLower" );
		create_check_menu_item_with_mnemonic( submenu, "Flatten Tool", "TerrainFlatten" );
		create_check_menu_item_with_mnemonic( submenu, "Smooth Tool", "TerrainSmooth" );
		create_check_menu_item_with_mnemonic( submenu, "Noise Tool", "TerrainNoise" );
		create_check_menu_item_with_mnemonic( submenu, "Erode Tool", "TerrainErode" );
		create_check_menu_item_with_mnemonic( submenu, "Paint Height Tool", "TerrainPaintHeight" );
		create_menu_item_with_mnemonic( submenu, "Fill Selection", "TerrainFillSelection" );
		create_menu_item_with_mnemonic( submenu, "Stitch Patches", "TerrainStitch" );
		create_menu_item_with_mnemonic( submenu, "Tool Off", "TerrainToolOff" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Brush Size +", "TerrainBrushSizeIncrease" );
		create_menu_item_with_mnemonic( submenu, "Brush Size -", "TerrainBrushSizeDecrease" );
		create_menu_item_with_mnemonic( submenu, "Strength +", "TerrainBrushStrengthIncrease" );
		create_menu_item_with_mnemonic( submenu, "Strength -", "TerrainBrushStrengthDecrease" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Tool Settings...", "TerrainToolSettings" );
	}
	create_menu_item_with_mnemonic( menu, "Bevel", "PatchBevel" );
	create_menu_item_with_mnemonic( menu, "End cap", "PatchEndCap" );
	create_menu_item_with_mnemonic( menu, "Cylinder (9x3)", "PatchCylinder" );
	create_menu_item_with_mnemonic( menu, "Square Cylinder (9x3)", "PatchSquareCylinder" );
	create_menu_item_with_mnemonic( menu, "Exact Cylinder...", "PatchXactCylinder" );
	create_menu_item_with_mnemonic( menu, "Cone (9x3)", "PatchCone" );
	create_menu_item_with_mnemonic( menu, "Exact Cone...", "PatchXactCone" );
	create_menu_item_with_mnemonic( menu, "Sphere (9x5)", "PatchSphere" );
	create_menu_item_with_mnemonic( menu, "Exact Sphere...", "PatchXactSphere" );
//	{
//		QMenu* submenu = menu->addMenu( "More Cylinders" );

//		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

//		create_menu_item_with_mnemonic( submenu, "Dense Cylinder", "PatchDenseCylinder" );
//		create_menu_item_with_mnemonic( submenu, "Very Dense Cylinder", "PatchVeryDenseCylinder" );
//		create_menu_item_with_mnemonic( submenu, "Square Cylinder", "PatchSquareCylinder" );
//	}
//	{
//		//not implemented
//		create_menu_item_with_mnemonic( menu, "Square Endcap", "PatchSquareBevel" );
//		create_menu_item_with_mnemonic( menu, "Square Bevel", "PatchSquareEndcap" );
//	}
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Cap Selection", "CapCurrentCurve" );
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( "Insert/Delete" );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Insert (2) First Columns", "PatchInsertFirstColumn" );
		create_menu_item_with_mnemonic( submenu, "Insert (2) Last Columns", "PatchInsertLastColumn" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Insert (2) First Rows", "PatchInsertFirstRow" );
		create_menu_item_with_mnemonic( submenu, "Insert (2) Last Rows", "PatchInsertLastRow" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Del First (2) Columns", "PatchDeleteFirstColumn" );
		create_menu_item_with_mnemonic( submenu, "Del Last (2) Columns", "PatchDeleteLastColumn" );
		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Del First (2) Rows", "PatchDeleteFirstRow" );
		create_menu_item_with_mnemonic( submenu, "Del Last (2) Rows", "PatchDeleteLastRow" );
	}
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( "Matrix" );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Invert", "InvertCurve" );
		create_menu_item_with_mnemonic( submenu, "Transpose", "MatrixTranspose" );

		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Re-disperse Rows", "RedisperseRows" );
		create_menu_item_with_mnemonic( submenu, "Re-disperse Columns", "RedisperseCols" );

		submenu->addSeparator();
		create_menu_item_with_mnemonic( submenu, "Smooth Rows", "SmoothRows" );
		create_menu_item_with_mnemonic( submenu, "Smooth Columns", "SmoothCols" );
	}
	menu->addSeparator();
	{
		QMenu* submenu = menu->addMenu( "Texture" );

		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

		create_menu_item_with_mnemonic( submenu, "Reset Texture", "TextureReset/Cap" );
		create_menu_item_with_mnemonic( submenu, "Naturalize", "NaturalizePatch" );
		create_menu_item_with_mnemonic( submenu, "Invert X", "InvertCurveTextureX" );
		create_menu_item_with_mnemonic( submenu, "Invert Y", "InvertCurveTextureY" );

	}
//	menu->addSeparator();
//	{ //unfinished
//		QMenu* submenu = menu->addMenu( "Overlay" );

//		submenu->setTearOffEnabled( g_Layout_enableDetachableMenus.m_value );

//		create_menu_item_with_mnemonic( submenu, "Set", "MakeOverlayPatch" );
//		create_menu_item_with_mnemonic( submenu, "Clear", "ClearPatchOverlays" );
//	}
	menu->addSeparator();
	create_menu_item_with_mnemonic( menu, "Deform...", "PatchDeform" );
	create_menu_item_with_mnemonic( menu, "Thicken...", "PatchThicken" );
}


#include "gtkutil/dialog.h"
#include "gtkutil/widget.h"
#include "gtkutil/spinbox.h"

#include <QDialog>
#include "gtkutil/combobox.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QButtonGroup>
#include <QRadioButton>
#include <algorithm>
#include <cmath>
#include <vector>
#include <random>

struct TerrainToolSettings
{
	int patchWidth = 9;
	int patchHeight = 9;
	int sizeX = 512;
	int sizeY = 512;
	int sizeZ = 64;
	int gridCols = 1;
	int gridRows = 1;
	int brushRadius = 192;
	int brushStrength = 16;
	float smoothFactor = 0.5f;
	bool flattenUseAverage = true;
	int flattenHeight = 0;
	float brushHardness = 0.5f;           // 0.0 = very soft, 1.0 = hard edge
	int brushFalloff = 0;                 // 0=Smooth(Cubic), 1=Linear, 2=Quadratic(legacy), 3=Gaussian, 4=Flat/Constant
	int smoothKernelRadius = 1;           // 1 = 3x3, 2 = 5x5, 3 = 7x7
	float noiseAmplitude = 8.0f;          // amplitude for noise tool
	float erodeStrength = 0.3f;           // erosion talus angle factor
	float paintHeight = 0.0f;             // target height for paint height tool
};

static TerrainToolSettings g_terrainToolSettings;

enum class ETerrainBrushFalloff
{
	Smooth,     // Cubic Hermite: 3t^2 - 2t^3 (smoothstep)
	Linear,     // 1 - dist/radius
	Quadratic,  // (1 - dist/radius)^2 (legacy)
	Gaussian,   // exp(-3 * (dist/radius)^2)
	Flat,       // Constant 1.0 within radius
};

enum class ETerrainBrushMode
{
	None,
	Raise,
	Lower,
	Flatten,
	Smooth,
	Noise,
	Erode,
	PaintHeight,
};

static ETerrainBrushMode g_terrainBrushMode = ETerrainBrushMode::None;
static bool g_terrainBrushStrokeActive = false;
static bool g_terrainBrushHasLastPoint = false;
static Vector3 g_terrainBrushLastPoint( g_vector3_identity );
static bool g_terrainBrushPreviewValid = false;
static Vector3 g_terrainBrushPreviewPoint( g_vector3_identity );

static bool TerrainRaiseModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Raise; }
static bool TerrainLowerModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Lower; }
static bool TerrainFlattenModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Flatten; }
static bool TerrainSmoothModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Smooth; }
static bool TerrainNoiseModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Noise; }
static bool TerrainErodeModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::Erode; }
static bool TerrainPaintHeightModeActive(){ return g_terrainBrushMode == ETerrainBrushMode::PaintHeight; }

template<bool( *BoolFunction )()>
class TerrainBoolFunctionExport
{
public:
	static void apply( const BoolImportCallback& importCallback ){
		importCallback( BoolFunction() );
	}
};

typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainRaiseModeActive>::apply> TerrainRaiseModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainLowerModeActive>::apply> TerrainLowerModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainFlattenModeActive>::apply> TerrainFlattenModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainSmoothModeActive>::apply> TerrainSmoothModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainNoiseModeActive>::apply> TerrainNoiseModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainErodeModeActive>::apply> TerrainErodeModeApplyCaller;
typedef FreeCaller1<const BoolImportCallback&, &TerrainBoolFunctionExport<TerrainPaintHeightModeActive>::apply> TerrainPaintHeightModeApplyCaller;

static TerrainRaiseModeApplyCaller g_terrainRaise_button_caller;
static TerrainLowerModeApplyCaller g_terrainLower_button_caller;
static TerrainFlattenModeApplyCaller g_terrainFlatten_button_caller;
static TerrainSmoothModeApplyCaller g_terrainSmooth_button_caller;
static TerrainNoiseModeApplyCaller g_terrainNoise_button_caller;
static TerrainErodeModeApplyCaller g_terrainErode_button_caller;
static TerrainPaintHeightModeApplyCaller g_terrainPaintHeight_button_caller;
static BoolExportCallback g_terrainRaise_button_callback( g_terrainRaise_button_caller );
static BoolExportCallback g_terrainLower_button_callback( g_terrainLower_button_caller );
static BoolExportCallback g_terrainFlatten_button_callback( g_terrainFlatten_button_caller );
static BoolExportCallback g_terrainSmooth_button_callback( g_terrainSmooth_button_caller );
static BoolExportCallback g_terrainNoise_button_callback( g_terrainNoise_button_caller );
static BoolExportCallback g_terrainErode_button_callback( g_terrainErode_button_caller );
static BoolExportCallback g_terrainPaintHeight_button_callback( g_terrainPaintHeight_button_caller );
ToggleItem g_terrainRaise_button( g_terrainRaise_button_callback );
ToggleItem g_terrainLower_button( g_terrainLower_button_callback );
ToggleItem g_terrainFlatten_button( g_terrainFlatten_button_callback );
ToggleItem g_terrainSmooth_button( g_terrainSmooth_button_callback );
ToggleItem g_terrainNoise_button( g_terrainNoise_button_callback );
ToggleItem g_terrainErode_button( g_terrainErode_button_callback );
ToggleItem g_terrainPaintHeight_button( g_terrainPaintHeight_button_callback );

static void TerrainToolChanged(){
	g_terrainRaise_button.update();
	g_terrainLower_button.update();
	g_terrainFlatten_button.update();
	g_terrainSmooth_button.update();
	g_terrainNoise_button.update();
	g_terrainErode_button.update();
	g_terrainPaintHeight_button.update();
}

static int terrain_clampOddPatchSize( int v ){
	if ( v < 3 ) {
		v = 3;
	}
	if ( v > 75 ) {
		v = 75;
	}
	if ( ( v & 1 ) == 0 ) {
		--v;
	}
	return v;
}

// Apply the selected falloff curve based on settings
static float terrain_applyFalloff( float dist, float radius ){
	// Inner radius based on hardness: hardness=1 means full effect everywhere, hardness=0 means gradual from center
	const float hardness = std::min( 1.f, std::max( 0.f, g_terrainToolSettings.brushHardness ) );
	const float innerRadius = radius * hardness;

	if ( dist <= innerRadius ) {
		return 1.f;
	}

	// Remap distance to [0,1] in the falloff zone (innerRadius to radius)
	const float range = radius - innerRadius;
	if ( range <= 0.f ) {
		return 1.f;
	}
	const float t = ( dist - innerRadius ) / range;  // 0 at inner edge, 1 at outer edge

	switch ( static_cast<ETerrainBrushFalloff>( g_terrainToolSettings.brushFalloff ) )
	{
	case ETerrainBrushFalloff::Smooth:     // Smoothstep (Cubic Hermite)
	{
		const float s = 1.f - t;
		return s * s * ( 3.f - 2.f * s );  // smoothstep inverse: smooth at edges
	}
	case ETerrainBrushFalloff::Linear:
		return 1.f - t;
	case ETerrainBrushFalloff::Quadratic:
	{
		const float s = 1.f - t;
		return s * s;
	}
	case ETerrainBrushFalloff::Gaussian:
		return std::exp( -3.f * t * t );
	case ETerrainBrushFalloff::Flat:
		return 1.f;
	}
	return ( 1.f - t ) * ( 1.f - t );  // fallback: quadratic
}

static float terrain_brushWeight( const Vector3& center, const Vector3& point, float radius ){
	if ( radius <= 0.f ) {
		return 0.f;
	}
	const float dx = point[0] - center[0];
	const float dy = point[1] - center[1];
	const float dist = std::sqrt( dx * dx + dy * dy );
	if ( dist >= radius ) {
		return 0.f;
	}
	return terrain_applyFalloff( dist, radius );
}

static Vector3 terrain_patchCenterXY( const Patch& patch ){
	Vector3 center( 0, 0, 0 );
	int count = 0;
	for ( const PatchControl& ctrl : patch.getControlPoints() ){
		center[0] += ctrl.m_vertex[0];
		center[1] += ctrl.m_vertex[1];
		center[2] += ctrl.m_vertex[2];
		++count;
	}
	if ( count > 0 ) {
		center /= static_cast<float>( count );
	}
	return center;
}

enum class ETerrainSculptOp
{
	Raise,
	Lower,
	Flatten,
	Smooth,
	Noise,
	Erode,
	PaintHeight,
};

static ETerrainSculptOp terrain_modeToOp( ETerrainBrushMode mode ){
	switch ( mode )
	{
	case ETerrainBrushMode::Raise: return ETerrainSculptOp::Raise;
	case ETerrainBrushMode::Lower: return ETerrainSculptOp::Lower;
	case ETerrainBrushMode::Flatten: return ETerrainSculptOp::Flatten;
	case ETerrainBrushMode::Smooth: return ETerrainSculptOp::Smooth;
	case ETerrainBrushMode::Noise: return ETerrainSculptOp::Noise;
	case ETerrainBrushMode::Erode: return ETerrainSculptOp::Erode;
	case ETerrainBrushMode::PaintHeight: return ETerrainSculptOp::PaintHeight;
	case ETerrainBrushMode::None: break;
	}
	return ETerrainSculptOp::Raise;
}

static bool terrain_viewAxes( int viewType, int& dim1, int& dim2, int& deformDim ){
	switch ( viewType )
	{
	case XY: dim1 = 0; dim2 = 1; deformDim = 2; return true;
	case XZ: dim1 = 0; dim2 = 2; deformDim = 1; return true;
	case YZ: dim1 = 1; dim2 = 2; deformDim = 0; return true;
	default: return false;
	}
}

static float terrain_brushWeightProjected( const Vector3& center, const Vector3& point, float radius, int dim1, int dim2 ){
	if ( radius <= 0.f ) {
		return 0.f;
	}
	const float d1 = point[dim1] - center[dim1];
	const float d2 = point[dim2] - center[dim2];
	const float dist = std::sqrt( d1 * d1 + d2 * d2 );
	if ( dist >= radius ) {
		return 0.f;
	}
	return terrain_applyFalloff( dist, radius );
}

static std::mt19937 g_terrainNoiseRng( 42 );

static bool Patch_terrainSculptAt( Patch& patch, ETerrainSculptOp op, const Vector3& center, int viewType ){
	if ( patch.getWidth() < 2 || patch.getHeight() < 2 ) {
		return false;
	}

	int dim1, dim2, deformDim;
	if ( !terrain_viewAxes( viewType, dim1, dim2, deformDim ) ) {
		return false;
	}

	const float radius = std::max( 1, g_terrainToolSettings.brushRadius );
	const float strength = static_cast<float>( g_terrainToolSettings.brushStrength );
	const float smoothFactor = std::min( 1.f, std::max( 0.f, g_terrainToolSettings.smoothFactor ) );
	const int smoothKernel = std::max( 1, std::min( 5, g_terrainToolSettings.smoothKernelRadius ) );

	std::vector<float> originalZ;
	originalZ.reserve( patch.getControlPoints().size() );
	for ( const PatchControl& ctrl : patch.getControlPoints() ){
		originalZ.push_back( ctrl.m_vertex[deformDim] );
	}

	float flattenTarget = static_cast<float>( g_terrainToolSettings.flattenHeight );
	if ( op == ETerrainSculptOp::Flatten && g_terrainToolSettings.flattenUseAverage ) {
		float sum = 0.f;
		int count = 0;
		for ( std::size_t row = 0; row < patch.getHeight(); ++row ){
			for ( std::size_t col = 0; col < patch.getWidth(); ++col ){
				const PatchControl& ctrl = patch.ctrlAt( row, col );
				if ( terrain_brushWeightProjected( center, ctrl.m_vertex, radius, dim1, dim2 ) > 0.f ) {
					sum += ctrl.m_vertex[deformDim];
					++count;
				}
			}
		}
		if ( count > 0 ) {
			flattenTarget = sum / static_cast<float>( count );
		}
	}

	// For noise tool: generate a distribution
	std::uniform_real_distribution<float> noiseDist( -1.f, 1.f );
	const float noiseAmplitude = g_terrainToolSettings.noiseAmplitude;

	// For erode tool: pre-compute neighbor height differences
	const float erodeStrength = std::min( 1.f, std::max( 0.f, g_terrainToolSettings.erodeStrength ) );

	// For paint height tool
	const float paintTarget = g_terrainToolSettings.paintHeight;

	bool changed = false;
	patch.undoSave();

	for ( std::size_t row = 0; row < patch.getHeight(); ++row ){
		for ( std::size_t col = 0; col < patch.getWidth(); ++col ){
			PatchControl& ctrl = patch.ctrlAt( row, col );
			const float w = terrain_brushWeightProjected( center, ctrl.m_vertex, radius, dim1, dim2 );
			if ( w <= 0.f ) {
				continue;
			}
			changed = true;

			switch ( op )
			{
			case ETerrainSculptOp::Raise:
				ctrl.m_vertex[deformDim] += strength * w;
				break;
			case ETerrainSculptOp::Lower:
				ctrl.m_vertex[deformDim] -= strength * w;
				break;
			case ETerrainSculptOp::Flatten:
				ctrl.m_vertex[deformDim] += ( flattenTarget - ctrl.m_vertex[deformDim] ) * w;
				break;
			case ETerrainSculptOp::Smooth:
			{
				// Enhanced Gaussian-weighted smooth with configurable kernel radius
				float weightedSum = 0.f;
				float totalKernelWeight = 0.f;
				const int kr = smoothKernel;
				const int r0 = std::max( 0, static_cast<int>( row ) - kr );
				const int r1 = std::min( static_cast<int>( patch.getHeight() ) - 1, static_cast<int>( row ) + kr );
				const int c0 = std::max( 0, static_cast<int>( col ) - kr );
				const int c1 = std::min( static_cast<int>( patch.getWidth() ) - 1, static_cast<int>( col ) + kr );
				for ( int rr = r0; rr <= r1; ++rr ){
					for ( int cc = c0; cc <= c1; ++cc ){
						const float dr = static_cast<float>( rr - static_cast<int>( row ) );
						const float dc = static_cast<float>( cc - static_cast<int>( col ) );
						const float distSq = dr * dr + dc * dc;
						// Gaussian kernel weight: sigma = kernelRadius * 0.5
						const float sigma = kr * 0.5f;
						const float kw = std::exp( -distSq / ( 2.f * sigma * sigma ) );
						weightedSum += originalZ[rr * patch.getWidth() + cc] * kw;
						totalKernelWeight += kw;
					}
				}
				if ( totalKernelWeight > 0.f ) {
					const float avg = weightedSum / totalKernelWeight;
					ctrl.m_vertex[deformDim] += ( avg - ctrl.m_vertex[deformDim] ) * ( smoothFactor * w );
				}
				break;
			}
			case ETerrainSculptOp::Noise:
			{
				const float noise = noiseDist( g_terrainNoiseRng );
				ctrl.m_vertex[deformDim] += noise * noiseAmplitude * w;
				break;
			}
			case ETerrainSculptOp::Erode:
			{
				// Simple thermal erosion: move height toward lowest neighbor
				const std::size_t idx = row * patch.getWidth() + col;
				const float currentH = originalZ[idx];
				float lowestNeighborH = currentH;
				const int r0 = std::max( 0, static_cast<int>( row ) - 1 );
				const int r1 = std::min( static_cast<int>( patch.getHeight() ) - 1, static_cast<int>( row ) + 1 );
				const int c0 = std::max( 0, static_cast<int>( col ) - 1 );
				const int c1 = std::min( static_cast<int>( patch.getWidth() ) - 1, static_cast<int>( col ) + 1 );
				for ( int rr = r0; rr <= r1; ++rr ){
					for ( int cc = c0; cc <= c1; ++cc ){
						if ( rr == static_cast<int>( row ) && cc == static_cast<int>( col ) ) {
							continue;
						}
						const float nh = originalZ[rr * patch.getWidth() + cc];
						if ( nh < lowestNeighborH ) {
							lowestNeighborH = nh;
						}
					}
				}
				// Only erode if we're higher than our lowest neighbor
				const float diff = currentH - lowestNeighborH;
				if ( diff > 0.f ) {
					ctrl.m_vertex[deformDim] -= diff * erodeStrength * w;
				}
				break;
			}
			case ETerrainSculptOp::PaintHeight:
			{
				// Paint exact height, blended by brush weight
				ctrl.m_vertex[deformDim] += ( paintTarget - ctrl.m_vertex[deformDim] ) * w;
				break;
			}
			}
		}
	}

	return changed;
}

struct TerrainStitchVertex
{
	Patch* patch;
	std::size_t row;
	std::size_t col;
};

static void Scene_PatchTerrainStitch_Selected( scene::Graph& graph, int viewType ){
	int dim1, dim2, deformDim;
	if ( !terrain_viewAxes( viewType, dim1, dim2, deformDim ) ) {
		return;
	}

	std::vector<TerrainStitchVertex> verts;
	Scene_forEachVisibleSelectedPatch( [&]( Patch& patch ){
		for ( std::size_t row = 0; row < patch.getHeight(); ++row ){
			for ( std::size_t col = 0; col < patch.getWidth(); ++col ){
				verts.push_back( TerrainStitchVertex{ &patch, row, col } );
			}
		}
	} );

	const float epsilon = 0.01f;
	for ( std::size_t i = 0; i < verts.size(); ++i ){
		PatchControl& a = verts[i].patch->ctrlAt( verts[i].row, verts[i].col );
		float sum = a.m_vertex[deformDim];
		int count = 1;

		for ( std::size_t j = i + 1; j < verts.size(); ++j ){
			PatchControl& b = verts[j].patch->ctrlAt( verts[j].row, verts[j].col );
			if ( std::fabs( a.m_vertex[dim1] - b.m_vertex[dim1] ) <= epsilon
				&& std::fabs( a.m_vertex[dim2] - b.m_vertex[dim2] ) <= epsilon ) {
				sum += b.m_vertex[deformDim];
				++count;
			}
		}

		if ( count <= 1 ) {
			continue;
		}

		const float avg = sum / static_cast<float>( count );
		a.m_vertex[deformDim] = avg;
		for ( std::size_t j = i + 1; j < verts.size(); ++j ){
			PatchControl& b = verts[j].patch->ctrlAt( verts[j].row, verts[j].col );
			if ( std::fabs( a.m_vertex[dim1] - b.m_vertex[dim1] ) <= epsilon
				&& std::fabs( a.m_vertex[dim2] - b.m_vertex[dim2] ) <= epsilon ) {
				b.m_vertex[deformDim] = avg;
			}
		}
	}
}

static void terrain_patchFillAxes( const Patch& patch, int& dim1, int& dim2, int& deformDim ){
	Vector3 normal = patch.Calculate_AvgNormal();
	normal[0] = std::fabs( normal[0] );
	normal[1] = std::fabs( normal[1] );
	normal[2] = std::fabs( normal[2] );
	deformDim = 2;
	if ( normal[0] >= normal[1] && normal[0] >= normal[2] ) {
		deformDim = 0;
	}
	else if ( normal[1] >= normal[0] && normal[1] >= normal[2] ) {
		deformDim = 1;
	}
	dim1 = ( deformDim + 1 ) % 3;
	dim2 = ( deformDim + 2 ) % 3;
}

struct TerrainFillAnchor
{
	std::size_t row;
	std::size_t col;
	Vector3 pos;
	float height;
};

static bool Patch_terrainFillFromSelectedVerts( PatchInstance& patchInstance ){
	Patch& patch = patchInstance.getPatch();
	const std::size_t width = patch.getWidth();
	const std::size_t height = patch.getHeight();
	if ( width < 2 || height < 2 ) {
		return false;
	}
	if ( !patchInstance.selectedVertices() ) {
		return false;
	}

	std::vector<TerrainFillAnchor> anchors;
	anchors.reserve( width * height );
	std::vector<unsigned char> selectedMask( width * height, 0 );

	patchInstance.forEachSelectedControlPoint( [&]( std::size_t index, const PatchControl& ctrl ){
		const std::size_t row = index / width;
		const std::size_t col = index % width;
		if ( row >= height || col >= width ) {
			return;
		}
		selectedMask[index] = 1;
		TerrainFillAnchor a;
		a.row = row;
		a.col = col;
		a.pos = ctrl.m_vertex;
		a.height = 0.f;
		anchors.push_back( a );
	} );

	if ( anchors.size() < 2 ) {
		return false;
	}

	int dim1, dim2, deformDim;
	terrain_patchFillAxes( patch, dim1, dim2, deformDim );
	for ( TerrainFillAnchor& a : anchors ){
		a.height = a.pos[deformDim];
	}

	std::size_t minRow = height - 1, maxRow = 0, minCol = width - 1, maxCol = 0;
	for ( const TerrainFillAnchor& a : anchors ){
		minRow = std::min( minRow, a.row );
		maxRow = std::max( maxRow, a.row );
		minCol = std::min( minCol, a.col );
		maxCol = std::max( maxCol, a.col );
	}
	if ( minRow == maxRow && minCol == maxCol ) {
		return false;
	}

	patch.undoSave();
	bool changed = false;

	for ( std::size_t row = minRow; row <= maxRow; ++row ){
		for ( std::size_t col = minCol; col <= maxCol; ++col ){
			const std::size_t idx = row * width + col;
			if ( selectedMask[idx] ) {
				continue;
			}

			PatchControl& ctrl = patch.ctrlAt( row, col );
			const float p1 = ctrl.m_vertex[dim1];
			const float p2 = ctrl.m_vertex[dim2];

			float weightedHeight = 0.f;
			float totalWeight = 0.f;
			bool exact = false;
			float exactHeight = ctrl.m_vertex[deformDim];

			for ( const TerrainFillAnchor& a : anchors ){
				const float d1 = p1 - a.pos[dim1];
				const float d2 = p2 - a.pos[dim2];
				const float distSq = d1 * d1 + d2 * d2;
				if ( distSq < 0.0001f ) {
					exact = true;
					exactHeight = a.height;
					break;
				}
				const float w = 1.f / distSq;
				weightedHeight += a.height * w;
				totalWeight += w;
			}

			if ( exact ) {
				if ( ctrl.m_vertex[deformDim] != exactHeight ) {
					ctrl.m_vertex[deformDim] = exactHeight;
					changed = true;
				}
				continue;
			}

			if ( totalWeight > 0.f ) {
				const float target = weightedHeight / totalWeight;
				if ( ctrl.m_vertex[deformDim] != target ) {
					ctrl.m_vertex[deformDim] = target;
					changed = true;
				}
			}
		}
	}

	if ( !changed ) {
		return false;
	}

	// Normalize the filled region into a smoother connection while preserving selected anchors.
	for ( int iter = 0; iter < 2; ++iter ){
		std::vector<float> heights( width * height, 0.f );
		for ( std::size_t row = 0; row < height; ++row ){
			for ( std::size_t col = 0; col < width; ++col ){
				heights[row * width + col] = patch.ctrlAt( row, col ).m_vertex[deformDim];
			}
		}

		for ( std::size_t row = minRow; row <= maxRow; ++row ){
			for ( std::size_t col = minCol; col <= maxCol; ++col ){
				const std::size_t idx = row * width + col;
				if ( selectedMask[idx] ) {
					continue;
				}

				const int r0 = std::max( static_cast<int>( minRow ), static_cast<int>( row ) - 1 );
				const int r1 = std::min( static_cast<int>( maxRow ), static_cast<int>( row ) + 1 );
				const int c0 = std::max( static_cast<int>( minCol ), static_cast<int>( col ) - 1 );
				const int c1 = std::min( static_cast<int>( maxCol ), static_cast<int>( col ) + 1 );

				float sum = 0.f;
				int count = 0;
				for ( int rr = r0; rr <= r1; ++rr ){
					for ( int cc = c0; cc <= c1; ++cc ){
						sum += heights[static_cast<std::size_t>( rr ) * width + static_cast<std::size_t>( cc )];
						++count;
					}
				}
				if ( count > 0 ) {
					const float avg = sum / static_cast<float>( count );
					PatchControl& ctrl = patch.ctrlAt( row, col );
					ctrl.m_vertex[deformDim] = ctrl.m_vertex[deformDim] + ( avg - ctrl.m_vertex[deformDim] ) * 0.65f;
				}
			}
		}
	}

	patch.controlPointsChanged();
	return true;
}

static void Scene_PatchTerrainFillFromSelectedVerts( scene::Graph& graph ){
	bool changed = false;
	Scene_forEachVisibleSelectedPatchInstance( [&]( PatchInstance& patchInstance ){
		changed = Patch_terrainFillFromSelectedVerts( patchInstance ) || changed;
	} );
	if ( changed ) {
		SceneChangeNotify();
	}
}

static void Scene_PatchTerrainSculpt_Selected( scene::Graph& graph, ETerrainSculptOp op ){
	Scene_forEachVisibleSelectedPatch( [op]( Patch& patch ){ Patch_terrainSculptAt( patch, op, terrain_patchCenterXY( patch ), XY ); } );
	SceneChangeNotify();
}

static void DoTerrainToolSettingsDlg(){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Terrain Tool Settings" );

	// Brush shape
	auto radius = new SpinBox( 1, 8192, g_terrainToolSettings.brushRadius );
	auto strength = new SpinBox( 1, 4096, g_terrainToolSettings.brushStrength );

	auto hardness = new DoubleSpinBox( 0.0, 1.0, 0, 2 );
	hardness->setSingleStep( 0.05 );
	hardness->setValue( g_terrainToolSettings.brushHardness );
	hardness->setToolTip( "0.0 = soft falloff from center, 1.0 = hard edge (no falloff)" );

	auto falloff = new ComboBox;
	falloff->addItem( "Smooth (Cubic)" );
	falloff->addItem( "Linear" );
	falloff->addItem( "Quadratic (Legacy)" );
	falloff->addItem( "Gaussian" );
	falloff->addItem( "Flat (Constant)" );
	falloff->setCurrentIndex( std::min( 4, std::max( 0, g_terrainToolSettings.brushFalloff ) ) );

	// Smooth settings
	auto smooth = new DoubleSpinBox( 0.0, 1.0, 0, 2 );
	smooth->setSingleStep( 0.05 );
	smooth->setValue( g_terrainToolSettings.smoothFactor );

	auto smoothKernel = new SpinBox( 1, 5, g_terrainToolSettings.smoothKernelRadius );
	smoothKernel->setToolTip( "Kernel radius: 1 = 3x3, 2 = 5x5, 3 = 7x7, etc." );

	// Flatten settings
	auto flattenAverage = new QCheckBox( "Flatten to average height in brush" );
	flattenAverage->setChecked( g_terrainToolSettings.flattenUseAverage );
	auto flattenHeight = new SpinBox( -65536, 65536, g_terrainToolSettings.flattenHeight );

	// Noise settings
	auto noiseAmp = new DoubleSpinBox( 0.1, 1024.0, 0, 1 );
	noiseAmp->setSingleStep( 1.0 );
	noiseAmp->setValue( g_terrainToolSettings.noiseAmplitude );

	// Erode settings
	auto erodeStr = new DoubleSpinBox( 0.0, 1.0, 0, 2 );
	erodeStr->setSingleStep( 0.05 );
	erodeStr->setValue( g_terrainToolSettings.erodeStrength );
	erodeStr->setToolTip( "How aggressively vertices move toward their lowest neighbor" );

	// Paint height settings
	auto paintH = new DoubleSpinBox( -65536.0, 65536.0, 0, 1 );
	paintH->setSingleStep( 1.0 );
	paintH->setValue( g_terrainToolSettings.paintHeight );

	auto form = new QFormLayout( &dialog );
	form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );

	// Brush Shape section
	form->addRow( "Brush radius:", radius );
	form->addRow( "Raise/lower strength:", strength );
	form->addRow( "Brush hardness:", hardness );
	form->addRow( "Falloff curve:", falloff );

	// Smooth section
	form->addRow( "Smooth factor:", smooth );
	form->addRow( "Smooth kernel radius:", smoothKernel );

	// Flatten section
	form->addRow( "", flattenAverage );
	form->addRow( "Flatten target Z:", flattenHeight );

	// Noise section
	form->addRow( "Noise amplitude:", noiseAmp );

	// Erode section
	form->addRow( "Erosion strength:", erodeStr );

	// Paint height section
	form->addRow( "Paint target Z:", paintH );

	auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
	form->addWidget( buttons );
	QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	if ( dialog.exec() ) {
		g_terrainToolSettings.brushRadius = radius->value();
		g_terrainToolSettings.brushStrength = strength->value();
		g_terrainToolSettings.brushHardness = static_cast<float>( hardness->value() );
		g_terrainToolSettings.brushFalloff = falloff->currentIndex();
		g_terrainToolSettings.smoothFactor = static_cast<float>( smooth->value() );
		g_terrainToolSettings.smoothKernelRadius = smoothKernel->value();
		g_terrainToolSettings.flattenUseAverage = flattenAverage->isChecked();
		g_terrainToolSettings.flattenHeight = flattenHeight->value();
		g_terrainToolSettings.noiseAmplitude = static_cast<float>( noiseAmp->value() );
		g_terrainToolSettings.erodeStrength = static_cast<float>( erodeStr->value() );
		g_terrainToolSettings.paintHeight = static_cast<float>( paintH->value() );
	}
}

static void DoTerrainPatchDlg(){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Create Terrain Patch" );

	const AABB bounds = PatchCreator_getBounds();
	auto densityW = new SpinBox( 3, 75, g_terrainToolSettings.patchWidth );
	auto densityH = new SpinBox( 3, 75, g_terrainToolSettings.patchHeight );
	auto sizeX = new SpinBox( 1, 65536, g_terrainToolSettings.sizeX ? g_terrainToolSettings.sizeX : static_cast<int>( bounds.extents[0] * 2 ) );
	auto sizeY = new SpinBox( 1, 65536, g_terrainToolSettings.sizeY ? g_terrainToolSettings.sizeY : static_cast<int>( bounds.extents[1] * 2 ) );
	auto sizeZ = new SpinBox( 1, 65536, g_terrainToolSettings.sizeZ ? g_terrainToolSettings.sizeZ : std::max( 1, static_cast<int>( bounds.extents[2] * 2 ) ) );
	auto gridCols = new SpinBox( 1, 128, g_terrainToolSettings.gridCols );
	auto gridRows = new SpinBox( 1, 128, g_terrainToolSettings.gridRows );

	auto form = new QFormLayout( &dialog );
	form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
	form->addRow( "Patch width:", densityW );
	form->addRow( "Patch height:", densityH );
	form->addRow( "Size X:", sizeX );
	form->addRow( "Size Y:", sizeY );
	form->addRow( "Size Z:", sizeZ );
	form->addRow( "Grid columns:", gridCols );
	form->addRow( "Grid rows:", gridRows );

	auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
	form->addWidget( buttons );
	QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	if ( dialog.exec() ) {
		g_terrainToolSettings.patchWidth = terrain_clampOddPatchSize( densityW->value() );
		g_terrainToolSettings.patchHeight = terrain_clampOddPatchSize( densityH->value() );
		g_terrainToolSettings.sizeX = std::max( 1, sizeX->value() );
		g_terrainToolSettings.sizeY = std::max( 1, sizeY->value() );
		g_terrainToolSettings.sizeZ = std::max( 1, sizeZ->value() );
		g_terrainToolSettings.gridCols = std::max( 1, gridCols->value() );
		g_terrainToolSettings.gridRows = std::max( 1, gridRows->value() );

		AABB terrainBounds = bounds;
		terrainBounds.extents[0] = g_terrainToolSettings.sizeX * 0.5f;
		terrainBounds.extents[1] = g_terrainToolSettings.sizeY * 0.5f;
		terrainBounds.extents[2] = g_terrainToolSettings.sizeZ * 0.5f;
		const int axis = GlobalXYWnd_getCurrentViewType();
		const char* shader = TextureBrowser_GetSelectedShader();
		Select_Delete();
		GlobalSelectionSystem().setSelectedAll( false );
		int dim1, dim2, deformDim;
		if ( !terrain_viewAxes( axis, dim1, dim2, deformDim ) ) {
			dim1 = 0; dim2 = 1;
		}
		for ( int gy = 0; gy < g_terrainToolSettings.gridRows; ++gy ){
			for ( int gx = 0; gx < g_terrainToolSettings.gridCols; ++gx ){
				AABB cell = terrainBounds;
				cell.origin[dim1] += ( gx - ( g_terrainToolSettings.gridCols - 1 ) * 0.5f ) * ( g_terrainToolSettings.sizeX );
				cell.origin[dim2] += ( gy - ( g_terrainToolSettings.gridRows - 1 ) * 0.5f ) * ( g_terrainToolSettings.sizeY );
				Scene_PatchConstructPrefab_Append(
					GlobalSceneGraph(),
					cell,
					shader,
					EPatchPrefab::Plane,
					axis,
					g_terrainToolSettings.patchWidth,
					g_terrainToolSettings.patchHeight,
					false
				);
			}
		}
	}
}

static void Patch_TerrainCreate(){
	UndoableCommand undo( "terrainPatchCreate" );
	DoTerrainPatchDlg();
}

static const char* terrain_modeName( ETerrainBrushMode mode ){
	switch ( mode )
	{
	case ETerrainBrushMode::Raise: return "Raise";
	case ETerrainBrushMode::Lower: return "Lower";
	case ETerrainBrushMode::Flatten: return "Flatten";
	case ETerrainBrushMode::Smooth: return "Smooth";
	case ETerrainBrushMode::Noise: return "Noise";
	case ETerrainBrushMode::Erode: return "Erode";
	case ETerrainBrushMode::PaintHeight: return "Paint Height";
	case ETerrainBrushMode::None: return "Off";
	}
	return "Off";
}

static void terrain_setBrushMode( ETerrainBrushMode mode ){
	g_terrainBrushMode = ( g_terrainBrushMode == mode ) ? ETerrainBrushMode::None : mode;
	g_terrainBrushHasLastPoint = false;
	g_terrainBrushPreviewValid = false;
	TerrainToolChanged();
}

static void Patch_TerrainSettings(){
	DoTerrainToolSettingsDlg();
}

static void Patch_TerrainRaise(){
	terrain_setBrushMode( ETerrainBrushMode::Raise );
}

static void Patch_TerrainLower(){
	terrain_setBrushMode( ETerrainBrushMode::Lower );
}

static void Patch_TerrainFlatten(){
	terrain_setBrushMode( ETerrainBrushMode::Flatten );
}

static void Patch_TerrainSmooth(){
	terrain_setBrushMode( ETerrainBrushMode::Smooth );
}

static void Patch_TerrainNoise(){
	terrain_setBrushMode( ETerrainBrushMode::Noise );
}

static void Patch_TerrainErode(){
	terrain_setBrushMode( ETerrainBrushMode::Erode );
}

static void Patch_TerrainPaintHeight(){
	terrain_setBrushMode( ETerrainBrushMode::PaintHeight );
}

static void Patch_TerrainFillSelection(){
	UndoableCommand undo( "terrainFillSelection" );
	Scene_PatchTerrainFillFromSelectedVerts( GlobalSceneGraph() );
}

static void Patch_TerrainStitch(){
	UndoableCommand undo( "terrainStitch" );
	Scene_PatchTerrainStitch_Selected( GlobalSceneGraph(), GlobalXYWnd_getCurrentViewType() );
	Scene_forEachVisibleSelectedPatch( []( Patch& patch ){
		patch.controlPointsChanged();
	} );
	SceneChangeNotify();
}

static void Patch_TerrainToolOff(){
	g_terrainBrushMode = ETerrainBrushMode::None;
	g_terrainBrushHasLastPoint = false;
	g_terrainBrushPreviewValid = false;
	TerrainToolChanged();
}

static void Patch_TerrainBrushSizeIncrease(){
	g_terrainToolSettings.brushRadius = std::min( 8192, g_terrainToolSettings.brushRadius + std::max( 1, g_terrainToolSettings.brushRadius / 4 ) );
}

static void Patch_TerrainBrushSizeDecrease(){
	g_terrainToolSettings.brushRadius = std::max( 1, g_terrainToolSettings.brushRadius - std::max( 1, g_terrainToolSettings.brushRadius / 4 ) );
}

static void Patch_TerrainBrushStrengthIncrease(){
	g_terrainToolSettings.brushStrength = std::min( 4096, g_terrainToolSettings.brushStrength + std::max( 1, g_terrainToolSettings.brushStrength / 4 ) );
}

static void Patch_TerrainBrushStrengthDecrease(){
	g_terrainToolSettings.brushStrength = std::max( 1, g_terrainToolSettings.brushStrength - std::max( 1, g_terrainToolSettings.brushStrength / 4 ) );
}

bool Patch_TerrainTool_IsActive(){
	return g_terrainBrushMode != ETerrainBrushMode::None;
}

int Patch_TerrainTool_GetBrushRadius(){
	return g_terrainToolSettings.brushRadius;
}

float Patch_TerrainTool_GetBrushInnerRadius(){
	return g_terrainToolSettings.brushRadius * g_terrainToolSettings.brushHardness;
}

void Patch_TerrainTool_Disable(){
	Patch_TerrainToolOff();
}

bool Patch_TerrainTool_GetPreviewPoint( Vector3& point ){
	if ( !g_terrainBrushPreviewValid ) {
		return false;
	}
	point = g_terrainBrushPreviewPoint;
	return true;
}

static bool Scene_PatchTerrainPaintAt_Selected( scene::Graph& graph, ETerrainSculptOp op, const Vector3& point, int viewType ){
	bool touched = false;
	std::vector<Patch*> changedPatches;
	Scene_forEachVisibleSelectedPatch( [&]( Patch& patch ){
		if ( Patch_terrainSculptAt( patch, op, point, viewType ) ) {
			changedPatches.push_back( &patch );
			touched = true;
		}
	} );
	if ( touched ) {
		if ( op == ETerrainSculptOp::Smooth ) {
			Scene_PatchTerrainStitch_Selected( graph, viewType );
		}
		for ( Patch* patch : changedPatches ) {
			patch->controlPointsChanged();
		}
		SceneChangeNotify();
	}
	return touched;
}

static bool Scene_PatchTerrainPaintAlongRay_Selected( scene::Graph& graph, ETerrainSculptOp op, const Vector3& rayOrigin, const Vector3& rayDir ){
	Vector3 bestPoint( g_vector3_identity );
	float bestT = 1e30f;
	bool foundHit = false;

	Scene_forEachVisibleSelectedPatch( [&]( Patch& patch ){
		const Vector3 normal = vector3_normalised( patch.Calculate_AvgNormal() );
		const Vector3 center = terrain_patchCenterXY( patch );
		const float denom = vector3_dot( normal, rayDir );
		if ( std::fabs( denom ) < 1e-4f ) {
			return;
		}
		const float t = vector3_dot( center - rayOrigin, normal ) / denom;
		if ( t <= 0.f || t >= bestT ) {
			return;
		}
		bestT = t;
		bestPoint = rayOrigin + rayDir * t;
		foundHit = true;
	} );

	if ( !foundHit ) {
		g_terrainBrushPreviewValid = false;
		return false;
	}
	g_terrainBrushPreviewPoint = bestPoint;
	g_terrainBrushPreviewValid = true;

	bool changed = false;
	std::vector<Patch*> changedPatches;
	Scene_forEachVisibleSelectedPatch( [&]( Patch& patch ){
		if ( Patch_terrainSculptAt( patch, op, bestPoint, XY ) ) {
			changedPatches.push_back( &patch );
			changed = true;
		}
	} );
	if ( changed ) {
		if ( op == ETerrainSculptOp::Smooth ) {
			Scene_PatchTerrainStitch_Selected( graph, XY );
		}
		for ( Patch* patch : changedPatches ) {
			patch->controlPointsChanged();
		}
		SceneChangeNotify();
	}
	return changed;
}

static bool Scene_PatchTerrainRayPreview_Selected( scene::Graph& graph, const Vector3& rayOrigin, const Vector3& rayDir ){
	bool found = false;
	float bestT = 1e30f;
	Vector3 bestPoint( g_vector3_identity );
	Scene_forEachVisibleSelectedPatch( [&]( Patch& patch ){
		const Vector3 normal = vector3_normalised( patch.Calculate_AvgNormal() );
		const Vector3 center = terrain_patchCenterXY( patch );
		const float denom = vector3_dot( normal, rayDir );
		if ( std::fabs( denom ) < 1e-4f ) {
			return;
		}
		const float t = vector3_dot( center - rayOrigin, normal ) / denom;
		if ( t <= 0.f || t >= bestT ) {
			return;
		}
		bestT = t;
		bestPoint = rayOrigin + rayDir * t;
		found = true;
	} );
	g_terrainBrushPreviewValid = found;
	if ( found ) {
		g_terrainBrushPreviewPoint = bestPoint;
	}
	return found;
}

static bool terrain_beginStrokeIfNeeded(){
	if ( !g_terrainBrushStrokeActive ) {
		GlobalUndoSystem().start();
		g_terrainBrushStrokeActive = true;
	}
	return true;
}

static bool terrain_paintAt( int viewType, const Vector3& point, bool spacingCheck ){
	if ( g_terrainBrushMode == ETerrainBrushMode::None ) {
		return false;
	}
	if ( Scene_GetUltimateSelectedVisiblePatch() == 0 ) {
		return false;
	}
	if ( spacingCheck && g_terrainBrushHasLastPoint ) {
		int dim1, dim2, deformDim;
		if ( terrain_viewAxes( viewType, dim1, dim2, deformDim ) ) {
			const float dx = point[dim1] - g_terrainBrushLastPoint[dim1];
			const float dy = point[dim2] - g_terrainBrushLastPoint[dim2];
			const float minStep = std::max( 1.f, g_terrainToolSettings.brushRadius * 0.08f );
			if ( dx * dx + dy * dy < minStep * minStep ) {
				return false;
			}
		}
	}

	terrain_beginStrokeIfNeeded();
	const bool touched = Scene_PatchTerrainPaintAt_Selected( GlobalSceneGraph(), terrain_modeToOp( g_terrainBrushMode ), point, viewType );
	if ( touched ) {
		g_terrainBrushLastPoint = point;
		g_terrainBrushHasLastPoint = true;
	}
	return touched;
}

bool Patch_TerrainTool_XYMouseDown( int viewType, const Vector3& point ){
	g_terrainBrushHasLastPoint = false;
	return terrain_paintAt( viewType, point, false );
}

bool Patch_TerrainTool_XYMouseMove( int viewType, const Vector3& point, bool leftButtonDown ){
	if ( !leftButtonDown ) {
		return false;
	}
	return terrain_paintAt( viewType, point, true );
}

bool Patch_TerrainTool_XYMouseUp(){
	if ( g_terrainBrushStrokeActive ) {
		GlobalUndoSystem().finish( "terrainBrushStroke" );
		g_terrainBrushStrokeActive = false;
		g_terrainBrushHasLastPoint = false;
		return true;
	}
	return false;
}

bool Patch_TerrainTool_CamMouseDown( const Vector3& rayOrigin, const Vector3& rayDirection ){
	g_terrainBrushHasLastPoint = false;
	if ( g_terrainBrushMode == ETerrainBrushMode::None || Scene_GetUltimateSelectedVisiblePatch() == 0 ) {
		return false;
	}
	terrain_beginStrokeIfNeeded();
	const bool touched = Scene_PatchTerrainPaintAlongRay_Selected( GlobalSceneGraph(), terrain_modeToOp( g_terrainBrushMode ), rayOrigin, rayDirection );
	return touched;
}

bool Patch_TerrainTool_CamMouseMove( const Vector3& rayOrigin, const Vector3& rayDirection, bool leftButtonDown ){
	if ( !leftButtonDown || g_terrainBrushMode == ETerrainBrushMode::None || Scene_GetUltimateSelectedVisiblePatch() == 0 ) {
		return false;
	}
	terrain_beginStrokeIfNeeded();
	return Scene_PatchTerrainPaintAlongRay_Selected( GlobalSceneGraph(), terrain_modeToOp( g_terrainBrushMode ), rayOrigin, rayDirection );
}

bool Patch_TerrainTool_CamMouseUp(){
	return Patch_TerrainTool_XYMouseUp();
}

void Patch_TerrainTool_CamHover( const Vector3& rayOrigin, const Vector3& rayDirection ){
	if ( g_terrainBrushMode == ETerrainBrushMode::None || Scene_GetUltimateSelectedVisiblePatch() == 0 ) {
		g_terrainBrushPreviewValid = false;
		return;
	}
	Scene_PatchTerrainRayPreview_Selected( GlobalSceneGraph(), rayOrigin, rayDirection );
}

void DoNewPatchDlg( EPatchPrefab prefab, int minrows, int mincols, int defrows, int defcols, int maxrows, int maxcols ){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Patch density" );

	auto width = new ComboBox;
	auto height = new ComboBox;
	auto redisperseCheckBox = new QCheckBox( "Square" );

	{
		auto form = new QFormLayout( &dialog );
		form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
		{
			{
				for ( int x = 3; x <= 75; x += 2 ) {
					if ( x >= mincols && ( !maxcols || x <= maxcols ) ) {
						width->addItem( QString::number( x ) );
					}
				}
				form->addRow( "Width:", width );
			}
			{
				for ( int x = 3; x <= 75; x += 2 ) {
					if ( x >= minrows && ( !maxrows || x <= maxrows ) ) {
						height->addItem( QString::number( x ) );
					}
				}
				form->addRow( "Height:", height );
			}

			if( prefab != EPatchPrefab::Plane ){
				redisperseCheckBox->setToolTip( "Redisperse columns & rows" );
				form->addWidget( redisperseCheckBox );
			}
		}
		{
			auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
			form->addWidget( buttons );
			QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
			QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
		}
	}

	// Initialize dialog
	width->setCurrentIndex( ( defcols - mincols ) / 2 );
	height->setCurrentIndex( ( defrows - minrows ) / 2 );

	if ( dialog.exec() ) {
		const int w = width->currentIndex() * 2 + mincols;
		const int h = height->currentIndex() * 2 + minrows;
		const bool redisperse = redisperseCheckBox->isChecked();
		Scene_PatchConstructPrefab( GlobalSceneGraph(), PatchCreator_getBounds(), TextureBrowser_GetSelectedShader(), prefab, GlobalXYWnd_getCurrentViewType(), w, h, redisperse );
	}
}


void DoPatchDeformDlg(){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Patch deform" );

	auto spin = new SpinBox( -9999, 9999, 64 );

	const char* pAxis[] = { "X", "Y", "Z" };
	RadioHBox radioBox = RadioHBox_new( StringArrayRange(pAxis) );
	radioBox.m_radio->button( 2 )->setChecked( true );

	{
		auto form = new QFormLayout( &dialog );
		form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
		form->addRow( new SpinBoxLabel( "Max deform:", spin ), spin );
		form->addRow( "", radioBox.m_hbox );
		{
			auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
			form->addWidget( buttons );
			QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
			QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
		}
	}

	if ( dialog.exec() ) {
		const int deform = spin->value();
		const int axis = radioBox.m_radio->checkedId();
		Scene_PatchDeform( GlobalSceneGraph(), deform, axis );
	}
}



void DoCapDlg(){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Cap" );

	auto group = new QButtonGroup( &dialog );
	{
		auto form = new QFormLayout( &dialog );
		form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
		{
			const char* iconlabel[][2] = { { "cap_bevel.png", "Bevel" },
			                               { "cap_endcap.png", "Endcap" },
			                               { "cap_ibevel.png", "Inverted Bevel" },
			                               { "cap_iendcap.png", "Inverted Endcap" },
			                               { "cap_cylinder.png", "Cylinder" } };
			for( size_t i = 0; i < std::size( iconlabel ); ++i ){
				const auto [ stricon, strlabel ] = iconlabel[i];
				auto label = new QLabel;
				label->setPixmap( new_local_image( stricon ) );
				auto button = new QRadioButton( strlabel );
				group->addButton( button, i ); // set ids 0+, default ones are negative
				form->addRow( label, button );
			}
			for( int i = 0; i < form->count(); ++i )
				form->itemAt( i )->setAlignment( Qt::AlignmentFlag::AlignVCenter );
		}
		{
			auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
			form->addWidget( buttons );
			QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
			QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
		}
	}

	// Initialize dialog
	group->button( 0 )->setChecked( true );

	if( dialog.exec() ){
		Scene_PatchDoCap_Selected( GlobalSceneGraph(), TextureBrowser_GetSelectedShader(), static_cast<EPatchCap>( group->checkedId() ) );
	}
}


void DoPatchThickenDlg(){
	QDialog dialog( MainFrame_getWindow(), Qt::Dialog | Qt::WindowCloseButtonHint );
	dialog.setWindowTitle( "Patch thicken" );

	const int grid = std::max( GetGridSize(), 1.f );
	auto spin = new SpinBox( -9999, 9999, grid, 2, grid );

	const char* pAxis[] = { "X", "Y", "Z", "Normal" };
	RadioHBox radioBox = RadioHBox_new( StringArrayRange(pAxis) );
	radioBox.m_radio->button( 3 )->setChecked( true );

	auto check = new QCheckBox( "Side walls" );
	check->setChecked( true );

	{
		auto form = new QFormLayout( &dialog );
		form->setSizeConstraint( QLayout::SizeConstraint::SetFixedSize );
		form->addRow( new SpinBoxLabel( "Thickness:", spin ), spin );
		form->addRow( "", radioBox.m_hbox );
		form->addWidget( check );
		{
			auto buttons = new QDialogButtonBox( QDialogButtonBox::StandardButton::Ok | QDialogButtonBox::StandardButton::Cancel );
			form->addWidget( buttons );
			QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
			QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
		}
	}

	if ( dialog.exec() ) {
		const int thickness = spin->value();
		const bool seams = check->isChecked();
		const int axis = radioBox.m_radio->checkedId(); // 3 == Extrude along normals

		Scene_PatchThicken( GlobalSceneGraph(), thickness, seams, axis );
	}
}




class PatchTexdefConstructor
{
public:
	brushprimit_texdef_t m_bp;
	Matrix4 m_local2tex;
	Matrix4 m_tex2local;
	Plane3 m_plane;
	// rip from UVManipulator::UpdateFaceData
	PatchTexdefConstructor( Patch *patch ){
		m_plane.normal() = patch->Calculate_AvgNormal();
		m_plane.dist() = vector3_dot( m_plane.normal(), patch->localAABB().origin );
		const size_t patchWidth = patch->getWidth();
		const size_t patchHeight = patch->getHeight();
		{	//! todo force or deduce orthogonal uv axes for convenience
			Vector3 wDir, hDir;
			patch->Calculate_AvgAxes( wDir, hDir );
			vector3_normalise( wDir );
			vector3_normalise( hDir );
//					globalOutputStream() << wDir << " wDir\n";
//					globalOutputStream() << hDir << " hDir\n";
//					globalOutputStream() << m_plane.normal() << " m_plane.normal()\n";

			/* find longest row and column */
			float wLength = 0, hLength = 0; //!? todo break, if some of these is 0
			std::size_t row = 0, col = 0;
			for ( std::size_t r = 0; r < patchHeight; ++r ){
				float length = 0;
				for ( std::size_t c = 0; c < patchWidth - 1; ++c ){
					length += vector3_length( patch->ctrlAt( r, c + 1 ).m_vertex - patch->ctrlAt( r, c ).m_vertex );
				}
				if( length - wLength > .1f || ( ( r == 0 || r == patchHeight - 1 ) && float_equal_epsilon( length, wLength, .1f ) ) ){ // prioritize first and last rows
					wLength = length;
					row = r;
				}
			}
			for ( std::size_t c = 0; c < patchWidth; ++c ){
				float length = 0;
				for ( std::size_t r = 0; r < patchHeight - 1; ++r ){
					length += vector3_length( patch->ctrlAt( r + 1, c ).m_vertex - patch->ctrlAt( r, c ).m_vertex );
				}
				if( length - hLength > .1f || ( ( c == 0 || c == patchWidth - 1 ) && float_equal_epsilon( length, hLength, .1f ) ) ){
					hLength = length;
					col = c;
				}
			}
			//! todo handle case, when uv start = end, like projection to cylinder
			//! todo consider max uv length to have manipulator size according to patch size
			/* pick 3 points at the found row and column */
			const PatchControl* p0, *p1, *p2;
			Vector3 v0, v1, v2;
			{
				float distW0 = 0, distW1 = 0;
				for ( std::size_t c = 0; c < col; ++c ){
					distW0 += vector3_length( patch->ctrlAt( row, c + 1 ).m_vertex - patch->ctrlAt( row, c ).m_vertex );
				}
				for ( std::size_t c = col; c < patchWidth - 1; ++c ){
					distW1 += vector3_length( patch->ctrlAt( row, c + 1 ).m_vertex - patch->ctrlAt( row, c ).m_vertex );
				}
				float distH0 = 0, distH1 = 0;
				for ( std::size_t r = 0; r < row; ++r ){
					distH0 += vector3_length( patch->ctrlAt( r + 1, col ).m_vertex - patch->ctrlAt( r, col ).m_vertex );
				}
				for ( std::size_t r = row; r < patchHeight - 1; ++r ){
					distH1 += vector3_length( patch->ctrlAt( r + 1, col ).m_vertex - patch->ctrlAt( r, col ).m_vertex );
				}

				if( ( distW0 > distH0 && distW0 > distH1 ) || ( distW1 > distH0 && distW1 > distH1 ) ){
					p0 = &patch->ctrlAt( 0, col );
					p1 = &patch->ctrlAt( patchHeight - 1, col );
					p2 = distW0 > distW1? &patch->ctrlAt( row, 0 ) : &patch->ctrlAt( row, patchWidth - 1 );
					v0 = p0->m_vertex; //! the altered line, we want realistic offset values
					v1 = v0 + hDir * hLength;
					v2 = v0 + hDir * distH0 + ( distW0 > distW1? ( wDir * -distW0 ) : ( wDir * distW1 ) );
				}
				else{
					p0 = &patch->ctrlAt( row, 0 );
					p1 = &patch->ctrlAt( row, patchWidth - 1 );
					p2 = distH0 > distH1? &patch->ctrlAt( 0, col ) : &patch->ctrlAt( patchHeight - 1, col );
					v0 = p0->m_vertex; //! the altered line, we want realistic offset values
					v1 = v0 + wDir * wLength;
					v2 = v0 + wDir * distW0 + ( distH0 > distH1? ( hDir * -distH0 ) : ( hDir * distH1 ) );
				}

				if( vector3_dot( plane3_for_points( v0, v1, v2 ).normal(), m_plane.normal() ) < 0 ){
					std::swap( p0, p1 );
					std::swap( v0, v1 );
				}
			}
			const DoubleVector3 vertices[3]{ v0, v1, v2 };
			const DoubleVector3 sts[3]{ DoubleVector3( p0->m_texcoord ),
										DoubleVector3( p1->m_texcoord ),
										DoubleVector3( p2->m_texcoord ) };
			Texdef_Construct_local2tex_from_ST( vertices, sts, m_local2tex );
			m_tex2local = matrix4_affine_inverse( m_local2tex );
			BP_from_ST( m_bp, vertices, sts, plane3_for_points( vertices ).normal() );
			m_bp.removeScale( patch->getShader()->getTexture().width, patch->getShader()->getTexture().height );
		}
	}
	bool valid() const {
		return !( !std::isfinite( m_local2tex[0] ) //nan
		       || !std::isfinite( m_tex2local[0] ) //nan
		       || fabs( vector3_dot( m_plane.normal(), m_tex2local.z().vec3() ) ) < 1e-6 //projected along face
		       || vector3_length_squared( m_tex2local.x().vec3() ) < .01 //srsly scaled down, limit at max 10 textures per world unit
		       || vector3_length_squared( m_tex2local.y().vec3() ) < .01 );
	}
};

void Scene_PatchGetTexdef_Selected( scene::Graph& graph, TextureProjection &projection ){
	if ( Patch* patch = Scene_GetUltimateSelectedVisiblePatch() ){
		PatchTexdefConstructor c( patch );
		if( c.valid() )
			projection.m_brushprimit_texdef = c.m_bp;
	}
}

bool Scene_PatchGetShaderTexdef_Selected( scene::Graph& graph, CopiedString& name, TextureProjection &projection ){
	Patch* patch = nullptr;
	if ( !( patch = Scene_GetUltimateSelectedVisiblePatch() ) )
		Scene_forEachSelectedPatch( [&patch]( PatchInstance& p ){ if( !patch ) patch = &p.getPatch(); } );
	if( patch ){
		name = patch->GetShader();
		PatchTexdefConstructor c( patch );
		if( c.valid() )
			projection.m_brushprimit_texdef = c.m_bp;
		return true;
	}
	return false;
}

void Patch_SetTexdef( const float* hShift, const float* vShift, const float* hScale, const float* vScale, const float* rotation ){
	Scene_forEachVisibleSelectedPatch( [hShift, vShift, hScale, vScale, rotation]( Patch& patch ){
		PatchTexdefConstructor c( &patch );
		if( c.valid() ){
			BPTexdef_Assign( c.m_bp, hShift, vShift, hScale, vScale, rotation );
			Matrix4 local2tex;
			c.m_bp.addScale( patch.getShader()->getTexture().width, patch.getShader()->getTexture().height );
			BP_Construct_local2tex( c.m_bp, c.m_plane, local2tex );
			matrix4_multiply_by_matrix4( local2tex, c.m_tex2local );
			patch.undoSave();
			for( auto& p : patch.getControlPoints() )
				p.m_texcoord = matrix4_transformed_point( local2tex, Vector3( p.m_texcoord ) ).vec2();
			patch.controlPointsChanged();
		}
		else{ // fallback //. fixme: this is not cool; may be more valid cases in PatchTexdefConstructor, as in find one good triangle in problematic case
			if( hShift )
				Scene_PatchTranslateTexture_Selected( GlobalSceneGraph(), *hShift > 0? 8 : -8, 0 );
			if( vShift )
				Scene_PatchTranslateTexture_Selected( GlobalSceneGraph(), 0, *vShift > 0? 8 : -8 );
			if( hScale )
				Scene_PatchScaleTexture_Selected( GlobalSceneGraph(), *hScale > 0? .5 : -.5, 0 );
			if( vScale )
				Scene_PatchScaleTexture_Selected( GlobalSceneGraph(), 0, *vScale > 0? .5 : -.5 );
			if( rotation )
				Scene_PatchRotateTexture_Selected( GlobalSceneGraph(), *rotation > 0? 15 : -15 );

		}
		Patch_textureChanged();
	} );
}


