/*******************************************************************************
 * libretroshare/src/rsserver: p3face-server.cc                                *
 *                                                                             *
 * libretroshare: retroshare core library                                      *
 *                                                                             *
 * Copyright 2015 by Robert Fernie   <retroshare.project@gmail.com>            *
 *                                                                             *
 * This program is free software: you can redistribute it and/or modify        *
 * it under the terms of the GNU Lesser General Public License as              *
 * published by the Free Software Foundation, either version 3 of the          *
 * License, or (at your option) any later version.                             *
 *                                                                             *
 * This program is distributed in the hope that it will be useful,             *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                *
 * GNU Lesser General Public License for more details.                         *
 *                                                                             *
 * You should have received a copy of the GNU Lesser General Public License    *
 * along with this program. If not, see <https://www.gnu.org/licenses/>.       *
 *                                                                             *
 *******************************************************************************/

#include "util/rstime.h"
#include "rsserver/p3face.h"
#include "retroshare/rsplugin.h"

#include "tcponudp/tou.h"
#include <unistd.h>

#include "pqi/authssl.h"
#include <sys/time.h>
#include "util/rstime.h"

#include "pqi/p3peermgr.h"
#include "pqi/p3linkmgr.h"
#include "pqi/p3netmgr.h"

#include "util/rsdebug.h"

#include "retroshare/rsevents.h"
#include "services/rseventsservice.h"


/*******************
#define TICK_DEBUG 1
*******************/

#define WARN_BIG_CYCLE_TIME	(0.2)

#ifdef WINDOWS_SYS
#include "util/rstime.h"
#include <sys/timeb.h>
#endif


/*extern*/ RsControl* rsControl = nullptr;

static double getCurrentTS()
{

#ifndef WINDOWS_SYS
        struct timeval cts_tmp;
        gettimeofday(&cts_tmp, NULL);
        double cts =  (cts_tmp.tv_sec) + ((double) cts_tmp.tv_usec) / 1000000.0;
#else
        struct _timeb timebuf;
        _ftime( &timebuf);
        double cts =  (timebuf.time) + ((double) timebuf.millitm) / 1000.0;
#endif
        return cts;
}

// These values should be tunable from the GUI, to offer a compromise between speed and CPU use.
// In some cases (VOIP) it's likely that we will need to set them temporarily to a very low 
// value, in order to favor a fast feedback

const double RsServer::minTickInterval = 0.05;
const double RsServer::maxTickInterval = 0.2;


RsServer::RsServer() :
	coreMutex("RsServer"), mShutdownCallback([](int){}),
	coreReady(false)
{
	{
		RsEventsService* tmpRsEvtPtr = new RsEventsService();
		rsEvents = tmpRsEvtPtr;
		startServiceThread(tmpRsEvtPtr, "RsEventsService");
	}

	// This is needed asap.
	//
	mNotify = new p3Notify() ;
	rsNotify = mNotify ;

	mPeerMgr = NULL;
	mLinkMgr = NULL;
	mNetMgr = NULL;
	mHistoryMgr = NULL;

	pqih = NULL;

	mPluginsManager = NULL;

	/* services */
	mHeart = NULL;
	mDisc = NULL;
	msgSrv = NULL;
	chatSrv = NULL;
	mStatusSrv = NULL;
	mGxsTunnels = NULL;

	mLastts = getCurrentTS();
	mTickInterval = maxTickInterval ;
	mAvgRunDuration = 0;
	mLastRunDuration = 0;
	mCycle1 = 0;
	mCycle2 = 0;
	mCycle3 = 0;
	mCycle4 = 0;

	/* caches (that need ticking) */

	/* config */
	mConfigMgr = NULL;
	mGeneralConfig = NULL;
}

RsServer::~RsServer()
{
	delete mGxsTrans;
}

// General Internal Helper Functions  ----> MUST BE LOCKED! 

void RsServer::threadTick()
{
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG ticking interval "<< mTickInterval << std::endl;
#endif

// we try to tick at a regular interval which depends on the load 
// if there is time left, we sleep
	double timeToSleep = mTickInterval - mAvgRunDuration;

	if (timeToSleep > 0)
	{
#ifdef TICK_DEBUG
		RsDbg() << "TICK_DEBUG will sleep " << timeToSleep << " ms" << std::endl;
#endif
		rstime::rs_usleep(timeToSleep * 1000000);
	}

	double ts = getCurrentTS();
	mLastts = ts;

// stuff we do always
// tick the core
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG ticking server" << std::endl;
#endif
	lockRsCore();
	int moreToTick = pqih->tick();
	unlockRsCore();
// tick the managers
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG ticking mPeerMgr" << std::endl;
#endif
	mPeerMgr->tick();
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG ticking mLinkMgr" << std::endl;
#endif
	mLinkMgr->tick();
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG ticking mNetMgr" << std::endl;
#endif
	mNetMgr->tick();


// stuff we do every second
	if (ts - mCycle1 > 1)
	{
#ifdef TICK_DEBUG
		RsDbg() << "TICK_DEBUG every second" << std::endl;
#endif
		// slow services
		if (rsPlugins)
			rsPlugins->slowTickPlugins((rstime_t)ts);
		// UDP keepalive
		// tou_tick_stunkeepalive();
		// other stuff to tick
		// update();
		mCycle1 = ts;
	}

// stuff we do every five seconds
	if (ts - mCycle2 > 5)
	{
#ifdef TICK_DEBUG
		RsDbg() << "TICK_DEBUG every 5 seconds" << std::endl;
#endif
		// save stuff
		mConfigMgr->tick();
		mCycle2 = ts;
	}

// stuff we do every minute
	if (ts - mCycle3 > 60)
	{
#ifdef TICK_DEBUG
		RsDbg() << "TICK_DEBUG every 60 seconds" << std::endl;
#endif
		// force saving FileTransferStatus TODO
		// ftserver->saveFileTransferStatus();
		// see if we need to resave certs
		// AuthSSL::getAuthSSL()->CheckSaveCertificates();
		mCycle3 = ts;
	}

// stuff we do every hour
	if (ts - mCycle4 > 3600)
	{
#ifdef TICK_DEBUG
		RsDbg() << "TICK_DEBUG every hour" << std::endl;
#endif
		mCycle4 = ts;
	}

// ticking is done, now compute new values of mLastRunDuration, mAvgRunDuration and mTickInterval
	ts = getCurrentTS();
	mLastRunDuration = ts - mLastts;  
	mAvgRunDuration = 0.1 * mLastRunDuration + 0.9 * mAvgRunDuration;

#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG new mLastRunDuration " << mLastRunDuration << " mAvgRunDuration " << mAvgRunDuration << std::endl;
	if (mLastRunDuration > WARN_BIG_CYCLE_TIME)
		RsDbg() << "TICK_DEBUG excessively long lycle time " << mLastRunDuration << std::endl;
#endif
	
// if the core has returned that there is more to tick we decrease the ticking interval, else we increse it
// this should be studied closer as I dont think that the core ever returns 1
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG moreToTick " << moreToTick << std::endl;
#endif
	if (moreToTick == 1)
		mTickInterval = 0.9 * mTickInterval;
	else
		mTickInterval = 1.1 * mTickInterval;
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG new tick interval " << mTickInterval << std::endl;
#endif

// keep the tick interval within allowed limits
	if (mTickInterval < minTickInterval)
		mTickInterval = minTickInterval;
	else if (mTickInterval > maxTickInterval)
		mTickInterval = maxTickInterval;
#ifdef TICK_DEBUG
	RsDbg() << "TICK_DEBUG new tick interval after limiter " << mTickInterval << std::endl;
#endif
}

