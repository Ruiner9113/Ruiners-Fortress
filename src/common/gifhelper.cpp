#include "cbase.h"
#include "tier0/vprof.h"
#include "gifhelper.h"
#include "gif_lib.h"
#include "pixelwriter.h"

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
int CGIFHelper::ReadData( GifFileType* pFile, GifByteType* pBuffer, int cubBuffer )
{
	auto pBuf = ( CUtlBuffer* )pFile->UserData;

	int nBytesToRead = MIN( cubBuffer, pBuf->GetBytesRemaining() );
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
	GetRGBA( &m_pPrevFrameBuffer );

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
// Purpose: Gets the current frame as an RGBA buffer
// Input  :	ppOutFrameBuffer - where should the buffer be copied to, size
//							   needs to be iScreenWide * iScreenTall * 4
//-----------------------------------------------------------------------------
void CGIFHelper::GetRGBA( uint8** ppOutFrameBuffer )
{
	VPROF( "CGIFHelper::GetRGBA" );

	if( !m_pImage )
		return;

	GifImageDesc &imageDesc = m_pImage->SavedImages[ m_iSelectedFrame ].ImageDesc;
	ColorMapObject *pColorMap = imageDesc.ColorMap ? imageDesc.ColorMap : m_pImage->SColorMap;

	int iScreenWide, iScreenTall;
	GetScreenSize( iScreenWide, iScreenTall );

	int nTransparentIndex = NO_TRANSPARENT_COLOR;
	int nDisposalMethod = DISPOSAL_UNSPECIFIED;

	GraphicsControlBlock gcb;
	if( DGifSavedExtensionToGCB( m_pImage, m_iSelectedFrame, &gcb ) == GIF_OK )
	{
		nTransparentIndex = gcb.TransparentColor;
		nDisposalMethod = gcb.DisposalMode;
	}

	uint8 *pCurFrameBuffer = ( uint8 * )stackalloc( iScreenWide * iScreenTall * 4 );
	Q_memcpy( pCurFrameBuffer, m_pPrevFrameBuffer, iScreenWide * iScreenTall * 4 );

	CPixelWriter pixelWriter;
	pixelWriter.SetPixelMemory(
		IMAGE_FORMAT_RGBA8888,
		pCurFrameBuffer,
		iScreenWide * 4
	);

	int iPixel = 0;
	auto lambdaComputeFrame = [ & ]( int nRowOffset = 0, int nRowIncrement = 1 )
	{
		for ( int y = nRowOffset; y < imageDesc.Height; y += nRowIncrement )
		{
			int iScreenY = y + imageDesc.Top;
			if ( iScreenY >= iScreenTall ) continue;

			pixelWriter.Seek( imageDesc.Left, iScreenY );

			for ( int x = 0; x < imageDesc.Width; x++ )
			{
				int iScreenX = x + imageDesc.Left;
				if ( iScreenX >= iScreenWide ) { iPixel++; pixelWriter.SkipPixels( 1 ); continue; }

				GifByteType idx = m_pImage->SavedImages[ m_iSelectedFrame ].RasterBits[ iPixel++ ];
				if ( idx < pColorMap->ColorCount && idx != nTransparentIndex )
				{
					const GifColorType &color = pColorMap->Colors[ idx ];
					pixelWriter.WritePixel( color.Red, color.Green, color.Blue, 255 );
				}
				else
				{
					pixelWriter.SkipPixels( 1 );
				}
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

	Q_memcpy( *ppOutFrameBuffer, pCurFrameBuffer, iScreenWide * iScreenTall * 4 );

	// update prev frame buffer depending on disposal method
	switch( nDisposalMethod )
	{
	case DISPOSE_BACKGROUND:
	{
		pixelWriter.SetPixelMemory(
			IMAGE_FORMAT_RGBA8888,
			m_pPrevFrameBuffer,
			iScreenWide * 4
		);

		if ( m_pImage->SBackGroundColor < m_pImage->SColorMap->ColorCount )
		{
			const GifColorType &color = m_pImage->SColorMap->Colors[ m_pImage->SBackGroundColor ];

			for ( int y = imageDesc.Top; y < imageDesc.Top + imageDesc.Height && y < iScreenTall; y++ )
			{
				pixelWriter.Seek( imageDesc.Left, y );
				for ( int x = 0; x < imageDesc.Left && ( x + imageDesc.Left ) < iScreenWide; x++ )
				{
					pixelWriter.WritePixel( color.Red, color.Green, color.Blue, 255 );
				}
			}
		}
		break;
	}
	case DISPOSE_PREVIOUS:
		break;
	case DISPOSAL_UNSPECIFIED:
	case DISPOSE_DO_NOT:
	default:
		Q_memcpy( m_pPrevFrameBuffer, pCurFrameBuffer, iScreenWide * iScreenTall * 4 );
		break;
	}

	stackfree( pCurFrameBuffer );
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
