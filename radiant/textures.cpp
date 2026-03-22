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

#include "textures.h"

#include "debugging/debugging.h"

#include "itextures.h"
#include "ivk.h"
#include "igl.h"            // GlobalOpenGLModuleRef alias
#include "preferencesystem.h"
#include "vktexture.h"

#include "texturelib.h"
#include "container/hashfunc.h"
#include "container/cache.h"
#include "generic/callback.h"
#include "stream/stringstream.h"
#include "stringio.h"

#include "image.h"
#include "texmanip.h"
#include "preferences.h"



enum ETexturesMode
{
	eTextures_NEAREST = 0,
	eTextures_NEAREST_MIPMAP_NEAREST = 1,
	eTextures_NEAREST_MIPMAP_LINEAR = 2,
	eTextures_LINEAR = 3,
	eTextures_LINEAR_MIPMAP_NEAREST = 4,
	eTextures_LINEAR_MIPMAP_LINEAR = 5,
};

enum TextureCompressionFormat
{
	TEXTURECOMPRESSION_NONE = 0,
	TEXTURECOMPRESSION_RGBA = 1,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT1 = 2,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT3 = 3,
	TEXTURECOMPRESSION_RGBA_S3TC_DXT5 = 4,
};

struct texture_globals_t
{
	TextureCompressionFormat m_nTextureCompressionFormat;
	float fGamma;

	texture_globals_t() :
		m_nTextureCompressionFormat( TEXTURECOMPRESSION_NONE ),
		fGamma( 1.0f ){}
};

texture_globals_t g_texture_globals;

// Map ETexturesMode to our VkTexFilter enum (used when uploading new textures)
static VkTexFilter texModeToFilter( ETexturesMode mode )
{
	switch ( mode )
	{
	case eTextures_NEAREST:                 return VK_TEX_NEAREST;
	case eTextures_NEAREST_MIPMAP_NEAREST:  return VK_TEX_NEAREST_MIPMAP_NEAREST;
	case eTextures_NEAREST_MIPMAP_LINEAR:   return VK_TEX_NEAREST_MIPMAP_LINEAR;
	case eTextures_LINEAR:                  return VK_TEX_LINEAR;
	case eTextures_LINEAR_MIPMAP_NEAREST:   return VK_TEX_LINEAR_MIPMAP_NEAREST;
	case eTextures_LINEAR_MIPMAP_LINEAR:
	default:                                return VK_TEX_LINEAR_MIPMAP_LINEAR;
	}
}

ETexturesMode g_texture_mode = eTextures_LINEAR_MIPMAP_LINEAR;
bool g_TextureAnisotropy = true;




byte g_gammatable[256];
void ResampleGamma( float fGamma ){
	int i,inf;
	if ( fGamma == 1.0 ) {
		for ( i = 0; i < 256; i++ )
			g_gammatable[i] = i;
	}
	else
	{
		for ( i = 0; i < 256; i++ )
		{
			inf = (int)( 255 * pow( static_cast<double>( ( i + 0.5 ) / 255.5 ), static_cast<double>( fGamma ) ) + 0.5 );
			if ( inf < 0 ) {
				inf = 0;
			}
			if ( inf > 255 ) {
				inf = 255;
			}
			g_gammatable[i] = inf;
		}
	}
}

int g_Textures_mipLevel = 0;

/// Upload RGBA pixels to a Vulkan 2-D texture; gamma-correct first.
void LoadTextureRGBA( qtexture_t* q, unsigned char* pPixels, int nWidth, int nHeight ){
	static float fGamma = -1;
	float total[3];
	int nCount = nWidth * nHeight;

	if ( fGamma != g_texture_globals.fGamma ) {
		fGamma = g_texture_globals.fGamma;
		ResampleGamma( fGamma );
	}

	q->width  = nWidth;
	q->height = nHeight;

	total[0] = total[1] = total[2] = 0.0f;

	for ( int i = 0; i < ( nCount * 4 ); i += 4 )
	{
		for ( int j = 0; j < 3; j++ )
		{
			total[j] += ( pPixels + i )[j];
			byte b = ( pPixels + i )[j];
			( pPixels + i )[j] = g_gammatable[b];
		}
	}

	q->color[0] = total[0] / ( nCount * 255 );
	q->color[1] = total[1] / ( nCount * 255 );
	q->color[2] = total[2] / ( nCount * 255 );

	q->texture_number = VKTexture_create2D(
	    pPixels,
	    static_cast<uint32_t>( nWidth ),
	    static_cast<uint32_t>( nHeight ),
	    texModeToFilter( g_texture_mode ),
	    g_TextureAnisotropy );
}

#if 0
/*
   ==============
   Texture_InitPalette
   ==============
 */
void Texture_InitPalette( byte *pal ){
	int r,g,b;
	int i;
	int inf;
	byte gammatable[256];
	float gamma;

	gamma = g_texture_globals.fGamma;

	if ( gamma == 1.0 ) {
		for ( i = 0; i < 256; i++ )
			gammatable[i] = i;
	}
	else
	{
		for ( i = 0; i < 256; i++ )
		{
			inf = (int)( 255 * pow( ( i + 0.5 ) / 255.5, gamma ) + 0.5 );
			if ( inf < 0 ) {
				inf = 0;
			}
			if ( inf > 255 ) {
				inf = 255;
			}
			gammatable[i] = inf;
		}
	}

	for ( i = 0; i < 256; i++ )
	{
		r = gammatable[pal[0]];
		g = gammatable[pal[1]];
		b = gammatable[pal[2]];
		pal += 3;

		//v = (r<<24) + (g<<16) + (b<<8) + 255;
		//v = BigLong (v);

		//tex_palette[i] = v;
		tex_palette[i * 3 + 0] = r;
		tex_palette[i * 3 + 1] = g;
		tex_palette[i * 3 + 2] = b;
	}
}
#endif

#if 0
class TestHashtable
{
public:
	TestHashtable(){
		HashTable<CopiedString, CopiedString, HashStringNoCase, StringEqualNoCase> strings;
		strings["Monkey"] = "bleh";
		strings["MonkeY"] = "blah";
	}
};

const TestHashtable g_testhashtable;

#endif

typedef std::pair<LoadImageCallback, CopiedString> TextureKey;

void qtexture_realise( qtexture_t& texture, const TextureKey& key ){
	texture.texture_number = 0;
	if ( !key.second.empty() ) {
		if( !key.first.m_skybox ){
			Image* image = key.first.loadImage( key.second.c_str() );
			if ( image != 0 ) {
				LoadTextureRGBA( &texture, image->getRGBAPixels(), image->getWidth(), image->getHeight() );
				texture.surfaceFlags = image->getSurfaceFlags();
				texture.contentFlags = image->getContentFlags();
				texture.value = image->getValue();
				image->release();
				globalOutputStream() << "Loaded Texture: \"" << key.second << "\"\n";
			}
			else
			{
				globalErrorStream() << "Texture load failed: \"" << key.second << "\"\n";
			}
		}
		else {
			Image *images[6]{};
			/* load in order: _ft _bk _up _dn _rt _lf — fix orientation in shader */
			const char *suffixes[] = { "_ft", "_bk", "_up", "_dn", "_rt", "_lf" };
			for( int i = 0; i < 6; ++i ){
				images[i] = key.first.loadImage( StringStream<64>( key.second, suffixes[i] ) );
			}
			if( std::all_of( images, images + std::size( images ), []( const Image *img ){ return img != nullptr; } ) ){
				// Normalise all faces to the same square size
				uint32_t size = 0;
				for( const auto img : images )
					size = std::max( { size, img->getWidth(), img->getHeight() } );

				std::vector<std::vector<byte>> resampledFaces( 6 );
				const unsigned char* facePtrs[6];
				for( int i = 0; i < 6; ++i ){
					const Image& img = *images[i];
					if( img.getWidth() != size || img.getHeight() != size ){
						resampledFaces[i].resize( size * size * 4 );
						R_ResampleTexture( img.getRGBAPixels(), img.getWidth(), img.getHeight(),
						                   resampledFaces[i].data(), size, size, 4 );
						facePtrs[i] = resampledFaces[i].data();
					} else {
						facePtrs[i] = img.getRGBAPixels();
					}
				}

				texture.texture_number = VKTexture_createCubeMap( facePtrs, size );
				globalOutputStream() << "Loaded Skybox: \"" << key.second << "\"\n";
			}
			else
			{
				globalErrorStream() << "Skybox load failed: \"" << key.second << "\"\n";
			}

			std::for_each_n( images, std::size( images ), []( Image *img ){ if( img != nullptr ) img->release(); } );
		}
	}
}

void qtexture_unrealise( qtexture_t& texture ){
	if ( GlobalVulkan().contextValid && texture.texture_number != 0 ) {
		VKTexture_destroy( texture.texture_number );
		texture.texture_number = 0;
	}
}

class TextureKeyEqualNoCase
{
public:
	bool operator()( const TextureKey& key, const TextureKey& other ) const {
		return key.first == other.first && string_equal_nocase( key.second.c_str(), other.second.c_str() );
	}
};

class TextureKeyHashNoCase
{
public:
	typedef hash_t hash_type;
	hash_t operator()( const TextureKey& key ) const {
		return hash_combine( string_hash_nocase( key.second.c_str() ), pod_hash( key.first ) );
	}
};

#define DEBUG_TEXTURES 0

class TexturesMap final : public TexturesCache
{
	class TextureConstructor
	{
		TexturesMap* m_cache;
	public:
		explicit TextureConstructor( TexturesMap* cache )
			: m_cache( cache ){
		}
		qtexture_t* construct( const TextureKey& key ){
			qtexture_t* texture = new qtexture_t( key.first, key.second.c_str() );
			if ( m_cache->realised() ) {
				qtexture_realise( *texture, key );
			}
			return texture;
		}
		void destroy( qtexture_t* texture ){
			if ( m_cache->realised() ) {
				qtexture_unrealise( *texture );
			}
			delete texture;
		}
	};

	typedef HashedCache<TextureKey, qtexture_t, TextureKeyHashNoCase, TextureKeyEqualNoCase, TextureConstructor> qtextures_t;
	qtextures_t m_qtextures;
	TexturesCacheObserver* m_observer;
	std::size_t m_unrealised;

public:
	TexturesMap() : m_qtextures( TextureConstructor( this ) ), m_observer( 0 ), m_unrealised( 1 ){
	}
	typedef qtextures_t::iterator iterator;

	iterator begin(){
		return m_qtextures.begin();
	}
	iterator end(){
		return m_qtextures.end();
	}

	LoadImageCallback defaultLoader() const {
		return LoadImageCallback( 0, QERApp_LoadImage );
	}
	Image* loadImage( const char* name ){
		return defaultLoader().loadImage( name );
	}
	qtexture_t* capture( const char* name ){
		return capture( defaultLoader(), name );
	}
	qtexture_t* capture( const LoadImageCallback& loader, const char* name ){
#if DEBUG_TEXTURES
		globalOutputStream() << "textures capture: " << makeQuoted( name ) << '\n';
#endif
		return m_qtextures.capture( TextureKey( loader, name ) ).get();
	}
	void release( qtexture_t* texture ){
#if DEBUG_TEXTURES
		globalOutputStream() << "textures release: " << makeQuoted( texture->name ) << '\n';
#endif
		m_qtextures.release( TextureKey( texture->load, texture->name ) );
	}
	void attach( TexturesCacheObserver& observer ){
		ASSERT_MESSAGE( m_observer == 0, "TexturesMap::attach: cannot attach observer" );
		m_observer = &observer;
	}
	void detach( TexturesCacheObserver& observer ){
		ASSERT_MESSAGE( m_observer == &observer, "TexturesMap::detach: cannot detach observer" );
		m_observer = 0;
	}
	void realise(){
		if ( --m_unrealised == 0 ) {
			for ( qtextures_t::iterator i = m_qtextures.begin(); i != m_qtextures.end(); ++i )
			{
				if ( !( *i ).value.empty() ) {
					qtexture_realise( *( *i ).value, ( *i ).key );
				}
			}
			if ( m_observer != 0 ) {
				m_observer->realise();
			}
		}
	}
	void unrealise(){
		if ( ++m_unrealised == 1 ) {
			if ( m_observer != 0 ) {
				m_observer->unrealise();
			}
			for ( qtextures_t::iterator i = m_qtextures.begin(); i != m_qtextures.end(); ++i )
			{
				if ( !( *i ).value.empty() ) {
					qtexture_unrealise( *( *i ).value );
				}
			}
		}
	}
	bool realised(){
		return m_unrealised == 0;
	}
};

TexturesMap* g_texturesmap;

TexturesCache& GetTexturesCache(){
	return *g_texturesmap;
}


void Textures_Realise(){
	g_texturesmap->realise();
}

void Textures_Unrealise(){
	g_texturesmap->unrealise();
}


Callback g_texturesModeChangedNotify;

void Textures_setModeChangedNotify( const Callback& notify ){
	g_texturesModeChangedNotify = notify;
}

void Textures_ModeChanged(){
	// In Vulkan, filter settings are baked into the sampler at upload time.
	// Trigger a full texture reload so all samplers are recreated with the new settings.
	if ( g_texturesmap->realised() ) {
		Textures_Unrealise();
		Textures_Realise();
	}
	g_texturesModeChangedNotify();
}

void Textures_SetMode( ETexturesMode mode ){
	if ( g_texture_mode != mode ) {
		g_texture_mode = mode;

		Textures_ModeChanged();
	}
}

void Textures_SetAnisotropy( bool anisotropy ){
	if ( g_TextureAnisotropy != anisotropy ) {
		g_TextureAnisotropy = anisotropy;

		Textures_ModeChanged();
	}
}

void Textures_UpdateTextureCompressionFormat(){
	// Vulkan uses VK_FORMAT_R8G8B8A8_UNORM; DXT/BC compression not yet implemented.
	// This function is kept as a no-op for preference-system stability.
}

void TextureGammaImport( float& self, float value ){
	if ( self != value ) {
		Textures_Unrealise();
		self = value;
		Textures_Realise();
	}
}
typedef ReferenceCaller1<float, float, TextureGammaImport> TextureGammaImportCaller;

void TextureModeImport( ETexturesMode& self, int value ){
	switch ( value )
	{
	case 0:
		Textures_SetMode( eTextures_NEAREST );
		break;
	case 1:
		Textures_SetMode( eTextures_NEAREST_MIPMAP_NEAREST );
		break;
	case 2:
		Textures_SetMode( eTextures_LINEAR );
		break;
	case 3:
		Textures_SetMode( eTextures_NEAREST_MIPMAP_LINEAR );
		break;
	case 4:
		Textures_SetMode( eTextures_LINEAR_MIPMAP_NEAREST );
		break;
	case 5:
		Textures_SetMode( eTextures_LINEAR_MIPMAP_LINEAR );
	}
}
typedef ReferenceCaller1<ETexturesMode, int, TextureModeImport> TextureModeImportCaller;

void TextureModeExport( ETexturesMode& self, const IntImportCallback& importer ){
	switch ( self )
	{
	case eTextures_NEAREST:
		importer( 0 );
		break;
	case eTextures_NEAREST_MIPMAP_NEAREST:
		importer( 1 );
		break;
	case eTextures_LINEAR:
		importer( 2 );
		break;
	case eTextures_NEAREST_MIPMAP_LINEAR:
		importer( 3 );
		break;
	case eTextures_LINEAR_MIPMAP_NEAREST:
		importer( 4 );
		break;
	case eTextures_LINEAR_MIPMAP_LINEAR:
		importer( 5 );
		break;
	default:
		importer( 4 );
	}
}
typedef ReferenceCaller1<ETexturesMode, const IntImportCallback&, TextureModeExport> TextureModeExportCaller;

void Textures_constructPreferences( PreferencesPage& page ){
	page.appendSpinner(
	    "Texture Gamma",
	    0.0,
	    5.0,
	    FloatImportCallback( TextureGammaImportCaller( g_texture_globals.fGamma ) ),
	    FloatExportCallback( FloatExportCaller( g_texture_globals.fGamma ) )
	);
	{
		const char* texture_mode[] = { "Nearest", "Nearest Mipmap", "Linear", "Bilinear", "Bilinear Mipmap", "Trilinear" };
		page.appendCombo(
		    "Texture Render Mode",
		    StringArrayRange( texture_mode ),
		    IntImportCallback( TextureModeImportCaller( g_texture_mode ) ),
		    IntExportCallback( TextureModeExportCaller( g_texture_mode ) )
		);
	}
	page.appendCheckBox( "", "Anisotropy",
	                     FreeCaller1<bool, Textures_SetAnisotropy>(),
	                     BoolExportCaller( g_TextureAnisotropy ) );
}
void Textures_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Textures", "Texture Settings" ) );
	Textures_constructPreferences( page );
}
void Textures_registerPreferencesPage(){
	PreferencesDialog_addDisplayPage( FreeCaller1<PreferenceGroup&, Textures_constructPage>() );
}

void Textures_Construct(){
	g_texturesmap = new TexturesMap;

	GlobalPreferenceSystem().registerPreference( "TextureFiltering", IntImportStringCaller( reinterpret_cast<int&>( g_texture_mode ) ), IntExportStringCaller( reinterpret_cast<int&>( g_texture_mode ) ) );
	GlobalPreferenceSystem().registerPreference( "TextureAnisotropy", BoolImportStringCaller( g_TextureAnisotropy ), BoolExportStringCaller( g_TextureAnisotropy ) );
	GlobalPreferenceSystem().registerPreference( "TextureMipLevel", IntImportStringCaller( g_Textures_mipLevel ), IntExportStringCaller( g_Textures_mipLevel ) );
	GlobalPreferenceSystem().registerPreference( "SI_Gamma", FloatImportStringCaller( g_texture_globals.fGamma ), FloatExportStringCaller( g_texture_globals.fGamma ) );

	Textures_registerPreferencesPage();

	Textures_ModeChanged();
}
void Textures_Destroy(){
	delete g_texturesmap;
}


#include "modulesystem/modulesmap.h"
#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"
#include "qerplugin.h"

class TexturesDependencies :
	public GlobalRadiantModuleRef,
	public GlobalOpenGLModuleRef,
	public GlobalPreferenceSystemModuleRef
{
	ImageModulesRef m_image_modules;
public:
	TexturesDependencies() :
		m_image_modules( GlobalRadiant().getRequiredGameDescriptionKeyValue( "texturetypes" ) ){
	}
	ImageModules& getImageModules(){
		return m_image_modules.get();
	}
};

class TexturesAPI
{
	TexturesCache* m_textures;
public:
	typedef TexturesCache Type;
	STRING_CONSTANT( Name, "*" );

	TexturesAPI(){
		Textures_Construct();

		m_textures = &GetTexturesCache();
	}
	~TexturesAPI(){
		Textures_Destroy();
	}
	TexturesCache* getTable(){
		return m_textures;
	}
};

typedef SingletonModule<TexturesAPI, TexturesDependencies> TexturesModule;
typedef Static<TexturesModule> StaticTexturesModule;
StaticRegisterModule staticRegisterTextures( StaticTexturesModule::instance() );

ImageModules& Textures_getImageModules(){
	return StaticTexturesModule::instance().getDependencies().getImageModules();
}
