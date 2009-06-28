/*
Copyright © Joan Charmant 2008-2009.
joan.charmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/

#include "stdafx.h"
#include <string.h>
#include <stdlib.h>
#include "VideoFile.h"

using namespace System::Diagnostics;
using namespace System::Drawing;
using namespace System::Drawing::Drawing2D;
//using namespace System::Drawing::Imaging; // We can't use it because System::Drawing::Imaging::PixelFormat clashes with FFMpeg.
using namespace System::IO;
using namespace System::Runtime::InteropServices;

namespace Kinovea
{
	namespace VideoFiles 
	{

// --------------------------------------- Construction/Destruction
VideoFile::VideoFile()
{
	log->Debug("Constructing VideoFile.");

	// FFMpeg init.
	av_register_all();
	avcodec_init();
	avcodec_register_all();

	// Data init.
	m_bIsLoaded = false;
	m_iVideoStream = -1;

	m_InfosVideo = gcnew InfosVideo();
	ResetInfosVideo();

	m_PrimarySelection = gcnew PrimarySelection();
	ResetPrimarySelection();

}
VideoFile::~VideoFile()
{
	if(m_bIsLoaded)
	{
		Unload();
	}
}
///<summary>
/// Finalizer. Attempt to free resources and perform other cleanup operations before the Object is reclaimed by garbage collection.
///</summary>
VideoFile::!VideoFile()
{
	if(m_bIsLoaded)
	{
		Unload();
	}
}

// --------------------------------------- Public Methods
/// <summary>
/// Loads specified file in the VideoFile instance.
/// </summary>
LoadResult VideoFile::Load(String^ _FilePath)
{

	LoadResult result = LoadResult::Success;

	int					iCurrentStream			= 0;
	int					iVideoStreamIndex		= -1;
	int					iMetadataStreamIndex	= -1;

	AVFormatContext*	pFormatCtx; 
	AVCodecContext*		pCodecCtx;
	AVCodec*			pCodec;
	int					iTranscodedFrames	= 0;
	bool				bNeedTranscoding	= false;

	m_FilePath = _FilePath;

	char*				cFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

	if(m_bIsLoaded) Unload();

	log->Debug("---------------------------------------------------");
	log->Debug("Entering : LoadMovie()");
	log->Debug("Input File : " + _FilePath);
	log->Debug("");


	do
	{
		// 0. Récupérer la taille du fichier
		FileInfo^ fileInfo = gcnew FileInfo(_FilePath);
		m_InfosVideo->iFileSize = fileInfo->Length;
		log->Debug(String::Format("File Size:{0}.", m_InfosVideo->iFileSize));

		// 1. Ouvrir le fichier et récupérer les infos sur le format.
		if(av_open_input_file(&pFormatCtx, cFilePath , NULL, 0, NULL) != 0)
		{
			result = LoadResult::FileNotOpenned;
			log->Debug("The file could not be openned. (Wrong path or not a video.)");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(cFilePath));

		// 2. Obtenir les infos sur les streams contenus dans le fichier.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			result = LoadResult::StreamInfoNotFound;
			log->Debug("The streams Infos were not Found.");
			break;
		}
		DumpStreamsInfos(pFormatCtx);

		// 3. Obtenir l'identifiant du premier stream de sous titre.
		iMetadataStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_SUBTITLE);

		// 4. Vérifier que ce stream est bien notre stream de meta données et pas un stream de sous-titres classique.
		// + Le parseur de meta données devra également être blindé contre les fichiers malicieusement malformés.
		if(iMetadataStreamIndex >= 0)
		{
			if( (pFormatCtx->streams[iMetadataStreamIndex]->codec->codec_id != CODEC_ID_TEXT) ||
				(strcmp((char*)pFormatCtx->streams[iMetadataStreamIndex]->language, "XML") != 0) )
			{
				log->Debug("Subtitle stream found, but not analysis meta data: will be ignored.");
				iMetadataStreamIndex = -1;
			}
		}

		// 5. Obtenir l'identifiant du premier stream vidéo.
		if( (iVideoStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_VIDEO)) < 0 )
		{
			result = LoadResult::VideoStreamNotFound;
			log->Debug("No Video stream found in the file. (File is audio only, or video stream is broken.)");
			break;
		}

		// 6. Obtenir un objet de paramètres du codec vidéo.
		pCodecCtx = pFormatCtx->streams[iVideoStreamIndex]->codec;
		log->Debug("Codec							: " + gcnew String(pCodecCtx->codec_name));
		
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			result = LoadResult::CodecNotFound;
			log->Debug("No suitable codec to decode the video. (Worse than an unsupported codec.)");
			break;
		}

		// 7. Ouvrir le Codec vidéo.
		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			result = LoadResult::CodecNotOpened;
			log->Debug("Codec could not be openned. (Codec known, but not supported yet.)");
			break;
		}

		//------------------------------------------------------------------------------------------------
		// [2007-09-14]
		// On utilise plus que les timestamps pour se localiser dans la vidéo.
		// (Par pourcentage du total de timestamps)
		//------------------------------------------------------------------------------------------------
		log->Debug("Duration (frames) if available	: " + pFormatCtx->streams[iVideoStreamIndex]->nb_frames);
		log->Debug("Duration (µs)					: " + pFormatCtx->duration);
		log->Debug("Format[Stream] timeBase			: " + pFormatCtx->streams[iVideoStreamIndex]->time_base.den + ":" + pFormatCtx->streams[iVideoStreamIndex]->time_base.num);
		log->Debug("Codec timeBase					: " + pCodecCtx->time_base.den + ":" + pCodecCtx->time_base.num);

		m_InfosVideo->fAverageTimeStampsPerSeconds = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
		log->Debug("Average timestamps per seconds	: " + m_InfosVideo->fAverageTimeStampsPerSeconds);
		
		// Compute total duration in TimeStamps.
		if(pFormatCtx->duration > 0)
		{
			// av_rescale ?
			m_InfosVideo->iDurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration/(double)AV_TIME_BASE)*m_InfosVideo->fAverageTimeStampsPerSeconds);
		}
		else
		{
			// todo : try SuperSeek technique. Seek @ +10 Hours, to get the last I-Frame
			m_InfosVideo->iDurationTimeStamps = 0;
		}
		log->Debug("Duration in timestamps			: " + m_InfosVideo->iDurationTimeStamps);
		log->Debug("Duration in seconds	(computed)	: " + (double)(double)m_InfosVideo->iDurationTimeStamps/(double)m_InfosVideo->fAverageTimeStampsPerSeconds);


		//----------------------------------------------------------------------------------------------------------------------
		// FPS Moyen.
		// Sur un Play, la cadence des frames ne reflètera pas forcément la vraie cadence si le fichier à un framerate variable.
		// On considère que c'est un cas rare et que la différence ne va pas trop géner.
		// 
		// Trois sources pour calculer le FPS moyen, à tester dans l'ordre :
		//
		//  - les infos de duration en frames et en µs, du conteneur et du stream. (Rarement disponibles, mais valide si oui)
		//	- le Stream->time_base	(Souvent ko, sous la forme de 90000:1, sert en fait à exprimer l'unité des timestamps)
		//  - le Codec->time_base	(Souvent ok, mais pas toujours.)
		//
		//----------------------------------------------------------------------------------------------------------------------
		m_InfosVideo->fFps = 0;
		m_InfosVideo->bFpsIsReliable = true;
		//-----------------------
		// 1.a. Par les durations
		//-----------------------
		if( (pFormatCtx->streams[iVideoStreamIndex]->nb_frames > 0) && (pFormatCtx->duration > 0))
		{	
			m_InfosVideo->fFps = ((double)pFormatCtx->streams[iVideoStreamIndex]->nb_frames * (double)AV_TIME_BASE)/(double)pFormatCtx->duration;
			log->Debug("Average Fps estimation method	: Durations.");
		}
		else
		{
		
			//-------------------------------------------------------
			// 1.b. Par le Stream->time_base, on invalide si >= 1000.
			//-------------------------------------------------------
			m_InfosVideo->fFps  = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
			
			if(m_InfosVideo->fFps < 1000)
			{
				log->Debug("Average Fps estimation method	: Format[Stream] context timebase.");
			}
			else
			{
				//------------------------------------------------------
				// 1.c. Par le Codec->time_base, on invalide si >= 1000.
				//------------------------------------------------------
				m_InfosVideo->fFps = (double)pCodecCtx->time_base.den / (double)pCodecCtx->time_base.num;

				if(m_InfosVideo->fFps < 1000)
				{
					log->Debug("Average Fps estimation method	: Codec context timebase.");
				}
				else
				{
					//---------------------------------------------------------------------------
					// Le fichier ne nous donne pas assez d'infos, ou le frame rate est variable.
					// Forcer à 25 fps. 
					//---------------------------------------------------------------------------
					m_InfosVideo->fFps = 25;
					m_InfosVideo->bFpsIsReliable = false;
					log->Debug("Average Fps estimation method	: Estimation failed. Fps Forced to : " + m_InfosVideo->fFps);
				}
			}
		}

		log->Debug("Average Fps						: " + m_InfosVideo->fFps);
		
		m_InfosVideo->iFrameInterval = (int)((double)1000/m_InfosVideo->fFps);
		log->Debug("Average Frame Interval (ms)		: " + m_InfosVideo->iFrameInterval);


		// av_rescale ?
		if(pFormatCtx->start_time > 0)
			m_InfosVideo->iFirstTimeStamp = (int64_t)((double)((double)pFormatCtx->start_time/(double)AV_TIME_BASE)*m_InfosVideo->fAverageTimeStampsPerSeconds);
		else
			m_InfosVideo->iFirstTimeStamp = 0;
	
		log->Debug("Start time (µs)					: " + pFormatCtx->start_time);
		log->Debug("First timestamp					: " + m_InfosVideo->iFirstTimeStamp);

		// Précomputations.
		m_InfosVideo->iAverageTimeStampsPerFrame = (int64_t)Math::Round(m_InfosVideo->fAverageTimeStampsPerSeconds / m_InfosVideo->fFps);

		//----------------
		// Other datas.
		//----------------
		m_InfosVideo->iWidth = pCodecCtx->width; 
		m_InfosVideo->iHeight = pCodecCtx->height;
		
		log->Debug("Width (pixels)					: " + pCodecCtx->width);
		log->Debug("Height (pixels)					: " + pCodecCtx->height);

		if(pCodecCtx->sample_aspect_ratio.num != 0 && pCodecCtx->sample_aspect_ratio.num != pCodecCtx->sample_aspect_ratio.den)
		{
			// Anamorphic video, non square pixels.
			log->Debug("Display Aspect Ratio type		: Anamorphic");

			if(pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO)
			{
				// If MPEG, sample_aspect_ratio is actually the DAR...
				// Reference for weird decision tree: mpeg12.c at mpeg_decode_postinit().
				double fDisplayAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
				m_InfosVideo->fPixelAspectRatio	= ((double)pCodecCtx->height * fDisplayAspectRatio) / (double)pCodecCtx->width;

				if(m_InfosVideo->fPixelAspectRatio < 1.0f)
				{
					m_InfosVideo->fPixelAspectRatio = fDisplayAspectRatio;
				}
			}
			else
			{
				m_InfosVideo->fPixelAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
			}	
				
			log->Debug("Pixel Aspect Ratio				: " + m_InfosVideo->fPixelAspectRatio);

			// Change image geometry so the video is displayed in its native display aspect ratio.
			m_InfosVideo->iDecodingHeight = (int)((double)pCodecCtx->height / m_InfosVideo->fPixelAspectRatio);
		}
		else
		{
			// Assume PAR=1:1.
			log->Debug("Display Aspect Ratio type		: Square Pixels");
			m_InfosVideo->fPixelAspectRatio = 1.0f;
			m_InfosVideo->iDecodingHeight = m_InfosVideo->iHeight;
		}

		// Fix unsupported width.
		if(m_InfosVideo->iWidth % 4 != 0)
		{
			m_InfosVideo->iDecodingWidth = 4 * ((m_InfosVideo->iWidth / 4) + 1);
		}
		else
		{
			m_InfosVideo->iDecodingWidth = m_InfosVideo->iWidth;
		}

		if(m_InfosVideo->iDecodingWidth != m_InfosVideo->iWidth)
		{
			log->Debug("Width is changed to 			: " + m_InfosVideo->iDecodingWidth);
		}

		if(m_InfosVideo->iDecodingHeight != m_InfosVideo->iHeight)
		{
			log->Debug("Height is changed to			: " + m_InfosVideo->iDecodingHeight);
		}
		//--------------------------------------------------------
		// Globalize Contexts (for GetNextFrame)
		//--------------------------------------------------------
		m_pFormatCtx	= pFormatCtx;
		m_pCodecCtx		= pCodecCtx;
		m_iVideoStream	= iVideoStreamIndex;
		m_iMetadataStream = iMetadataStreamIndex;

		m_bIsLoaded = true;				

		// Bitrate.
		GetInputBitrate( m_InfosVideo->iWidth, m_InfosVideo->iHeight);

		result = LoadResult::Success;
	}
	while(false);

	//--------------------------------------------------
	// CLEANUP SI KO (?)
	// (On fera un Unload, mais est-ce suffisant ?)
	//--------------------------------------------------
	log->Debug(""); 
	log->Debug("Exiting LoadMovie");
	log->Debug("---------------------------------------------------");

	return result;

}
/// <summary>
/// Unload the video and dispose unmanaged resources.
/// </summary>
void VideoFile::Unload()
{
	if(m_bIsLoaded)
	{
		ResetPrimarySelection();
		ResetInfosVideo();

		// Current AVFrame.
		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
			m_pCurrentDecodedFrameBGR = nullptr;
			delete [] m_Buffer;
			m_Buffer = nullptr;
		}

		// Release images extracted to memory.
		if(m_FrameList)
		{
			for(int i = 0;i<m_FrameList->Count;i++)
			{
				delete m_FrameList[i]->BmpImage;
				delete m_FrameList[i];
			}
		
			delete m_FrameList;
		}

		// FFMpeg-close file and codec.
		if(m_pCodecCtx != nullptr)
		{
			avcodec_close(m_pCodecCtx);
			//av_free(m_pCodecCtx);
		}
		if(m_pFormatCtx != nullptr)
		{
			av_close_input_file(m_pFormatCtx);
			//av_free(m_pFormatCtx);	
		}

		m_bIsLoaded = false;
	}
}
/// <summary>
/// Get the metadata XML string embeded in the file. null if not found.
/// </summary>
String^ VideoFile::GetMetadata()
{
	String^ szMetadata;

	if(m_iMetadataStream >= 0)
	{
		bool done = false;
		do
		{
			AVPacket	InputPacket;
			int			iReadFrameResult;

			if( (iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket)) >= 0)
			{
				log->Debug("GetMetadata, InputPacket, dts:" + InputPacket.dts);
				log->Debug("GetMetadata, InputPacket, stream:" + InputPacket.stream_index);

				if(InputPacket.stream_index == m_iMetadataStream)
				{
					log->Debug("Subtitle Packet found.");
					int test = InputPacket.size;
					
					szMetadata = gcnew String((char*)InputPacket.data);
					log->Debug("Meta Data: " + szMetadata);

					done = true;
				}
				else
				{
					// Not Subtitle stream : Skip packet.
				}
			}
			else
			{
				log->Debug("ERROR: av_read_frame() failed");
				break;
			}
		}
		while(!done);
		
		// Se repositionner au début du fichier
		if(av_seek_frame(m_pFormatCtx, m_iVideoStream, m_InfosVideo->iFirstTimeStamp, AVSEEK_FLAG_BACKWARD) < 0)
		{
			log->Debug("ERROR: av_seek_frame() failed");
		}
	}

	return szMetadata;
}
ReadResult VideoFile::ReadFrame(int64_t _iTimeStampToSeekTo, int _iFramesToDecode)
{
	//--------------------------------------------------------
	// Paramères : 
	// Si _iTimeStampToSeekTo = -1, utiliser _iFramesToDecode.
	// Sinon utiliser _iTimeStampToSeekTo.
	//--------------------------------------------------------

	ReadResult			result = ReadResult::Success;
	int					iReadFrameResult;		// result of av_read_frame. should be >= 0.
	AVFrame*			pDecodingFrameBuffer; 
	
	int					frameFinished;
	int					iSizeBuffer;
	int					iFramesDecoded	= 0;
	int					iFramesToDecode = 0;
	int64_t				iTargetTimeStamp = _iTimeStampToSeekTo;
	bool				bIsSeeking = false;
#ifdef TRACE 
	Stopwatch^ m_DecodeWatch;
	m_DecodeWatch = gcnew Stopwatch();
	m_DecodeWatch->Reset();
    m_DecodeWatch->Start();
#endif

	if(!m_bIsLoaded) return ReadResult::MovieNotLoaded;

	if(m_PrimarySelection->iAnalysisMode == 1)
	{
		if(iTargetTimeStamp >= 0)
		{	
			// Retrouver la frame correspondante au TimeStamp
			m_PrimarySelection->iCurrentFrame = (int)GetFrameNumber(iTargetTimeStamp);
		}
		else
		{
			if(m_PrimarySelection->iCurrentFrame + _iFramesToDecode < 0)
			{
				// ?
				m_PrimarySelection->iCurrentFrame = 0;
			}
			else if(m_PrimarySelection->iCurrentFrame + _iFramesToDecode >= m_FrameList->Count)
			{
				// fin de zone
				m_PrimarySelection->iCurrentFrame = m_FrameList->Count -1;
			}
			else
			{
				// Cas normal.
				m_PrimarySelection->iCurrentFrame += _iFramesToDecode;
			}
		}

		m_BmpImage = m_FrameList[m_PrimarySelection->iCurrentFrame]->BmpImage;
		m_PrimarySelection->iCurrentTimeStamp = m_FrameList[m_PrimarySelection->iCurrentFrame]->iTimeStamp;

		result = ReadResult::Success;
	}
	else
	{
		//------------------------------------------------------------------------
		// Décoder une frame si iTargetTimeStamp à -1, seeker là-bas sinon.
		//------------------------------------------------------------------------
		
		if((iTargetTimeStamp >= 0) || (_iFramesToDecode < 0))
		{	
			bIsSeeking = true;
			
			if(_iFramesToDecode < 0)
			{
				// Déplacement négatif : retrouver le timestamp.
				iTargetTimeStamp = m_PrimarySelection->iCurrentTimeStamp + (_iFramesToDecode * m_InfosVideo->iAverageTimeStampsPerFrame);
				if(iTargetTimeStamp < 0) iTargetTimeStamp = 0;
			}

			//------------------------------------------------------------------------------------------
			// seek.
			// AVSEEK_FLAG_BACKWARD -> Va à la dernière I-Frame avant la position demandée.
			// necessitera de continuer à décoder tant que le PTS lu est inférieur au TimeStamp demandé.
			//------------------------------------------------------------------------------------------
			log->Debug(String::Format("Seeking to [{0}]", iTargetTimeStamp));
			av_seek_frame(m_pFormatCtx, m_iVideoStream, iTargetTimeStamp, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers( m_pFormatCtx->streams[m_iVideoStream]->codec);
			iFramesToDecode = 1;
		}
		else
		{
			iFramesToDecode = _iFramesToDecode;
		}

		// Allocate video frames, one for decoding, one to hold the picture after conversion.
		pDecodingFrameBuffer = avcodec_alloc_frame();

		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
		}

		m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();

		if( (m_pCurrentDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// Figure out required size for image buffer and allocate it.
			iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			if(iSizeBuffer > 0)
			{
				if(m_Buffer == nullptr)
					m_Buffer = new uint8_t[iSizeBuffer];

				// Assign appropriate parts of buffer to image planes in pFrameBGR
				avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			
				//----------------------
				// Reading/Decoding loop
				//----------------------
				bool done = false;
				bool bFirstPass = true;

				do
				{
					// Read next packet
					AVPacket	InputPacket;
					iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket);

					if(iReadFrameResult >= 0)
					{
						// Is this a packet from the video stream ?
						if(InputPacket.stream_index == m_iVideoStream)
						{
							// Decode video packet. This is needed even if we're not on the final frame yet.
							// I Frames data is kept internally by ffmpeg and we'll need it to build the final frame. 
							avcodec_decode_video(m_pCodecCtx, pDecodingFrameBuffer, &frameFinished, InputPacket.data, InputPacket.size);
							
							if(frameFinished)
							{
								// Update positions
								
								if(InputPacket.dts == AV_NOPTS_VALUE)
								{
									m_PrimarySelection->iCurrentTimeStamp = 0;
								}
								else
								{
									m_PrimarySelection->iCurrentTimeStamp = InputPacket.dts;
								}

								
								if(bIsSeeking && bFirstPass && m_PrimarySelection->iCurrentTimeStamp > iTargetTimeStamp && iTargetTimeStamp >= 0)
								{
									
									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back 1 full second behind
									// the target and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;
									
									// place the new target one second before the original one.
									int64_t iForceSeekTimestamp = iTargetTimeStamp - (int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds;
									if(iForceSeekTimestamp < 0) iForceSeekTimestamp = 0;

									// Do the seek.
									log->Debug(String::Format("First decoded frame [{0}] already after target. Force seek back 1 second to [{1}]", m_PrimarySelection->iCurrentTimeStamp, iForceSeekTimestamp));
									av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&InputPacket);

									// Loop back.
									continue;
								}

								bFirstPass = false;
								
								iFramesDecoded++;

								//-------------------------------------------------------------------------------
								// If we're done, convert the image and store it into its final recipient.
								// - In case of a seek, if we reached the target timestamp.
								// - In case of a linear decoding, if we decoded the required number of frames.
								//-------------------------------------------------------------------------------
								if(	((iTargetTimeStamp >= 0) && (m_PrimarySelection->iCurrentTimeStamp >= iTargetTimeStamp)) ||
									((iTargetTimeStamp < 0)	&& (iFramesDecoded >= iFramesToDecode)))
								{
									done = true;

									//----------------------------------------------------------------
									// Rescale image if needed and convert formats
									// Deinterlace (must be done at original size and pix_fmt YUV420P)
									//-----------------------------------------------------------------

									RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced);

									try
									{
										//------------------------------------------------------------------------------------
										// Accepte uniquement un Stride multiple de 4.
										// Tous les fichiers aux formats non standard sont refusés par le constructeur Bitmap.
										// Todo : padding.
										//------------------------------------------------------------------------------------
										int iImageStride	= m_pCurrentDecodedFrameBGR->linesize[0];
										IntPtr* ptr = new IntPtr((void*)m_pCurrentDecodedFrameBGR->data[0]); 
										
										if(m_BmpImage) delete m_BmpImage;
										m_BmpImage = gcnew Bitmap( m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, iImageStride, Imaging::PixelFormat::Format24bppRgb, *ptr );

										//Console::WriteLine("decoding (6):{0}", m_DecodeWatch->ElapsedMilliseconds);

										/*Bitmap^ tmpImage = gcnew Bitmap( iNewWidth, iNewHeight, iImageStride, Imaging::PixelFormat::Format32bppRgb, *ptr );
										m_BmpImage = gcnew Bitmap(iNewWidth, iNewHeight, Imaging::PixelFormat::Format32bppPArgb);
										Graphics^ g = Graphics::FromImage(m_BmpImage);
										g->PixelOffsetMode = PixelOffsetMode::HighSpeed;
										g->CompositingQuality = CompositingQuality::HighSpeed;
										g->InterpolationMode = InterpolationMode::Default;
										g->SmoothingMode = SmoothingMode::None;
										g->DrawImageUnscaled(tmpImage, 0, 0);
										delete tmpImage;*/

									}
									catch(Exception^)
									{
										m_BmpImage = nullptr;
										result = ReadResult::ImageNotConverted;
									}
								}
								else
								{
									// frame completely decoded but target frame not reached yet.
								}
							}
							else
							{
								// Frame not complete. Keep decoding packets until it is.
							}
						
						}
						else
						{						
							// Not the video stream.
						}
						
						// Free the packet that was allocated by av_read_frame
						av_free_packet(&InputPacket);
					}
					else
					{
						
						// Cas d'une erreur de lecture sur une frame.
						// On ne sait pas si l'erreur est survenue sur une frame vidéo ou audio...

						//Console::WriteLine("SHOW_NEXT_FRAME_READFRAME_ERROR");

						done = true;
						result = ReadResult::FrameNotRead;
					}
				}
				while(!done);
				
				// Needs to be freed here to avoid a 'yet to be investigated' crash.
				av_free(pDecodingFrameBuffer);
			}
			else
			{
				// Codec was opened succefully but we got garbage in width and height. (Some FLV files.)
				result = ReadResult::MemoryNotAllocated;
			}
			//av_free(pDecodingFrameBuffer);
		}
		else
		{
			result = ReadResult::MemoryNotAllocated;
		}

		// can't free pDecodingFrameBuffer this late ?

	}		
	return result;
}
InfosThumbnail^ VideoFile::GetThumbnail(String^ _FilePath, int _iPicWidth)
{

	InfosThumbnail^ infos = gcnew InfosThumbnail();
	infos->Thumbnails = gcnew List<Bitmap^>();
	int iMaxThumbnails = 4;
	int64_t iIntervalTimestamps = 1;
	
	Bitmap^ bmp = nullptr;
	bool bGotPicture = false;
	bool bCodecOpened = false;

	AVFormatContext* pFormatCtx;
	AVCodecContext* pCodecCtx;

	do
	{
		char* cFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

		// 1. Ouvrir le fichier et récupérer les infos sur le format.
		
		if(av_open_input_file(&pFormatCtx, cFilePath , NULL, 0, NULL) != 0)
		{
			log->Error("GetThumbnail Error : Input file not opened");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(cFilePath));
		
		// 2. Obtenir les infos sur les streams contenus dans le fichier.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			log->Error("GetThumbnail Error : Stream infos not found");
			break;
		}

		// 3. Obtenir l'identifiant du premier stream vidéo.
		int iVideoStreamIndex = -1;
		if( (iVideoStreamIndex = GetFirstStreamIndex(pFormatCtx, CODEC_TYPE_VIDEO)) < 0 )
		{
			log->Error("GetThumbnail Error : First video stream not found");
			break;
		}

		// 4. Obtenir un objet de paramètres du codec vidéo.
		AVCodec* pCodec;
		pCodecCtx = pFormatCtx->streams[iVideoStreamIndex]->codec;
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			log->Error("GetThumbnail Error : Decoder not found");
			break;
		}

		// 5. Ouvrir le Codec vidéo.
		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			log->Error("GetThumbnail Error : Decoder not opened");
			break;
		}
		bCodecOpened = true;


		// TODO:
		// Fill up a InfosThumbnail object with data.
		// (Fixes anamorphic, unsupported width, compute length, etc.)

		// 5.b Compute duration in timestamps.
		double fAverageTimeStampsPerSeconds = (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.den / (double)pFormatCtx->streams[iVideoStreamIndex]->time_base.num;
		if(pFormatCtx->duration > 0)
		{
			infos->iDurationMilliseconds = pFormatCtx->duration / 1000;
			
			// Compute the interval in timestamps at which we will extract thumbs.
			int64_t iDurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration / (double)AV_TIME_BASE) * fAverageTimeStampsPerSeconds);
			iIntervalTimestamps = iDurationTimeStamps / iMaxThumbnails;
		}
		else
		{
			// No duration infos, only get one pic.
			iMaxThumbnails = 1;
		}

		// 6. Allocate video frames, one for decoding, one to hold the picture after conversion.
		AVFrame* pDecodingFrameBuffer = avcodec_alloc_frame();
		AVFrame* pDecodedFrameBGR = avcodec_alloc_frame();

		//fLastRamValue = TraceMemoryUsage(RamCounter, fLastRamValue, "avcodec_alloc_frame x2", &fRamBalance);

		if( (pDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// We ask for pictures already reduced in size to lighten GDI+ burden: max at _iPicWidth px width.
			// This also takes care of image size which are not multiple of 4.
			float fWidthRatio = (float)pCodecCtx->width / _iPicWidth;
			int iDecodingWidth = _iPicWidth;
			int iDecodingHeight = (int)((float)pCodecCtx->height / fWidthRatio);

			int iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, iDecodingWidth, iDecodingHeight);
			if(iSizeBuffer < 1)
			{
				log->Error("GetThumbnail Error : Frame buffer not allocated");
				break;
			}
			uint8_t* pBuffer = new uint8_t[iSizeBuffer];

			// Assign appropriate parts of buffer to image planes in pFrameBGR
			avpicture_fill((AVPicture *)pDecodedFrameBGR, pBuffer , PIX_FMT_BGR24, iDecodingWidth, iDecodingHeight);
			
			int iTotalReadFrames = 0;
			
			//-------------------
			// Read the first frame.
			//-------------------
			bool done = false;
			do
			{
				AVPacket	InputPacket;
				
				int iReadFrameResult = av_read_frame( pFormatCtx, &InputPacket);

				if(iReadFrameResult >= 0)
				{
					// Is this a packet from the video stream ?
					if(InputPacket.stream_index == iVideoStreamIndex)
					{
						// Decode video frame
						int	frameFinished;
						avcodec_decode_video(pCodecCtx, pDecodingFrameBuffer, &frameFinished, InputPacket.data, InputPacket.size);

						if(frameFinished)
						{
							iTotalReadFrames++;
							if(iTotalReadFrames > iMaxThumbnails-1)
							{
								done=true;
							}

							SwsContext* pSWSCtx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, iDecodingWidth, iDecodingHeight, PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL); 
							sws_scale(pSWSCtx, pDecodingFrameBuffer->data, pDecodingFrameBuffer->linesize, 0, pCodecCtx->height, pDecodedFrameBGR->data, pDecodedFrameBGR->linesize); 
							sws_freeContext(pSWSCtx);

							try
							{
								IntPtr* ptr = new IntPtr((void*)pDecodedFrameBGR->data[0]); 
								Bitmap^ tmpBitmap = gcnew Bitmap( iDecodingWidth, iDecodingHeight, pDecodedFrameBGR->linesize[0], Imaging::PixelFormat::Format24bppRgb, *ptr );
							
								//---------------------------------------------------------------------------------
								// Dupliquer complètement, 
								// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
								//---------------------------------------------------------------------------------
								bmp = AForge::Imaging::Image::Clone(tmpBitmap, tmpBitmap->PixelFormat);
								infos->Thumbnails->Add(bmp);
								delete tmpBitmap;
								tmpBitmap = nullptr;
								bGotPicture = true;
							}
							catch(Exception^)
							{
								log->Error("GetThumbnail Error : Bitmap creation failed");
								bmp = nullptr;
							}
							
							//-------------------------------------------
							// Seek to next image. 
							// Approximation : We don't care about first timestamp being greater than 0.	
							//-------------------------------------------
							if(iTotalReadFrames > 0 && iTotalReadFrames < iMaxThumbnails)
							{
								try
								{
									//log->Debug(String::Format("Jumping to {0} to extract thumbnail {1}.", iTotalReadFrames * iIntervalTimestamps, iTotalReadFrames+1));
									av_seek_frame(pFormatCtx, iVideoStreamIndex, (iTotalReadFrames * iIntervalTimestamps), AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(pFormatCtx->streams[iVideoStreamIndex]->codec);
								}
								catch(Exception^)
								{
									log->Error("GetThumbnail Error : Jumping to next extraction point failed.");
									done = true;
								}
							}
						}
						else
						{
							int iFrameNotFinished=1; // test pour debug
						}
					}
					else
					{
						// Not the first video stream.
						//Console::WriteLine("This is Stream #{0}, Loop until stream #{1}", InputPacket.stream_index, iVideoStreamIndex);
					}
					
					// Free the packet that was allocated by av_read_frame
					av_free_packet(&InputPacket);
				}
				else
				{
					// Reading error.
					done = true;
					log->Error("GetThumbnail Error : Frame reading failed");
				}
			}
			while(!done);
			
			// Clean Up
			delete []pBuffer;
			pBuffer = nullptr;
			
			av_free(pDecodingFrameBuffer);
			av_free(pDecodedFrameBGR);
		}
		else
		{
			// Not enough memory to allocate the frame.
			log->Error("GetThumbnail Error : AVFrame holders not allocated");
		}
	}
	while(false);

	if(bCodecOpened)
	{
		avcodec_close(pCodecCtx);
		//av_free(pCodecCtx);
	}

	if(pFormatCtx != nullptr)
	{
		av_close_input_file(pFormatCtx);
		//av_free(pFormatCtx);	
	}

	//Console::WriteLine("Leaving GetThumbnail, Available RAM: {0}", RamCounter->NextValue());
	//delete RamCounter;

	return infos;
}
bool VideoFile::CanExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, int _maxSeconds, int _maxMemory)
{
	// Check if the current selection could switch to analysis mode, according to the current settings.
	// _maxMemory is in Mib.
	
	// To be analyzable, both conditions must be met.
	int iDurationTimeStamps = (int)(_iEndTimeStamp - _iStartTimeStamp);
	double iDurationSeconds = (double)iDurationTimeStamps / m_InfosVideo->fAverageTimeStampsPerSeconds;
	
	int iFrameMemoryBytes = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
	double iFrameMemoryMegaBytes = (double)iFrameMemoryBytes  / 1048576;
	int iTotalFrames = (int)(iDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);
	int iDurationMemory = (int)((double)iTotalFrames * iFrameMemoryMegaBytes);
	
	bool result = false;
	if( (iDurationSeconds > 0) && (iDurationSeconds <= _maxSeconds) && (iDurationMemory <= _maxMemory))
	{
		result = true;
	}
    
	return result;
}
void VideoFile::ExtractToMemory(int64_t _iStartTimeStamp, int64_t _iEndTimeStamp, bool _bForceReload)
{
	
	int					iReadFrameResult;
	AVFrame				*pDecodingFrameBuffer; 
	AVPacket			packet;
	int					frameFinished;
	int					iSizeBuffer;
	int					iResult = 0;
	int64_t				iCurrentTimeStamp = 0;
	int					iFramesDecoded = 0;
	int64_t				iOldStart = 0;
	int64_t				iOldEnd = 0;
	

	

	//---------------------------------------------------
	// Check what we need to load.
	// If reducing, reduces the selection.
	//---------------------------------------------------
	ImportStrategy strategy = PrepareSelection(_iStartTimeStamp, _iEndTimeStamp, _bForceReload);
	
	// Reinitialize the reading type in case we fail.
	m_PrimarySelection->iAnalysisMode = 0;
	
	// If not complete, we'll only decode frames we don't already have.
	if(strategy != ImportStrategy::Complete)
	{
		if(m_FrameList->Count > 0)
		{
			iOldStart = m_FrameList[0]->iTimeStamp;
			iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;
			log->Debug(String::Format("Optimized sentinels: [{0}]->[{1}]", _iStartTimeStamp, _iEndTimeStamp));
		}
	}

	if(strategy != ImportStrategy::Reduction)
	{
		int  iEstimatedNumberOfFrames = EstimateNumberOfFrames(_iStartTimeStamp, _iEndTimeStamp);

		//-----------------------------------------
		// Seek au début de la selection (ou avant)
		// TODO : et si ret = -1 ?
		//-----------------------------------------
		int ret = av_seek_frame(m_pFormatCtx, m_iVideoStream, _iStartTimeStamp, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

		//-----------------------------------------------------------------------------------
		// Allocate video frames, one for decoding, one to hold the picture after conversion.
		//-----------------------------------------------------------------------------------
		pDecodingFrameBuffer = avcodec_alloc_frame();
		if(m_pCurrentDecodedFrameBGR != nullptr)
		{
			av_free(m_pCurrentDecodedFrameBGR);
		}
		m_pCurrentDecodedFrameBGR = avcodec_alloc_frame();

		if( (m_pCurrentDecodedFrameBGR != NULL) && (pDecodingFrameBuffer != NULL) )
		{
			// Final container (resized)
			iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);
			if(iSizeBuffer > 0)
			{
				if(m_Buffer == nullptr)
					m_Buffer  = new uint8_t[iSizeBuffer];

				// Assign appropriate parts of buffer to image planes in pFrameBGR
				avpicture_fill((AVPicture *)m_pCurrentDecodedFrameBGR, m_Buffer , PIX_FMT_BGR24, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight);

				
				bool done = false;
				bool bFirstPass = true;

				//-----------------
				// Read the frames.
				//-----------------
				do
				{
					iReadFrameResult = av_read_frame( m_pFormatCtx, &packet);

					if(iReadFrameResult >= 0)
					{
						// Is this a packet from the video stream?
						if(packet.stream_index == m_iVideoStream)
						{
							// Decode video frame
							avcodec_decode_video(m_pCodecCtx, pDecodingFrameBuffer, &frameFinished, packet.data, packet.size);

							// L'image est elle complète ? (En cas de B-frame ?)
							if(frameFinished)
							{
								if(packet.dts == AV_NOPTS_VALUE)
								{
									iCurrentTimeStamp = 0;
								}
								else
								{
									iCurrentTimeStamp = packet.dts;
								}

								if(bFirstPass && iCurrentTimeStamp > _iStartTimeStamp && _iStartTimeStamp >= 0)
								{
									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back 1 full second behind
									// the target and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;

									// Place the new target one second before the original one.
									int64_t iForceSeekTimestamp = _iStartTimeStamp - (int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds;
									if(iForceSeekTimestamp < 0) iForceSeekTimestamp = 0;

									// Do the seek
									log->Error(String::Format("First decoded frame [{0}] already after start stamp. Force seek back 1 second to [{1}]", iCurrentTimeStamp, iForceSeekTimestamp));
									av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&packet);

									// Loop back.
									continue;
								}

								bFirstPass = false;

								// Attention, comme on a fait un seek, il est possible qu'on soit en train de décoder des images
								// situées AVANT le début de la selection. Tant que c'est le cas, on décode dans le vide.
								if( iCurrentTimeStamp >= _iStartTimeStamp /*&& !bSeekAgain*/)
								{
									iFramesDecoded++;

									if((_iEndTimeStamp > 0) && (iCurrentTimeStamp >= _iEndTimeStamp))
									{
										done = true;
									}
									
									RescaleAndConvert(m_pCurrentDecodedFrameBGR, pDecodingFrameBuffer, m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced); 
									
									try
									{
										//------------------------------------------------------------------------------------
										// Accepte uniquement un Stride multiple de 4.
										// Tous les fichiers aux formats non standard sont refusés par le constructeur Bitmap.
										//------------------------------------------------------------------------------------
										IntPtr* ptr = new IntPtr((void*)m_pCurrentDecodedFrameBGR->data[0]); 
										int iImageStride	= m_pCurrentDecodedFrameBGR->linesize[0];
										Bitmap^ m_BmpImage = gcnew Bitmap( m_InfosVideo->iDecodingWidth, m_InfosVideo->iDecodingHeight, iImageStride, Imaging::PixelFormat::Format24bppRgb, *ptr );
										
										//-------------------------------------------------------------------------------------------------------
										// Dupliquer complètement, 
										// sinon toutes les images vont finir par utiliser le même pointeur : m_pCurrentDecodedFrameBGR->data[0].
										// Bitmap.Clone n'est pas suffisant, on continue de pointer vers les mêmes données.
										//------------------------------------------------------------------------------------------------------- 
										DecompressedFrame^ DecFrame = gcnew DecompressedFrame();
										DecFrame->BmpImage = AForge::Imaging::Image::Clone(m_BmpImage, m_BmpImage->PixelFormat); 
										DecFrame->iTimeStamp = iCurrentTimeStamp;

										//---------------------------------------------------------------------------
										// En modes d'agrandissement, 
										// faire attention au chevauchement de la selection demandée avec l'existant.
										//---------------------------------------------------------------------------
										if((strategy == ImportStrategy::InsertionBefore) && (iCurrentTimeStamp < iOldStart))
										{
											log->Debug(String::Format("Inserting frame before the original selection - [{0}]", iCurrentTimeStamp));	
											m_FrameList->Insert(iFramesDecoded - 1, DecFrame);
										}
										else if((strategy == ImportStrategy::InsertionAfter) && (iCurrentTimeStamp > iOldEnd))
										{
											log->Debug(String::Format("Inserting frame after the original selection - [{0}]", iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else if(strategy == ImportStrategy::Complete)
										{
											log->Debug(String::Format("Appending frame to selection - [{0}]", iCurrentTimeStamp));
											m_FrameList->Add(DecFrame);
										}
										else
										{
											// We already have this one. Do nothing.
											log->Error(String::Format("Frame not imported. Already in selection - [{0}]", iCurrentTimeStamp ));
										}
										
										delete m_BmpImage;

										// Avoid crashing if there's a refresh of the main screen.
										m_BmpImage = DecFrame->BmpImage;

										// Report Progress
										if(m_bgWorker != nullptr)
										{
											m_bgWorker->ReportProgress(iFramesDecoded, iEstimatedNumberOfFrames);
										}
									}
									catch(Exception^)
									{
										// TODO sortir en erreur desuite.
										done = true;
										log->Error("Conversion error during selection import.");
									}
								}
								else
								{
									log->Debug(String::Format("Decoded frame is before the new start sentinel - [{0}]", iCurrentTimeStamp));
								}
							}
							else
							{
								int iFrameNotFinished=1; // test pour debug
							}
						}
						else
						{
							int iAudio=1; // test pour debug
						}
						
						// Free the packet that was allocated by av_read_frame
						av_free_packet(&packet);
					}
					else
					{
						// Terminaison par fin du parcours de l'ensemble de la vidéo, ou par erreur...
						done = true;
					}
				}
				while(!done);
			
				av_free(pDecodingFrameBuffer);
			}
			else
			{
				// Codec was opened succefully but we got garbage in width and height. (Some FLV files.)
				iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
			}
		}
		else
		{
			iResult = 2;	// SHOW_NEXT_FRAME_ALLOC_ERROR
		}


	}

	// If reduction, frames were deleted at PrepareSelection time.
	

	//----------------------------
	// Fin de l'import.
	//----------------------------
	if(m_FrameList->Count > 0)
	{
		m_PrimarySelection->iAnalysisMode = 1;
		m_PrimarySelection->iDurationFrame = m_FrameList->Count;
		if(m_PrimarySelection->iCurrentFrame > m_PrimarySelection->iDurationFrame-1)
		{
			m_PrimarySelection->iCurrentFrame = m_PrimarySelection->iDurationFrame - 1;
		}
		else if(m_PrimarySelection->iCurrentFrame < 0)
		{
			m_PrimarySelection->iCurrentFrame = 0;
		}

		// Image en cours
		m_BmpImage = m_FrameList[m_PrimarySelection->iCurrentFrame]->BmpImage;
	}
	else
	{
		m_PrimarySelection->iAnalysisMode = 0;
		m_PrimarySelection->iCurrentFrame = -1;
		//m_PrimarySelection->iCurrentTimeStamp = Reste à sa valeur.

		m_PrimarySelection->iDurationFrame = 0;
		//m_PrimarySelection->iStartTimeStamp = 0;
		//m_PrimarySelection->iEndTimeStamp = 0;

		// Image en cours : ?
		// /!\ Attention m_pCurrentDecodedFrameBGR a été invalidé.
		m_BmpImage = nullptr;
	}

}

SaveResult VideoFile::Save( String^ _FilePath, int _iFramesInterval, int64_t _iSelStart, int64_t _iSelEnd, String^ _Metadata, bool _bFlushDrawings, bool _bKeyframesOnly, DelegateGetOutputBitmap^ _delegateGetOutputBitmap)
{
	//-------------------------------------------------------------------
	// Save.
	//
	// Fonction utilisée pour tous les types d'enregistrement vidéo:
	// Enregistrement avec ou sans incrustation des dessins,
	// en tenant compte ou pas du ralentit,
	// que l'on soit en mode analyse ou pas,
	// création de diaporama ne contenant que les keyframes.
	//
	// Se replace au début de la selection primaire 
	// Si besoin fait un transcode de la vidéo vers un format cible supporté.
	// Eventuellement muxe les metadata.
	// Enregistre le resultat dans le fichier cible
	//
	//-------------------------------------------------------------------
	
	SaveResult result = SaveResult::Success;
	
	// -- Input --
	int					iReadFrameResult;
	int					iFramesToStartFrame		= 0;
	
	int					iFramesInterval;
	int					iDuplicateFactor = 1;

	// -- Output --
	AVOutputFormat*		pOutputFormat;				// Infos générales muxeur. (mime, extensions, codecs supportés, etc.)
	AVFormatContext*	pOutputFormatContext;		// Paramètres du Muxeur.
	AVStream*			pOutputVideoStream;			// Stream de sortie, contenant les frames bufferisée.
	AVCodec*			pOutputCodec;				// Infos générales encodeur. (codec_id etc.)
	AVCodecContext*		pOutputCodecContext;		// Paramètres de l'encodeur.
	AVStream*			pOutputDataStream;			// Stream de sortie, contenant les meta-données.
	bool				bOutputCodecOpened		= false;
	int					iOutputWidth			=  m_pCodecCtx->width; //m_InfosVideo->iDecodingWidth;
	int					iOutputHeight			=  m_pCodecCtx->height;//m_InfosVideo->iDecodingHeight;
	
	char* pFilePath	= static_cast<char*>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

	bool					bTranscodeSuccess	= false;
	int						res					= 0;
	int						iTranscodedFrames	= 0;
	int						iBitrate			= 0;
	int						iOutputMuxer		= -1;
	
	bool					bNeedEncoding		= true;
	bool					bHasMetadata		= (_Metadata->Length > 0);
	
	//--------------------------------------------------------


	if(!m_bIsLoaded) return SaveResult::MovieNotLoaded;


	// Framerate réel et doublonnage si nécessaire.
	if(_iFramesInterval == 0) 
	{
		iFramesInterval = 40;
	}
	else
	{
		//---------------------------------------------------------------------
		// Impossible d'enregistrer à moins de 8 img/s... (fixme ?)
		// On va donc doublonner les images dès que nécessaire.
		// DuplicateFactor = nombre de fois que l'on doit répéter chaque frame.
		//---------------------------------------------------------------------
		iDuplicateFactor = (int)Math::Ceiling((double)_iFramesInterval / 125.0);
		iFramesInterval = _iFramesInterval  / iDuplicateFactor;	
	}
	log->Debug(String::Format("iFramesInterval:{0}, iDuplicateFactor:{1}", iFramesInterval, iDuplicateFactor));

	// Choix de la technique d'encodage. Dépend du framerate, du blending des drawings...
	bNeedEncoding = NeedEncoding(iFramesInterval, _bFlushDrawings, _bKeyframesOnly);

	// Seek to start. (We'll need to decode the source anyway, even if we are in mode analysis).
	MoveToTimestamp(_iSelStart);

	// Approximate Bitrate
	iBitrate = GetInputBitrate(iOutputWidth, iOutputHeight);

	// sàb
	do
	{
		// 1. Choix du muxeur
		if ((pOutputFormat = GuessOutputFormat(_FilePath, bHasMetadata)) == nullptr) 
		{
			log->Debug("muxer not found");
			break;
		}


		// 1.5. FIXME : le conteneur MKV fait des siennes sur l'enregistrement par copie. (Packet to Packet)
		// On rebascule en encodage complet dans ce cas.
		// Impacte uniquement les gens qui veulent enregistrer une video MPEG4-ASP en MKV sans y avoir touché.
		// Reste relativement génant car MKV est le seul format qui supporte les metadonnées...
		// Donc chaque fois qu'on ne modifie que les Drawings et qu'on enregistre le tout muxé, on perd un peu de qualité.
		if(_FilePath->EndsWith("mkv") || bHasMetadata)
		{
			bNeedEncoding = true;
		}

		// 2. Allocation mémoire pour les paramètres du muxeur.
		if ((pOutputFormatContext = av_alloc_format_context()) == nullptr) 
		{
			log->Debug("muxer config object not allocated");
			break;
		}
		
		// 3. Paramètres du muxeur
		if(!SetupMuxer(pOutputFormatContext, pOutputFormat, pFilePath, iBitrate))
		{
			log->Debug("muxer parameters not set");
			break;
		}

		// 4. Création du stream Vidéo. (convention : 0 pour la vidéo, 1 pour l'audio)
		if ((pOutputVideoStream = av_new_stream(pOutputFormatContext, 0)) == nullptr) 
		{
			log->Debug("video stream not created");
			break;
		}

		if(bNeedEncoding)
		{
			// 10. Choix de l'encodeur vidéo. (0.7.* => Mpeg4 seulement)
			// CODEC_ID_MPEG4 => Compatible XviD, DivX, etc. 
			// CODEC_ID_H264 => Broken sous Windows pour l'instant...
			if ((pOutputCodec = avcodec_find_encoder(CODEC_ID_MPEG4)) == nullptr)
			{
				log->Error("encoder not found");
				break;
			}

			// 11. Allouer l'objet des paramètres de l'encodeur.
			if ((pOutputCodecContext = avcodec_alloc_context()) == nullptr) 
			{
				log->Error("encoder config object not allocated");
				break;
			}

			// 13. Options d'encodage
			Size s = Size(iOutputWidth, iOutputHeight);
			if(!SetupEncoder(pOutputCodecContext, pOutputCodec, s, iFramesInterval, iBitrate))
			{
				log->Error("encoder parameters not set");
				break;
			}

			// 14. Ouverture du Codec avec ces paramètres.
			if (avcodec_open(pOutputCodecContext, pOutputCodec) < 0)
			{
				log->Error("codec not opened");
				log->Error(String::Format("iOutputWidth:{0}, iOutputHeight:{1}, iFramesInterval:{2}, iBitrate:{3}", iOutputWidth, iOutputHeight, iFramesInterval, iBitrate));
				
				break;
			}

			bOutputCodecOpened = true;
			
			// 15. association avec le stream...
			pOutputVideoStream->codec = pOutputCodecContext;
		}
		else
		{
			log->Error("Re-encoding not needed.");

			// ? 
			//pOutputVideoStream->time_base.den = m_pFormatCtx->streams[m_iVideoStream]->time_base.den;
			//pOutputVideoStream->time_base.num = m_pFormatCtx->streams[m_iVideoStream]->time_base.num;

			// 11. Allouer l'objet des paramètres de l'encodeur.
			if ((pOutputCodecContext = avcodec_alloc_context()) == nullptr) 
			{
				log->Error("encoder config object not allocated");
				break;
			}

			// 13. Options d'encodage			
			if(!SetupEncoderForCopy(pOutputCodecContext, pOutputVideoStream))
			{
				log->Error("encoder parameters not set");
				break;
			}

			// 15. association avec le stream...
			pOutputVideoStream->codec = pOutputCodecContext;

		}

		if(bHasMetadata)
		{
			// 16. Ajout d'un stream pour les meta données.
			if ((pOutputDataStream = av_new_stream(pOutputFormatContext, 1)) == nullptr) 
			{
				log->Error("metadata stream not created");
				break;
			}

			// 17. Obtenir la conf par défaut comme si c'était un stream de sous titre.
			// (Se charge de l'allocation du CodecCtx pointé.)
			avcodec_get_context_defaults2(pOutputDataStream->codec, CODEC_TYPE_SUBTITLE);
			//avcodec_get_context_defaults(pOutputDataStream->codec);
			//pOutputDataStream->codec->codec_type = CODEC_TYPE_SUBTITLE;

			// 18. Identifiant du codec. Sous mkv, apparaîtra comme "S_TEXT/UTF8".
			pOutputDataStream->codec->codec_id = CODEC_ID_TEXT;

			// code ISO 639 du langage (3 lettres) du stream de sous titres. ( -> en.wikipedia.org/wiki/List_of_ISO_639-3_codes)	 
			// => "XML" est en réalité "Malaysian Sign Language".
			av_strlcpy(pOutputDataStream->language, "XML", sizeof(pOutputDataStream->language));

			log->Debug("Muxing metadata into a subtitle stream.");
		}

		// 19. Ouverture du Fichier.
		if ((res = url_fopen(&pOutputFormatContext->pb, pFilePath, URL_WRONLY)) < 0) 
		{
			log->Error(String::Format("file not opened, AVERROR:{0}", res));
			break;
		}

		int test12 = pOutputVideoStream->time_base.num; // OK

		// 20. Ecriture du header du fichier
		if((res = av_write_header(pOutputFormatContext)) < 0)
		{
			Console::WriteLine(String::Format("file header not written, AVERROR:{0}", res));
			break;
		}

		int test2 = pOutputVideoStream->time_base.num; // KO
	
		// 21. Allocation de mémoire pour le receptacle de l'input frame courante. 
		// (la même variable sera réutilisé pour chaque frame.)
		AVFrame* pInputFrame;
		if ((pInputFrame = avcodec_alloc_frame()) == nullptr) 
		{
			log->Error("input frame not allocated");
			break;
		}
			
		// 22. Contexte de transformation (taille et format de pixel) qui sera appliquée à toutes les images.
		SwsContext* pScalingContext;
		if(bNeedEncoding)
		{
			pScalingContext = sws_getContext(m_pCodecCtx->width, m_pCodecCtx->height, m_pCodecCtx->pix_fmt, iOutputWidth, iOutputHeight, pOutputCodecContext->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL); 
		}

		// Ecriture du Packet de metadonnées dans le fichier
		// Utiliser les metadonnées de type author, title, etc. est impossible. (limités à 500 octets chacune)
		if(bHasMetadata)
		{
			if(!WriteMetadata(pOutputFormatContext, pOutputCodecContext, pOutputVideoStream, pOutputDataStream, _Metadata))
			{
				log->Error("metadata not written");
				break;
			}
		}

		//----------------------------------------------------------
		// 23. Boucle principale de transcodage.
		//-----------------------------------------------------------


		// TODO: Si Decoding pas nécessaire (mode analysis) passer par une autre boucle.

		AVPacket InputPacket;

		bool done = false;
		bool bFirstPass = true;
		log->Debug(String::Format("Start Saving video, Encoding:{0}", bNeedEncoding));
		while(!done)
		{
			//----------------------------------------------------------------------------------
			// Lecture d'un Packet en provenance du fichier d'entrée.
			// Un Packet contient des données d'un stream particulier, à un moment particulier.
			// En vidéo, il peut contenir tout ou partie d'une image.
			// On sépare la lecture packet de la lecture frames, car dans le cas où on a pas 
			// besoin de réencoder, on va directement copier packet to packet.
			//----------------------------------------------------------------------------------
			iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket);

			if(iReadFrameResult >= 0)
			{
				// -> Packet correctement lu.
				
				if(InputPacket.stream_index == m_iVideoStream)
				{
					// -> Packet du stream vidéo.
					int64_t iCurrentTimeStamp;
					if(InputPacket.dts == AV_NOPTS_VALUE)
					{
						iCurrentTimeStamp = 0;
					}
					else
					{
						iCurrentTimeStamp = InputPacket.dts;
					}

					if(iCurrentTimeStamp >= _iSelStart)
					{
						// -> Packet à enregistrer.
						// On doit pousser ce Packet dans le fichier de sortie.
						// Soit par décodage + encodage, soit par copie directe.

						if(bNeedEncoding)
						{
							//-----------------------------------------------------------------------------
							// todo: 
							// 1. décoder l'image d'input.
							// 2. Extraire une bitmap de cette AVFrame. (/!\ taille modulo 4)
							// 3. Tenter d'obtenir une image complète via le FlushOnGraphics.
							// 4. Si fading on ou qu'on est sur une KF, on récupère une image, sinon null.
							// 5. Si on a pas eu l'image, tenter par le tableau de mode analyse
							// 6. Envoyer à encodeAndWriteVideoFrame en passant une Image ou une AVFrame.
							//-----------------------------------------------------------------------------

							// On doit la décoder via ffmpeg, (même si on va la récupérer du Flush ou directement du tableau) 
							// afin de mettre à jour le Codec Context, etc. Sinon, il y a un soucis avec les keyframes / timestamps...
							int	 iFrameFinished	= 0;
							log->Debug("avcodec_decode_video()");
							avcodec_decode_video(m_pCodecCtx, pInputFrame, &iFrameFinished, InputPacket.data, InputPacket.size);

							if(iFrameFinished)
							{
								if(bFirstPass && iCurrentTimeStamp > _iSelStart)
								{
									// If the current ts is already after the target, we are dealing with this kind of files
									// where the seek doesn't work as advertised. We'll seek back 1 full second behind
									// the target and then decode until we get to it.
									
									// Do this only once.
									bFirstPass = false;
									
									// place the new target one second before the original one.
									int64_t iForceSeekTimestamp = _iSelStart - (int64_t)m_InfosVideo->fAverageTimeStampsPerSeconds;
									if(iForceSeekTimestamp < 0) iForceSeekTimestamp = 0;

									// Do the seek.
									log->Debug(String::Format("First fully decoded frame [{0}] already after target. Force seek back 1 second to [{1}]", iCurrentTimeStamp, iForceSeekTimestamp));
									av_seek_frame(m_pFormatCtx, m_iVideoStream, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD);
									avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

									// Free the packet that was allocated by av_read_frame
									av_free_packet(&InputPacket);

									// Loop back.
									continue;
								}

								bFirstPass = false;
								uint8_t* pBuffer;
								AVFrame* pConvertedFrame;


								// 1. Get the input image from analysis array or from the AVFrame we just decoded. 
								Bitmap^ InputBitmap = nullptr;
								if(m_PrimarySelection->iAnalysisMode == 1)
								{
									log->Debug("Get the input image from analysis array. InputBitmap : AForge Cloning");
									int iFrameNumber = (int)GetFrameNumber(iCurrentTimeStamp);
									Bitmap^ img = m_FrameList[iFrameNumber]->BmpImage;
									InputBitmap = AForge::Imaging::Image::Clone(img, img->PixelFormat);
								}
								else
								{
									log->Debug("Get the input image by converting from ffmpeg. (InputBitmap : gcnew)");
									int iDecodingWidth = m_pCodecCtx->width;
									if(iDecodingWidth % 4 != 0)
									{
										iDecodingWidth = 4 * ((m_pCodecCtx->width / 4) + 1);
									}

									log->Debug("avcodec_alloc_frame().");
									pConvertedFrame = avcodec_alloc_frame();
									int iSizeBuffer = avpicture_get_size(PIX_FMT_BGR24, iDecodingWidth, m_pCodecCtx->height);
									pBuffer = new uint8_t[iSizeBuffer];
									
									avpicture_fill((AVPicture *)pConvertedFrame, pBuffer , PIX_FMT_BGR24, iDecodingWidth, m_pCodecCtx->height);

									// convert+[deinterlace] but don't resize.
									RescaleAndConvert( pConvertedFrame, pInputFrame, iDecodingWidth, m_pCodecCtx->height, PIX_FMT_BGR24, m_InfosVideo->bDeinterlaced);
								
									IntPtr* ptr = new IntPtr((void*)pConvertedFrame->data[0]); 
									InputBitmap = gcnew Bitmap( iDecodingWidth, m_pCodecCtx->height, pConvertedFrame->linesize[0], Imaging::PixelFormat::Format24bppRgb, *ptr );
								}

								if(InputBitmap != nullptr && _delegateGetOutputBitmap != nullptr)
								{
									bool bShouldEncode = _delegateGetOutputBitmap(Graphics::FromImage(InputBitmap), iCurrentTimeStamp, _bFlushDrawings, _bKeyframesOnly);
									
									if(bShouldEncode)
									{
										// Encode.
										for(int iDuplicate=0;iDuplicate<iDuplicateFactor;iDuplicate++)
										{
											if(!EncodeAndWriteVideoFrame(pOutputFormatContext, pOutputCodecContext, pOutputVideoStream, iOutputWidth, iOutputHeight, pScalingContext, InputBitmap))
											{
												log->Error("error while writing output frame");
												break;
											}
										}
									}
									else
									{
										// May happen if we want only the KF and we are not on a kf.
									}
									log->Debug("delete InputBitmap");
									delete InputBitmap;

									log->Debug("Free temporary resources (pBuffer, pConvertedFrame)");
									av_free(pBuffer);
									av_free(pConvertedFrame);
								}
							}
							else
							{
								log->Debug(String::Format("Frame not finished, keep decoding."));
							}
						}
						else
						{
							// La vidéo n'a pas besoin d'être réencodée. (Flux d'entrée supporté en sortie.)
							
							log->Debug(String::Format("Video doesn't need to be encoded. (Input stream supported as output.)"));

							/*
							AVPacket OutputPacket;
							av_init_packet(&OutputPacket);

							// données du Packet
							OutputPacket.pts			= InputPacket.pts;
							OutputPacket.flags			= InputPacket.flags;
						
							OutputPacket.stream_index	= pOutputVideoStream->index;
							OutputPacket.data			= InputPacket.data;
							OutputPacket.size			= InputPacket.size;

							// Persister dans le fichier de sortie
							int iWriteRes = av_write_frame(pOutputFormatContext, &OutputPacket);
							*/


							// ~ Adapté de ffmpeg.c
						
							AVPacket OutputPacket;
							av_init_packet(&OutputPacket);

							// ?
							//if (!pOutputVideoStream->frame_number && !(InputPacket.flags & PKT_FLAG_KEY))
							//	continue;

							// Hack
							/*AVFrame avframe;
							avcodec_get_frame_defaults(&avframe);
							pOutputVideoStream->codec->coded_frame = &avframe;
							avframe.key_frame = InputPacket.flags & PKT_FLAG_KEY;*/

							//video_size += data_size;
							//pOutputVideoStream->sync_opts++;
							
							OutputPacket.stream_index = pOutputVideoStream->index;
	                        
							// PTS/DTS
							if(InputPacket.pts != AV_NOPTS_VALUE)
							{
								OutputPacket.pts = InputPacket.pts;
							}
							else
							{
								OutputPacket.pts= 0;
							}
							
							if (InputPacket.dts != AV_NOPTS_VALUE)
							{
								OutputPacket.dts = InputPacket.dts;
							}
							else
							{
								OutputPacket.dts = 0;
							}
	
							// 
							OutputPacket.duration = (int)av_rescale_q(InputPacket.duration, m_pFormatCtx->streams[m_iVideoStream]->time_base, pOutputVideoStream->time_base);
							OutputPacket.flags = InputPacket.flags;

							//FIXME ??
							/*if(av_parser_change(m_pFormatCtx->streams[m_iVideoStream]->parser, pOutputVideoStream->codec, &OutputPacket.data, &OutputPacket.size, InputPacket.data, InputPacket.size, InputPacket.flags & PKT_FLAG_KEY))
							{
								OutputPacket.destruct = av_destruct_packet;
							}*/

							OutputPacket.data			= InputPacket.data;
							OutputPacket.size			= InputPacket.size;

							//write_frame(os, &OutputPacket, pOutputVideoStream->codec, bitstream_filters[ost->file_index][opkt.stream_index]);
							int iWriteRes = av_write_frame(pOutputFormatContext, &OutputPacket);

							pOutputVideoStream->codec->frame_number++;
							//pOutputVideoStream->frame_number++;

							av_free_packet(&OutputPacket);
						}


						// Remontée d'infos vers la progress bar
						if(m_bgWorker != nullptr)
						{
							m_bgWorker->ReportProgress((int)(iCurrentTimeStamp - _iSelStart), (int)(_iSelEnd - _iSelStart));
						}


						// Check if we're done. (only if we actually decoded one complete frame.)
						if(iCurrentTimeStamp >= _iSelEnd && !bFirstPass)
						{
							done = true;
						}
					}
					else
					{
						log->Debug("Target not reached yet. Keep reading packets.");
						// Pas encore arrivés au premier Packet de la sélection primaire, 
						// on continue d'avancer sans encoder ni copier.
					
						// Si Encoding, on a quand même besoin de récupérer les données de la frame...
						// Mais quand est-ce qu'on la libère ?
						if(bNeedEncoding)
						{
							log->Debug("Force decoding to prepare a complete frame.");
							int	 iFrameFinished	= 0;
							avcodec_decode_video(m_pCodecCtx, pInputFrame, &iFrameFinished, InputPacket.data, InputPacket.size);
						}
					}
				}
				else
				{
					// Packet d'entrée non vidéo.
					// on continue d'avancer sans encoder ni copier.
				}

				// Fin du traitement sur le Packet d'input.
				log->Debug("av_free_packet(&InputPacket)");
				av_free_packet(&InputPacket);
			}
			else
			{
				// -> Packet non lu : Fin de la vidéo.
				done = true;
			}
		}
		// </while>

		// Fin du traitement de toute les frames de la vidéo.
		bTranscodeSuccess = true;

		// Free the InputFrame holder
		log->Debug("av_free(pInputFrame)");
		av_free(pInputFrame);

		// Libérer le Contexte de rescaling.					 
		if(bNeedEncoding)
		{
			sws_freeContext(pScalingContext);
		}
	}
	while(false);

	//-----------------------------
	// Clean up
	//-----------------------------

	Marshal::FreeHGlobal(safe_cast<IntPtr>(pFilePath));

	// Fermeture du codec vidéo
	if(bOutputCodecOpened )
	{
		avcodec_close(pOutputVideoStream->codec);
	}


	int test3 = pOutputVideoStream->time_base.num;


	// Ecriture du trailer après les données.
	if(bTranscodeSuccess)
		av_write_trailer(pOutputFormatContext);


	int test4 = pOutputVideoStream->time_base.num;

	// Libération du stream (equivalent à libérer : pOutputCodec, pOutputVideoStream)
	for(int i = 0; i < (int)pOutputFormatContext->nb_streams; i++) 
	{
		if(bNeedEncoding) 
		{
			av_freep(&pOutputFormatContext->streams[i]->codec);
		}
		else
		{
			// Si on a directement copié le flux, ne pas libérer le codec. (c'est m_pCodecCtx !)
		}

		av_freep(&pOutputFormatContext->streams[i]);
	}

	// Fermeture du fichier
	url_fclose(pOutputFormatContext->pb);

	// Libération due l'objet de paramétrage du muxeur
	av_free(pOutputFormatContext);


	// pOutputFormat ?

	//-----------------------------------------------------
	// Se replacer en début de sélection
	//-----------------------------------------------------
	//MoveToTimestamp(_iSelStart);
	// GetNext ?

	if(!bTranscodeSuccess && result == SaveResult::Success)
	{
		// On est pas allé au bout de l'enregistrement.
		result = SaveResult::TranscodeNotFinished;
	}
	
	return result;
}


int64_t VideoFile::GetTimeStamp(int64_t _iPosition)
{
	// TimeStamp from Frame Number
	int iFrameNumber = (int)_iPosition;

	if(m_PrimarySelection->iAnalysisMode == 1)
	{
		// It is assumed here that m_FrameList is not nullptr and m_FrameList->Count > 0. 
		if(iFrameNumber > m_FrameList->Count - 1)
		{
			iFrameNumber = m_FrameList->Count - 1;
		}
		else if(iFrameNumber < 0)
		{
			iFrameNumber = 0;
		}

		return m_FrameList[iFrameNumber]->iTimeStamp;
	}
	else
	{
		return _iPosition;
	}
}
int64_t VideoFile::GetFrameNumber(int64_t _iPosition)
{
	// Frame Number from TimeStamp.
	int iFrame = 0;
	if(m_FrameList != nullptr)
	{
		if(m_FrameList->Count > 0)
		{
			int64_t iCurrentTimeStamp = m_FrameList[0]->iTimeStamp;		
			while((iCurrentTimeStamp < _iPosition) && (iFrame < m_FrameList->Count-1))
			{
				iFrame++;
				iCurrentTimeStamp = m_FrameList[iFrame]->iTimeStamp;
			}
		}
	}

	return iFrame;
}



// New save methods.
SaveResult VideoFile::OpenSavingContext(String^ _FilePath)
{
	SaveResult result = SaveResult::Success;

	if(m_SavingContext != nullptr)
	{
		delete m_SavingContext;
	}
	m_SavingContext = gcnew SavingContext();

	// Arbitrary parameters (fixme?)
	int iBitrate = 25000000;
	log->Debug("Setting encoding bitrate to 25Mb/s. (DV)");
	CodecID encoderID = CODEC_ID_MPEG4;
	log->Debug("Setting encoder to MPEG4-ASP");
	bool bHasMetadata = false;
	//-------------------------------

	m_SavingContext->pFilePath = static_cast<char*>(Marshal::StringToHGlobalAnsi(_FilePath).ToPointer());

	do
	{
		// 1. Muxer selection.
		if ((m_SavingContext->pOutputFormat = GuessOutputFormat(_FilePath, bHasMetadata)) == nullptr) 
		{
			result = SaveResult::MuxerNotFound;
			log->Error("Muxer not found");
			break;
		}

		// 2. Allocate muxer parameters object.
		if ((m_SavingContext->pOutputFormatContext = av_alloc_format_context()) == nullptr) 
		{
			result = SaveResult::MuxerParametersNotAllocated;
			log->Error("Muxer parameters object not allocated");
			break;
		}
		
		// 3. Configure muxer.
		if(!SetupMuxer(m_SavingContext->pOutputFormatContext, m_SavingContext->pOutputFormat, m_SavingContext->pFilePath, iBitrate))
		{
			result = SaveResult::MuxerParametersNotSet;
			log->Error("Muxer parameters not set");
			break;
		}

		// 4. Create video stream.
		if ((m_SavingContext->pOutputVideoStream = av_new_stream(m_SavingContext->pOutputFormatContext, 0)) == nullptr) 
		{
			result = SaveResult::VideoStreamNotCreated;
			log->Error("Video stream not created");
			break;
		}

		// 5. Encoder selection
		if ((m_SavingContext->pOutputCodec = avcodec_find_encoder(encoderID)) == nullptr)
		{
			result = SaveResult::EncoderNotFound;
			log->Error("Encoder not found");
			break;
		}

		// 6. Allocate encoder parameters object.
		if ((m_SavingContext->pOutputCodecContext = avcodec_alloc_context()) == nullptr) 
		{
			result = SaveResult::EncoderParametersNotAllocated;
			log->Error("Encoder parameters object not allocated");
			break;
		}

		// 7. Configure encoder.
		if(!SetupEncoder(m_SavingContext->pOutputCodecContext, m_SavingContext->pOutputCodec, m_SavingContext->outputSize, m_SavingContext->iFramesInterval, iBitrate))
		{
			result = SaveResult::EncoderParametersNotSet;
			log->Error("Encoder parameters not set");
			break;
		}

		// 8. Open encoder.
		if (avcodec_open(m_SavingContext->pOutputCodecContext, m_SavingContext->pOutputCodec) < 0)
		{
			result = SaveResult::EncoderNotOpened;
			log->Error("Encoder not opened");
			break;
		}

		m_SavingContext->bEncoderOpened = true;
		
		// 9. Associate encoder to stream.
		m_SavingContext->pOutputVideoStream->codec = m_SavingContext->pOutputCodecContext;

		int iFFMpegResult;

		// 10. Open the file.
		if ((iFFMpegResult = url_fopen(&(m_SavingContext->pOutputFormatContext)->pb, m_SavingContext->pFilePath, URL_WRONLY)) < 0) 
		{
			result = SaveResult::FileNotOpened;
			log->Error(String::Format("File not opened, AVERROR:{0}", iFFMpegResult));
			break;
		}

		// int test1 = pOutputVideoStream->time_base.num; // OK

		// 11. Write file header.
		if((iFFMpegResult = av_write_header(m_SavingContext->pOutputFormatContext)) < 0)
		{
			result = SaveResult::FileHeaderNotWritten;
			log->Error(String::Format("File header not written, AVERROR:{0}", iFFMpegResult));
			break;
		}

		// int test2 = pOutputVideoStream->time_base.num; // KO
	
		// 12. Allocate memory for the current incoming frame holder. (will be reused for each frame). 
		if ((m_SavingContext->pInputFrame = avcodec_alloc_frame()) == nullptr) 
		{
			result = SaveResult::InputFrameNotAllocated;
			log->Error("input frame not allocated");
			break;
		}
			
		// 13. Contexte de transformation (taille et format de pixel) qui sera appliquée à toutes les images.
		/*SwsContext* pScalingContext;
		if(bNeedEncoding)
		{
			pScalingContext = sws_getContext(m_pCodecCtx->width, m_pCodecCtx->height, m_pCodecCtx->pix_fmt, iOutputWidth, iOutputHeight, pOutputCodecContext->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL); 
		}*/

		// Ecriture du Packet de metadonnées dans le fichier
		// Utiliser les metadonnées de type author, title, etc. est impossible. (limités à 500 octets chacune)
		/*if(bHasMetadata)
		{
			if(!WriteMetadata(pOutputFormatContext, pOutputCodecContext, pOutputVideoStream, pOutputDataStream, _Metadata))
			{
				log->Error("metadata not written");
				break;
			}
		}*/

	}
	while(false);

	return result;
}
SaveResult VideoFile::CloseSavingContext(bool _bEncodingSuccess)
{
	SaveResult result = SaveResult::Success;

	if(_bEncodingSuccess)
	{
		// Write file trailer.		
		av_write_trailer(m_SavingContext->pOutputFormatContext);
	}

	if(m_SavingContext->bEncoderOpened)
	{
		avcodec_close(m_SavingContext->pOutputVideoStream->codec);
	
		// Free the InputFrame holder
		log->Debug("av_free(pInputFrame)");
		av_free(m_SavingContext->pInputFrame);

		// Free rescaling context.					 
		/*if(bNeedEncoding)
		{
			sws_freeContext(pScalingContext);
		}*/
	}
		
	Marshal::FreeHGlobal(safe_cast<IntPtr>(m_SavingContext->pFilePath));
	
	// Stream release (equivalent to freeing pOutputCodec + pOutputVideoStream)
	for(int i = 0; i < (int)m_SavingContext->pOutputFormatContext->nb_streams; i++) 
	{
		av_freep(&(m_SavingContext->pOutputFormatContext)->streams[i]->codec);
		av_freep(&(m_SavingContext->pOutputFormatContext)->streams[i]);
	}

	// Close file.
	url_fclose(m_SavingContext->pOutputFormatContext->pb);

	// Release muxer parameter object.
	av_free(m_SavingContext->pOutputFormatContext);

	// release pOutputFormat ?

	return result;
}
SaveResult VideoFile::SaveFrame(Bitmap^ _image)
{
	SaveResult result = SaveResult::Success;

	

	return result;
}
// --------------------------------------- Private Methods


void VideoFile::ResetPrimarySelection()
{
	m_PrimarySelection->iAnalysisMode		= 0;
	m_PrimarySelection->bFiltered			= false;
	m_PrimarySelection->iCurrentFrame		= 0;
	m_PrimarySelection->iCurrentTimeStamp	= 0;
	m_PrimarySelection->iDurationFrame		= 0;
}
void VideoFile::ResetInfosVideo()
{
	// Read only
	m_InfosVideo->iFileSize = 0;
	m_InfosVideo->iWidth = 320;
	m_InfosVideo->iHeight = 240;
	m_InfosVideo->fPixelAspectRatio = 1.0f;
	m_InfosVideo->fFps = 1.0f;
	m_InfosVideo->bFpsIsReliable = false;
	m_InfosVideo->iFrameInterval = 40;
	m_InfosVideo->iDurationTimeStamps = 1;
	m_InfosVideo->iFirstTimeStamp = 0;
	m_InfosVideo->fAverageTimeStampsPerSeconds = 1.0f;
	
	// Read / Write
	m_InfosVideo->iDecodingWidth = 320;
	m_InfosVideo->iDecodingHeight = 240;
	m_InfosVideo->fDecodingStretchFactor = 1.0f;
	m_InfosVideo->iDecodingFlag = SWS_FAST_BILINEAR;
	m_InfosVideo->bDeinterlaced = false;
} 






int VideoFile::GetFirstStreamIndex(AVFormatContext* _pFormatCtx, int _iCodecType)
{
	//-----------------------------------------------
	// Look for the first stream of type _iCodecType.
	// return its index if found, -1 otherwise.
	//-----------------------------------------------

	unsigned int	iCurrentStreamIndex		= 0;
	unsigned int	iBestStreamIndex		= -1;
	int64_t			iBestFrames				= -1;

	// We loop around all streams and keep the one with most frames.
	while( iCurrentStreamIndex < _pFormatCtx->nb_streams)
	{
		if(_pFormatCtx->streams[iCurrentStreamIndex]->codec->codec_type == _iCodecType)
		{
			int64_t frames = _pFormatCtx->streams[iCurrentStreamIndex]->nb_frames;
			if(frames > iBestFrames)
			{
				iBestFrames = frames;
				iBestStreamIndex = iCurrentStreamIndex;
			}
		}
		iCurrentStreamIndex++;
	}

	return (int)iBestStreamIndex;
}

void VideoFile::DumpStreamsInfos(AVFormatContext* _pFormatCtx)
{
	log->Debug("Total Streams					: " + _pFormatCtx->nb_streams);

	for(int i = 0;i<(int)_pFormatCtx->nb_streams;i++)
	{
		log->Debug("Stream #" + i);
		switch((int)_pFormatCtx->streams[i]->codec->codec_type)
		{
		case CODEC_TYPE_UNKNOWN:
			log->Debug("	Type						: CODEC_TYPE_UNKNOWN");
			break;
		case CODEC_TYPE_VIDEO:
			log->Debug("	Type						: CODEC_TYPE_VIDEO");
			break;
		case CODEC_TYPE_AUDIO:
			log->Debug("	Type						: CODEC_TYPE_AUDIO");
			break;
		case CODEC_TYPE_DATA:
			log->Debug("	Type						: CODEC_TYPE_DATA");
			break;
		case CODEC_TYPE_SUBTITLE:
			log->Debug("	Type						: CODEC_TYPE_SUBTITLE");
			break;
		//case CODEC_TYPE_ATTACHMENT:
		//	log->Debug("	Type						: CODEC_TYPE_ATTACHMENT");
		//	break;
		default:
			log->Debug("	Type						: CODEC_TYPE_UNKNOWN");
			break;
		}
		log->Debug("	Language					: " + gcnew String(_pFormatCtx->streams[i]->language));
		log->Debug("	NbFrames					: " + _pFormatCtx->streams[i]->nb_frames);
	}
}




	

ImportStrategy VideoFile::PrepareSelection(int64_t% _iStartTimeStamp, int64_t% _iEndTimeStamp, bool _bForceReload)
{
	//--------------------------------------------------------------
	// détermine si la selection à réellement besoin d'être chargée.
	// Modifie simplement la selection en cas de réduction.
	// Spécifie où et quelles frames doivent être chargées sinon.
	//--------------------------------------------------------------

	ImportStrategy result;

	if(m_PrimarySelection->iAnalysisMode == 0 || _bForceReload)
	{
		//----------------------------------------------------------------------------		
		// On était pas en mode Analyse ou forcé : Chargement complet.
		// (Garder la liste même quand on sort du mode analyse, pour réutilisation ? )
		//----------------------------------------------------------------------------
		if(m_FrameList != nullptr)
		{
			for(int i = 0;i<m_FrameList->Count;i++) 
			{ 
				delete m_FrameList[i]->BmpImage;
				delete m_FrameList[i]; 
			}			
			delete m_FrameList;
		}

		log->Debug(String::Format("Preparing Selection for import : [{0}]->[{1}].", _iStartTimeStamp, _iEndTimeStamp));
		m_FrameList = gcnew List<DecompressedFrame^>();

		result = ImportStrategy::Complete; 
	}
	else
	{
		// Traitement différent selon les frames déjà chargées...
		
		if(m_FrameList == nullptr)
		{
			// Ne devrait pas passer par là.
			m_FrameList = gcnew List<DecompressedFrame^>();
			result = ImportStrategy::Complete; 
		}
		else
		{
			int64_t iOldStart = m_FrameList[0]->iTimeStamp;
			int64_t iOldEnd = m_FrameList[m_FrameList->Count - 1]->iTimeStamp;

			log->Debug(String::Format("Preparing Selection for import. Current selection: [{0}]->[{1}], new selection: [{2}]->[{3}].", iOldStart, iOldEnd, _iStartTimeStamp, _iEndTimeStamp));

			// Since some videos are causing problems in timestamps reported, it is possible that we end up with double updates.
			// e.g. reduction at left AND expansion at right, expansion on both sides, etc.
			// We'll deal with reduction first, then expansions.

			if(_iEndTimeStamp < iOldEnd)
			{
				log->Debug("Frames needs to be deleted at the end of the existing selection.");					
				int iNewLastIndex = (int)GetFrameNumber(_iEndTimeStamp);
				
				for(int i=iNewLastIndex+1;i<m_FrameList->Count;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}
				m_FrameList->RemoveRange(iNewLastIndex+1, (m_FrameList->Count-1) - iNewLastIndex);

				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}

			if(_iStartTimeStamp > iOldStart)
			{
				log->Debug("Frames needs to be deleted at the begining of the existing selection.");
				int iNewFirstIndex = (int)GetFrameNumber(_iStartTimeStamp);
				
				for(int i=0;i<iNewFirstIndex;i++)
				{
					delete m_FrameList[i]->BmpImage;
				}

				m_FrameList->RemoveRange(0, iNewFirstIndex);
				
				// Reduced until further notice.
				result = ImportStrategy::Reduction;
			}


			// We expand the selection if the new sentinel is at least one frame out the current selection.
			// Expanding on both sides is not supported yet.

			if(_iEndTimeStamp >= iOldEnd + m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the end of the existing selection.");
				_iStartTimeStamp = iOldEnd;
				result = ImportStrategy::InsertionAfter;
			}
			else if(_iStartTimeStamp <= iOldStart - m_InfosVideo->iAverageTimeStampsPerFrame)
			{
				log->Debug("Frames needs to be added at the begining of the existing selection.");
				_iEndTimeStamp = iOldStart;
				result = ImportStrategy::InsertionBefore;
			}
		}
	}

	return result;
}

int VideoFile::EstimateNumberOfFrames( int64_t _iStartTimeStamp, int64_t _iEndTimeStamp) 
{
	//-------------------------------------------------------------
	// Calcul du nombre d'images à charger (pour le ReportProgress)
	//-------------------------------------------------------------
	int iEstimatedNumberOfFrames = 0;
	int64_t iSelectionDurationTimeStamps;
	if(_iEndTimeStamp == -1) 
	{ 
		iSelectionDurationTimeStamps = m_InfosVideo->iDurationTimeStamps - _iStartTimeStamp; 
	}
	else 
	{ 
		iSelectionDurationTimeStamps = _iEndTimeStamp - _iStartTimeStamp; 
	}

	iEstimatedNumberOfFrames = (int)(iSelectionDurationTimeStamps / m_InfosVideo->iAverageTimeStampsPerFrame);

	return iEstimatedNumberOfFrames;
}
void VideoFile::RescaleAndConvert(AVFrame* _pOutputFrame, AVFrame* _pInputFrame, int _OutputWidth, int _OutputHeight, int _OutputFmt, bool _bDeinterlace)
{
	//------------------------------------------------------------------------
	// Function used by GetNextFrame, ImportAnalysis and SaveMovie.
	// Take the frame we just decoded and turn it to the right size/deint/fmt.
	// todo: sws_getContext could be done only once.
	//------------------------------------------------------------------------
	SwsContext* pSWSCtx = sws_getContext(m_pCodecCtx->width, m_pCodecCtx->height, m_pCodecCtx->pix_fmt, _OutputWidth, _OutputHeight, (PixelFormat)_OutputFmt, m_InfosVideo->iDecodingFlag, NULL, NULL, NULL); 

	if(_bDeinterlace)
	{
		AVPicture*	pDeinterlacingFrame;
		AVPicture	tmpPicture;

		// Deinterlacing happens before resizing.
		int iSizeDeinterlaced = avpicture_get_size(m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);
		uint8_t* pDeinterlaceBuffer = new uint8_t[iSizeDeinterlaced];
		pDeinterlacingFrame = &tmpPicture;
		avpicture_fill(pDeinterlacingFrame, pDeinterlaceBuffer, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		int resDeint = avpicture_deinterlace(pDeinterlacingFrame, (AVPicture*)_pInputFrame, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		if(resDeint < 0) 
		{
			// Deinterlacing failed, use original image.
			av_free(pDeinterlaceBuffer);
			pDeinterlaceBuffer = nullptr;
			sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
		}
		else
		{
			// Use deinterlaced image.
			sws_scale(pSWSCtx, pDeinterlacingFrame->data, pDeinterlacingFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
		}
	}
	else
	{
		sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
	}

	sws_freeContext(pSWSCtx);

}
void VideoFile::MoveToTimestamp(int64_t _iPosition)
{
	// Revenir au timestamp demandé, ou avant.
	log->Debug(String::Format("Seeking to [{0}]", _iPosition));
	av_seek_frame(m_pFormatCtx, m_iVideoStream, _iPosition, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers( m_pFormatCtx->streams[m_iVideoStream]->codec);
}



AVOutputFormat* VideoFile::GuessOutputFormat(String^ _FilePath, bool _bHasMetadata)
{
	//---------------------------------------------------------------
	// Pour trouver la liste des noms des formats,
	// chercher la structure AVOutputFormat dans le source du format.
	// généralement à la fin du fichier.
	//---------------------------------------------------------------

	AVOutputFormat*		pOutputFormat;

	String^ Filepath = gcnew String(_FilePath->ToLower());

	if(Filepath->EndsWith("mkv") || _bHasMetadata)
	{
		pOutputFormat = guess_format("matroska", nullptr, nullptr);
	}
	else if(Filepath->EndsWith("mp4")) 
	{
		pOutputFormat = guess_format("mp4", nullptr, nullptr);
	}
	else
	{
		pOutputFormat = guess_format("avi", nullptr, nullptr);
	}

	return pOutputFormat;
}
bool VideoFile::NeedEncoding(int _iFramesInterval, bool _bFlushDrawings, bool _bKeyframesOnly)
{

	// La selection est déjà en MPEG4-ASP, on a pas filtré les frames et on reste à la même vitesse.
	// => Ne pas décoder, muxer directement les Packets. Plus rapide et pas de perte de qualité.

	bool bNeedEncoding = false;

	if(m_pCodecCtx->codec_id != CODEC_ID_MPEG4)
	{
		bNeedEncoding = true;
	}

	if((m_PrimarySelection->iAnalysisMode == 1) /*&& (m_PrimarySelection->bFiltered)*/)
	{
		bNeedEncoding = true;
	}
	
	if(_iFramesInterval != m_InfosVideo->iFrameInterval)
	{
		bNeedEncoding = true;	
	}

	if(_bFlushDrawings)
	{
		bNeedEncoding = true;
	}

	if(_bKeyframesOnly)
	{
		bNeedEncoding = true;
	}

	return bNeedEncoding;
}

int VideoFile::GetInputBitrate(int _iOutputWidth, int _iOutputHeight)
{
	// Le bitrate ne servira que lors de l'enregistrement.

	int iBitrate = 0;
	
	double fDurationSeconds = (double)m_InfosVideo->iDurationTimeStamps / m_InfosVideo->fAverageTimeStampsPerSeconds;
	int iBitrateFromFileSize = (int)((double)(m_InfosVideo->iFileSize*8) / fDurationSeconds);

	log->Debug("Format bitrate					: " + m_pFormatCtx->bit_rate);
	log->Debug("Codec Bitrate					: " + m_pCodecCtx->bit_rate);
	log->Debug("FileSize Bitrate				: " + iBitrateFromFileSize);

	// On prend le plus grand des trois.
	iBitrate = Math::Max(iBitrateFromFileSize, Math::Max(m_pFormatCtx->bit_rate, m_pCodecCtx->bit_rate)); 

	if(iBitrate < 750000)
	{
		iBitrate = 5000000;
		log->Debug("Bitrates non computable or suspiciously low. Manually setting the bitrate to 5Mb/s.");		
	}
	else
	{
		log->Debug("Final bitrate estimation		: " + iBitrate);	
	}

	return iBitrate;
}
bool VideoFile::SetupMuxer(AVFormatContext* _pOutputFormatContext, AVOutputFormat* _pOutputFormat, char* _pFilePath, int _iBitrate)
{
	bool bResult = true;

	_pOutputFormatContext->oformat = _pOutputFormat;
	
	av_strlcpy(_pOutputFormatContext->filename, _pFilePath, sizeof(_pOutputFormatContext->filename));
		
	_pOutputFormatContext->timestamp = 0;
		
	_pOutputFormatContext->bit_rate = _iBitrate;

		
	// Paramètres (par défaut ?) du muxeur
	AVFormatParameters	fpOutFile;
	memset(&fpOutFile, 0, sizeof(AVFormatParameters));
	if (av_set_parameters(_pOutputFormatContext, &fpOutFile) < 0)
	{
		log->Error("muxer parameters not set");
		return false;
	}

	// ?
	_pOutputFormatContext->preload   = (int)(0.5 * AV_TIME_BASE);
	_pOutputFormatContext->max_delay = (int)(0.7 * AV_TIME_BASE); 

	return bResult;
}
bool VideoFile::SetupEncoder(AVCodecContext* _pOutputCodecContext, AVCodec* _pOutputCodec, Size _OutputSize, int _iFramesInterval, int _iBitrate)
{
	//----------------------------------------
	// Parameters for encoding.
	// some tweaked, some taken from Mencoder.
	// Not all clear...
	//----------------------------------------

	// TODO:
	// Implement from ref: http://www.mplayerhq.hu/DOCS/HTML/en/menc-feat-dvd-mpeg4.html


	// Codec.
	// Equivalent to : -vcodec mpeg4
	_pOutputCodecContext->codec_id = _pOutputCodec->id;
	_pOutputCodecContext->codec_type = CODEC_TYPE_VIDEO;

	// The average bitrate (unused for constant quantizer encoding.)
	// Source: Input video or computed from file size. 
	_pOutputCodecContext->bit_rate = _iBitrate;

	// Number of bits the bitstream is allowed to diverge from the reference.
    // the reference can be CBR (for CBR pass1) or VBR (for pass2)
	// Source: Avidemux.
	_pOutputCodecContext->bit_rate_tolerance = 8000000;


	// Motion estimation algorithm used for video coding. 
	// src: MEncoder.
	_pOutputCodecContext->me_method = ME_EPZS;

	// Framerate - timebase.
	// Certains codecs (MPEG1/2) ne supportent qu'un certain nombre restreints de framerates.
	// src [kinovea]
	if(_iFramesInterval == 0)
		_iFramesInterval = 40;

	int iFramesPerSecond = 1000 / _iFramesInterval;
	_pOutputCodecContext->time_base.den			= iFramesPerSecond ;
	_pOutputCodecContext->time_base.num			= 1;


	// Picture width / height.
	// src: [kinovea]
	_pOutputCodecContext->width					= _OutputSize.Width;
	_pOutputCodecContext->height				= _OutputSize.Height;
	

	//-------------------------------------------------------------------------------------------
	// Mode d'encodage (i, b, p frames)
	//
	// gop_size		: the number of pictures in a group of pictures, or 0 for intra_only. (default : 12)
	// max_b_frames	: maximum number of B-frames between non-B-frames (default : 0)
	//				  Note: The output will be delayed by max_b_frames+1 relative to the input.
	//
	// [kinovea]	: Intra only so we can always access prev frame right away in the Player.
	// [kinovea]	: Player doesn't support B-frames.
	//-------------------------------------------------------------------------------------------
	_pOutputCodecContext->gop_size				= 0;	
	_pOutputCodecContext->max_b_frames			= 0;								

	// Pixel format
	// src:ffmpeg.
	_pOutputCodecContext->pix_fmt = PIX_FMT_YUV420P; 	


	// Frame rate emulation. If not zero, the lower layer (i.e. format handler) has to read frames at native frame rate.
	// src: ?
	// ->rate_emu


	// Quality/Technique of encoding.
	//_pOutputCodecContext->flags |= ;			// CODEC_FLAG_QSCALE : Constant Quantization = Best quality but innacceptably high file sizes.
	_pOutputCodecContext->qcompress = 0.5;		// amount of qscale change between easy & hard scenes (0.0-1.0) 
    _pOutputCodecContext->qblur = 0.5;			// amount of qscale smoothing over time (0.0-1.0)
	_pOutputCodecContext->qmin = 2;				// minimum quantizer (def:2)
	_pOutputCodecContext->qmax = 16;			// maximum quantizer (def:31)
	_pOutputCodecContext->max_qdiff = 3;		// maximum quantizer difference between frames (def:3)
	_pOutputCodecContext->mpeg_quant = 0;		// 0 -> h263 quant, 1 -> mpeg quant. (def:0)
	//_pOutputCodecContext->b_quant_factor (qscale factor between IP and B-frames)


	// Sample Aspect Ratio.
	
	// Assume PAR=1:1 (square pixels).
	_pOutputCodecContext->sample_aspect_ratio.num = 1;
	_pOutputCodecContext->sample_aspect_ratio.den = 1;
			
	if(m_pCodecCtx != nullptr)
	{
		if(m_pCodecCtx->sample_aspect_ratio.num != 0 && m_pCodecCtx->sample_aspect_ratio.num != m_pCodecCtx->sample_aspect_ratio.den)
		{
			// -> Anamorphic video, non square pixels.
			
			if(m_pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO)
			{
				// If MPEG, sample_aspect_ratio is actually the DAR...
				// Reference for weird decision tree: mpeg12.c at mpeg_decode_postinit().
				double fDisplayAspectRatio	= (double)m_pCodecCtx->sample_aspect_ratio.num / (double)m_pCodecCtx->sample_aspect_ratio.den;
				double fPixelAspectRatio	= ((double)m_pCodecCtx->height * fDisplayAspectRatio) / (double)m_pCodecCtx->width;
	
				if(fPixelAspectRatio > 1.0f)
				{
					// In this case the input sample aspect ratio was actually the display aspect ratio.
					// We will recompute the aspect ratio.
					int gcd = GreatestCommonDenominator((int)((double)m_pCodecCtx->width*fPixelAspectRatio), m_pCodecCtx->width);
					_pOutputCodecContext->sample_aspect_ratio.num = (int)(((double)m_pCodecCtx->width*fPixelAspectRatio)/gcd);
					_pOutputCodecContext->sample_aspect_ratio.den = m_pCodecCtx->width/gcd;
				}
				else
				{
					_pOutputCodecContext->sample_aspect_ratio.num = m_pCodecCtx->sample_aspect_ratio.num;
					_pOutputCodecContext->sample_aspect_ratio.den = m_pCodecCtx->sample_aspect_ratio.den;
				}
			}
			else
			{
				_pOutputCodecContext->sample_aspect_ratio.num = m_pCodecCtx->sample_aspect_ratio.num;
				_pOutputCodecContext->sample_aspect_ratio.den = m_pCodecCtx->sample_aspect_ratio.den;
			}	
		}
	}

	
	// Todo: Utiliser ce pointeur pour stocker les metadonnées ?
	// Private data of the user, can be used to carry app specific stuff.
	// void *opaque;
	

	//-----------------------------------
	// h. Other settings. (From MEncoder) 
	//-----------------------------------
	_pOutputCodecContext->strict_std_compliance= -1;		// strictly follow the standard (MPEG4, ...)
	_pOutputCodecContext->luma_elim_threshold = 0;		// luma single coefficient elimination threshold
	_pOutputCodecContext->chroma_elim_threshold = 0;		// chroma single coeff elimination threshold
	_pOutputCodecContext->lumi_masking = 0.0;;
	_pOutputCodecContext->dark_masking = 0.0;
	// codecContext->codec_tag							// 4CC : if not set then the default based on codec_id will be used.
	// pre_me (prepass for motion estimation)
	// sample_rate
	// codecContext->channels = 2;
	// codecContext->mb_decision = 0;

	return true;

}
int VideoFile::GreatestCommonDenominator(int a, int b)
{
     if (a == 0) return b;
     if (b == 0) return a;

     if (a > b)
        return GreatestCommonDenominator(a % b, b);
     else
        return GreatestCommonDenominator(a, b % a);
}
bool VideoFile::SetupEncoderForCopy(AVCodecContext* _pOutputCodecContext, AVStream* _pOutputVideoStream)
{
	
	_pOutputCodecContext->codec_id = m_pCodecCtx->codec_id;
    _pOutputCodecContext->codec_type = m_pCodecCtx->codec_type;

	// FourCC
    _pOutputCodecContext->codec_tag = m_pCodecCtx->codec_tag;

	// Others
    _pOutputCodecContext->bit_rate = m_pCodecCtx->bit_rate;
    _pOutputCodecContext->extradata= m_pCodecCtx->extradata;
    _pOutputCodecContext->extradata_size= m_pCodecCtx->extradata_size;

	//mini fix du timebase.
    if(  av_q2d(m_pCodecCtx->time_base) > av_q2d(m_pFormatCtx->streams[m_iVideoStream]->time_base) && 
		 av_q2d(m_pFormatCtx->streams[m_iVideoStream]->time_base) < 1.0/1000)
	{
        _pOutputCodecContext->time_base = m_pCodecCtx->time_base;
	}
	else
	{
        _pOutputCodecContext->time_base = m_pFormatCtx->streams[m_iVideoStream]->time_base;
	}

    _pOutputCodecContext->pix_fmt = m_pCodecCtx->pix_fmt;
    _pOutputCodecContext->width = m_pCodecCtx->width;
    _pOutputCodecContext->height = m_pCodecCtx->height;
    _pOutputCodecContext->has_b_frames = m_pCodecCtx->has_b_frames;

	return true;
}
bool VideoFile::WriteFrame(int _iEncodedSize, AVFormatContext* _pOutputFormatContext, AVCodecContext* _pOutputCodecContext, AVStream* _pOutputVideoStream, uint8_t* _pOutputVideoBuffer, bool bForceKeyframe)
{
	if (_iEncodedSize > 0) 
	{

		// h. Placer le résultat de l'encodage dans un Packet.
		AVPacket OutputPacket;
		av_init_packet(&OutputPacket);

		// i. Calculer la position du Packet.
		OutputPacket.pts = av_rescale_q(_pOutputCodecContext->coded_frame->pts, _pOutputCodecContext->time_base, _pOutputVideoStream->time_base);
		//Console::WriteLine("OutputPacket.pts : {0}", OutputPacket.pts);


		// j. Flagguer les Keyframes, normalement toutes car on est en "intra only".
		if(_pOutputCodecContext->coded_frame->key_frame || bForceKeyframe)
		{
			OutputPacket.flags |= PKT_FLAG_KEY;
			//Console::WriteLine("WriteFrame : PKT_FLAG_KEY)");
		}

		// k. Associer les buffers
		OutputPacket.stream_index = _pOutputVideoStream->index;
		OutputPacket.data= _pOutputVideoBuffer;
		OutputPacket.size= _iEncodedSize;

		// l. Persister la frame dans le fichier de sortie
		int iWriteRes = av_write_frame(_pOutputFormatContext, &OutputPacket);
	} 
	else
	{
		// la frame à été bufferisée (?)
		log->Error("encoded size not positive");
	}

	return true;
}

bool VideoFile::WriteMetadata(AVFormatContext* _pOutputFormatContext, AVCodecContext* _pOutputCodecContext, AVStream* _pOutputVideoStream, AVStream* _pOutputDataStream, String^ _Metadata)
{
	
	// Création du Packet correspondant
	AVPacket OutputPacket;
	av_init_packet(&OutputPacket);					
	
	// Position du Packet. On réutilise les données du stream vidéo. (?)
	OutputPacket.pts = av_rescale_q(_pOutputCodecContext->coded_frame->pts, _pOutputCodecContext->time_base, _pOutputVideoStream->time_base);

	// Stream de sous-titre
	OutputPacket.stream_index = _pOutputDataStream->index;

	// Pas d'encodage particulier
	char* pMetadata	= static_cast<char*>(Marshal::StringToHGlobalAnsi(_Metadata).ToPointer());

	OutputPacket.data = (uint8_t*)pMetadata;
	OutputPacket.size = _Metadata->Length;
	
	// Ecriture dans le fichier de sortie.
	int iWriteRes = av_write_frame(_pOutputFormatContext, &OutputPacket);
	
	Marshal::FreeHGlobal(safe_cast<IntPtr>(pMetadata));

	return true;
}


bool VideoFile::EncodeAndWriteVideoFrame(AVFormatContext* _pOutputFormatContext, AVCodecContext* _pOutputCodecContext, AVStream* _pOutputVideoStream, int _iOutputWidth, int _iOutputHeight, SwsContext* _pScalingContext, AVFrame* _pInputFrame)
{
	//-----------------------------------------------------------
	// This version of the method takes an AVFrame as input.
	// used when we got the frame directly from the input stream.
	//-----------------------------------------------------------

	bool bWritten = false;

	//sab
	do
	{
		// a. L'objet frame receptacle de sortie.
		AVFrame* pOutputFrame;
		if ((pOutputFrame = avcodec_alloc_frame()) == nullptr) 
		{
			log->Error("output frame not allocated");
			break;
		}
		
		// b. Le poids d'une image selon le PIX_FMT de sortie et la taille donnée. 
		int iSizeOutputFrameBuffer = avpicture_get_size(_pOutputCodecContext->pix_fmt, _iOutputWidth, _iOutputHeight);
		
		// c. Allouer le buffer contenant les données réelles de la frame.
		uint8_t* pOutputFrameBuffer = (uint8_t*)av_malloc(iSizeOutputFrameBuffer);
		if (pOutputFrameBuffer == nullptr) 
		{
			log->Error("output frame buffer not allocated");
			av_free(pOutputFrame);
			break;
		}
		
		// d. Mise en place de pointeurs internes reliant certaines adresses à d'autres.
		int iImageDataSize = avpicture_fill((AVPicture *)pOutputFrame, pOutputFrameBuffer, _pOutputCodecContext->pix_fmt, _iOutputWidth, _iOutputHeight);
		
		// e. Convertir l'image de son format de pixels d'origine vers le format de pixels de sortie.
		if (sws_scale(_pScalingContext, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, pOutputFrame->data, pOutputFrame->linesize) < 0) 
		{
			log->Error("scaling failed");
			av_free(pOutputFrameBuffer);
			av_free(pOutputFrame);
			break;
		}


		//------------------------------------------------------------------------------------------
		// -> Ici, pOutputFrame contient une bitmap non compressée, au nouveau PIX_FMT. (=> YUV420P)
		//------------------------------------------------------------------------------------------


		// f. allouer le buffer pour les données de la frame après compression. ( -> valeur tirée de ffmpeg.c)
		int iSizeOutputVideoBuffer = 4 *  _iOutputWidth *  _iOutputHeight;		
		uint8_t* pOutputVideoBuffer = (uint8_t*)av_malloc(iSizeOutputVideoBuffer);
		if (pOutputVideoBuffer == nullptr) 
		{
			log->Error("output video buffer not allocated");
			av_free(pOutputFrameBuffer);
			av_free(pOutputFrame);
			break;
		}
		
		// g. encodage vidéo.
		// AccessViolationException ? => problème de memalign. Recompiler libavc avec le bon gcc.
		int iEncodedSize = avcodec_encode_video(_pOutputCodecContext, pOutputVideoBuffer, iSizeOutputVideoBuffer, pOutputFrame);
		
		// Ecriture du packet vidéo dans le fichier 
		if(!WriteFrame(iEncodedSize, _pOutputFormatContext, _pOutputCodecContext, _pOutputVideoStream, pOutputVideoBuffer, false ))
		{
			log->Error("problem while writing frame to file");
		}
		
		// Cleanup local
		av_free(pOutputFrameBuffer);
		av_free(pOutputFrame);
		av_free(pOutputVideoBuffer);

		bWritten = true;
	}
	while(false);

	return bWritten;

}
bool VideoFile::EncodeAndWriteVideoFrame(AVFormatContext* _pOutputFormatContext, AVCodecContext* _pOutputCodecContext, AVStream* _pOutputVideoStream, int _iOutputWidth, int _iOutputHeight, SwsContext* _pScalingContext, Bitmap^ _InputBitmap)
{

	//----------------------------------------------------------------------------------------
	// This version of the method takes a Bitmap as input.
	// used when we got the frame from the analysis array or a frame flush (blended drawings).
	// Input pix fmt must be PIX_FMT_BGR24.
	//----------------------------------------------------------------------------------------

	bool bWritten = false;
	bool bInputFrameAllocated = false;
	bool bOutputFrameAllocated = false;
	bool bBitmapLocked = false;
	
	AVFrame* pInputFrame;
	uint8_t* pInputFrameBuffer;
	AVFrame* pOutputFrame;
	uint8_t* pOutputFrameBuffer;
	System::Drawing::Imaging::BitmapData^ InputDataBitmap;

	//Console::WriteLine("Encoding frame. Input pixfmt:{0}, Output pixfmt:{1}", (int)_pOutputCodecContext->pix_fmt);

	//sab
	do
	{
		// a. L'objet frame d'input que l'on va remplir avec la bitmap.
		if ((pInputFrame = avcodec_alloc_frame()) == nullptr) 
		{
			log->Error("input frame not allocated");
			break;
		}
		
		// b. Le poids d'une image selon le PIX_FMT de la bitmap et la taille donnée. 
		int iSizeInputFrameBuffer = avpicture_get_size(PIX_FMT_BGR24, _InputBitmap->Width, _InputBitmap->Height);
		
		// c. Allouer le buffer contenant les données réelles de la frame.
		pInputFrameBuffer = (uint8_t*)av_malloc(iSizeInputFrameBuffer);
		if (pInputFrameBuffer == nullptr) 
		{
			log->Error("input frame buffer not allocated");
			av_free(pInputFrame);
			break;
		}

		bInputFrameAllocated = true;
		
		// d. Mise en place de pointeurs internes reliant certaines adresses à d'autres.
		avpicture_fill((AVPicture *)pInputFrame, pInputFrameBuffer, PIX_FMT_BGR24, _InputBitmap->Width, _InputBitmap->Height);
		
		// e. Associer les données de la Bitmap à la Frame.
		Rectangle rect = Rectangle(0, 0, _InputBitmap->Width, _InputBitmap->Height);
		InputDataBitmap = _InputBitmap->LockBits(rect, Imaging::ImageLockMode::ReadOnly, _InputBitmap->PixelFormat);
		bBitmapLocked = true;

		// todo : pin_ptr ?
		uint8_t* data = (uint8_t*)InputDataBitmap->Scan0.ToPointer();
		pInputFrame->data[0] = data;
		pInputFrame->linesize[0] = InputDataBitmap->Stride;
			

		//------------------------------------------------------------------------------------------
		// -> Ici, pInputFrame contient une bitmap non compressée, au PIX_FMT .NET (BGR24)
		//------------------------------------------------------------------------------------------

		// f. L'objet frame receptacle de sortie.
		if ((pOutputFrame = avcodec_alloc_frame()) == nullptr) 
		{
			log->Error("output frame not allocated");
			break;
		}
		
		// g. Le poids d'une image selon le PIX_FMT de sortie et la taille donnée. 
		int iSizeOutputFrameBuffer = avpicture_get_size(_pOutputCodecContext->pix_fmt, _iOutputWidth, _iOutputHeight);
		
		// h. Allouer le buffer contenant les données réelles de la frame.
		pOutputFrameBuffer = (uint8_t*)av_malloc(iSizeOutputFrameBuffer);
		if (pOutputFrameBuffer == nullptr) 
		{
			log->Error("output frame buffer not allocated");
			av_free(pOutputFrame);
			break;
		}

		bOutputFrameAllocated = true;

		// i. Mise en place de pointeurs internes reliant certaines adresses à d'autres.
		avpicture_fill((AVPicture *)pOutputFrame, pOutputFrameBuffer, _pOutputCodecContext->pix_fmt, _iOutputWidth, _iOutputHeight);
		
		// j. Nouveau scaling context
		SwsContext* scalingContext = sws_getContext(_InputBitmap->Width, _InputBitmap->Height, PIX_FMT_BGR24, _iOutputWidth, _iOutputHeight, _pOutputCodecContext->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL); 

		// k. Convertir l'image de son format de pixels d'origine vers le format de pixels de sortie.
		if (sws_scale(scalingContext, pInputFrame->data, pInputFrame->linesize, 0, _InputBitmap->Height, pOutputFrame->data, pOutputFrame->linesize) < 0) 
		{
			log->Error("scaling failed");
			sws_freeContext(scalingContext);
			break;
		}

		sws_freeContext(scalingContext);


		//------------------------------------------------------------------------------------------
		// -> Ici, pOutputFrame contient une bitmap non compressée, au nouveau PIX_FMT. (=> YUV420P)
		//------------------------------------------------------------------------------------------


		// f. allouer le buffer pour les données de la frame après compression. ( -> valeur tirée de ffmpeg.c)
		int iSizeOutputVideoBuffer = 4 *  _iOutputWidth *  _iOutputHeight;		
		uint8_t* pOutputVideoBuffer = (uint8_t*)av_malloc(iSizeOutputVideoBuffer);
		if (pOutputVideoBuffer == nullptr) 
		{
			log->Error("output video buffer not allocated");
			break;
		}
		
		// g. encodage vidéo.
		// AccessViolationException ? => problème de memalign. Recompiler libavc avec le bon gcc.
		int iEncodedSize = avcodec_encode_video(_pOutputCodecContext, pOutputVideoBuffer, iSizeOutputVideoBuffer, pOutputFrame);
		
		// Ecriture du packet vidéo dans le fichier (Force Keyframe)
		if(!WriteFrame(iEncodedSize, _pOutputFormatContext, _pOutputCodecContext, _pOutputVideoStream, pOutputVideoBuffer, true ))
		{
			log->Error("problem while writing frame to file");
		}
		
		av_free(pOutputVideoBuffer);

		bWritten = true;
	}
	while(false);

	// Cleanup
	if(bInputFrameAllocated)
	{
		av_free(pInputFrameBuffer);
		av_free(pInputFrame);
	}

	if(bBitmapLocked)
	{
		_InputBitmap->UnlockBits(InputDataBitmap);
	}

	if(bOutputFrameAllocated)
	{
		av_free(pOutputFrameBuffer);
		av_free(pOutputFrame);
	}

	return bWritten;
}




#ifdef TRACE
float VideoFile::TraceMemoryUsage(PerformanceCounter^ _ramCounter, float _fLastRamValue, String^ _comment, float* fRamBalance) 
{
	float fCurrentRam = _ramCounter->NextValue();
	float fDelta = _fLastRamValue - fCurrentRam;
	
	*fRamBalance = *fRamBalance + fDelta;

	if(fDelta > 0)
	{
		Console::WriteLine("RAM: +{0} KB ({2}) Balance:{1}", fDelta, *fRamBalance, _comment );
	}
	else
	{
		Console::WriteLine("RAM: {0} KB ({2}) Balance:{1}", fDelta, *fRamBalance, _comment );
	}
	
    return _ramCounter->NextValue(); 
}
#endif

}	
}