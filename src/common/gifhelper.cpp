#include "cbase.h"
#include "tier0/vprof.h"
#include "gifhelper.h"
#include "gif_lib.h"

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
int CGIFHelper::ReadData( GifFileType* pFile, GifByteType* pBuffer, int cubBuffer )
{
	auto pBuf = ( CUtlBuffer* )pFile->UserData;

	int nBytesToRead = Min( cubBuffer, pBuf->GetBytesRemaining() );
	if( nBytesToRead > 0 )
		pBuf->Get( pBuffer, nBytesToRead );

	return nBytesToRead;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
bool CGIFHelper::OpenImage( CUtlBuffer* pBuf )
{
	if( m_pImage )
	{
		CloseImage();
	}

	int nError;
	m_pImage = DGifOpen( pBuf, ReadData, &nError );
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

	int iWide, iTall;
	GetScreenSize( iWide, iTall );
	m_pPrevFrameBuffer = new uint8[ iWide * iTall * 4 ];
	GetRGBA( m_pPrevFrameBuffer );

	return true;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CGIFHelper::CloseImage( void )
{
	if( !m_pImage )
		return;

	delete[] m_pPrevFrameBuffer;

	int nError;
	if( DGifCloseFile( m_pImage, &nError ) != GIF_OK )
	{
		DevWarning( "[CGIFHelper] Failed to close GIF image: %s\n", GifErrorString( nError ) );
	}
	m_pImage = NULL;
	m_pPrevFrameBuffer = NULL;
	m_iSelectedFrame = 0;
	m_dIterateTime = 0.0;
}

//-----------------------------------------------------------------------------
// Purpose: Iterates the current frame index
// Output : true - looped back to frame 0
//-----------------------------------------------------------------------------
bool CGIFHelper::NextFrame( void )
{
	if( !m_pImage )
		return false;

	m_iSelectedFrame++;

	if( m_iSelectedFrame >= m_pImage->ImageCount )
	{
		// Loop
		m_iSelectedFrame = 0;
	}

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		// simulates web browsers "throttling" short time delays so
		// gif animation speed is similar to Steam's
		static const double k_dMinTime = .02, k_dDefaultTime = .1; // Chrome defaults

		double dDelayTime = gcb.DelayTime * .01;
		m_dIterateTime = ( dDelayTime < k_dMinTime ? k_dDefaultTime : dDelayTime ) + Plat_FloatTime();
	}

	return m_iSelectedFrame == 0;
}

//-----------------------------------------------------------------------------
// Purpose: Gets the total number of frames in the open image
// Output : int
//-----------------------------------------------------------------------------
int CGIFHelper::GetFrameCount( void ) const
{
	return m_pImage ? m_pImage->ImageCount : 0;
}

//-----------------------------------------------------------------------------
// Purpose: Gets the current frame as an RGBA buffer
// Input  :	pOutFrameBuffer - where should the buffer be copied to, size
//							  needs to be iScreenWide * iScreenTall * 4
//-----------------------------------------------------------------------------
void CGIFHelper::GetRGBA( uint8* pOutFrameBuffer )
{
	VPROF( "CGIFHelper::GetRGBA" );

	if( !m_pImage )
		return;

	static const int k_nBytesPerPixel = 4;

	GifImageDesc &imageDesc = m_pImage->SavedImages[ m_iSelectedFrame ].ImageDesc;
	ColorMapObject *pColorMap = imageDesc.ColorMap ? imageDesc.ColorMap : m_pImage->SColorMap;

	int iScreenWide, iScreenTall;
	GetScreenSize( iScreenWide, iScreenTall );

	const int nScreenStride = iScreenWide * k_nBytesPerPixel;

	int nTransparentIndex = NO_TRANSPARENT_COLOR;
	int nDisposalMethod = DISPOSAL_UNSPECIFIED;

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		nTransparentIndex = gcb.TransparentColor;
		nDisposalMethod = gcb.DisposalMode;
	}

	Q_memcpy( pOutFrameBuffer, m_pPrevFrameBuffer, nScreenStride * iScreenTall );

	auto lambdaComputeFrame = [ & ]( int nRowOffset = 0, int nRowIncrement = 1 )
	{
		int iPixel = nRowOffset * imageDesc.Width;
		for ( int y = nRowOffset; y < imageDesc.Height; y += nRowIncrement )
		{
			const int iScreenY = y + imageDesc.Top;
			if ( iScreenY >= iScreenTall ) { iPixel += imageDesc.Width; continue; }

			uint8 *pDest = pOutFrameBuffer + ( iScreenY * nScreenStride ) + ( imageDesc.Left * k_nBytesPerPixel );
			for ( int x = 0; x < imageDesc.Width; x++, iPixel++ )
			{
				const int iScreenX = x + imageDesc.Left;
				if ( iScreenX >= iScreenWide ) continue;

				GifByteType idx = m_pImage->SavedImages[ m_iSelectedFrame ].RasterBits[ iPixel ];
				if ( idx < pColorMap->ColorCount && idx != nTransparentIndex )
				{
					const GifColorType &color = pColorMap->Colors[ idx ];
					pDest[ 0 ] = color.Red;
					pDest[ 1 ] = color.Green;
					pDest[ 2 ] = color.Blue;
					pDest[ 3 ] = 255;
				}
				pDest += k_nBytesPerPixel;
			}
		}
	};
	
	if ( imageDesc.Interlace )
	{
		// https://giflib.sourceforge.net/gifstandard/GIF89a.html#interlacedimages
		static const int k_rowOffsets[] = { 0, 4, 2, 1 };
		static const int k_rowIncrements[] = { 8, 8, 4, 2 };

		for ( int nPass = 0; nPass < 4; nPass++ )
		{
			lambdaComputeFrame( k_rowOffsets[ nPass ], k_rowIncrements[ nPass ] );
		}
	}
	else
	{
		lambdaComputeFrame();
	}

	// update prev frame buffer depending on disposal method
	switch ( nDisposalMethod )
	{
	case DISPOSE_BACKGROUND:
		if ( m_pImage->SBackGroundColor < m_pImage->SColorMap->ColorCount )
		{
			const GifColorType &color =	m_pImage->SColorMap->Colors[ m_pImage->SBackGroundColor ];
			const int iFillTall = Min( imageDesc.Height, iScreenTall - imageDesc.Top );
			const int iFillWide = Min( imageDesc.Width, iScreenWide - imageDesc.Left );
			uint32 unFillColor = ( color.Red ) | ( color.Green << 8 ) | ( color.Blue << 16 ) | ( 0xFF << 24 );
			for ( int y = 0; y < iFillTall; ++y )
			{
				uint32 *pRow = reinterpret_cast< uint32 * >( m_pPrevFrameBuffer + ( ( y + imageDesc.Top ) * nScreenStride ) + imageDesc.Left * k_nBytesPerPixel );
				for ( int x = 0; x < iFillWide; ++x )
				{
					pRow[ x ] = unFillColor;
				}
			}
		}
	case DISPOSE_PREVIOUS:
		break;
	case DISPOSAL_UNSPECIFIED:
	case DISPOSE_DO_NOT:
	default:
		Q_memcpy( m_pPrevFrameBuffer, pOutFrameBuffer, nScreenStride * iScreenTall );
		break;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Gets the size of the logical screen in pixels
//-----------------------------------------------------------------------------
void CGIFHelper::GetScreenSize( int& iWide, int& iTall ) const
{
	if( !m_pImage )
	{
		iWide = iTall = 0;
		return;
	}

	iWide = m_pImage->SWidth;
	iTall = m_pImage->SHeight;
}
