# TRV-ZBT Valve Console

A browser dashboard for the SONOFF TRV-ZBT Gen 2 radiator valve. It currently has two modes:

- **GitHub Pages preview:** the public site is a simulation and does not control hardware.
- **Local control:** run this small Node.js service on the same computer as the browser. It reads and controls a Home Assistant `climate` entity, while keeping the Home Assistant token on that computer.

## Direct eWeLink cloud control: feasibility

SONOFF/CoolKit publishes an eWeLink cloud API with endpoints to list devices and send device status updates. A personal developer API option is documented, but access is limited to authorized device brands and types. The API documentation does not specify the TRV-ZBT's cloud setpoint parameter, and the TRV-ZBT gateway list does not include ZBBridge-PR2. The PR2 manual documents Matter bridging and eWeLink pairing, but not a direct developer API.

So direct cloud control is **not yet verified for this exact TRV + PR2 combination**. The dashboard cannot safely call the eWeLink API from its public GitHub Pages JavaScript: application credentials and account authorization would be exposed. A working cloud version would need a private backend to hold credentials, plus confirmation that eWeLink authorizes this TRV and the correct setpoint/status fields. Do not put eWeLink credentials or API tokens in this repository.

Before implementing that connector:

1. Apply for a personal developer application through the official eWeLink developer portal and check whether its authorized device list includes TRV-ZBT and ZBBridge-PR2/sub-devices.
2. Pair the TRV to the PR2 in eWeLink and confirm the cloud API returns the TRV as a controllable thing, with its current and target temperatures.
3. Confirm the documented API status-update call accepts a target-temperature command for this device and verify it physically changes the valve setpoint.
4. Only then add a server-side eWeLink connector. Keep the app credentials and OAuth tokens server-side; the GitHub Pages site must remain a preview unless deployed with a private backend.

Until those checks pass, this project does not claim direct eWeLink cloud control. Its existing local connector is still the Home Assistant prototype described below; it has not been changed to send unverified cloud commands.

The intended device path is **TRV-ZBT → ZBBridge-PR2 → Matter → Home Assistant → this dashboard**. The PR2 is a Zigbee/Matter bridge, not a browser API. Home Assistant must first expose the valve as a controllable climate entity. SONOFF's TRV-ZBT manual does not currently list the PR2 in its gateway list, so confirm the PR2 can pair this valve and expose its thermostat controls to Home Assistant before relying on the dashboard.

## Next steps: validate the cloud route

1. Set up the ZBBridge-PR2 in eWeLink, update its firmware, and pair the TRV-ZBT as a Zigbee sub-device. The TRV manual says to put it in `OFF` mode and hold its middle button for about 2 seconds to enter pairing mode.
2. Apply for an eWeLink personal developer app and confirm the authorization includes the TRV-ZBT device type. Ask SONOFF/CoolKit whether TRV-ZBT sub-devices behind the PR2 are supported by the cloud API and which status fields set the target temperature and mode.
3. With an authorized app, verify that the API can list the paired TRV, read its target/current temperature, and send a small setpoint change. Do this before enabling controls in the dashboard.
4. If those checks pass, add a server-side eWeLink connector and keep its app credentials/tokens outside Git. GitHub Pages alone cannot safely store those credentials.

There is no eWeLink cloud connector in the project yet because the device authorization and TRV command fields are unverified. The `server.js` connector is an earlier Home Assistant prototype; it is not needed for the proposed direct eWeLink route and should not be treated as proof that the PR2/TRV pairing works.

## Current dashboard behavior

The mobile dashboard now surfaces the Gen 2 feature set: three weekly plans, 7-day schedules with up to 12 changes per day, Auto/Manual/Off modes, boost and timer, external temperature, window and presence linkage, child lock, frost protection, temperature calibration/accuracy, adaptive control, valve opening limits, low-battery valve behavior, screen direction, valve calibration, diagnostics, and a history graph. The graph uses labeled illustrative data in preview mode and shows no history when connected to the current Home Assistant connector.

GitHub Pages is a visual preview only. Schedule and quick-action settings in the preview are saved in that browser and do not reach the physical valve. The legacy `server.js` connector reads a Home Assistant `climate` entity and sends only target-temperature and supported HVAC-mode changes. In local mode, the newly surfaced Gen 2 features are shown but disabled because that connector does not implement them. It does not connect directly to eWeLink or the PR2.

SONOFF's product page lists ZBBridge-U among its Matter bridges and its current TRV manual recommends ZBBridge-P/U, NSPanel Pro, iHost and several dongles; the PR2 is not explicitly named in those TRV-specific compatibility lists. SONOFF also states that full native Zigbee2MQTT support for TRV-ZBT requires Zigbee2MQTT 2.12.1 or later. Zigbee2MQTT documents the individual device controls and schedule fields. This confirms a path for a later feature-complete connector, but does not verify the PR2/eWeLink developer API path.

## Hardware references

- [TRV-ZBT user manual and pairing guide](https://support.sonoff.tech/trv-zbt-usermanual/)
- [TRV-ZBT product features and compatibility](https://sonoff.tech/products/sonoff-trv-gen2-zigbee-thermostatic-radiator-valve-trv-zbt)
- [Zigbee2MQTT TRV-ZBT supported controls](https://www.zigbee2mqtt.io/devices/TRV-ZBT.html)
- [ZBBridge-PR2 user manual and Matter setup](https://support.sonoff.tech/zb-bridge-pr2-usermanual/)
- [eWeLink cloud API documentation](https://github.com/CoolKit-Technologies/eWeLink-API)
- [eWeLink API access options and limits](https://github.com/CoolKit-Technologies/eWeLink-API/blob/main/en/Pricing.md)
- [eWeLink developer portal](https://dev.ewelink.cc/)

## Files

- `index.html` — dashboard UI. It runs in demo mode on GitHub Pages and connects to `/api` when served locally.
- `server.js` — legacy local Home Assistant connector and static file server; not an eWeLink connector.
- `.env.example` — configuration template for that legacy connector. Do not add eWeLink credentials here unless a future server-side connector is implemented.
