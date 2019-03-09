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

#define RAPIDJSON_HAS_STDSTRING 1

#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <numeric> // accumulate

#include "p8-platform/util/StringUtils.h"

#include "client.h"
#include "Freebox.h"

#include "openssl/sha.h"
#include "openssl/hmac.h"
#include "openssl/bio.h"
#include "openssl/buffer.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/ostreamwrapper.h"

using namespace std;
using namespace rapidjson;
using namespace ADDON;

#define PVR_FREEBOX_C_STR(s) s.empty () ? NULL : s.c_str ()

#define PVR_FREEBOX_TIMER_MANUAL     1
#define PVR_FREEBOX_TIMER_EPG        2
#define PVR_FREEBOX_TIMER_GENERATED  3
#define PVR_FREEBOX_GENERATOR_MANUAL 4
#define PVR_FREEBOX_GENERATOR_EPG    5

inline
void freebox_debug (const Value & data)
{
  OStreamWrapper wrapper (cout);
  Writer<OStreamWrapper> writer (wrapper);
  data.Accept (writer);
  cout << endl;
}

inline
string freebox_base64 (const char * buffer, unsigned int length)
{
  BIO * b64 = BIO_new (BIO_f_base64 ());
  BIO * mem = BIO_new (BIO_s_mem ());
  BIO * bio = BIO_push (b64, mem);

  BIO_set_flags (bio, BIO_FLAGS_BASE64_NO_NL);

  BIO_write (bio, buffer, length);
  BIO_flush (bio);

  BUF_MEM * b;
  BIO_get_mem_ptr (bio, &b);
  string r (b->data, b->length);

  BIO_free_all (bio);

  return r;
}

inline
int freebox_http (const string & custom, const string & url, const string & request, string * response, const string & session)
{
  // URL.
  void * f = XBMC->CURLCreate (url.c_str ());
  // Custom request.
  XBMC->CURLAddOption (f, XFILE::CURL_OPTION_PROTOCOL, "customrequest", custom.c_str ());
  // Header.
  if (! session.empty ())
    XBMC->CURLAddOption (f, XFILE::CURL_OPTION_HEADER, "X-Fbx-App-Auth", session.c_str ());
  // POST?
  if (! request.empty ())
  {
    string base64 = freebox_base64 (request.c_str (), request.length ());
    XBMC->CURLAddOption (f, XFILE::CURL_OPTION_PROTOCOL, "postdata", base64.c_str ());
  }
  // Perform HTTP query.
  if (! XBMC->CURLOpen (f, XFILE::READ_NO_CACHE))
    return -1;
  // Read HTTP response.
  char buffer [1024];
  while (int size = XBMC->ReadFile (f, buffer, 1024))
    response->append (buffer, size);
  // HTTP status code.
  string header = XBMC->GetFilePropertyValue (f, XFILE::FILE_PROPERTY_RESPONSE_PROTOCOL, "");
  istringstream iss (header); string protocol; int status;
  if (! (iss >> protocol >> status >> ws)) return -1;
  // Cleanup.
  XBMC->CloseFile (f);
  return status;
}

/* static */
enum Freebox::Source Freebox::ParseSource (const string & s)
{
  if (s == "")     return Source::AUTO;
  if (s == "iptv") return Source::IPTV;
  if (s == "dvb")  return Source::DVB;
  return Source::DEFAULT;
}

/* static */
enum Freebox::Quality Freebox::ParseQuality (const string & q)
{
  if (q == "auto") return Quality::AUTO;
  if (q == "hd")   return Quality::HD;
  if (q == "sd")   return Quality::SD;
  if (q == "ld")   return Quality::LD;
  if (q == "3d")   return Quality::STEREO;
  return Quality::DEFAULT;
}

/* static */
bool Freebox::HTTP (const string & custom,
                    const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  string url = URL (path);
  string session = m_session_token;
  lock.Unlock ();

  StringBuffer buffer;
  if (! request.IsNull ())
  {
    Writer<StringBuffer> writer (buffer);
    request.Accept (writer);
  }

  string response;
  long http = freebox_http (custom, url, buffer.GetString (), &response, session);
  XBMC->Log (LOG_DEBUG, "%s %s: %s", custom.c_str (), url.c_str (), response.c_str ());

  doc->Parse (response);

  if (doc->HasParseError ()) return false;

  if (! doc->IsObject ()) return false;

  auto s = doc->FindMember ("success");
  if (s == doc->MemberEnd ()) return false;

  bool success = s->value.GetBool ();
  if (success && type != kNullType)
  {
    auto r = doc->FindMember ("result");
    if (r == doc->MemberEnd () || r->value.GetType () != type) return false;
  }

  if (http != 200)
  {
    XBMC->QueueNotification (QUEUE_INFO, "HTTP %d", http);
    cout << "HTTP " << http << " : " << response << endl;
    return false;
  }

  return success;
}

/* static */
bool Freebox::GET (const string & path,
                   Document * doc, Type type) const
{
  return HTTP ("GET", path, Document (), doc, type);
}

/* static */
bool Freebox::POST (const string & path,
                    const Document & request,
                    Document * doc, Type type) const
{
  return HTTP ("POST", path, request, doc, type);
}

/* static */
bool Freebox::PUT (const string & path,
                   const Document & request,
                   Document * doc, Type type) const
{
  return HTTP ("PUT", path, request, doc, type);
}

/* static */
bool Freebox::DELETE (const string & path,
                      Document * doc) const
{
  return HTTP ("DELETE", path, Document (), doc, kNullType);
}

/* static */
string Freebox::Password (const string & token, const string & challenge)
{
  unsigned char password [EVP_MAX_MD_SIZE];
  unsigned int length;

  HMAC (EVP_sha1 (),
        token.c_str (), token.length (),
        (const unsigned char *) challenge.c_str (), challenge.length (),
        password, &length);

  ostringstream oss;
  oss << hex << setfill ('0');
  for (unsigned int i = 0; i < length; ++i)
    oss << setw (2) << (int) password [i];

  return oss.str ();
}

bool Freebox::StartSession ()
{
  P8PLATFORM::CLockObject lock (m_mutex);

  if (m_app_token.empty ())
  {
    string file = m_path + "app_token.txt";
    if (! XBMC->FileExists (file.c_str (), false))
    {
      char hostname [HOST_NAME_MAX + 1];
      gethostname (hostname, HOST_NAME_MAX);
      cout << "StartSession: hostname: " << hostname << endl;

      Document request (kObjectType);
      request.AddMember ("app_id",      PVR_FREEBOX_APP_ID,      request.GetAllocator ());
      request.AddMember ("app_name",    PVR_FREEBOX_APP_NAME,    request.GetAllocator ());
      request.AddMember ("app_version", PVR_FREEBOX_APP_VERSION, request.GetAllocator ());
      request.AddMember ("device_name", StringRef (hostname),    request.GetAllocator ());

      Document response;
      if (! POST ("/api/v6/login/authorize", request, &response)) return false;
      m_app_token = JSON<string> (response["result"], "app_token");
      m_track_id  = JSON<int>    (response["result"], "track_id");

      ofstream ofs (file);
      ofs << m_app_token << ' ' << m_track_id;
    }
    else
    {
      ifstream ifs (file);
      ifs >> m_app_token >> m_track_id;
    }

    //cout << "app_token: " << m_app_token << endl;
    //cout << "track_id: " << m_track_id << endl;
  }

  Document login;
  if (! GET ("/api/v6/login/", &login))
    return false;

  if (! login["result"]["logged_in"].GetBool ())
  {
    Document d;
    string track = to_string (m_track_id);
    string url   = "/api/v6/login/authorize/" + track;
    if (! GET (url, &d)) return false;
    string status    = JSON<string> (d["result"], "status", "unknown");
    string challenge = JSON<string> (d["result"], "challenge");
    //string salt      = JSON<string> (d["result"], "password_salt");
    //cout << status << ' ' << challenge << ' ' << salt << endl;

    if (status == "granted")
    {
      string password = Password (m_app_token, challenge);
      //cout << "password: " << password << " [" << password.length () << ']' << endl;

      Document request (kObjectType);
      request.AddMember ("app_id",   PVR_FREEBOX_APP_ID, request.GetAllocator ());
      request.AddMember ("password", password,           request.GetAllocator ());

      Document response;
      if (! POST ("/api/v6/login/session", request, &response)) return false;
      m_session_token = JSON<string> (response["result"], "session_token");

      cout << "StartSession: session_token: " << m_session_token << endl;
      return true;
    }
    else
    {
      char * notification = XBMC->GetLocalizedString (30001); // "Authorization required"
      XBMC->QueueNotification (QUEUE_INFO, notification);
      XBMC->FreeString (notification);
      return false;
    }
  }

  return true;
}

bool Freebox::CloseSession ()
{
  if (! m_session_token.empty ())
  {
    Document response;
    return POST ("/api/v6/login/logout/", Document (), &response, kNullType);
  }

  return true;
}

class Conflict
{
  public:
    string uuid;
    int major, minor; // numéro de chaîne
    int position;     // position dans le bouquet

  public:
    inline Conflict (const string & id,
                     int n1, int n2,
                     int p) :
      uuid (id),
      major (n1), minor (n2),
      position (p)
    {
    }
};

class ConflictComparator
{
  public:
    inline bool operator() (const Conflict & c1, const Conflict & c2) const
    {
      return tie (c1.major, c1.minor) < tie (c2.major, c2.minor);
    }
};

inline string StrUUIDs (const vector<Conflict> & v)
{
  string text;
  if (! v.empty ())
  {
    text += v[0].uuid;
    for (size_t i = 1; i < v.size (); ++i)
      text += ", " + v[i].uuid;
  }
  return '[' + text + ']';
}

inline string StrNumber (const Conflict & c, bool minor)
{
  return to_string (c.major) + (minor ? '.' + to_string (c.minor) : "");
}

inline string StrNumbers (const vector<Conflict> & v)
{
  string text;
  switch (v.size ())
  {
    case 0:
      break;

    case 1:
      text = StrNumber (v[0], false);
      break;

    default:
      text = StrNumber (v[0], true);
      for (size_t i = 1; i < v.size (); ++i)
        text += ", " + StrNumber (v[i], true);
      break;
  }
  return '[' + text + ']';
}

Freebox::Stream::Stream (enum Source  source,
                         enum Quality quality,
                         const string & url) :
  source (source),
  quality (quality),
  url (url)
{
}

int Freebox::Stream::score (enum Source s) const
{
  switch (s)
  {
    case Source::AUTO:
      switch (source)
      {
        case Source::AUTO: return 100;
        case Source::IPTV: return 10;
        case Source::DVB:  return 1;
        default:           return 0;
      }

    case Source::IPTV:
      switch (source)
      {
        case Source::AUTO: return 10;
        case Source::IPTV: return 100;
        case Source::DVB:  return 1;
        default:           return 0;
      }

    case Source::DVB:
      switch (source)
      {
        case Source::AUTO: return 10;
        case Source::IPTV: return 1;
        case Source::DVB:  return 100;
        default:           return 0;
      }

    default:
      return 0;
  }
}

int Freebox::Stream::score (enum Quality q) const
{
  switch (q)
  {
    case Quality::AUTO:
      switch (quality)
      {
        case Quality::AUTO: return 1000;
        case Quality::HD:   return 100;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1;
        default:            return 0;
      }

    case Quality::HD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1000;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1;
        default:            return 0;
      }

    case Quality::SD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1;
        case Quality::SD:   return 1000;
        case Quality::LD:   return 10;
        default:            return 0;
      }

    case Quality::LD:
      switch (quality)
      {
        case Quality::AUTO: return 100;
        case Quality::HD:   return 1;
        case Quality::SD:   return 10;
        case Quality::LD:   return 1000;
        default:            return 0;
      }

    case Quality::STEREO:
      return (quality == Quality::STEREO) ? 1000 : 0;

    default:
      return 0;
  }
}

int Freebox::Stream::score (enum Source s, enum Quality q) const
{
  return 10000 * score (s) + score (q);
}

Freebox::Channel::Channel (const string & uuid,
                           const string & name,
                           const string & logo,
                           int major, int minor,
                           const vector<Stream> & streams) :
  radio (false),
  uuid (uuid),
  name (name),
  logo (logo),
  major (major), minor (minor),
  streams (streams)
{
}

bool Freebox::Channel::IsHidden () const
{
  return streams.empty ();
}

void Freebox::Channel::GetChannel (ADDON_HANDLE handle, bool radio) const
{
  PVR_CHANNEL channel;
  memset (&channel, 0, sizeof (PVR_CHANNEL));

  channel.iUniqueId         = ChannelId (uuid);
  channel.bIsRadio          = radio;
  channel.iChannelNumber    = major;
  channel.iSubChannelNumber = minor;
  strncpy (channel.strChannelName, name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (channel.strIconPath,    logo.c_str (), PVR_ADDON_URL_STRING_LENGTH  - 1);
  channel.bIsHidden         = IsHidden ();

  PVR->TransferChannelEntry (handle, &channel);
}

PVR_ERROR Freebox::Channel::GetStreamProperties (enum Source source, enum Quality quality,
                                                 PVR_NAMED_VALUE * properties, unsigned int * count) const
{
  if (! streams.empty ())
  {
    int index = 0;
    int score = streams[0].score (source, quality);

    for (size_t i = 1; i < streams.size (); ++i)
    {
      int s = streams[i].score (source, quality);
      if (s > score)
      {
        index = i;
        score = s;
      }
    }

    strncpy (properties[0].strName,  PVR_STREAM_PROPERTY_STREAMURL,         PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[0].strValue, streams[index].url.c_str (),           PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[1].strName,  PVR_STREAM_PROPERTY_ISREALTIMESTREAM,  PVR_ADDON_NAME_STRING_LENGTH - 1);
    strncpy (properties[1].strValue, "true",                                PVR_ADDON_NAME_STRING_LENGTH - 1);
    *count = 2;
  }

  return PVR_ERROR_NO_ERROR;
}

int Freebox::Event::Category (int c)
{
  switch (c)
  {
    case  0: return 0x0 <<4| 0x0;
    case  1: return 0x1 <<4| 0x0; // Film
    case  2: return 0x1 <<4| 0x0; // Téléfilm
    case  3: return 0x1 <<4| 0x0; // Série/Feuilleton
    case  4: return 0x1 <<4| 0x5; // Feuilleton
    case  5: return 0x2 <<4| 0x3; // Documentaire
    case  6: return 0x7 <<4| 0x0; // Théâtre
    case  7: return 0x6 <<4| 0x5; // Opéra
    case  8: return 0x0 <<4| 0x0;
    case  9: return 0x3 <<4| 0x2; // Variétés
    case 10: return 0x8 <<4| 0x1; // Magazine
    case 11: return 0x5 <<4| 0x0; // Jeunesse
    case 12: return 0x3 <<4| 0x1; // Jeu
    case 13: return 0x6 <<4| 0x0; // Musique
    case 14: return 0x3 <<4| 0x2; // Divertissement
    case 15: return 0x0 <<4| 0x0;
    case 16: return 0x5 <<4| 0x5; // Dessin animé
    case 17: return 0x0 <<4| 0x0;
    case 18: return 0x0 <<4| 0x0;
    case 19: return 0x4 <<4| 0x0; // Sport
    case 20: return 0x2 <<4| 0x1; // Journal
    case 21: return 0x0 <<4| 0x0;
    case 22: return 0x2 <<4| 0x4; // Débat
    case 23: return 0x0 <<4| 0x0;
    case 24: return 0x7 <<4| 0x0; // Spectacle
    case 25: return 0x0 <<4| 0x0;
    case 26: return 0x0 <<4| 0x0;
    case 27: return 0x0 <<4| 0x0;
    case 28: return 0x0 <<4| 0x0;
    case 29: return 0x0 <<4| 0x0;
    case 30: return 0x0 <<4| 0x0;
    case 31: return 0x7 <<4| 0x3; // Emission religieuse
    default: return 0;
  };
}

Freebox::Event::CastMember::CastMember (const Value & c) :
  job        (JSON<string> (c, "job")),
  first_name (JSON<string> (c, "first_name")),
  last_name  (JSON<string> (c, "last_name")),
  role       (JSON<string> (c, "role"))
{
}

Freebox::Event::Event (const Value & e, unsigned int channel, time_t date) :
  channel  (channel),
  uuid     (JSON<string> (e, "id")),
  date     (JSON<int>    (e, "date", date)),
  duration (JSON<int>    (e, "duration")),
  title    (JSON<string> (e, "title")),
  subtitle (JSON<string> (e, "sub_title")),
  season   (JSON<int>    (e, "season_number")),
  episode  (JSON<int>    (e, "episode_number")),
  category (JSON<int>    (e, "category")),
  picture  (JSON<string> (e, "picture_big", JSON<string> (e, "picture"))),
  plot     (JSON<string> (e, "desc")),
  outline  (JSON<string> (e, "short_desc")),
  year     (JSON<int>    (e, "year")),
  cast     ()
{
  if (category != 0 && Category (category) == 0)
  {
    string name = JSON<string> (e, "category_name");
    cout << category << " : " << name << endl;
  }

  auto f = e.FindMember ("cast");
  if (f != e.MemberEnd ())
  {
    const Value & c = f->value;
    if (c.IsArray ())
      for (SizeType i = 0; i < c.Size (); ++i)
        cast.emplace_back (c[i]);
  }
}

Freebox::Event::ConcatIf::ConcatIf (const string & job) :
  m_job (job)
{
}

string Freebox::Event::ConcatIf::operator() (const string & input, const Freebox::Event::CastMember & m) const
{
  if (m.job != m_job) return input;
  return (input.empty () ? "" : input + EPG_STRING_TOKEN_SEPARATOR) + (m.first_name + ' ' + m.last_name);
}

string Freebox::Event::GetCastDirector () const
{
  static const ConcatIf CONCAT ("Réalisateur");
  return accumulate (cast.begin (), cast.end (), string (), CONCAT);
}

string Freebox::Event::GetCastActors () const
{
  static const ConcatIf CONCAT ("Acteur");
  return accumulate (cast.begin (), cast.end (), string (), CONCAT);
}

bool Freebox::ProcessChannels ()
{
  m_tv_channels.clear ();

  Document channels;
  if (! GET ("/api/v6/tv/channels", &channels)) return false;

  char * notification = XBMC->GetLocalizedString (30000); // "%d channels loaded"
  XBMC->QueueNotification (QUEUE_INFO, notification, channels["result"].MemberCount ());
  XBMC->FreeString (notification);

  //Document bouquets;
  //GET ("/api/v6/tv/bouquets", &m_tv_bouquets);

  Document bouquet;
  if (! GET ("/api/v6/tv/bouquets/freeboxtv/channels", &bouquet, kArrayType)) return false;

  // Conflict list.
  typedef vector<Conflict> Conflicts;
  // Conflicts by UUID.
  map<string, Conflicts> conflicts_by_uuid;
  // Conflicts by major.
  map<int, Conflicts> conflicts_by_major;

  const Value & r = bouquet ["result"];
  for (SizeType i = 0; i < r.Size (); ++i)
  {
    string uuid  = r[i]["uuid"].GetString ();
    int    major = r[i]["number"].GetInt ();
    int    minor = r[i]["sub_number"].GetInt ();

    Conflict c (uuid, major, minor, i);

    conflicts_by_uuid [uuid] .push_back (c);
    conflicts_by_major[major].push_back (c);
  }

  static const ConflictComparator comparator;

#ifndef ANDROID
  for (auto & [major, v1] : conflicts_by_major)
#else
  for (auto i : conflicts_by_major)
#endif
  {
#ifdef ANDROID
    int major = i.first;
    auto & v1 = i.second;
#endif
    sort (v1.begin (), v1.end (), comparator);

    for (size_t j = 1; j < v1.size (); ++j)
    {
      Conflicts & v2 = conflicts_by_uuid [v1[j].uuid];
      v2.erase (remove_if (v2.begin (), v2.end (),
        [major] (const Conflict & c) {return c.major == major;}));
    }

    v1.erase (v1.begin () + 1, v1.end ());
  }

#ifndef ANDROID
  for (auto & [uuid, v1] : conflicts_by_uuid)
#else
  for (auto i : conflicts_by_uuid)
#endif
  {
#ifdef ANDROID
    string uuid = i.first;
    auto & v1   = i.second;
#endif
    if (! v1.empty ())
    {
      sort (v1.begin (), v1.end (), comparator);

      for (size_t j = 1; j < v1.size (); ++j)
      {
        Conflicts & v2 = conflicts_by_major [v1[j].major];
        v2.erase (remove_if (v2.begin (), v2.end (),
          [&uuid] (const Conflict & c) {return c.uuid == uuid;}));
      }

      v1.erase (v1.begin () + 1, v1.end ());
    }
  }

#if 0
  for (auto i : conflicts_by_major)
    cout << i.first << " : " << StrUUIDs (i.second) << endl;
#endif

#if 0
  for (auto i = conflicts_by_uuid)
    cout << i.first << " : " << StrNumbers (i.second) << endl;
#endif

  for (auto i : conflicts_by_major)
  {
    const vector<Conflict> & q = i.second;
    if (! q.empty ())
    {
      const Conflict & ch = q.front ();
      const Value  & channel = channels["result"][ch.uuid];
      const string & name    = channel["name"].GetString ();
      const string & logo    = URL (channel["logo_url"].GetString ());
      const Value  & item    = bouquet["result"][ch.position];

      vector<Stream> data;
      if (item.HasMember("available") && item["available"].GetBool ()
       && item.HasMember("streams")   && item["streams"].IsArray ())
      {
        const Value & streams = item["streams"];
        for (SizeType i = 0; i < streams.Size (); ++i)
        {
          const Value  & s = streams [i];
          const string & t = s["type"].GetString ();
          const string & q = s["quality"].GetString ();
          const string & r = s["rtsp"].GetString ();
          data.emplace_back (ParseSource (t), ParseQuality (q), r);
        }
      }
      m_tv_channels.emplace (ChannelId (ch.uuid), Channel (ch.uuid, name, logo, ch.major, ch.minor, data));
    }
  }

  return true;
}

Freebox::Freebox (const string & path,
                  int quality,
                  int days,
                  bool extended,
                  int delay) :
  m_path (path),
  m_server ("mafreebox.freebox.fr"),
  m_delay (delay),
  m_app_token (),
  m_track_id (),
  m_session_token (),
  m_tv_channels (),
  m_tv_source (Source::AUTO),
  m_tv_quality (Quality (quality)),
  m_epg_queries (),
  m_epg_cache (),
  m_epg_days (0),
  m_epg_last (0),
  m_epg_extended (extended),
  m_recordings (),
  m_unique_id (1),
  m_generators (),
  m_timers ()
{
  SetDays (days);
  ProcessChannels ();
  CreateThread (false);
}

Freebox::~Freebox ()
{
  StopThread ();
  CloseSession ();
}

string Freebox::GetServer () const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_server;
}

// NOT thread-safe !
string Freebox::URL (const string & query) const
{
  return "http://" + m_server + query;
}

void Freebox::SetSource (int s)
{
  P8PLATFORM::CLockObject lock (m_mutex);
cout << "source: " << s << endl;
  m_tv_source = Source (s);
}

void Freebox::SetQuality (int q)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_tv_quality = Quality (q);
}

void Freebox::SetDays (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_days = d != EPG_TIMEFRAME_UNLIMITED ? min (d, 7) : 7;
}

void Freebox::SetExtended (bool e)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_epg_extended = e;
}

void Freebox::SetDelay (int d)
{
  P8PLATFORM::CLockObject lock (m_mutex);
  m_delay = d;
}

void Freebox::ProcessEvent (const Event & e, EPG_EVENT_STATE state)
{
  // FIXME: SHOULDN'T HAPPEN!
  if (e.uuid.find ("pluri_") != 0)
  {
#if 1
  cout << e.uuid << " : " << '"' << e.title << '"' << ' ' << e.date << '+' << e.duration << " (" << e.channel << ')' << endl;
  cout << "  " << e.category << ' ' << e.season << 'x' << e.episode << ' ' << '"' << e.subtitle << '"' << ' ' << '[' << e.picture << ']' << endl;
  cout << "  " << '"' << e.outline << '"' << endl;
  cout << "  " << '"' << e.plot << '"' << endl;
#endif

    XBMC->Log (LOG_ERROR, "%s : \"%s\" %d+%d", e.uuid.c_str (), e.title.c_str (), e.date, e.duration);
    return;
  }

  string picture = e.picture;
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    if (! picture.empty ()) picture = URL (picture);
  }

  string actors   = e.GetCastActors   ();
  string director = e.GetCastDirector ();

  EPG_TAG tag;
  memset (&tag, 0, sizeof (EPG_TAG));

  tag.iUniqueBroadcastId  = BroadcastId (e.uuid);
  tag.strTitle            = PVR_FREEBOX_C_STR (e.title);
  tag.iUniqueChannelId    = e.channel;
  tag.startTime           = e.date;
  tag.endTime             = e.date + e.duration;
  tag.strPlotOutline      = PVR_FREEBOX_C_STR (e.outline);
  tag.strPlot             = PVR_FREEBOX_C_STR (e.plot);
  tag.strOriginalTitle    = NULL;
  tag.strCast             = PVR_FREEBOX_C_STR (actors);
  tag.strDirector         = PVR_FREEBOX_C_STR (director);
  tag.strWriter           = NULL;
  tag.iYear               = e.year;
  tag.strIMDBNumber       = NULL;
  tag.strIconPath         = PVR_FREEBOX_C_STR (picture);
#if 1
  int c = Event::Category (e.category);
  tag.iGenreType          = c & 0xF0;
  tag.iGenreSubType       = c & 0x0F;
  tag.strGenreDescription = NULL;
#else
  tag.iGenreType          = EPG_GENRE_USE_STRING;
  tag.iGenreSubType       = 0;
  tag.strGenreDescription = PVR_FREEBOX_C_STR (e.category);
#endif
  tag.iParentalRating     = 0;
  tag.iStarRating         = 0;
  tag.bNotify             = false;
  tag.iSeriesNumber       = e.season;
  tag.iEpisodeNumber      = e.episode;
  tag.iEpisodePartNumber  = 0;
  tag.strEpisodeName      = PVR_FREEBOX_C_STR (e.subtitle);
  tag.iFlags              = EPG_TAG_FLAG_UNDEFINED;

  PVR->EpgEventStateChange (&tag, state);
}

void Freebox::ProcessEvent (const Value & event, unsigned int channel, time_t date, EPG_EVENT_STATE state)
{
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    auto f = m_tv_channels.find (channel);
    if (f == m_tv_channels.end () || f->second.IsHidden ()) return;
  }

  Event e (event, channel, date);

  if (state == EPG_EVENT_CREATED)
  {
    P8PLATFORM::CLockObject lock (m_mutex);
    if (m_epg_extended)
    {
      string query = "/api/v6/tv/epg/programs/" + e.uuid;
      m_epg_queries.emplace (EVENT, query, channel, date);
    }
  }

  ProcessEvent (e, state);
}

void Freebox::ProcessChannel (const Value & epg, unsigned int channel)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    const Value & event = i->value;

    string uuid = JSON<string> (event, "id");
    time_t date = JSON<int>    (event, "date");

    static const string PREFIX = "pluri_";
    if (uuid.find (PREFIX) != 0) continue;

    string query = "/api/v6/tv/epg/programs/" + uuid;

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      if (m_epg_cache.count (query) > 0) continue;
    }

    ProcessEvent (event, channel, date, EPG_EVENT_CREATED);

    {
      P8PLATFORM::CLockObject lock (m_mutex);
      m_epg_cache.insert (query);
    }
  }
}

void Freebox::ProcessFull (const Value & epg)
{
  for (auto i = epg.MemberBegin (); i != epg.MemberEnd (); ++i)
  {
    string uuid = i->name.GetString ();
    ProcessChannel (i->value, ChannelId (uuid));
  }
}

void * Freebox::Process ()
{
  while (! IsStopped ())
  {
    m_mutex.Lock ();
    int    delay = m_delay;
    int    days  = m_epg_days;
    time_t now   = time (NULL);
    time_t end   = now + days * 24 * 60 * 60;
    time_t last  = max (now, m_epg_last);
    m_mutex.Unlock ();

    if (StartSession ())
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      ProcessGenerators ();
      ProcessTimers ();
      ProcessRecordings ();
    }

    for (time_t t = last - (last % 3600); t < end; t += 3600)
    {
      string epoch = to_string (t);
      string query = "/api/v6/tv/epg/by_time/" + epoch;
      {
        P8PLATFORM::CLockObject lock (m_mutex);
        m_epg_queries.emplace (FULL, query);
        //XBMC->Log (LOG_INFO, "Queued: '%s' %d < %d", query.c_str (), t, end);
        m_epg_last = t + 3600;
      }
    }

    Query q;
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      if (! m_epg_queries.empty ())
      {
        q = m_epg_queries.front ();
        m_epg_queries.pop ();
      }
    }

    if (q.type != NONE)
    {
      //cout << q.query << " [" << delay << ']' << endl;
      XBMC->Log (LOG_INFO, "Processing: '%s'", q.query.c_str ());

      Document json;
      if (GET (q.query, &json))
      {
        switch (q.type)
        {
          case FULL    : ProcessFull    (json["result"]); break;
          case CHANNEL : ProcessChannel (json["result"], q.channel); break;
          case EVENT   : ProcessEvent   (json["result"], q.channel, q.date, EPG_EVENT_UPDATED); break;
        }
      }
    }
    else
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      m_epg_cache.clear ();
    }

    Sleep (delay * 1000);
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// C H A N N E L S /////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

int Freebox::GetChannelsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_tv_channels.size ();
}

PVR_ERROR Freebox::GetChannels (ADDON_HANDLE handle, bool radio)
{
  P8PLATFORM::CLockObject lock (m_mutex);

  //for (auto i = m_tv_channels.begin (); i != m_tv_channels.end (); ++i)
  for (auto i : m_tv_channels)
    i.second.GetChannel (handle, radio);

  return PVR_ERROR_NO_ERROR;
}

int Freebox::GetChannelGroupsAmount ()
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return 0;
}

PVR_ERROR Freebox::GetChannelGroups (ADDON_HANDLE handle, bool radio)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelGroupMembers (ADDON_HANDLE handle, const PVR_CHANNEL_GROUP & group)
{
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetChannelStreamProperties (const PVR_CHANNEL * channel, PVR_NAMED_VALUE * properties, unsigned int * count)
{
  if (! channel || ! properties || ! count || *count < 2)
    return PVR_ERROR_INVALID_PARAMETERS;

  P8PLATFORM::CLockObject lock (m_mutex);
  auto f = m_tv_channels.find (channel->iUniqueId);
  if (f != m_tv_channels.end ())
    return f->second.GetStreamProperties (m_tv_source, m_tv_quality, properties, count);

  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// R E C O R D I N G S /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Recording::Recording (const Value & json) :
  id              (JSON<int>    (json, "id")),
  start           (JSON<int>    (json, "start")),
  end             (JSON<int>    (json, "end")),
  name            (JSON<string> (json, "name")),
  subname         (JSON<string> (json, "subname")),
  channel_uuid    (JSON<string> (json, "channel_uuid")),
  channel_name    (JSON<string> (json, "channel_name")),
//channel_quality (JSON<string> (json, "channel_quality")),
//channel_type    (JSON<string> (json, "channel_type")),
//broadcast_type  (JSON<string> (json, "broadcast_type")),
  media           (JSON<string> (json, "media")),
  path            (JSON<string> (json, "path")),
  filename        (JSON<string> (json, "filename")),
  secure          (JSON<bool>   (json, "secure"))
{
}

void Freebox::ProcessRecordings ()
{
  m_recordings.clear ();

  Document recordings;
  if (GET ("/api/v6/pvr/finished/", &recordings, kArrayType))
  {
    Value & result = recordings ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int id = result[i]["id"].GetInt ();
      m_recordings.emplace (id, Recording (result [i]));
    }

    PVR->TriggerRecordingUpdate ();
  }
}

int Freebox::GetRecordingsAmount (bool deleted) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_recordings.size ();
}

PVR_ERROR Freebox::GetRecordings (ADDON_HANDLE handle, bool deleted) const
{
  P8PLATFORM::CLockObject lock (m_mutex);

  for (auto & [id, r] : m_recordings)
    if (! r.secure)
    {
      PVR_RECORDING recording;
      memset (&recording, 0, sizeof (PVR_RECORDING));

      recording.recordingTime = r.start;
      recording.iDuration     = r.end - r.start;
      recording.iChannelUid   = ChannelId (r.channel_uuid);
      recording.channelType   = PVR_RECORDING_CHANNEL_TYPE_TV; // r.broadcast_type == "tv"

      strncpy (recording.strRecordingId, to_string (r.id).c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strTitle,       r.name.c_str (),           PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strEpisodeName, r.subname.c_str (),        PVR_ADDON_NAME_STRING_LENGTH - 1);
      strncpy (recording.strChannelName, r.channel_name.c_str (),   PVR_ADDON_NAME_STRING_LENGTH - 1);

      PVR->TransferRecordingEntry (handle, &recording);
    }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::GetRecordingStreamProperties (const PVR_RECORDING * recording, PVR_NAMED_VALUE * properties, unsigned int * count) const
{
  if (! recording || ! properties || ! count || *count < 2)
    return PVR_ERROR_INVALID_PARAMETERS;

  int id = stoi (recording->strRecordingId);

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  const Recording & r = i->second;
  string stream = "smb://" + m_server + '/' + r.media + '/' + r.path + '/' + r.filename;
  strncpy (properties[0].strName,  PVR_STREAM_PROPERTY_STREAMURL,        PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[0].strValue, stream.c_str (),                      PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[1].strName,  PVR_STREAM_PROPERTY_ISREALTIMESTREAM, PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy (properties[1].strValue, "false",                              PVR_ADDON_NAME_STRING_LENGTH - 1);
  *count = 2;

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::RenameRecording (const PVR_RECORDING & recording)
{
  StartSession ();

  int    id      = stoi (recording.strRecordingId);
  string name    = recording.strTitle;
  string subname = recording.strEpisodeName;

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Payload.
  Document d (kObjectType);
  d.AddMember ("name",    name,    d.GetAllocator ());
  d.AddMember ("subname", subname, d.GetAllocator ());

  // Update recording (Freebox).
  Document response;
  if (! PUT ("/api/v6/pvr/finished/" + to_string (id), d, &response))
    return PVR_ERROR_SERVER_ERROR;

  // Update recording (locally).
  i->second = Recording (response["result"]);
  PVR->TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::DeleteRecording (const PVR_RECORDING & recording)
{
  StartSession ();

  int id = stoi (recording.strRecordingId);

  P8PLATFORM::CLockObject lock (m_mutex);
  auto i = m_recordings.find (id);
  if (i == m_recordings.end ())
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (Freebox).
  Document response;
  if (! DELETE ("/api/v6/pvr/finished/" + to_string (id), &response))
    return PVR_ERROR_SERVER_ERROR;

  // Delete recording (locally).
  m_recordings.erase (i);
  PVR->TriggerRecordingUpdate ();

  return PVR_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
// T I M E R S /////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Freebox::Generator::Generator (const Value & json) :
  id               (JSON<int>    (json, "id")),
//type             (JSON<string> (json, "type")),
  media            (JSON<string> (json, "media")),
  path             (JSON<string> (json, "path")),
  name             (JSON<string> (json, "name")),
//subname          (JSON<string> (json, "name")),
  channel_uuid     (JSON<string> (json["params"], "channel_uuid")),
//channel_type     (JSON<string> (json["params"], "channel_type")),
//channel_quality  (JSON<string> (json["params"], "channel_quality")),
//channel_strict   (JSON<bool>   (json["params"], "channel_strict"))
//broadcast_type   (JSON<bool>   (json["params"], "broadcast_type"))
  start_hour       (JSON<int>    (json["params"], "start_hour")),
  start_min        (JSON<int>    (json["params"], "start_min")),
  duration         (JSON<int>    (json["params"], "duration")),
  margin_before    (JSON<int>    (json["params"], "margin_before")),
  margin_after     (JSON<int>    (json["params"], "margin_after")),
  repeat_monday    (JSON<bool>   (json["params"]["repeat_days"], "monday")),
  repeat_tuesday   (JSON<bool>   (json["params"]["repeat_days"], "tuesday")),
  repeat_wednesday (JSON<bool>   (json["params"]["repeat_days"], "wednesday")),
  repeat_thursday  (JSON<bool>   (json["params"]["repeat_days"], "thursday")),
  repeat_friday    (JSON<bool>   (json["params"]["repeat_days"], "friday")),
  repeat_saturday  (JSON<bool>   (json["params"]["repeat_days"], "saturday")),
  repeat_sunday    (JSON<bool>   (json["params"]["repeat_days"], "sunday"))
{
}

void Freebox::ProcessGenerators ()
{
  m_generators.clear ();

  Document generators;
  if (GET ("/api/v6/pvr/generator/", &generators, kArrayType))
  {
    Value & result = generators ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique_id, Generator (result [i]));
    }

    PVR->TriggerTimerUpdate ();
  }
}

Freebox::Timer::Timer (const Value & json) :
  id             (JSON<int>    (json, "id")),
  start          (JSON<int>    (json, "start")),
  end            (JSON<int>    (json, "end")),
  margin_before  (JSON<int>    (json, "margin_before")),
  margin_after   (JSON<int>    (json, "margin_after")),
  name           (JSON<string> (json, "name")),
  subname        (JSON<string> (json, "subname")),
  channel_uuid   (JSON<string> (json, "channel_uuid")),
  channel_name   (JSON<string> (json, "channel_name")),
//channel_type   (JSON<string> (json, "channel_type")),
//broadcast_type (JSON<string> (json, "broadcast_type")),
  media          (JSON<string> (json, "media")),
  path           (JSON<string> (json, "path")),
  has_record_gen (JSON<bool>   (json, "has_record_gen")),
  record_gen_id  (JSON<int>    (json, "record_gen_id")),
  enabled        (JSON<bool>   (json, "enabled")),
  conflict       (JSON<bool>   (json, "conflict")),
  state          (JSON<string> (json, "state")),
  error          (JSON<string> (json, "error"))
{
}

void Freebox::ProcessTimers ()
{
  m_timers.clear ();

  Document timers;
  if (GET ("/api/v6/pvr/programmed/", &timers, kArrayType))
  {
    Value & result = timers ["result"];
    for (SizeType i = 0; i < result.Size (); ++i)
    {
      int        id = result[i]["id"].GetInt ();
      int unique_id = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique_id, Timer (result [i]));
    }

    PVR->TriggerTimerUpdate ();
  }
}

PVR_ERROR Freebox::GetTimerTypes (PVR_TIMER_TYPE types [], int * size) const
{
  if (! size || *size < 5)
    return PVR_ERROR_INVALID_PARAMETERS;

  const unsigned int ATTRIBS =
    PVR_TIMER_TYPE_SUPPORTS_CHANNELS         |
    PVR_TIMER_TYPE_SUPPORTS_START_TIME       |
    PVR_TIMER_TYPE_SUPPORTS_END_TIME         |
    PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN;

  // One-shot manual.
  //strncpy (types[0].strDescription, "PVR_FREEBOX_TIMER_MANUAL", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[0].iId = PVR_FREEBOX_TIMER_MANUAL;
  types[0].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL;
  types[0].iPrioritiesSize = 0;
  types[0].iLifetimesSize = 0;
  types[0].iPreventDuplicateEpisodesSize = 0;
  types[0].iRecordingGroupSize = 0;
  types[0].iMaxRecordingsSize = 0;

  // One-shot EPG.
  //strncpy (types[1].strDescription, "PVR_FREEBOX_TIMER_EPG", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[1].iId = PVR_FREEBOX_TIMER_EPG;
  types[1].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE;
  types[1].iPrioritiesSize = 0;
  types[1].iLifetimesSize = 0;
  types[1].iPreventDuplicateEpisodesSize = 0;
  types[1].iRecordingGroupSize = 0;
  types[1].iMaxRecordingsSize = 0;

  // One-shot generated (read-only).
  //strncpy (types[2].strDescription, "PVR_FREEBOX_TIMER_GENERATED", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[2].iId = PVR_FREEBOX_TIMER_GENERATED;
  types[2].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_READONLY |
                         PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES |
                         PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE;
  types[2].iPrioritiesSize = 0;
  types[2].iLifetimesSize = 0;
  types[2].iPreventDuplicateEpisodesSize = 0;
  types[2].iRecordingGroupSize = 0;
  types[2].iMaxRecordingsSize = 0;

  // Repeating manual.
  //strncpy (types[3].strDescription, "PVR_FREEBOX_GENERATOR_MANUAL", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[3].iId = PVR_FREEBOX_GENERATOR_MANUAL;
  types[3].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_MANUAL |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS;
  types[3].iPrioritiesSize = 0;
  types[3].iLifetimesSize = 0;
  types[3].iPreventDuplicateEpisodesSize = 0;
  types[3].iRecordingGroupSize = 0;
  types[3].iMaxRecordingsSize = 0;

  // Repeating EPG.
  //strncpy (types[4].strDescription, "PVR_FREEBOX_GENERATOR_EPG", PVR_ADDON_TIMERTYPE_STRING_LENGTH - 1);
  types[4].iId = PVR_FREEBOX_GENERATOR_EPG;
  types[4].iAttributes = ATTRIBS |
                         PVR_TIMER_TYPE_IS_REPEATING |
                         PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS |
                         PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE;
  types[4].iPrioritiesSize = 0;
  types[4].iLifetimesSize = 0;
  types[4].iPreventDuplicateEpisodesSize = 0;
  types[4].iRecordingGroupSize = 0;
  types[4].iMaxRecordingsSize = 0;

  *size = 5;
  return PVR_ERROR_NO_ERROR;
}

int Freebox::GetTimersAmount () const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  return m_generators.size () + m_timers.size ();
}

PVR_ERROR Freebox::GetTimers (ADDON_HANDLE handle) const
{
  P8PLATFORM::CLockObject lock (m_mutex);
  //cout << "Freebox::GetTimers" << endl;

  for (auto & [id, g] : m_generators)
  {
    PVR_TIMER timer;
    memset (&timer, 0, sizeof (PVR_TIMER));

    time_t now = time (NULL);
    tm today = *localtime (&now);
    today.tm_hour = g.start_hour;
    today.tm_min  = g.start_min;
    today.tm_sec  = 0;
    time_t start = mktime (&today);

    timer.iTimerType         = PVR_FREEBOX_GENERATOR_MANUAL;
    timer.iParentClientIndex = PVR_TIMER_NO_PARENT;
    timer.iClientIndex       = id;
    timer.iClientChannelUid  = ChannelId (g.channel_uuid);
    timer.startTime          = start;
    timer.endTime            = start + g.duration;
    timer.iMarginStart       = g.margin_before / 60;
    timer.iMarginEnd         = g.margin_after  / 60;
    timer.iWeekdays          = (g.repeat_monday    ? PVR_WEEKDAY_MONDAY    : 0) |
                               (g.repeat_tuesday   ? PVR_WEEKDAY_TUESDAY   : 0) |
                               (g.repeat_wednesday ? PVR_WEEKDAY_WEDNESDAY : 0) |
                               (g.repeat_thursday  ? PVR_WEEKDAY_THURSDAY  : 0) |
                               (g.repeat_friday    ? PVR_WEEKDAY_FRIDAY    : 0) |
                               (g.repeat_saturday  ? PVR_WEEKDAY_SATURDAY  : 0) |
                               (g.repeat_sunday    ? PVR_WEEKDAY_SUNDAY    : 0);

    strncpy (timer.strTitle, g.name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);

    PVR->TransferTimerEntry (handle, &timer);
  }

  for (auto & [id, t] : m_timers)
  {
    PVR_TIMER timer;
    memset (&timer, 0, sizeof (PVR_TIMER));

    if (t.has_record_gen)
    {
      timer.iTimerType         = PVR_FREEBOX_TIMER_GENERATED;
      timer.iParentClientIndex = m_unique_id ("generator/" + to_string (t.record_gen_id));
    }
    else
    {
      timer.iTimerType         = PVR_FREEBOX_TIMER_MANUAL;
      timer.iParentClientIndex = PVR_TIMER_NO_PARENT;
    }

    timer.iClientIndex       = id;
    timer.iClientChannelUid  = ChannelId (t.channel_uuid);
    timer.startTime          = t.start;
    timer.endTime            = t.end;
    timer.iMarginStart       = t.margin_before / 60;
    timer.iMarginEnd         = t.margin_after  / 60;

    /**/ if (t.state == "disabled")           timer.state = PVR_TIMER_STATE_DISABLED;
    else if (t.state == "start_error")        timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "waiting_start_time") timer.state = PVR_TIMER_STATE_SCHEDULED; // FIXME: t.conflict?
    else if (t.state == "starting")           timer.state = PVR_TIMER_STATE_RECORDING;
    else if (t.state == "running")            timer.state = PVR_TIMER_STATE_RECORDING;
    else if (t.state == "running_error")      timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "failed")             timer.state = PVR_TIMER_STATE_ERROR;
    else if (t.state == "finished")           timer.state = PVR_TIMER_STATE_COMPLETED;

    strncpy (timer.strTitle, t.name.c_str (), PVR_ADDON_NAME_STRING_LENGTH - 1);

    PVR->TransferTimerEntry (handle, &timer);
  }

  return PVR_ERROR_NO_ERROR;
}

inline Value freebox_generator_weekdays (int w, Document::AllocatorType & a)
{
  Value r (kObjectType);
  r.AddMember ("monday",    (w & PVR_WEEKDAY_MONDAY)    != 0, a);
  r.AddMember ("tuesday",   (w & PVR_WEEKDAY_TUESDAY)   != 0, a);
  r.AddMember ("wednesday", (w & PVR_WEEKDAY_WEDNESDAY) != 0, a);
  r.AddMember ("thursday",  (w & PVR_WEEKDAY_THURSDAY)  != 0, a);
  r.AddMember ("friday",    (w & PVR_WEEKDAY_FRIDAY)    != 0, a);
  r.AddMember ("saturday",  (w & PVR_WEEKDAY_SATURDAY)  != 0, a);
  r.AddMember ("sunday",    (w & PVR_WEEKDAY_SUNDAY)    != 0, a);
  return r;
}

inline Document freebox_generator_request (const PVR_TIMER & timer)
{
  string channel_uuid = "uuid-webtv-" + to_string (timer.iClientChannelUid);
  string title        = timer.strTitle;
  time_t start        = timer.startTime;
  tm     date         = *localtime (&start);
  int    duration     = timer.endTime - start;

  Document d (kObjectType);
  Document::AllocatorType & a = d.GetAllocator ();
  d.AddMember ("type", "manual_repeat", a);
  d.AddMember ("name", title,           a);
  Value p (kObjectType);
  p.AddMember ("start_hour",    date.tm_hour, a);
  p.AddMember ("start_min",     date.tm_min,  a);
  p.AddMember ("start_sec",     0,            a);
  p.AddMember ("duration",      duration,     a);
  p.AddMember ("margin_before", timer.iMarginStart * 60, a);
  p.AddMember ("margin_after",  timer.iMarginEnd   * 60, a);
  p.AddMember ("channel_uuid",  channel_uuid, a);
  p.AddMember ("repeat_days",   freebox_generator_weekdays (timer.iWeekdays, a), a);
  d.AddMember ("params",        p, a);
  return d;
}

PVR_ERROR Freebox::AddTimer (const PVR_TIMER & timer)
{
  StartSession ();

  int    type         = timer.iTimerType;
  int    channel      = timer.iClientChannelUid;
  string channel_uuid = "uuid-webtv-" + to_string (channel);
  string title        = timer.strTitle;

  P8PLATFORM::CLockObject lock (m_mutex);
  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      //cout << "AddTimer: TIMER[" << type << ']' << endl;

      string subtitle;
      if (timer.iEpgUid != EPG_TAG_INVALID_UID)
      {
        Document epg;
        string epg_id = "pluri_" + to_string (timer.iEpgUid);
        if (GET ("/api/v6/tv/epg/programs/" + epg_id, &epg))
        {
          Event e (epg ["result"], channel, timer.startTime);
          ostringstream oss;
          if (e.season  != 0) oss << 'S' << setfill ('0') << setw (2) << e.season;
          if (e.episode != 0) oss << 'E' << setfill ('0') << setw (2) << e.episode;
          string prefix = oss.str ();
          subtitle = (prefix.empty () ? "" : prefix + " - ") + e.subtitle;
        }
      }

      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
      d.AddMember ("start",           (int64_t) timer.startTime, a);
      d.AddMember ("end",             (int64_t) timer.endTime,   a);
      d.AddMember ("margin_before",   timer.iMarginStart * 60,   a);
      d.AddMember ("margin_after",    timer.iMarginEnd   * 60,   a);
      d.AddMember ("channel_uuid",    channel_uuid,              a);
      d.AddMember ("channel_type",    "",                        a);
      d.AddMember ("channel_quality", "auto",                    a);
      d.AddMember ("broadcast_type",  "tv",                      a);
      d.AddMember ("name",            title,                     a);
      d.AddMember ("subname",         subtitle,                  a);
    //d.AddMember ("media",           "Disque dur",              a);
    //d.AddMember ("path",            "Enregistrements",         a);

      // Add timer (Freebox).
      Document response;
      if (! POST ("/api/v6/pvr/programmed/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add timer (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("programmed/" + to_string (id));
      m_timers.emplace (unique, Timer (response["result"]));
      PVR->TriggerTimerUpdate ();

      // Update recordings if timer is running.
      string state = JSON<string> (response["result"], "state");
      //cout << "AddTimer: TIMER[" << type << "]: '" << state << "'" << endl;
      if (state == "starting" || state == "running")
        ProcessRecordings (); // FIXME: doesn't work!

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      //cout << "AddTimer: GENERATOR[" << type << ']' << endl;

      // Payload.
      Document d = freebox_generator_request (timer);

      // Add generator (Freebox).
      Document response;
      if (! POST ("/api/v6/pvr/generator/", d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Add generator (locally).
      int id     = response["result"]["id"].GetInt ();
      int unique = m_unique_id ("generator/" + to_string (id));
      m_generators.emplace (unique, Generator (response["result"]));

      // Reload timers.
      ProcessTimers ();
      // Reload recordings.
      ProcessRecordings ();

      break;
    }

    default:
    {
      //cout << "AddTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::UpdateTimer (const PVR_TIMER & timer)
{
  StartSession ();

  int type = timer.iTimerType;

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      string channel_uuid = "uuid-webtv-" + to_string (timer.iClientChannelUid);
      string title        = timer.strTitle;

      // Payload.
      Document d (kObjectType);
      Document::AllocatorType & a = d.GetAllocator ();
    //d.AddMember ("enabled",         enabled,                   a);
      d.AddMember ("start",           (int64_t) timer.startTime, a);
      d.AddMember ("end",             (int64_t) timer.endTime,   a);
      d.AddMember ("margin_before",   timer.iMarginStart * 60,   a);
      d.AddMember ("margin_after",    timer.iMarginEnd   * 60,   a);
      d.AddMember ("channel_uuid",    channel_uuid,              a);
    //d.AddMember ("channel_type",    "",                        a);
    //d.AddMember ("channel_quality", "auto",                    a);
      d.AddMember ("name",            title,                     a);
    //d.AddMember ("subname",         "",                        a);
    //d.AddMember ("media",           "Disque dur",              a);
    //d.AddMember ("path",            "Enregistrements",         a);

      // Update timer (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER[" << type << "]: '" << i->second.state << "'" << endl;
      PVR->TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_TIMER_GENERATED :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: TIMER_GENERATED: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d (kObjectType);
      d.AddMember ("enabled", timer.state != PVR_TIMER_STATE_DISABLED, d.GetAllocator ());

      // Update generated timer (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/programmed/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update generated timer (locally).
      i->second = Timer (response["result"]);
      //cout << "UpdateTimer: TIMER_GENERATED: '" << i->second.state << "'" << endl;
      PVR->TriggerTimerUpdate ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_generators.find (timer.iClientIndex);
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "UpdateTimer: GENERATOR[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Payload.
      Document d = freebox_generator_request (timer);

      // Update generator (Freebox).
      Document response;
      if (! PUT ("/api/v6/pvr/generator/" + to_string (id), d, &response))
        return PVR_ERROR_SERVER_ERROR;

      // Update generator (locally).
      i->second = Generator (response["result"]);
      ProcessTimers ();
      ProcessRecordings ();

      break;
    }

    default:
    {
      //cout << "UpdateTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR Freebox::DeleteTimer (const PVR_TIMER & timer, bool force)
{
  StartSession ();

  int type = timer.iTimerType;

  switch (type)
  {
    case PVR_FREEBOX_TIMER_MANUAL :
    case PVR_FREEBOX_TIMER_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_timers.find (timer.iClientIndex);
      if (i == m_timers.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: TIMER[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Delete timer (Freebox).
      Document response;
      if (! DELETE ("/api/v6/pvr/programmed/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Delete timer (locally).
      m_timers.erase (i);
      PVR->TriggerTimerUpdate ();

      // Update recordings if timer was running.
      if (timer.state == PVR_TIMER_STATE_RECORDING)
        ProcessRecordings ();

      break;
    }

    case PVR_FREEBOX_GENERATOR_MANUAL :
    case PVR_FREEBOX_GENERATOR_EPG :
    {
      P8PLATFORM::CLockObject lock (m_mutex);
      auto i = m_generators.find (timer.iClientIndex);
      if (i == m_generators.end ())
        return PVR_ERROR_SERVER_ERROR;

      int id = i->second.id;
      //cout << "DeleteTimer: GENERATOR[" << type << "]: " << timer.iClientIndex << " > " << id << endl;

      // Delete generator (Freebox).
      Document response;
      if (! DELETE ("/api/v6/pvr/generator/" + to_string (id), &response))
        return PVR_ERROR_SERVER_ERROR;

      // Delete generated timers (locally).
      for (auto i = m_timers.begin (); i != m_timers.end ();)
        if (i->second.record_gen_id == id)
          i = m_timers.erase (i);
        else
          ++i;

      // Delete generator (locally).
      m_generators.erase (i);
      PVR->TriggerTimerUpdate ();

      break;
    }

    default:
    {
      //cout << "DeleteTimer: UNKNOWN TYPE!" << endl;
      return PVR_ERROR_SERVER_ERROR;
    }
  }

  return PVR_ERROR_NO_ERROR;
}

