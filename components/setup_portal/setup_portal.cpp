#include "setup_portal.h"

#include "flights_config.h"
#include "storage.h"

#include <cstdio>
#include <string>

namespace setup_portal {

namespace {

std::string htmlEscape(const std::string& input) {
    std::string output;
    output.reserve(input.length() + 8);
    for (char c : input) {
        if (c == '&') {
            output += "&amp;";
        } else if (c == '"') {
            output += "&quot;";
        } else if (c == '<') {
            output += "&lt;";
        } else if (c == '>') {
            output += "&gt;";
        } else {
            output += c;
        }
    }
    return output;
}

std::string formatDouble(double value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.6f", value);
    return buffer;
}

}  // namespace

std::string renderPage(const std::string& basePath) {
    std::string ssid;
    std::string pass;
    settings.getWifi(ssid, pass);
    ssid = htmlEscape(ssid);

    const std::string lat = formatDouble(settings.getReceiverLatitude());
    const std::string lon = formatDouble(settings.getReceiverLongitude());
    const std::string sleep = std::to_string(settings.getDisplaySleepMin());
    const std::string radarRange = std::to_string(settings.getRadarRangeMiles());
    const std::string feederUrl = htmlEscape(settings.getFeederUrl());
    const std::string passPlaceholder = pass.empty() ? "Required" : "Leave blank to keep current password";

    return std::string(
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>FlightsAbove Setup</title><style>"
        ":root{color-scheme:dark;--bg:#07100d;--panel:#10211c;--panel2:#162b24;--text:#f4f7f2;"
        "--muted:#92a098;--line:#28443a;--green:#47d16c;--cyan:#48d6d2;--bad:#ff6b6b}"
        "*{box-sizing:border-box}body{font:16px system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);"
        "color:var(--text);max-width:820px;margin:0 auto;padding:20px;line-height:1.45}"
        "h1{font-size:28px;margin:0 0 4px}h2{font-size:18px;margin:0 0 14px}.sub{color:var(--muted);margin-bottom:20px}"
        "section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:18px;margin:14px 0}"
        "label{display:block;color:var(--muted);font-size:13px;margin-top:12px}input,button{width:100%;font:inherit;padding:12px;"
        "margin-top:6px;border-radius:8px;border:1px solid var(--line);background:#091814;color:var(--text)}"
        "button{background:var(--green);color:#06100c;border:0;font-weight:700;cursor:pointer}.secondary{background:var(--panel2);"
        "color:var(--text);border:1px solid var(--line)}.danger{background:#4a1719;color:#ffdada;border:1px solid #773033}"
        ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.status{white-space:pre-wrap;color:var(--cyan);"
        "background:#07120f;border-radius:8px;padding:12px;min-height:96px}.hint{color:var(--muted);font-size:13px}"
        "@media(max-width:640px){.grid{grid-template-columns:1fr}}</style></head><body>"
        "<h1>FlightsAbove Setup</h1><div class=sub>Configure Wi-Fi, map center, radar range, and firmware updates.</div>"
        "<section><h2>Status</h2><pre id=status class=status>Loading...</pre>"
        "<button class=secondary onclick=refreshStatus()>Refresh</button></section>"
        "<section><h2>Wi-Fi</h2><form method=post action='" + basePath + "/save-wifi'>"
        "<label>Network name</label><input name=ssid autocomplete='off' required value='" + ssid + "'>"
        "<label>Password</label><input name=pass type=password autocomplete=new-password placeholder='" + passPlaceholder + "'>"
        "<button>Save Wi-Fi</button></form></section>"
        "<section><h2>Map Center</h2><form method=post action='" + basePath + "/save-receiver'>"
        "<div class=grid><div><label>Latitude</label><input name=lat type=number step=0.000001 min=-90 max=90 value='" + lat + "'></div>"
        "<div><label>Longitude</label><input name=lon type=number step=0.000001 min=-180 max=180 value='" + lon + "'></div></div>"
        "<label>Maximum radar range in miles</label><input name=range type=number min='" + std::to_string(cfg::kMinRadarRangeMiles) +
        "' max='" + std::to_string(cfg::kMaxRadarRangeMiles) + "' value='" + radarRange + "'>"
        "<label>Turn display off after minutes (0 disables)</label><input name=sleep type=number min=0 max=120 value='" + sleep + "'>"
        "<button>Save map settings</button></form><p class=hint>The map autosizes to current aircraft up to this maximum range.</p></section>"
        "<section><h2>Feeder</h2><form method=post action='" + basePath + "/save-feeder'>"
        "<label>Aircraft JSON URL</label><input name=url type=url inputmode=url required value='" + feederUrl + "'>"
        "<button>Save feeder</button></form><p class=hint>Use the local readsb/tar1090 aircraft JSON endpoint.</p></section>"
        "<section><h2>OTA Firmware Update</h2><input id=fw type=file accept='.bin,application/octet-stream' required>"
        "<button onclick=uploadFirmware()>Upload firmware</button><pre id=ota class=status></pre>"
        "<p class=hint>Upload the app binary from <code>build/flightsabove.bin</code>. "
        "The device reboots after a successful update.</p></section>"
        "<section><h2>Actions</h2><div class=grid>"
        "<form method=post action='" + basePath + "/restart'><button class=secondary>Restart</button></form>"
        "<form method=post action='" + basePath + "/factory-reset' onsubmit=\"return confirm('Erase saved Wi-Fi and receiver settings?')\">"
        "<button class=danger>Factory reset</button></form></div></section>"
        "<script>async function refreshStatus(){try{let r=await fetch('" + basePath +
        "/status');let j=await r.json();document.getElementById('status').textContent="
        "Object.entries(j).map(([k,v])=>k+': '+v).join('\\n')}catch(e){document.getElementById('status').textContent=e}}"
        "async function uploadFirmware(){let f=document.getElementById('fw').files[0];let o=document.getElementById('ota');"
        "if(!f){o.textContent='Choose a firmware .bin first';return}o.textContent='Uploading '+f.name+'...';"
        "let r=await fetch('" + basePath + "/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
        "o.textContent=await r.text();}"
        "refreshStatus();setInterval(refreshStatus,5000)</script></body></html>");
}

}  // namespace setup_portal
