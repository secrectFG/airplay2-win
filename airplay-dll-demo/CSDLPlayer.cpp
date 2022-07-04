#include "CSDLPlayer.h"
#include <stdio.h>
#include "CAutoLock.h"
#include <libyuv/convert_argb.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>

using namespace std;

/* This function may run in a separate event thread */
int FilterEvents(const SDL_Event* event) {
// 	static int boycott = 1;
// 
// 	/* This quit event signals the closing of the window */
// 	if ((event->type == SDL_QUIT) && boycott) {
// 		printf("Quit event filtered out -- try again.\n");
// 		boycott = 0;
// 		return(0);
// 	}
// 	if (event->type == SDL_MOUSEMOTION) {
// 		printf("Mouse moved to (%d,%d)\n",
// 			event->motion.x, event->motion.y);
// 		return(0);    /* Drop it, we've handled it */
// 	}
	return(1);
}

CSDLPlayer::CSDLPlayer()
	: m_surface(NULL)
	, m_yuv(NULL)
	, m_bAudioInited(false)
	, m_bDumpAudio(false)
	, m_fileWav(NULL)
	, m_sAudioFmt()
	, m_rect()
	, m_server()
	//, m_fRatio(1.0f)
{
	ZeroMemory(&m_sAudioFmt, sizeof(SFgAudioFrame));
	ZeroMemory(&m_rect, sizeof(SDL_Rect));
	m_mutexAudio = CreateMutex(NULL, FALSE, NULL);
	m_mutexVideo = CreateMutex(NULL, FALSE, NULL);
	m_toVcamBuffer = new uint8_t[VCAM_WIDTH * VCAM_HEIGHT * 4];
	memset(m_toVcamBuffer, 0, VCAM_WIDTH * VCAM_HEIGHT * 4);
}

CSDLPlayer::~CSDLPlayer()
{
	unInit();

	CloseHandle(m_mutexAudio);
	CloseHandle(m_mutexVideo);

	delete m_toVcamBuffer;
}

bool CSDLPlayer::init()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}

	/* Clean up on exit, exit on window close and interrupt */
	atexit(SDL_Quit);

	SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_IGNORE);

#if TEST_SHOW
	ifstream inFile("testdump.dat", ios::in | ios::binary); //二进制读方式打开
	inFile.read((char*)&d, sizeof(d));
	vector<char>* vec = new vector<char>();
	char c;
	while (inFile.read(&c, 1))
	{
		vec->push_back(c);
	}
	d.data = (unsigned char*) & (*vec)[0];

	initVideo(d.width, d.height);
#else
	initVideo(300, 100);
#endif // TEST_SHOW

	

	

	/* Filter quit and mouse motion events */
	SDL_SetEventFilter(FilterEvents);

	m_server.start(this);
	SDL_WM_SetCaption("AirPlay Demo - Started [s - start server, q - stop server]", NULL);

	m_vcamShared = new SharedImageMemory(0);
	auto b = m_vcamShared->SendIsReady();
	if(!b){
		printf("SharedImageMemory open failed!");
		MessageBox(nullptr,TEXT("虚拟摄像头启动失败"), TEXT(""), 0);
		exit(1);
	}
#if TEST_SHOW
	m_thread = thread([this] {
			while (1)
			{
				Sleep(1000 / 60);
				if (m_vcamShared != nullptr && m_argbBuffer != nullptr)
					outputVideo(&d);
				else break;
			}
		});
#else
	m_keepVcamRenderThread = thread([this] {
		while (1)
		{
			Sleep(1000/60);
			if (m_vcamShared == nullptr)break;

			if (GetTickCount64() - lastRenderTime > 1000 / 20) {
				lastRenderTime = GetTickCount64();
				if (m_argbBuffer != nullptr) {
					auto width = lw;
					auto height = lh;
					if (m_vcamShared == nullptr)break;
#if CONVERT_RESOLUTION
					//printf("render %lld\n %d %d", lastRenderTime, lw, lh);
					/**/
					m_vcamShared->Send(VCAM_WIDTH, VCAM_HEIGHT, VCAM_WIDTH, VCAM_WIDTH * VCAM_HEIGHT * 4,
						SharedImageMemory::EFormat::FORMAT_UINT8,
						SharedImageMemory::EResizeMode::RESIZEMODE_DISABLED,
						SharedImageMemory::EMirrorMode::MIRRORMODE_DISABLED,
						1000, m_toVcamBuffer);
#else
					m_vcamShared->Send(width, height, width, width * height * 4,
						SharedImageMemory::EFormat::FORMAT_UINT8,
						SharedImageMemory::EResizeMode::RESIZEMODE_DISABLED,
						SharedImageMemory::EMirrorMode::MIRRORMODE_DISABLED,
						1000, m_argbBuffer);
#endif
				}
			}
		}
		});
#endif

	return true;
}

void CSDLPlayer::unInit()
{
	if (m_vcamShared)delete m_vcamShared;
	if (m_argbBuffer)delete m_argbBuffer;

	m_vcamShared = nullptr;
	m_argbBuffer = nullptr;

#if TEST_SHOW
	m_thread.join();
#else
	m_keepVcamRenderThread.join();
#endif

	unInitVideo();
	unInitAudio();

	SDL_Quit();
}

void CSDLPlayer::loopEvents()
{
	SDL_Event event;

	BOOL bEndLoop = FALSE;
	/* Loop waiting for ESC+Mouse_Button */
	while (SDL_WaitEvent(&event) >= 0) {
		switch (event.type) {
		case SDL_USEREVENT: {
			if (event.user.code == VIDEO_SIZE_CHANGED_CODE) {
				unsigned int width = (unsigned int)event.user.data1;
				unsigned int height = (unsigned int)event.user.data2;
				if (width != m_rect.w || height != m_rect.h || m_yuv == NULL) {
					unInitVideo();
					initVideo(width, height);
				}
			}
			break;
		}
		case SDL_VIDEOEXPOSE: {
			break;
		}
		case SDL_ACTIVEEVENT: {
			if (event.active.state & SDL_APPACTIVE) {
				if (event.active.gain) {
					//printf("App activated\n");
				}
				else {
					//printf("App iconified\n");
				}
			}
			break;
		}
		case SDL_KEYUP: {
			switch (event.key.keysym.sym)
			{
				case SDLK_q: {
					printf("SDLK_q key down\n");
					m_server.stop();
					SDL_WM_SetCaption("AirPlay Demo - Stopped [s - start server, q - stop server]", NULL);
					break;
				}
				case SDLK_s: {
					printf("SDLK_s key down\n");
					m_server.start(this);
					SDL_WM_SetCaption("AirPlay Demo - Started [s - start server, q - stop server]", NULL);
					break;
				}
				case SDLK_EQUALS: {
					printf("SDLK_EQUALS key down\n");
					m_fRatio *= 1.02f;
					//m_fRatio = m_server.setVideoScale(m_fRatio);
					break;
				}
				case SDLK_MINUS: {
					printf("SDLK_MINUS key down\n");
					m_fRatio /= 1.02f;
					//m_fRatio = m_server.setVideoScale(m_fRatio);
					break;
				}
				case SDLK_HOME: {
					testkey = true;
					printf("SDLK_HOME key down\n");
					break;
				}
			}
				break;
		}

		case SDL_QUIT: {
			printf("Quit requested, quitting.\n");
			m_server.stop();
			bEndLoop = TRUE;
			break;
		}
		}
		if (bEndLoop)
		{
			break;
		}
	}
}


static bool haswrite = false;

/// <summary>
/// 视频输出
/// </summary>
/// <param name="data"></param>
void CSDLPlayer::outputVideo(SFgVideoFrame* data) 
{
	
	if (data->width == 0 || data->height == 0) {
		return;
	}
	//printf("%d %d %d \n", data->isKey, haswrite, testkey);
	//if (data->isKey && !haswrite) {
	//	haswrite = true;
	//	ofstream outFile("testdump.dat", ios::out | ios::binary);
	//	SFgVideoFrame d = *data;
	//	outFile.write((char*)&d, sizeof(SFgVideoFrame));
	//	outFile.write((char*)d.data, d.dataTotalLen);
	//	outFile.close();
	//	printf("testdump wrote\n");
	//}
	

	if (data->width != m_rect.w || data->height != m_rect.h) {
		{
			CAutoLock oLock(m_mutexVideo, "unInitVideo");
			if (NULL != m_yuv) {
				SDL_FreeYUVOverlay(m_yuv);
				m_yuv = NULL;
			}
		}
		m_evtVideoSizeChange.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.type = SDL_USEREVENT;
		m_evtVideoSizeChange.user.code = VIDEO_SIZE_CHANGED_CODE;
		m_evtVideoSizeChange.user.data1 = (void*)data->width;
		m_evtVideoSizeChange.user.data2 = (void*)data->height;

		SDL_PushEvent(&m_evtVideoSizeChange);

		resolutionChange = true;
		return;
	}

	CAutoLock oLock(m_mutexVideo, "outputVideo");
	if (m_yuv == NULL) {
		return;
	}

	SDL_LockYUVOverlay(m_yuv);

	for (size_t i = 0; i < data->height; i++)
	{
		if (i >= m_yuv->h) {
			break;
		}
		memcpy(m_yuv->pixels[0] + i * m_yuv->pitches[0], data->data + i * data->pitch[0], min(m_yuv->pitches[0], data->pitch[0]));
		if (i % 2 == 0) {
			memcpy(m_yuv->pixels[1] + (i >> 1)* m_yuv->pitches[1],
				data->data + data->dataLen[0] + (i >> 1)* data->pitch[1], min(m_yuv->pitches[1], data->pitch[1]));
			memcpy(m_yuv->pixels[2] + (i >> 1)* m_yuv->pitches[2],
				data->data + data->dataLen[0] + data->dataLen[1] + (i >> 1)* data->pitch[2], min(m_yuv->pitches[2], data->pitch[2]));
		}
	}

	SDL_UnlockYUVOverlay(m_yuv);

	m_rect.x = 0;
	m_rect.y = 0;
	m_rect.w = data->width;
	m_rect.h = data->height;

	auto newRect = m_rect;
	
	newRect.w *= m_fRatio;
	newRect.h *= m_fRatio;
	newRect.x = (m_rect.w - newRect.w)/2;

	SDL_DisplayYUVOverlay(m_yuv, &newRect);

	auto width = (int)data->width, height = (int)data->height;
	libyuv::I420ToARGB(m_yuv->pixels[0], m_yuv->pitches[0],
		m_yuv->pixels[2], m_yuv->pitches[2], 
		m_yuv->pixels[1], m_yuv->pitches[1],
		m_argbBuffer,
		width*4, width, height
		);

#if CONVERT_RESOLUTION
	auto dw1w2 = ((int)width - VCAM_WIDTH) / 2;
	auto dh1h2 = ((int)height - VCAM_HEIGHT) / 2;

	if (resolutionChange) {
		memset(m_toVcamBuffer,0, VCAM_WIDTH * VCAM_HEIGHT * 4);
		printf("resolutionChange %d %d\n", width,height);
		resolutionChange = false;
	}

	//	for (int col = 0; col < width; col++)
	for (int h = 0; h < VCAM_HEIGHT && h < height; h++) {
		//printf("%d\n", int((h * VCAM_WIDTH) * 4));
		for (int w = 0; w < width; w++) {
			auto inexSrc = w + h * width;
			auto indexDst = w + h * VCAM_WIDTH;
			indexDst -= dw1w2;
			if (indexDst < 0)break;
			auto nh = indexDst / VCAM_WIDTH;
			auto nw = indexDst % VCAM_WIDTH;
			if (nh >= VCAM_HEIGHT || nw >= VCAM_WIDTH)break;
			memcpy(m_toVcamBuffer + indexDst * 4, m_argbBuffer + inexSrc * 4, 4);
		}
	}



	m_vcamShared->Send(VCAM_WIDTH, VCAM_HEIGHT, VCAM_WIDTH, VCAM_WIDTH * VCAM_HEIGHT * 4,
		SharedImageMemory::EFormat::FORMAT_UINT8,
		SharedImageMemory::EResizeMode::RESIZEMODE_DISABLED,
		SharedImageMemory::EMirrorMode::MIRRORMODE_DISABLED,
		1000, m_toVcamBuffer);
#else
	m_vcamShared->Send(width, height, width, width * height * 4,
		SharedImageMemory::EFormat::FORMAT_UINT8, 
		SharedImageMemory::EResizeMode::RESIZEMODE_DISABLED,
		SharedImageMemory::EMirrorMode::MIRRORMODE_DISABLED,
		1000, m_argbBuffer);
#endif

	


	

	lw = width;
	lh = height;

	
	lastRenderTime = GetTickCount64();


}

void CSDLPlayer::outputAudio(SFgAudioFrame* data)
{
	if (data->channels == 0) {
		return;
	}

	initAudio(data);

	if (m_bDumpAudio) {
		if (m_fileWav != NULL) {
			fwrite(data->data, data->dataLen, 1, m_fileWav);
		}
	}

	SDemoAudioFrame* dataClone = new SDemoAudioFrame();
	dataClone->dataTotal = data->dataLen;
	dataClone->pts = data->pts;
	dataClone->dataLeft = dataClone->dataTotal;
	dataClone->data = new uint8_t[dataClone->dataTotal];
	memcpy(dataClone->data, data->data, dataClone->dataTotal);

	{
		CAutoLock oLock(m_mutexAudio, "outputAudio");
		m_queueAudio.push(dataClone);
	}
}

void CSDLPlayer::initVideo(int width, int height)
{
	printf("initVideo %d %d\n",width,height);
	// 0x115
	m_surface = SDL_SetVideoMode(width, height, 0, SDL_SWSURFACE);
	SDL_WM_SetCaption("AirPlay Demo [s - start server, q - stop server]", NULL);

	{
		CAutoLock oLock(m_mutexVideo, "initVideo");
		m_yuv = SDL_CreateYUVOverlay(width, height, SDL_IYUV_OVERLAY, m_surface);

		memset(m_yuv->pixels[0], 0, m_yuv->pitches[0] * m_yuv->h);
		memset(m_yuv->pixels[1], 128, m_yuv->pitches[1] * m_yuv->h >> 1);
		memset(m_yuv->pixels[2], 128, m_yuv->pitches[2] * m_yuv->h >> 1);
		m_rect.x = 0;
		m_rect.y = 0;
		m_rect.w = width;
		m_rect.h = height;

		lw = width;
		lh = height;

		

		SDL_DisplayYUVOverlay(m_yuv, &m_rect);
		if (m_argbBuffer)delete m_argbBuffer;
		m_argbBuffer = new uint8_t[width * height * 4];
		
	}
}

void CSDLPlayer::unInitVideo()
{
	if (NULL != m_surface) {
		SDL_FreeSurface(m_surface);
		m_surface = NULL;
	}

	{
		CAutoLock oLock(m_mutexVideo, "unInitVideo");
		if (NULL != m_yuv) {
			SDL_FreeYUVOverlay(m_yuv);
			m_yuv = NULL;
		}
		m_rect.w = 0;
		m_rect.h = 0;
	}

	unInitAudio();
}

void CSDLPlayer::initAudio(SFgAudioFrame* data)
{
	if ((data->sampleRate != m_sAudioFmt.sampleRate || data->channels != m_sAudioFmt.channels)) {
		unInitAudio();
	}
	if (!m_bAudioInited) {
		SDL_AudioSpec wanted_spec, obtained_spec;
		wanted_spec.freq = data->sampleRate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = data->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = 1920;
		wanted_spec.callback = sdlAudioCallback;
		wanted_spec.userdata = this;

		if (SDL_OpenAudio(&wanted_spec, &obtained_spec) < 0)
		{
			printf("can't open audio.\n");
			return;
		}

		SDL_PauseAudio(1);

		m_sAudioFmt.bitsPerSample = data->bitsPerSample;
		m_sAudioFmt.channels = data->channels;
		m_sAudioFmt.sampleRate = data->sampleRate;
		m_bAudioInited = true;

		if (m_bDumpAudio) {
			m_fileWav = fopen("demo-audio.wav", "wb");
		}
	}
	if (m_queueAudio.size() > 5) {
		SDL_PauseAudio(0);
	}
}

void CSDLPlayer::unInitAudio()
{
	SDL_CloseAudio();
	m_bAudioInited = false;
	memset(&m_sAudioFmt, 0, sizeof(m_sAudioFmt));

	{
		CAutoLock oLock(m_mutexAudio, "unInitAudio");
		while (!m_queueAudio.empty())
		{
			SDemoAudioFrame* pAudioFrame = m_queueAudio.front();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
			m_queueAudio.pop();
		}
	}

	if (m_fileWav != NULL) {
		fclose(m_fileWav);
		m_fileWav = NULL;
	}
}

void CSDLPlayer::sdlAudioCallback(void* userdata, Uint8* stream, int len)
{
	CSDLPlayer* pThis = (CSDLPlayer*)userdata;
	int needLen = len;
	int streamPos = 0;
	memset(stream, 0, len);

	CAutoLock oLock(pThis->m_mutexAudio, "sdlAudioCallback");
	while (!pThis->m_queueAudio.empty())
	{
		SDemoAudioFrame* pAudioFrame = pThis->m_queueAudio.front();
		int pos = pAudioFrame->dataTotal - pAudioFrame->dataLeft;
		int readLen = min(pAudioFrame->dataLeft, needLen);

		//SDL_MixAudio(stream + streamPos, pAudioFrame->data + pos, readLen, 100);
		memcpy(stream + streamPos, pAudioFrame->data + pos, readLen);

		pAudioFrame->dataLeft -= readLen;
		needLen -= readLen;
		streamPos += readLen;
		if (pAudioFrame->dataLeft <= 0) {
			pThis->m_queueAudio.pop();
			delete[] pAudioFrame->data;
			delete pAudioFrame;
		}
		if (needLen <= 0) {
			break;
		}
	}
}
