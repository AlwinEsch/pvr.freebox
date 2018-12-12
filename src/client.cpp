/*
 *      Copyright (C) 2018 Aassif Benassarou
 *      http://github.com/aassif/pvr.freebox/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "client.h"
#include "xbmc_pvr_dll.h"
#include "PVRFreeboxData.h"
#include "p8-platform/util/util.h"

using namespace ADDON;

#ifdef TARGET_WINDOWS
#define snprintf _snprintf
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

std::string      path;
int              quality  = 1;
bool             extended = false;
int              delay    = 0;
bool             init     = false;
ADDON_STATUS     status   = ADDON_STATUS_UNKNOWN;
PVRFreeboxData * data     = nullptr;

CHelper_libXBMC_addon * XBMC = nullptr;
CHelper_libXBMC_pvr   * PVR  = nullptr;

extern "C" {

void ADDON_ReadSettings ()
{
  if (! XBMC->GetSetting ("quality",  &quality))  quality  = 1;
  if (! XBMC->GetSetting ("extended", &extended)) extended = false;
  if (! XBMC->GetSetting ("delay",    &delay))    delay    = 0;
}

ADDON_STATUS ADDON_Create (void * callbacks, void * properties)
{
  if (! callbacks || ! properties)
  {
    return ADDON_STATUS_UNKNOWN;
  }

  PVR_PROPERTIES * p = (PVR_PROPERTIES *) properties;

  XBMC = new CHelper_libXBMC_addon;
  if (! XBMC->RegisterMe (callbacks))
  {
    SAFE_DELETE (XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  PVR = new CHelper_libXBMC_pvr;
  if (! PVR->RegisterMe (callbacks))
  {
    SAFE_DELETE (PVR);
    SAFE_DELETE (XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  XBMC->Log (LOG_DEBUG, "%s - Creating the Freebox TV add-on", __FUNCTION__);

  status        = ADDON_STATUS_UNKNOWN;
//  g_strUserPath   = p->strUserPath;
//  g_strClientPath = p->strClientPath;

/*
  if (! XBMC->DirectoryExists (g_strUserPath.c_str ()))
  {
    XBMC->CreateDirectory (g_strUserPath.c_str ());
  }
*/
  ADDON_ReadSettings ();

  data   = new PVRFreeboxData (p->strClientPath, quality, p->iEpgMaxDays, extended, delay);
  status = ADDON_STATUS_OK;
  init   = true;

  return status;
}

ADDON_STATUS ADDON_GetStatus ()
{
  return status;
}

void ADDON_Destroy ()
{
  delete data;
  status = ADDON_STATUS_UNKNOWN;
  init   = false;
}

ADDON_STATUS ADDON_SetSetting (const char * name, const void * value)
{
  if (data)
  {
    if (std::string (name) == "quality")
      data->SetQuality (*((int *) value));

    if (std::string (name) == "delay")
      data->SetDelay (*((int *) value));
  }

  return ADDON_STATUS_OK;
}

/***********************************************************
 * PVR Client AddOn specific public library functions
 ***********************************************************/

void OnSystemSleep ()
{
}

void OnSystemWake ()
{
}

void OnPowerSavingActivated ()
{
}

void OnPowerSavingDeactivated ()
{
}

PVR_ERROR GetAddonCapabilities (PVR_ADDON_CAPABILITIES * caps)
{
  caps->bSupportsEPG                      = true;
  caps->bSupportsTV                       = true;
  caps->bSupportsRadio                    = false;
  caps->bSupportsChannelGroups            = false;
  caps->bSupportsRecordings               = false;
  caps->bSupportsRecordingsRename         = false;
  caps->bSupportsRecordingsLifetimeChange = false;
  caps->bSupportsDescrambleInfo           = false;
  caps->bSupportsAsyncEPGTransfer         = true;

  return PVR_ERROR_NO_ERROR;
}

const char * GetBackendName ()
{
  return "Freebox TV";
}

const char * GetBackendVersion ()
{
  return "1.0a";
}

const char * GetConnectionString ()
{
  return "1.0a";
}

const char * GetBackendHostname ()
{
  if (data)
  {
    static std::string server;
    server = data->GetServer ();
    return server.c_str ();
  }

  return NULL;
}

PVR_ERROR SetEPGTimeFrame (int days)
{
  if (data)
    data->SetDays (days);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR GetEPGForChannel (ADDON_HANDLE handle, const PVR_CHANNEL & channel, time_t start, time_t end)
{
  return PVR_ERROR_NO_ERROR;
}

int GetChannelsAmount ()
{
  if (data)
    return data->GetChannelsAmount ();

  return -1;
}

PVR_ERROR GetChannels (ADDON_HANDLE handle, bool radio)
{
  if (data)
    return data->GetChannels (handle, radio);

  return PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetChannelStreamProperties (const PVR_CHANNEL * channel, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  if (data)
    return data->GetChannelStreamProperties (channel, properties, count);

  return PVR_ERROR_SERVER_ERROR;
}

int GetChannelGroupsAmount ()
{
  if (data)
    return data->GetChannelGroupsAmount ();

  return -1;
}

PVR_ERROR GetChannelGroups (ADDON_HANDLE handle, bool radio)
{
  if (data)
    return data->GetChannelGroups (handle, radio);

  return PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetChannelGroupMembers (ADDON_HANDLE handle, const PVR_CHANNEL_GROUP & group)
{
  if (data)
    return data->GetChannelGroupMembers (handle, group);

  return PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR GetDriveSpace (long long *, long long *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SignalStatus (PVR_SIGNAL_STATUS &) {return PVR_ERROR_NOT_IMPLEMENTED;}

/** UNUSED API FUNCTIONS */
bool CanPauseStream () {return false;}
int GetRecordingsAmount (bool) {return -1;}
PVR_ERROR GetRecordings (ADDON_HANDLE, bool) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetRecordingStreamProperties (const PVR_RECORDING *, PVR_NAMED_VALUE *, unsigned int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR OpenDialogChannelScan () {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR CallMenuHook (const PVR_MENUHOOK &, const PVR_MENUHOOK_DATA &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR DeleteChannel (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR RenameChannel (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR OpenDialogChannelSettings (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR OpenDialogChannelAdd (const PVR_CHANNEL &) {return PVR_ERROR_NOT_IMPLEMENTED;}
void CloseLiveStream () {}
bool OpenRecordedStream (const PVR_RECORDING &) {return false;}
bool OpenLiveStream (const PVR_CHANNEL &) {return false;}
void CloseRecordedStream () {}
int ReadRecordedStream (unsigned char *, unsigned int) {return 0;}
long long SeekRecordedStream (long long, int) {return 0;}
long long LengthRecordedStream () {return 0;}
void DemuxReset () {}
void DemuxFlush () {}
int ReadLiveStream (unsigned char *, unsigned int) {return 0;}
long long SeekLiveStream (long long, int) {return -1;}
long long LengthLiveStream () {return -1;}
PVR_ERROR DeleteRecording (const PVR_RECORDING &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR RenameRecording (const PVR_RECORDING &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SetRecordingPlayCount (const PVR_RECORDING &, int) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SetRecordingLastPlayedPosition (const PVR_RECORDING &, int) {return PVR_ERROR_NOT_IMPLEMENTED;}
int GetRecordingLastPlayedPosition (const PVR_RECORDING &) {return -1;}
PVR_ERROR GetRecordingEdl (const PVR_RECORDING &, PVR_EDL_ENTRY [], int *) {return PVR_ERROR_NOT_IMPLEMENTED;};
PVR_ERROR GetTimerTypes (PVR_TIMER_TYPE [], int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
int GetTimersAmount () {return -1;}
PVR_ERROR GetTimers (ADDON_HANDLE) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR AddTimer (const PVR_TIMER &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR DeleteTimer (const PVR_TIMER &, bool) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR UpdateTimer (const PVR_TIMER &) {return PVR_ERROR_NOT_IMPLEMENTED;}
void DemuxAbort () {}
DemuxPacket* DemuxRead () {return NULL;}
bool IsTimeshifting () {return false;}
bool IsRealTimeStream () {return true;}
void PauseStream (bool) {}
bool CanSeekStream () {return false;}
bool SeekTime (double, bool, double *) {return false;}
void SetSpeed (int) {};
PVR_ERROR UndeleteRecording (const PVR_RECORDING &) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR DeleteAllRecordingsFromTrash () {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetDescrambleInfo (PVR_DESCRAMBLE_INFO *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR SetRecordingLifetime (const PVR_RECORDING *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamTimes (PVR_STREAM_TIMES *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamProperties (PVR_STREAM_PROPERTIES *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR IsEPGTagRecordable (const EPG_TAG *, bool *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR IsEPGTagPlayable (const EPG_TAG *, bool *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetEPGTagStreamProperties (const EPG_TAG *, PVR_NAMED_VALUE *, unsigned int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetEPGTagEdl (const EPG_TAG *, PVR_EDL_ENTRY [], int *) {return PVR_ERROR_NOT_IMPLEMENTED;}
PVR_ERROR GetStreamReadChunkSize (int *) {return PVR_ERROR_NOT_IMPLEMENTED;}

} // extern "C"
