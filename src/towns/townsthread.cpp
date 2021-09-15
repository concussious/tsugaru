/* LICENSE>>
Copyright 2020 Soji Yamakawa (CaptainYS, http://www.ysflight.com)

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

<< LICENSE */
#include <iostream>
#include <chrono>
#include <memory>

#include "townsthread.h"


TownsThread::TownsThread(void) : renderingThread(new TownsRenderingThread)
{
	runMode=RUNMODE_PAUSE;
}

void TownsThread::VMStart(FMTowns *townsPtr,Outside_World *outside_world,class TownsUIThread *uiThread)
{
	renderingThread->imageNeedsFlip=outside_world->ImageNeedsFlip();

	this->townsPtr=townsPtr;
	townsPtr->cdrom.SetOutsideWorld(outside_world);
	townsPtr->sound.SetOutsideWorld(outside_world);
	townsPtr->scsi.SetOutsideWorld(outside_world);

	outside_world->Start();

	if(""!=townsPtr->var.startUpStateFName)
	{
		townsPtr->LoadState(townsPtr->var.startUpStateFName,*outside_world);
	}

	switch(townsPtr->state.appSpecificSetting)
	{
	case TOWNS_APPSPECIFIC_BRANDISH:
		// Brandish doesn't know about 80486.  It confuses as 80386SX and messes up.
		townsPtr->state.pretend386DX=true;
		break;
	}
}
void TownsThread::VMMainLoop(FMTowns *townsPtr,Outside_World *outside_world,class TownsUIThread *uiThread)
{
	// Just in case, if there is a remains of the rendering from the previous run, discard it.
	renderingThread->DiscardRunningRenderingTask();

	TownsRender render;
	bool terminate=false;
	for(;true!=terminate;)
	{
		auto realTime0=std::chrono::high_resolution_clock::now();
		auto townsTime0=townsPtr->state.townsTime;

		int runModeCopy=0;

		runModeCopy=runMode;

		bool clockTicking=false;  // Will be made true if VM is running.

		townsPtr->var.justLoadedState=false;

		switch(runMode)
		{
		case RUNMODE_PAUSE:
			renderingThread->WaitIdle();
			townsPtr->ForceRender(render,*outside_world);
			outside_world->DevicePolling(*townsPtr);
			if(true==outside_world->PauseKeyPressed())
			{
				runMode=RUNMODE_RUN;
				townsPtr->debugger.stop=false;
			}
			break;
		case RUNMODE_RUN:
			clockTicking=true;
			{
				auto nextTimeSync=townsPtr->state.cpuTime+NANOSECONDS_PER_TIME_SYNC;
				while(townsPtr->state.cpuTime<nextTimeSync && true!=townsPtr->CheckAbort())
				{
					townsPtr->RunOneInstruction();
					townsPtr->pic.ProcessIRQ(townsPtr->cpu,townsPtr->mem);
					townsPtr->RunFastDevicePolling();
					townsPtr->RunScheduledTasks();
					if(true==townsPtr->debugger.stop)
					{
						if(townsPtr->cpu.state.CS().value==townsPtr->var.powerOffAt.SEG &&
						   townsPtr->cpu.state.EIP==townsPtr->var.powerOffAt.OFFSET)
						{
							std::cout << "Break at the power-off point." << std::endl;
							std::cout << "Normal termination of a unit testing." << std::endl;
							townsPtr->var.powerOff=true;
							break;
						}
						if(true!=townsPtr->debugger.lastBreakPointInfo.ShouldBreak())
						{
							if(0!=(townsPtr->debugger.lastBreakPointInfo.flags&i486Debugger::BRKPNT_FLAG_MONITOR_ONLY) ||
							   0==(townsPtr->debugger.lastBreakPointInfo.flags&i486Debugger::BRKPNT_FLAG_SILENT_UNTIL_BREAK))
							{
								std::cout << "Passed " << townsPtr->debugger.lastBreakPointInfo.passedCount << " times." << std::endl;
								PrintStatus(*townsPtr);
							}
							townsPtr->debugger.ClearStopFlag();
							this->SetRunMode(RUNMODE_RUN);
						}
						else
						{
							std::cout << "Passed " << townsPtr->debugger.lastBreakPointInfo.passedCount << " times." << std::endl;
							PrintStatus(*townsPtr);
							std::cout << ">";
							runMode=RUNMODE_PAUSE;
						}
						break;
					}
				}
			}
			townsPtr->ProcessSound(outside_world);
			townsPtr->cdrom.UpdateCDDAState(townsPtr->state.townsTime,*outside_world);

			// townsPtr->CheckRenderingTimer(render,*outside_world);
			renderingThread->CheckRenderingTimer(*townsPtr,render);
			renderingThread->CheckImageReady(*townsPtr,*outside_world);

			outside_world->ProcessAppSpecific(*townsPtr);
			if(townsPtr->state.nextDevicePollingTime<townsPtr->state.cpuTime)
			{
				outside_world->DevicePolling(*townsPtr);
				townsPtr->state.nextDevicePollingTime=townsPtr->state.cpuTime+FMTowns::DEVICE_POLLING_INTERVAL;
			}
			townsPtr->eventLog.Interval(*townsPtr);
			if(true==townsPtr->CheckAbort() || outside_world->PauseKeyPressed())
			{
				PrintStatus(*townsPtr);
				std::cout << ">";
				runMode=RUNMODE_PAUSE;
			}
			break;
		case RUNMODE_ONE_INSTRUCTION:
			if(true!=townsPtr->CheckAbort())
			{
				townsPtr->RunOneInstruction();
				townsPtr->pic.ProcessIRQ(townsPtr->cpu,townsPtr->mem);
				townsPtr->RunFastDevicePolling();
				townsPtr->RunScheduledTasks();
			}
			PrintStatus(*townsPtr);
			std::cout << ">";
			runMode=RUNMODE_PAUSE;
			break;
		case RUNMODE_EXIT:
			terminate=true;
			break;
		default:
			terminate=true;
			std::cout << "Undefined VM RunMode!" << std::endl;
			break;
		}
		if(true==townsPtr->var.powerOff)
		{
			if(true!=townsPtr->var.pauseOnPowerOff)
			{
				runMode=RUNMODE_EXIT;
			}
			else if(RUNMODE_PAUSE!=runMode)
			{
				PrintStatus(*townsPtr);
				std::cout << ">";
				runMode=RUNMODE_PAUSE;
			}
		}
		if(townsPtr->state.nextSecondInTownsTime<=townsPtr->state.townsTime)
		{
			townsPtr->state.nextSecondInTownsTime+=PER_SECOND;
			townsPtr->fdc.SaveModifiedDiskImages();
			townsPtr->physMem.state.memCard.SaveRawImageIfModified();
		}

		if(RUNMODE_PAUSE==runModeCopy)
		{
			townsPtr->fdc.SaveModifiedDiskImages();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if(true==returnOnPause)
			{
				break;
			}
		}

		uiThread->uiLock.lock();
		uiThread->ExecCommandQueue(*this,*townsPtr,outside_world);
		uiThread->uiLock.unlock();
		if(true==townsPtr->var.justLoadedState)
		{
			renderingThread->DiscardRunningRenderingTask();
		}
		else if(true==clockTicking)
		{
			AdjustRealTime(townsPtr,townsPtr->state.townsTime-townsTime0,realTime0,outside_world);
		}
	}

	// Rendering thread may be working on local TownsRender.
	// WaitIdle to make sure the rendering thread is done with rendering before leaving this function.
	renderingThread->DiscardRunningRenderingTask();
}
void TownsThread::VMEnd(FMTowns *townsPtr,Outside_World *outside_world,class TownsUIThread *uiThread)
{
	uiThread->uiLock.lock();
	uiThread->vmTerminated=true;
	uiThread->uiLock.unlock();

	std::cout << "Ending Towns Thread." << std::endl;
	townsPtr->fdc.SaveModifiedDiskImages();

	if(0<townsPtr->var.CMOSFName.size())
	{
		if(true!=cpputil::WriteBinaryFile(townsPtr->var.CMOSFName,TOWNS_CMOS_SIZE,townsPtr->physMem.state.CMOSRAM))
		{
			std::cout << "Failed to save CMOS." << std::endl;
		}
	}

	outside_world->Stop();

	if(true==townsPtr->var.forceQuitOnPowerOff)
	{
		exit(0);
	}
}


// Case 1
// Time deficit occurs when the VM lags behind the real-time (realTimePassed>cpuTimePassed):
// 
// RealTime  |------------>|1500us
// VM        |----->|1000us
//                  |      |deficit 500us
// Hopefully it is due to screen rendering, disk access, or something temporary.

// Case 2
// When the VM catches up (deficit+realTimePassed<cpuTimePassed):
// 
// RealTime  |    |deficit-->|RalTimePassed-->|Wait------>|
// VM        |    |----------------------------------->|
//                                                     |->| New deficit

// Case 3 : General case of Case 1
// When the VM doesn't catch up, but pay back some deficit (cpuTimePassed>realTimePassed):
// RealTime  |    |deficit-->|RealTimePassed-->|
// VM        |    |------------------->|       |
//                                     |------>|  New deficit


void TownsThread::AdjustRealTime(FMTowns *townsPtr,long long int cpuTimePassed,std::chrono::time_point<std::chrono::high_resolution_clock> time0,Outside_World *outside_world)
{
	long long int realTimePassed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now()-time0).count();

	townsPtr->var.timeAdjustLog[townsPtr->var.timeAdjustLogPtr]=cpuTimePassed-realTimePassed;
	townsPtr->var.timeDeficitLog[townsPtr->var.timeAdjustLogPtr]=townsPtr->state.timeDeficit;
	townsPtr->var.timeAdjustLogPtr=(townsPtr->var.timeAdjustLogPtr+1)&(FMTowns::Variable::TIME_ADJUSTMENT_LOG_LEN-1);

	int64_t balance=cpuTimePassed-(townsPtr->state.timeDeficit+realTimePassed);
	if(balance<0)  // Case 3
	{
		if(true==townsPtr->var.catchUpRealTime)
		{
			townsPtr->state.timeDeficit=(-balance);
		}
		else
		{
			townsPtr->state.timeDeficit=0;
		}
	}
	else // Case 2
	{
		if(true!=townsPtr->state.noWait)
		{
			while(townsPtr->state.timeDeficit+realTimePassed<cpuTimePassed)
			{
				townsPtr->ProcessSound(outside_world);
				realTimePassed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now()-time0).count();
			}
			int64_t newBalance=cpuTimePassed-(townsPtr->state.timeDeficit+realTimePassed);
			townsPtr->state.timeDeficit=-newBalance;
		}
		else
		{
			townsPtr->state.timeDeficit=0;
		}
	}

	if(FMTowns::State::CATCHUP_DEFICIT_CUTOFF<townsPtr->state.timeDeficit)
	{
		townsPtr->state.timeDeficit=FMTowns::State::CATCHUP_DEFICIT_CUTOFF;
	}
}

int TownsThread::GetRunMode(void) const
{
	return runMode;
}
void TownsThread::SetRunMode(int nextRunMode)
{
	runMode=nextRunMode;
}

void TownsThread::SetReturnOnPause(bool flag)
{
	returnOnPause=flag;
}

void TownsThread::PrintStatus(const FMTowns &towns) const
{
	towns.PrintStatus();
}


////////////////////////////////////////////////////////////


void TownsUIThread::Run(TownsThread *vmThread,FMTowns *towns,const TownsARGV *argv,Outside_World *outside_world)
{
	Main(*vmThread,*towns,*argv,*outside_world);
}
