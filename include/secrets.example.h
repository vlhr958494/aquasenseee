#pragma once
// ---------------------------------------------------------------
//  Copy this file to  include/secrets.h  and fill in your own values.
//  secrets.h is listed in .gitignore and must NEVER be committed.
//  (WiFiNetwork is defined in main.cpp before this file is included.)
// ---------------------------------------------------------------

// Wi-Fi networks, tried in order. ESP32 supports 2.4 GHz ONLY (case-sensitive).
// Leave unused rows as {"", ""}.
const WiFiNetwork WIFI_NETWORKS[] = {
  {"YOUR_WIFI_NAME",     "YOUR_WIFI_PASSWORD"},
  {"YOUR_PHONE_HOTSPOT", "HOTSPOT_PASSWORD"},
  {"", ""},
};

// Google Apps Script "Web app" URL (Deploy > New deployment > Web app)
const char GOOGLE_SCRIPT_URL[] = "https://script.google.com/macros/s/PASTE_YOUR_DEPLOYMENT_ID_HERE/exec";

// Must be identical to SECRET_TOKEN in google_apps_script/Code.gs
const char GOOGLE_TOKEN[] = "CHANGE_ME_LONG_RANDOM_TOKEN";

const char DEVICE_ID[] = "AQUASENSE-01";
