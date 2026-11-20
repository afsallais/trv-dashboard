# SONOFF TRV-ZBT Cloud Dashboard

A mobile-first dashboard for the SONOFF TRV-ZBT Gen 2 and Zigbee sensors connected through a SONOFF ZBBridge-P. The intended deployment is Cloudflare Pages with a server-side Pages Function that talks to eWeLink Cloud. GitHub Pages remains a demo only because it cannot run this protected backend.

## Hardware support confirmed in SONOFF documentation

- **ZBBridge-P** is listed by SONOFF as a recommended gateway for TRV-ZBT, SNZB-02P, and SNZB-04PR2.
- **SNZB-02P** reports temperature and humidity. SONOFF says some features depend on the gateway.
- **SNZB-04PR2** reports open/closed and tamper state and can trigger eWeLink scenes.
- **TRV-ZBT Gen 2** supports Off/Frost, Auto, Manual, Boost, temporary timer, child lock, schedules, open-window detection, and remote temperature-source features through supported app/gateway functions.

The dashboard discovers account devices and reads their latest eWeLink status. It displays temperature, humidity, contact, tamper, and battery when those values are actually included in the API response. It enables TRV setpoint or mode commands only after you configure a verified field map for the exact model. No guessed write fields are enabled.

## Cloudflare Pages setup

1. Create a Cloudflare Pages project connected to this Git repository. Use the repository root as the output directory; there is no build command. Pages automatically serves `functions/api/` as server-side API routes.
2. Create a Cloudflare KV namespace and add it to the Pages project as a binding named **`EWELINK_KV`**.
3. Add these as **Pages environment variables/secrets** (Production and Preview as appropriate). Never commit them or put them in dashboard JavaScript:

   | Name | Type | Purpose |
   | --- | --- | --- |
   | `DASHBOARD_PASSWORD` | Secret | Password for the dashboard sign-in form. Use a unique long password. |
   | `EWELINK_APP_ID` | Secret | APPID assigned to your eWeLink developer application. |
   | `EWELINK_APP_SECRET` | Secret | App secret issued with that application. |
   | `EWELINK_ACCOUNT` | Secret | eWeLink account email or phone number. |
   | `EWELINK_PASSWORD` | Secret | eWeLink account password used by the documented login endpoint. |
   | `EWELINK_COUNTRY_CODE` | Variable | `+31` for your Netherlands account. |
   | `EWELINK_REGION` | Variable | `eu` for the Netherlands. |
   | `EWELINK_CONTROL_FIELDS` | Secret/variable | Optional JSON mapping of model-specific writable fields, only after verified against your device. Leave unset initially. |

4. Deploy. Open the Pages URL and sign in with `DASHBOARD_PASSWORD`. The API session is an HTTP-only, same-site cookie; failed passwords are rate-limited using the KV namespace. The eWeLink password and tokens stay in the Pages Function and KV, not in the browser or repository.
5. In the eWeLink app, set up the ZBBridge-P, update its firmware, then add each Zigbee device to that bridge. In the dashboard, use **Sensors → Refresh devices**. The bridge, TRV, and sensors must be visible in the eWeLink account associated with the API credentials.

### eWeLink developer access is required

The official API requires an assigned APPID/APPSECRET, account authorization for the requested device types, and account login. SONOFF/CoolKit's current developer guide says APPIDs are assigned after business cooperation. If the developer portal still does not let you create an application, the connector cannot authenticate yet. Ask eWeLink/CoolKit for an APPID and confirmation that **TRV-ZBT plus Zigbee sub-devices behind ZBBridge-P** are authorized for that APPID. An eWeLink account alone is not enough.

## Verify the device-specific commands after pairing

The eWeLink API exposes a device's status `params` and accepts a `params` object for control, but its generic endpoint documentation does not define the writable TRV-ZBT Gen 2 field names or accepted values. The hardware has not arrived/been paired yet, so those commands cannot be validated here.

After the devices arrive:

1. Pair the TRV-ZBT, SNZB-02P, and SNZB-04PR2 with the ZBBridge-P in the eWeLink app and confirm they work in the app first.
2. Sign into this dashboard and check **Sensors**. Confirm the live temperature/humidity, open/closed, tamper, and battery values match eWeLink. The cloud API must list the Zigbee sub-devices individually for the dashboard to show them.
3. Check the TRV's reported parameters with eWeLink developer support or an authorized API diagnostic. Identify the exact writable setpoint parameter and its unit/scale; identify the mode parameter and accepted values. Do not infer writable keys from display labels.
4. Only after confirming a small setpoint change with the actual valve, configure a model entry in `EWELINK_CONTROL_FIELDS`, for example:

   ```json
   {
     "TRV-ZBT": {
       "temperature": { "key": "REPLACE_WITH_VERIFIED_TARGET_FIELD", "scale": 1 },
       "currentTemperature": "REPLACE_WITH_VERIFIED_ROOM_TEMPERATURE_FIELD",
       "battery": "REPLACE_WITH_VERIFIED_BATTERY_FIELD",
       "mode": {
         "key": "REPLACE_WITH_VERIFIED_MODE_FIELD",
         "values": { "off": "VERIFIED_OFF_VALUE", "auto": "VERIFIED_AUTO_VALUE", "heat": "VERIFIED_MANUAL_VALUE" }
       }
     }
   }
   ```

   Replace every example value with the values confirmed for your API response; the placeholders above are not usable commands. `currentTemperature`, `battery`, and `valvePosition` are optional read-only field mappings. Set the JSON as a Cloudflare Pages variable/secret and redeploy. Then verify each mode and setpoint against the physical TRV screen and eWeLink app.
5. The dashboard currently leaves schedules, Boost, temporary timer, child lock, calibration, valve limits, and other advanced settings disabled in cloud mode. Enable those only after each exact field and behavior has been verified; they cannot be safely invented from the generic cloud API docs.

## Link the extra sensors to the TRV

The dashboard can display readings and states that the API returns. A displayed SNZB-02P value does **not** automatically become the TRV's thermostat input, and a displayed open window does **not** itself stop heating. In eWeLink, create the supported sensor/valve linkage or smart scene and verify it operates with the phone/dashboard closed. SONOFF's TRV manual describes a linked door/window sensor as a trigger for open-window detection; configure the supported linkage in the eWeLink app. This allows the gateway/app automation to perform the Zigbee action independently of the dashboard.

The API supports cloud control and device polling, not a direct browser-to-Zigbee connection. This design uses Cloudflare Pages Functions as the always-available cloud connector; it requires no home PC or Raspberry Pi. Zigbee actions still require the powered ZBBridge-P and eWeLink cloud/account availability.

## API and deployment notes

- Cloud API base for Europe: `https://eu-apia.coolkit.cc`.
- The backend reads the current device list and the project devices' status at a 60-second dashboard refresh. It spaces individual status requests to respect eWeLink's documented minimum call interval and polls at most 40 device statuses per refresh. Keep polling at this interval.
- The public GitHub repository must never contain eWeLink credentials, KV contents, dashboard password, or session tokens. A private GitHub repository does not replace server-side secrets.
- Cloudflare Pages deployments use the `functions/api/[[path]].js` handler. `server.js` is retained only as the earlier Home Assistant local prototype and is not used by the Cloudflare deployment.
- History, Boost countdown, weather estimate, and schedule UI have distinct preview behavior. The generic eWeLink API does not provide the TRV's six-month history or confirm advanced TRV command semantics through the documentation used here; no preview values should be mistaken for device history or a sent command.

## References

- [TRV-ZBT Gen 2 user manual](https://support.sonoff.tech/trv-zbt-usermanual/)
- [SNZB-02P user manual](https://support.sonoff.tech/snzb-02p-usermanual/)
- [SNZB-04PR2 user manual](https://support.sonoff.tech/snzb-04pr2-usermanual/)
- [ZBBridge-P product page](https://sonoff.tech/en-us/products/sonoff-zigbee-bridge-pro)
- [eWeLink API v2 reference](https://github.com/CoolKit-Technologies/eWeLink-API/blob/main/en/APICenterV2.md)
- [eWeLink API developer guide](https://github.com/CoolKit-Technologies/eWeLink-API/blob/main/en/DeveloperGuideV2.md)

## Project files

- `index.html` — dashboard UI, demo mode on GitHub Pages and Cloudflare-connected status/controls when deployed with Functions.
- `functions/api/[[path]].js` — protected Cloudflare Pages API for session login, eWeLink authentication, device discovery/status, and verified-field TRV writes.
- `server.js` — legacy local Home Assistant prototype.
