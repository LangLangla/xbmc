/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DetectDVDType.h"

#include "cdioSupport.h"
#include "filesystem/File.h"
#include "guilib/LocalizeStrings.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <mutex>
#ifdef TARGET_POSIX
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#if !defined(TARGET_DARWIN) && !defined(TARGET_FREEBSD)
#include <linux/cdrom.h>
#endif
#endif
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "GUIUserMessages.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "Application.h"
#include "ServiceBroker.h"
#include "storage/MediaManager.h"


using namespace XFILE;
using namespace MEDIA_DETECT;
using namespace std::chrono_literals;

CCriticalSection CDetectDVDMedia::m_muReadingMedia;
CEvent CDetectDVDMedia::m_evAutorun;
int CDetectDVDMedia::m_DriveState = DRIVE_CLOSED_NO_MEDIA;
CCdInfo* CDetectDVDMedia::m_pCdInfo = NULL;
time_t CDetectDVDMedia::m_LastPoll = 0;
CDetectDVDMedia* CDetectDVDMedia::m_pInstance = NULL;
std::string CDetectDVDMedia::m_diskLabel = "";
std::string CDetectDVDMedia::m_diskPath = "";
UTILS::DISCS::DiscInfo CDetectDVDMedia::m_discInfo;

CDetectDVDMedia::CDetectDVDMedia() : CThread("DetectDVDMedia"),
  m_cdio(CLibcdio::GetInstance())
{
  m_bStop = false;
  m_pInstance = this;
}

CDetectDVDMedia::~CDetectDVDMedia() = default;

void CDetectDVDMedia::OnStartup()
{
  // SetPriority( ThreadPriority::LOWEST );
  CLog::Log(LOGDEBUG, "Compiled with libcdio Version 0.{}", LIBCDIO_VERSION_NUM);
}

void CDetectDVDMedia::Process()
{
// for apple - currently disable this check since cdio will return null if no media is loaded
#if !defined(TARGET_DARWIN)
  //Before entering loop make sure we actually have a CDrom drive
  CdIo_t *p_cdio = m_cdio->cdio_open(NULL, DRIVER_DEVICE);
  if (p_cdio == NULL)
    return;
  else
    m_cdio->cdio_destroy(p_cdio);
#endif

  while (( !m_bStop ))
  {
    if (g_application.GetAppPlayer().IsPlayingVideo())
    {
      CThread::Sleep(10000ms);
    }
    else
    {
      UpdateDvdrom();
      m_bStartup = false;
      CThread::Sleep(2000ms);
      if ( m_bAutorun )
      {
        // Media in drive, wait 1.5s more to be sure the device is ready for playback
        CThread::Sleep(1500ms);
        m_evAutorun.Set();
        m_bAutorun = false;
      }
    }
  }
}

void CDetectDVDMedia::OnExit()
{
}

// Gets state of the DVD drive
void CDetectDVDMedia::UpdateDvdrom()
{
  // Signal for WaitMediaReady()
  // that we are busy detecting the
  // newly inserted media.
  {
    std::unique_lock<CCriticalSection> waitLock(m_muReadingMedia);
    switch (GetTrayState())
    {
      case DRIVE_NONE:
        //! @todo reduce / stop polling for drive updates
        break;

      case DRIVE_OPEN:
        {
          // Send Message to GUI that disc been ejected
          SetNewDVDShareUrl(CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath),
                            false, g_localizeStrings.Get(502));
          CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_REMOVED_MEDIA);
          CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage( msg );
          // Clear all stored info
          Clear();
          // Update drive state
          waitLock.unlock();
          m_DriveState = DRIVE_OPEN;
          return;
        }
        break;

      case DRIVE_NOT_READY:
        {
          // Drive is not ready (closing, opening)
          SetNewDVDShareUrl(CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath),
                            false, g_localizeStrings.Get(503));
          m_DriveState = DRIVE_NOT_READY;
          // DVD-ROM in undefined state
          // Better delete old CD Information
          if ( m_pCdInfo != NULL )
          {
            delete m_pCdInfo;
            m_pCdInfo = NULL;
          }
          waitLock.unlock();
          CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_SOURCES);
          CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage( msg );
          // Do we really need sleep here? This will fix: [ 1530771 ] "Open tray" problem
          // CThread::Sleep(6000ms);
          return ;
        }
        break;

      case DRIVE_CLOSED_NO_MEDIA:
        {
          // Nothing in there...
          SetNewDVDShareUrl(CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath),
                            false, g_localizeStrings.Get(504));
          m_DriveState = DRIVE_CLOSED_NO_MEDIA;
          // Send Message to GUI that disc has changed
          CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_SOURCES);
          waitLock.unlock();
          CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage( msg );
          return ;
        }
        break;
      case DRIVE_READY:
#if !defined(TARGET_DARWIN)
        return ;
#endif
      case DRIVE_CLOSED_MEDIA_PRESENT:
        {
          if ( m_DriveState != DRIVE_CLOSED_MEDIA_PRESENT)
          {
            m_DriveState = DRIVE_CLOSED_MEDIA_PRESENT;
            // Detect ISO9660(mode1/mode2) or CDDA filesystem
            DetectMediaType();
            CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_SOURCES);
            waitLock.unlock();
            CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage( msg );
            // Tell the application object that a new Cd is inserted
            // So autorun can be started.
            if ( !m_bStartup )
              m_bAutorun = true;
          }
          return ;
        }
        break;
    }

    // We have finished media detection
    // Signal for WaitMediaReady()
  }


}

// Generates the drive url, (like iso9660://)
// from the CCdInfo class
void CDetectDVDMedia::DetectMediaType()
{
  bool bCDDA(false);
  CLog::Log(LOGINFO, "Detecting DVD-ROM media filesystem...");

  // Probe and store DiscInfo result
  // even if no valid tracks are detected we might still be able to play the disc via libdvdnav or libbluray
  // as long as they can correctly detect the disc
  UTILS::DISCS::DiscInfo discInfo;
  if (UTILS::DISCS::GetDiscInfo(discInfo,
                                CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath)))
  {
    m_discInfo = discInfo;
  }

  std::string strNewUrl;
  CCdIoSupport cdio;

  // Delete old CD-Information
  if ( m_pCdInfo != NULL )
  {
    delete m_pCdInfo;
    m_pCdInfo = NULL;
  }

  // Detect new CD-Information
  m_pCdInfo = cdio.GetCdInfo();
  if (m_pCdInfo == NULL)
  {
    CLog::Log(LOGERROR, "Detection of DVD-ROM media failed.");
    return ;
  }
  CLog::Log(LOGINFO, "Tracks overall:{}; Audio tracks:{}; Data tracks:{}",
            m_pCdInfo->GetTrackCount(), m_pCdInfo->GetAudioTrackCount(),
            m_pCdInfo->GetDataTrackCount());

  // Detect ISO9660(mode1/mode2), CDDA filesystem or UDF
  if (m_pCdInfo->IsISOHFS(1) || m_pCdInfo->IsIso9660(1) || m_pCdInfo->IsIso9660Interactive(1))
  {
    strNewUrl = "iso9660://";
  }
  else
  {
    if (m_pCdInfo->IsUDF(1))
      strNewUrl = CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath);
    else if (m_pCdInfo->IsAudio(1))
    {
      strNewUrl = "cdda://local/";
      bCDDA = true;
    }
    else
      strNewUrl = CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath);
  }

  if (m_pCdInfo->IsISOUDF(1))
  {
    if (!CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_detectAsUdf)
    {
      strNewUrl = "iso9660://";
    }
    else
    {
      strNewUrl = CServiceBroker::GetMediaManager().TranslateDevicePath(m_diskPath);
    }
  }

  CLog::Log(LOGINFO, "Using protocol {}", strNewUrl);

  if (m_pCdInfo->IsValidFs())
  {
    if (!m_pCdInfo->IsAudio(1))
      CLog::Log(LOGINFO, "Disc label: {}", m_pCdInfo->GetDiscLabel());
  }
  else
  {
    CLog::Log(LOGWARNING, "Filesystem is not supported");
  }

  std::string strLabel;
  if (bCDDA)
  {
    strLabel = "Audio-CD";
  }
  else
  {
    strLabel = m_pCdInfo->GetDiscLabel();
    StringUtils::TrimRight(strLabel);
  }

  SetNewDVDShareUrl( strNewUrl , bCDDA, strLabel);
}

void CDetectDVDMedia::SetNewDVDShareUrl( const std::string& strNewUrl, bool bCDDA, const std::string& strDiscLabel )
{
  std::string strDescription = "DVD";
  if (bCDDA) strDescription = "CD";

  if (strDiscLabel != "") strDescription = strDiscLabel;

  // Store it in case others want it
  m_diskLabel = strDescription;
  m_diskPath = strNewUrl;
}

DWORD CDetectDVDMedia::GetTrayState()
{
#ifdef TARGET_POSIX

  char* dvdDevice = m_cdio->GetDeviceFileName();
  if (strlen(dvdDevice) == 0)
    return DRIVE_NONE;

  // The following code works with libcdio >= 0.78
  // To enable it, download and install the latest version from
  // http://www.gnu.org/software/libcdio/
  // -d4rk 06/27/07


  m_dwTrayState = TRAY_CLOSED_MEDIA_PRESENT;
  CdIo_t* cdio = m_cdio->cdio_open(dvdDevice, DRIVER_UNKNOWN);
  if (cdio)
  {
    static discmode_t discmode = CDIO_DISC_MODE_NO_INFO;
    int status = m_cdio->mmc_get_tray_status(cdio);
    static int laststatus = -1;
    // We only poll for new discmode when status has changed or there have been read errors (The last usually happens when new media is inserted)
    if (status == 0 && (laststatus != status || discmode == CDIO_DISC_MODE_ERROR))
      discmode = m_cdio->cdio_get_discmode(cdio);

    switch(status)
    {
    case 0: //closed
      if (discmode==CDIO_DISC_MODE_NO_INFO || discmode==CDIO_DISC_MODE_ERROR)
        m_dwTrayState = TRAY_CLOSED_NO_MEDIA;
      else
        m_dwTrayState = TRAY_CLOSED_MEDIA_PRESENT;
      break;

    case 1: //open
      m_dwTrayState = TRAY_OPEN;
      break;
    }
    laststatus = status;
    m_cdio->cdio_destroy(cdio);
  }
  else
    return DRIVE_NOT_READY;

#endif // TARGET_POSIX

  if (m_dwTrayState == TRAY_CLOSED_MEDIA_PRESENT)
  {
    if (m_dwLastTrayState != TRAY_CLOSED_MEDIA_PRESENT)
    {
      m_dwLastTrayState = m_dwTrayState;
      return DRIVE_CLOSED_MEDIA_PRESENT;
    }
    else
    {
      return DRIVE_READY;
    }
  }
  else if (m_dwTrayState == TRAY_CLOSED_NO_MEDIA)
  {
    if ( (m_dwLastTrayState != TRAY_CLOSED_NO_MEDIA) && (m_dwLastTrayState != TRAY_CLOSED_MEDIA_PRESENT) )
    {
      m_dwLastTrayState = m_dwTrayState;
      return DRIVE_CLOSED_NO_MEDIA;
    }
    else
    {
      return DRIVE_READY;
    }
  }
  else if (m_dwTrayState == TRAY_OPEN)
  {
    if (m_dwLastTrayState != TRAY_OPEN)
    {
      m_dwLastTrayState = m_dwTrayState;
      return DRIVE_OPEN;
    }
    else
    {
      return DRIVE_READY;
    }
  }
  else
  {
    m_dwLastTrayState = m_dwTrayState;
  }

#ifdef HAS_DVD_DRIVE
  return DRIVE_NOT_READY;
#else
  return DRIVE_READY;
#endif
}

void CDetectDVDMedia::UpdateState()
{
  std::unique_lock<CCriticalSection> waitLock(m_muReadingMedia);
  m_pInstance->DetectMediaType();
}

// Static function
// Wait for drive, to finish media detection.
void CDetectDVDMedia::WaitMediaReady()
{
  std::unique_lock<CCriticalSection> waitLock(m_muReadingMedia);
}

// Static function
// Returns status of the DVD Drive
int CDetectDVDMedia::DriveReady()
{
  return m_DriveState;
}

// Static function
// Whether a disc is in drive
bool CDetectDVDMedia::IsDiscInDrive()
{
  return m_DriveState == DRIVE_CLOSED_MEDIA_PRESENT;
}

// Static function
// Returns a CCdInfo class, which contains
// Media information of the current inserted CD.
// Can be NULL
CCdInfo* CDetectDVDMedia::GetCdInfo()
{
  std::unique_lock<CCriticalSection> waitLock(m_muReadingMedia);
  CCdInfo* pCdInfo = m_pCdInfo;
  return pCdInfo;
}

const std::string &CDetectDVDMedia::GetDVDLabel()
{
  if (!m_discInfo.empty())
  {
    return m_discInfo.name;
  }

  return m_diskLabel;
}

const std::string &CDetectDVDMedia::GetDVDPath()
{
  return m_diskPath;
}


void CDetectDVDMedia::Clear()
{
  if (!m_discInfo.empty())
  {
    m_discInfo.clear();
  }
  m_diskLabel.clear();
  m_diskPath.clear();
}
