//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================

#include "cbase.h"
#include <vgui_controls/Controls.h>
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include "vgui_avatarimage.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#if defined( _X360 )
#include "xbox/xbox_win32stubs.h"
#endif
#include "steam/steam_api.h"
#include "gif_lib.h"

DECLARE_BUILD_FACTORY( CAvatarImagePanel );

CUtlMap< AvatarImagePair_t, int> CAvatarImage::s_AvatarImageCache; // cache of steam id's to textureids to use for images
bool CAvatarImage::m_sbInitializedAvatarCache = false;

ConVar cl_animated_avatars( "cl_animated_avatars", "1", FCVAR_ARCHIVE, "Enable animated avatars" );

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CAvatarImage::CAvatarImage( void )
: m_sPersonaStateChangedCallback( this, &CAvatarImage::OnPersonaStateChanged ),
m_sEquippedProfileItemsChangedCallback( this, &CAvatarImage::OnEquippedProfileItemsChanged )
{
	ClearAvatarSteamID();
	m_pFriendIcon = NULL;
	m_nX = 0;
	m_nY = 0;
	m_wide = m_tall = 0;
	m_avatarWide = m_avatarTall = 0;
	m_Color = Color( 255, 255, 255, 255 );
	m_bLoadPending = false;
	m_bSetDesiredSize = false;
	m_fNextLoadTime = 0.0f;
	m_AvatarSize = k_EAvatarSize32x32;
	
	//=============================================================================
	// HPE_BEGIN:
	//=============================================================================
	// [tj] Default to drawing the friend icon for avatars
	m_bDrawFriend = true;

	m_textureIDs.EnsureCapacity( 1 );

	// [menglish] Default icon for avatar icons if there is no avatar icon for the player
	m_textureIDs.AddToTail( -1 );

	// set up friend icon
	m_pFriendIcon = gHUD.GetIcon( "ico_friend_indicator_avatar" );

	m_pDefaultImage = NULL;

	SetAvatarSize(DEFAULT_AVATAR_SIZE, DEFAULT_AVATAR_SIZE);

	//=============================================================================
	// HPE_END
	//=============================================================================

	if ( !m_sbInitializedAvatarCache) 
	{
		m_sbInitializedAvatarCache = true;
		SetDefLessFunc( s_AvatarImageCache );
	}
}

//-----------------------------------------------------------------------------
// Purpose: reset the image to a default state (will render with the default image)
//-----------------------------------------------------------------------------
void CAvatarImage::ClearAvatarSteamID( void )
{
	m_bValid = false;
	m_bFriend = false;
	m_bLoadPending = false;
	m_SteamID.Set( 0, k_EUniverseInvalid, k_EAccountTypeInvalid );
	m_sPersonaStateChangedCallback.Unregister();
}


//-----------------------------------------------------------------------------
// Purpose: Set the CSteamID for this image; this will cause a deferred load
//-----------------------------------------------------------------------------
bool CAvatarImage::SetAvatarSteamID( CSteamID steamIDUser, EAvatarSize avatarSize /*= k_EAvatarSize32x32 */ )
{
	ClearAvatarSteamID();

	m_SteamID = steamIDUser;
	// misyl: We determine this in UpdateAvatarImageSize.
	//m_AvatarSize = avatarSize;
	m_bLoadPending = true;

	m_sPersonaStateChangedCallback.Register( this, &CAvatarImage::OnPersonaStateChanged );
	m_sEquippedProfileItemsChangedCallback.Register( this, &CAvatarImage::OnEquippedProfileItemsChanged );

	SteamAPICall_t hRequestItemsCall = steamapicontext->SteamFriends()->RequestEquippedProfileItems( m_SteamID );
	m_sEquippedProfileItemsRequestedCallback.Set( hRequestItemsCall, this, &CAvatarImage::OnEquippedProfileItemsRequested );

	if ( m_bSetDesiredSize )
	{
		LoadAvatarImage();
	}
	UpdateFriendStatus();

	return m_bValid;
}

//-----------------------------------------------------------------------------
// Purpose: Called when somebody changes their avatar image
//-----------------------------------------------------------------------------
void CAvatarImage::OnPersonaStateChanged( PersonaStateChange_t *info )
{
	if ( ( info->m_ulSteamID == m_SteamID.ConvertToUint64() ) && ( info->m_nChangeFlags & k_EPersonaChangeAvatar ) )
	{
		// Mark us as invalid.
		m_bValid = false;
		m_bLoadPending = true;

		// Poll
		UpdateAvatarImageSize();
		LoadAvatarImage();
	}
}

//-----------------------------------------------------------------------------
// Purpose: EquippedProfileItems_t callresult
//-----------------------------------------------------------------------------
void CAvatarImage::OnEquippedProfileItemsRequested( EquippedProfileItems_t* pInfo, bool bIOFailure )
{
	if( pInfo->m_steamID == m_SteamID )
	{
		m_bLoadPending = true;
		LoadAvatarImage();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called when somebody changes their equipped profile items
//-----------------------------------------------------------------------------
void CAvatarImage::OnEquippedProfileItemsChanged( EquippedProfileItemsChanged_t* pInfo )
{
	if( pInfo->m_steamID == m_SteamID )
	{
		SteamAPICall_t hRequestItemsCall = steamapicontext->SteamFriends()->RequestEquippedProfileItems( m_SteamID );
		m_sEquippedProfileItemsRequestedCallback.Set( hRequestItemsCall, this, &CAvatarImage::OnEquippedProfileItemsRequested );
	}
}

//-----------------------------------------------------------------------------
// Purpose: HTTPRequestCompleted_t callresult
//-----------------------------------------------------------------------------
void CAvatarImage::OnHTTPRequestCompleted( HTTPRequestCompleted_t* pInfo, bool bIOFailure )
{
	if( pInfo->m_ulContextValue == m_SteamID.ConvertToUint64() )
	{
		uint32 unBytes;
		Verify( SteamHTTP()->GetHTTPResponseBodySize(pInfo->m_hRequest, &unBytes));
		CUtlBuffer buf;
		buf.EnsureCapacity( unBytes );
		buf.SeekPut( CUtlBuffer::SEEK_HEAD, unBytes );
		Verify( SteamHTTP()->GetHTTPResponseBodyData( pInfo->m_hRequest, ( uint8* )buf.Base(), unBytes ) );

		m_animatedImage.SetImage( &buf );

		// Destroy old textures
		FOR_EACH_VEC( m_textureIDs, i )
		{
			vgui::surface()->DestroyTextureID( m_textureIDs[ i ] );
		}
		m_textureIDs.Purge();

		// Construct textures from the gif data
		do
		{
			// Create a new texture for this frame
			int i = m_textureIDs.AddToTail( vgui::surface()->CreateNewTextureID( true ) );

			// Get the RGBA data for this frame
			int iWide, iTall;
			m_animatedImage.GetSize( iWide, iTall );
			uint8* pDest = new uint8[ iWide * iTall * 4 ];
			m_animatedImage.GetRGBA( &pDest );

			// Bind to the texture
			g_pMatSystemSurface->DrawSetTextureRGBAEx2( m_textureIDs[ i ], pDest, iWide, iTall, IMAGE_FORMAT_RGBA8888, true );
			delete[] pDest;
		} while( !m_animatedImage.NextFrame() );

		m_bAnimating = true;

		steamapicontext->SteamHTTP()->ReleaseHTTPRequest( pInfo->m_hRequest );
	}
}

void CAvatarImage::UpdateAvatarImageSize()
{
	int nTall = GetAvatarTall();

	EAvatarSize eNewSize = k_EAvatarSize32x32;
	if ( nTall > 32 )
		eNewSize = k_EAvatarSize64x64;
	if ( nTall > 64 )
		eNewSize = k_EAvatarSize184x184;

	if ( m_AvatarSize != eNewSize )
		m_bLoadPending = true;

	m_AvatarSize = eNewSize;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImage::LoadAnimatedAvatar()
{
	if( steamapicontext->SteamHTTP() && steamapicontext->SteamFriends()->BHasEquippedProfileItem( m_SteamID, k_ECommunityProfileItemType_AnimatedAvatar ) )
	{
		const char* pszAvatarUrl = SteamFriends()->GetProfileItemPropertyString( m_SteamID, k_ECommunityProfileItemType_AnimatedAvatar, k_ECommunityProfileItemProperty_ImageSmall );

		HTTPRequestHandle hRequest = steamapicontext->SteamHTTP()->CreateHTTPRequest( k_EHTTPMethodGET, pszAvatarUrl );
		if( hRequest == INVALID_HTTPREQUEST_HANDLE )
		{
			// Try again next tick
			m_bValid = false;
			return;
		}

		SteamAPICall_t hSendCall;
		steamapicontext->SteamHTTP()->SetHTTPRequestContextValue( hRequest, m_SteamID.ConvertToUint64() );
		if( !steamapicontext->SteamHTTP()->SendHTTPRequest( hRequest, &hSendCall ) )
		{
			// Try again next tick
			steamapicontext->SteamHTTP()->ReleaseHTTPRequest( hRequest );
			m_bValid = false;
			return;
		}
		m_sHTTPRequestCompletedCallback.Set( hSendCall, this, &CAvatarImage::OnHTTPRequestCompleted );
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImage::LoadStaticAvatar()
{
	if( !steamapicontext->SteamFriends()->RequestUserInformation( m_SteamID, false ) )
	{
		int iAvatar = 0;
		switch( m_AvatarSize )
		{
		case k_EAvatarSize32x32:
			iAvatar = steamapicontext->SteamFriends()->GetSmallFriendAvatar( m_SteamID );
			break;
		case k_EAvatarSize64x64:
			iAvatar = steamapicontext->SteamFriends()->GetMediumFriendAvatar( m_SteamID );
			break;
		case k_EAvatarSize184x184:
			iAvatar = steamapicontext->SteamFriends()->GetLargeFriendAvatar( m_SteamID );
			break;
		}

		//Msg( "Got avatar %d for SteamID %llud (%s)\n", iAvatar, m_SteamID.ConvertToUint64(), steamapicontext->SteamFriends()->GetFriendPersonaName( m_SteamID ) );

		if( iAvatar > 0 ) // if its zero, user doesn't have an avatar.  If -1, Steam is telling us that it's fetching it
		{
			uint32 wide = 0, tall = 0;
			if( steamapicontext->SteamUtils()->GetImageSize( iAvatar, &wide, &tall ) && wide > 0 && tall > 0 )
			{
				int destBufferSize = wide * tall * 4;
				byte* rgbDest = ( byte* )stackalloc( destBufferSize );
				if( steamapicontext->SteamUtils()->GetImageRGBA( iAvatar, rgbDest, destBufferSize ) )
					InitFromRGBA( iAvatar, rgbDest, wide, tall );

				stackfree( rgbDest );
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: load the avatar image if we have a load pending
//-----------------------------------------------------------------------------
void CAvatarImage::LoadAvatarImage()
{
	UpdateAvatarImageSize();

#ifdef CSS_PERF_TEST
	return;
#endif
	// attempt to retrieve the avatar image from Steam
	if ( m_bLoadPending && steamapicontext->SteamFriends() && steamapicontext->SteamUtils() && gpGlobals->curtime >= m_fNextLoadTime )
	{
		LoadStaticAvatar();
		if( cl_animated_avatars.GetBool() )
			LoadAnimatedAvatar();

		if ( m_bValid )
		{
			// if we have a valid image, don't attempt to load it again
			m_bLoadPending = false;
		}
		else
		{
			// otherwise schedule another attempt to retrieve the image
			m_fNextLoadTime = gpGlobals->curtime + 1.0f;
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Query Steam to set the m_bFriend status flag
//-----------------------------------------------------------------------------
void CAvatarImage::UpdateFriendStatus( void )
{
	if ( !m_SteamID.IsValid() )
		return;

	if ( steamapicontext->SteamFriends() && steamapicontext->SteamUtils() )
		m_bFriend = steamapicontext->SteamFriends()->HasFriend( m_SteamID, k_EFriendFlagImmediate );
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the surface with the supplied raw RGBA image data
//-----------------------------------------------------------------------------
void CAvatarImage::InitFromRGBA( int iAvatar, const byte *rgba, int width, int height )
{
	int iTexIndex = s_AvatarImageCache.Find( AvatarImagePair_t( m_SteamID, iAvatar ) );
	if ( iTexIndex == s_AvatarImageCache.InvalidIndex() )
	{
		m_textureIDs[ 0 ] = vgui::surface()->CreateNewTextureID(true);
		g_pMatSystemSurface->DrawSetTextureRGBAEx2( m_textureIDs[ 0 ], rgba, width, height, IMAGE_FORMAT_RGBA8888, true );
		iTexIndex = s_AvatarImageCache.Insert( AvatarImagePair_t( m_SteamID, iAvatar ) );
		s_AvatarImageCache[ iTexIndex ] = m_textureIDs[ 0 ];
	}
	else
		m_textureIDs[ 0 ] = s_AvatarImageCache[iTexIndex];
	
	m_bValid = true;
}

//-----------------------------------------------------------------------------
// Purpose: Draw the image and optional friend icon
//-----------------------------------------------------------------------------
void CAvatarImage::Paint( void )
{
	if ( m_bFriend && m_pFriendIcon && m_bDrawFriend)
	{
		m_pFriendIcon->DrawSelf( m_nX, m_nY, m_wide, m_tall, m_Color );
	}

	int posX = m_nX;
	int posY = m_nY;

	if (m_bDrawFriend)
	{
		posX += FRIEND_ICON_AVATAR_INDENT_X * m_avatarWide / DEFAULT_AVATAR_SIZE;
		posY += FRIEND_ICON_AVATAR_INDENT_Y * m_avatarTall / DEFAULT_AVATAR_SIZE;
	}

	UpdateAvatarImageSize();
	
	if ( m_bLoadPending )
	{
		LoadAvatarImage();
	}

	int iTextureToDraw = m_textureIDs[ 0 ];

	// if we are an animated image, update the frame if needed
	if ( m_bAnimating )
	{
		if( m_animatedImage.ShouldIterateFrame() )
			m_animatedImage.NextFrame();

		iTextureToDraw = m_textureIDs[ m_animatedImage.GetSelectedFrame() ];
	}

	if ( m_bValid )
	{
		vgui::surface()->DrawSetTexture( iTextureToDraw );
		vgui::surface()->DrawSetColor( m_Color );
		vgui::surface()->DrawTexturedRect(posX, posY, posX + m_avatarWide, posY + m_avatarTall);
	}
	else if (m_pDefaultImage)
	{
		// draw default
		m_pDefaultImage->SetSize(m_avatarWide, m_avatarTall);
		m_pDefaultImage->SetPos(posX, posY);
		m_pDefaultImage->SetColor(m_Color);
		m_pDefaultImage->Paint();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Set the avatar size; scale the total image and friend icon to fit
//-----------------------------------------------------------------------------
void CAvatarImage::SetAvatarSize(int wide, int tall)
{
	m_avatarWide = wide;
	m_avatarTall = tall;

	if (m_bDrawFriend)
	{
		// scale the size of the friend background frame icon
		m_wide = FRIEND_ICON_SIZE_X * m_avatarWide / DEFAULT_AVATAR_SIZE;
		m_tall = FRIEND_ICON_SIZE_Y * m_avatarTall / DEFAULT_AVATAR_SIZE;
	}
	else
	{
		m_wide = m_avatarWide;
		m_tall = m_avatarTall;
	}

	m_bSetDesiredSize = true;

	UpdateAvatarImageSize();
}


//-----------------------------------------------------------------------------
// Purpose: Set the total image size; scale the avatar portion to fit
//-----------------------------------------------------------------------------
void CAvatarImage::SetSize( int wide, int tall )
{
	m_wide = wide;
	m_tall = tall;

	if (m_bDrawFriend)
	{
		// scale the size of the avatar portion based on the total image size
		m_avatarWide = DEFAULT_AVATAR_SIZE * m_wide / FRIEND_ICON_SIZE_X;
		m_avatarTall = DEFAULT_AVATAR_SIZE * m_tall / FRIEND_ICON_SIZE_Y ;
	}
	else
	{
		m_avatarWide = m_wide;
		m_avatarTall = m_tall;
	}
}

bool CAvatarImage::Evict()
{
	return false;
}

int CAvatarImage::GetNumFrames()
{
	return 0;
}

void CAvatarImage::SetFrame( int nFrame )
{
}

vgui::HTexture CAvatarImage::GetID()
{
	return 0;
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
CAvatarImagePanel::CAvatarImagePanel( vgui::Panel *parent, const char *name ) : BaseClass( parent, name )
{
	m_bScaleImage = false;
	m_pImage = new CAvatarImage();
	m_bSizeDirty = true;
	m_bClickable = false;
}


//-----------------------------------------------------------------------------
// Purpose: Set the avatar by C_BasePlayer pointer
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer( C_BasePlayer *pPlayer, EAvatarSize avatarSize )
{
	if ( pPlayer )
	{
		int iIndex = pPlayer->entindex();
		SetPlayer(iIndex, avatarSize);
	}
	else
		m_pImage->ClearAvatarSteamID();

}


//-----------------------------------------------------------------------------
// Purpose: Set the avatar by entity number
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer( int entindex, EAvatarSize avatarSize )
{
	m_pImage->ClearAvatarSteamID();

	player_info_t pi;
	if ( engine->GetPlayerInfo(entindex, &pi) )
	{
		if ( pi.friendsID != 0 	&& steamapicontext->SteamUtils() )
		{		
			CSteamID steamIDForPlayer( pi.friendsID, 1, GetUniverse(), k_EAccountTypeIndividual );
			SetPlayer(steamIDForPlayer, avatarSize);
		}
		else
		{
			m_pImage->ClearAvatarSteamID();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Set the avatar by SteamID
//-----------------------------------------------------------------------------
void CAvatarImagePanel::SetPlayer(CSteamID steamIDForPlayer, EAvatarSize avatarSize )
{
	m_pImage->ClearAvatarSteamID();

	if (steamIDForPlayer.GetAccountID() != 0 )
		m_pImage->SetAvatarSteamID( steamIDForPlayer, avatarSize );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CAvatarImagePanel::PaintBackground( void )
{
	if ( m_bSizeDirty )
		UpdateSize();

	m_pImage->Paint();
}

void CAvatarImagePanel::ClearAvatar()
{
	m_pImage->ClearAvatarSteamID();
}

void CAvatarImagePanel::SetDefaultAvatar( vgui::IImage* pDefaultAvatar )
{
	m_pImage->SetDefaultImage(pDefaultAvatar);
}

void CAvatarImagePanel::SetAvatarSize( int width, int height )
{
	if ( m_bScaleImage )
	{
		// panel is charge of image size - setting avatar size this way not allowed
		Assert(false);
		return;
	}
	else
	{
		m_pImage->SetAvatarSize( width, height );
		m_bSizeDirty = true;
	}
}

void CAvatarImagePanel::OnSizeChanged( int newWide, int newTall )
{
	BaseClass::OnSizeChanged(newWide, newTall);
	m_bSizeDirty = true;
}

void CAvatarImagePanel::OnMousePressed(vgui::MouseCode code)
{
	if ( !m_bClickable || code != MOUSE_LEFT )
		return;

	PostActionSignal( new KeyValues("AvatarMousePressed") );

	// audible feedback
	const char *soundFilename = "ui/buttonclick.wav";

	vgui::surface()->PlaySound( soundFilename );
}

void CAvatarImagePanel::SetShouldScaleImage( bool bScaleImage )
{
	m_bScaleImage = bScaleImage;
	m_bSizeDirty = true;
}

void CAvatarImagePanel::SetShouldDrawFriendIcon( bool bDrawFriend )
{
	m_pImage->SetDrawFriend(bDrawFriend);
	m_bSizeDirty = true;
}

void CAvatarImagePanel::UpdateSize()
{
	if ( m_bScaleImage )
	{
		// the panel is in charge of the image size
		m_pImage->SetAvatarSize(GetWide(), GetTall());
	}
	else
	{
		// the image is in charge of the panel size
		SetSize(m_pImage->GetAvatarWide(), m_pImage->GetAvatarTall() );
	}

	m_bSizeDirty = false;
}

void CAvatarImagePanel::ApplySettings( KeyValues *inResourceData )
{
	m_bScaleImage = inResourceData->GetInt("scaleImage", 0);

	BaseClass::ApplySettings(inResourceData);
}

int CGIFHelper::ReadData( GifFileType* pFile, GifByteType* pBuffer, int cubBuffer )
{
	auto pBuf = ( CUtlBuffer* )pFile->UserData;

	int nBytesToRead = MIN( cubBuffer, pBuf->GetBytesRemaining() );
	if( nBytesToRead > 0 )
		pBuf->Get( pBuffer, nBytesToRead );

	return nBytesToRead;
}

bool CGIFHelper::SetImage( CUtlBuffer* pData )
{
	if( m_pImage )
	{
		CloseImage();
	}

	int nError;
	m_pImage = DGifOpen( pData, ReadData, &nError );
	if( !m_pImage )
	{
		DevWarning( "[CGIFHelper] Failed to open GIF image: %s\n", GifErrorString( nError ) );
		return false;
	}

	if( DGifSlurp( m_pImage ) != GIF_OK )
	{
		DevWarning( "[CGIFHelper] Failed to slurp GIF image: %s\n", GifErrorString( m_pImage->Error ) );
		CloseImage();
		return false;
	}

	return true;
}

void CGIFHelper::CloseImage( void )
{
	if( !m_pImage )
		return;

	int nError;
	if( DGifCloseFile( m_pImage, &nError ) != GIF_OK )
	{
		DevWarning( "[CGIFHelper] Failed to close GIF image: %s\n", GifErrorString( nError ) );
	}
	m_pImage = NULL;
	m_iSelectedFrame = 0;
}

bool CGIFHelper::NextFrame( void )
{
	if( !m_pImage )
		return false;

	m_iSelectedFrame++;

	bool bLooped = false;
	if( m_iSelectedFrame >= m_pImage->ImageCount )
	{
		// Loop
		m_iSelectedFrame = 0;
		bLooped = true;
	}

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		m_dIterateTime = ( gcb.DelayTime * 0.01 ) + Plat_FloatTime();
	}

	return bLooped;
}

void CGIFHelper::GetRGBA( uint8** ppOutFrameData )
{
	if( !m_pImage )
		return;

	SavedImage* pFrame = &m_pImage->SavedImages[ m_iSelectedFrame ];

	ColorMapObject* pColorMap = pFrame->ImageDesc.ColorMap ? pFrame->ImageDesc.ColorMap : m_pImage->SColorMap;
	GifByteType* pRasterBits = pFrame->RasterBits;
	int iWidth = pFrame->ImageDesc.Width;
	int iHeight = pFrame->ImageDesc.Height;

	int iPixel = 0;
	for( int y = 0; y < iHeight; y++ )
	{
		for( int x = 0; x < iWidth; x++ )
		{
			GifColorType& color = pColorMap->Colors[ pRasterBits[ iPixel ] ];
			ppOutFrameData[ 0 ][ iPixel * 4 + 0 ] = color.Red;
			ppOutFrameData[ 0 ][ iPixel * 4 + 1 ] = color.Green;
			ppOutFrameData[ 0 ][ iPixel * 4 + 2 ] = color.Blue;
			ppOutFrameData[ 0 ][ iPixel * 4 + 3 ] = 255; // We don't care about transparency
			iPixel++;
		}
	}
}

void CGIFHelper::GetSize( int& iWidth, int& iHeight ) const
{
	if( !m_pImage )
	{
		iWidth = iHeight = 0;
		return;
	}

	GifImageDesc& imageDesc = m_pImage->SavedImages[ m_iSelectedFrame ].ImageDesc;
	iWidth = imageDesc.Width;
	iHeight = imageDesc.Height;
}

bool CGIFHelper::ShouldIterateFrame( void ) const
{
	return m_dIterateTime < Plat_FloatTime();
}