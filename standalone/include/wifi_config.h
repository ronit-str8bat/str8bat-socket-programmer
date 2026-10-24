#pragma once
/* Keep deployment secrets in wifi_config.local.h (git-ignored).
 * The local-hotspot profile uses WIFI_LOCAL_SSID / WIFI_LOCAL_PASSWORD.
 * Factory STA, OTA, NTP and cloud settings below apply only to legacy network
 * builds. Each Pico exposes its board ID at GET /status.
 */
#if __has_include("wifi_config.local.h")
#include "wifi_config.local.h"
#endif
/* Local-hotspot base name for the standard non-factory build. Factory builds
 * use Str8bat_Master_<id> and Str8bat_Slave_<id> automatically. */
#ifndef WIFI_LOCAL_SSID
#define WIFI_LOCAL_SSID "Str8bat_Pico"
#endif
#ifndef WIFI_LOCAL_PASSWORD
#define WIFI_LOCAL_PASSWORD "str8bat@pico"
#endif
/* Master/slave traffic has an additional application key. Set a private value
 * in wifi_config.local.h before factory deployment; it must match both builds. */
#ifndef FACTORY_GROUP_KEY
#define FACTORY_GROUP_KEY "CHANGE-THIS-FACTORY-KEY"
#endif
/* Fixed factory pairing. These IDs are the 16-character Pico board IDs that
 * appear at the end of the factory Wi-Fi names. The Master accepts only the
 * listed Slave IDs. Every Slave connects only to FACTORY_MASTER_ID.
 *
 * To add a later Slave, replace the empty string in SLAVE_ID_2 or SLAVE_ID_3
 * with that Pico's 16-character ID, then rebuild and install the Master UF2.
 */
#ifndef FACTORY_MASTER_ID
#define FACTORY_MASTER_ID "882078473D7590B2"
#endif
#ifndef FACTORY_SLAVE_ID_1
#define FACTORY_SLAVE_ID_1 "16AB9FF57CE41A89"
#endif
#ifndef FACTORY_SLAVE_ID_2
#define FACTORY_SLAVE_ID_2 ""
#endif
#ifndef FACTORY_SLAVE_ID_3
#define FACTORY_SLAVE_ID_3 ""
#endif
/* Offline operation is allowed. CSV timestamps remain UNSET until POST /time. */
#ifndef LOCAL_REQUIRE_TIME
#define LOCAL_REQUIRE_TIME 0
#endif
#ifndef WIFI_FACTORY_SSID
#define WIFI_FACTORY_SSID ""
#endif
#ifndef WIFI_FACTORY_PASSWORD
#define WIFI_FACTORY_PASSWORD ""
#endif
#ifndef WIFI_SETUP_SSID
#define WIFI_SETUP_SSID "Pico_Programmer_Setup"
#endif
/* Bench default ONLY: choose a private fixture password before deployment. */
#ifndef WIFI_SETUP_PASSWORD
#define WIFI_SETUP_PASSWORD "PicoSetup-ChangeMe"
#endif
#define WIFI_CONNECT_TIMEOUT_MS 15000u
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL ""
#endif
#ifndef LOG_UPLOAD_URL
#define LOG_UPLOAD_URL ""
#endif
#ifndef SERVER_BEARER_TOKEN
#define SERVER_BEARER_TOKEN ""
#endif
/* PEM CA certificate, including BEGIN/END lines and newline escapes. Empty
 * disables remote requests; there is no insecure TLS/HTTP fallback. */
#ifndef SERVER_CA_PEM
#define SERVER_CA_PEM ""
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef FACTORY_IMAGE_REVISION
#define FACTORY_IMAGE_REVISION 40002u
#endif
#define OTA_CHECK_INTERVAL_MS 3600000u
#define LOG_RETRY_INTERVAL_MS 60000u
