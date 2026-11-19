# TRV-ZBT Valve Console

A browser dashboard for the SONOFF TRV-ZBT Gen 2 radiator valve. It has two modes:

- **GitHub Pages preview:** the public site is a simulation and does not control hardware.
- **Local control:** run this small Node.js service on the same computer as the browser. It reads and controls a Home Assistant `climate` entity, while keeping the Home Assistant token on that computer.

The intended device path is **TRV-ZBT → ZBBridge-PR2 → Matter → Home Assistant → this dashboard**. The PR2 is a Zigbee/Matter bridge, not a browser API. Home Assistant must first expose the valve as a controllable climate entity. SONOFF's TRV-ZBT manual does not currently list the PR2 in its gateway list, so confirm the PR2 can pair this valve and expose its thermostat controls to Home Assistant before relying on the dashboard.

## Next steps

### 1. Pair the valve to the PR2

1. Set up the ZBBridge-PR2 in the eWeLink app and update its firmware.
2. Add the TRV-ZBT as a Zigbee sub-device in eWeLink. Put the valve in `OFF` mode, then hold its middle button for about 2 seconds until the pairing indicator flashes. Complete pairing from the app.
3. In the PR2's **Matter** settings, start Matter setup and enable synchronization of bridged sub-devices.

### 2. Add the bridge to Home Assistant

1. Install and start Home Assistant's Matter Server, then set up the Matter integration.
2. Commission the PR2 into Home Assistant using its Matter setup code/QR code. Follow the PR2 manual's Matter setup flow.
3. Confirm that the TRV appears in Home Assistant as a `climate` entity and that its state exposes `temperature`, `current_temperature`, and `hvac_modes`. Set a harmless test target in Home Assistant to confirm the valve responds.

If the PR2 does not expose the TRV-ZBT as a controllable `climate` entity, stop here: this dashboard cannot control it through that hub as configured. The PR2 product documentation describes Matter bridging, but that alone does not guarantee every Zigbee feature of this valve is exposed to every Matter controller. Do not put eWeLink or Home Assistant credentials in the dashboard or GitHub.

### 3. Run the local dashboard connector

Requirements: Node.js 18 or newer, Home Assistant reachable from this computer, and a Home Assistant long-lived access token.

1. In Home Assistant, create a long-lived access token from your user profile.
2. Copy `.env.example` to `.env.local` and fill in:
   - `HA_URL`: your Home Assistant base URL, for example `http://homeassistant.local:8123`.
   - `HA_TOKEN`: the long-lived access token. Keep this file private; it is ignored by Git.
   - `HA_ENTITY_ID`: the valve's `climate...` entity ID from Home Assistant.
3. From this folder, run `node server.js`.
4. Open [http://127.0.0.1:8787](http://127.0.0.1:8787). The header should show **Connected** with the selected device name. Use the target dial and mode buttons to control the valve.

The service binds to `127.0.0.1` only. The Home Assistant token stays in `.env.local` and is used by the local service, not by browser JavaScript. Do not deploy this connector or `.env.local` to GitHub Pages. The GitHub Pages site remains a non-controlling preview because it cannot securely hold a private Home Assistant token or reach a local-only service.

## Supported controls

The local connector reads the climate entity state, current and target temperatures, available HVAC modes, and heating action. It sends temperature and mode changes through Home Assistant's `climate.set_temperature` and `climate.set_hvac_mode` services. Mode buttons are enabled only when Home Assistant reports that mode as supported. The remaining simulated settings in the preview are not sent to the physical valve.

## Hardware references

- [TRV-ZBT Gen 2 product page](https://sonoff.tech/products/sonoff-trv-gen2-zigbee-thermostatic-radiator-valve-trv-zbt)
- [TRV-ZBT user manual and pairing guide](https://support.sonoff.tech/trv-zbt-usermanual/)
- [ZBBridge-PR2 user manual and Matter setup](https://support.sonoff.tech/zb-bridge-pr2-usermanual/)
- [Home Assistant Matter integration](https://www.home-assistant.io/integrations/matter/)
- [Home Assistant REST API](https://developers.home-assistant.io/docs/api/rest/)

## Files

- `index.html` — dashboard UI. It runs in demo mode on GitHub Pages and connects to `/api` when served locally.
- `server.js` — local Home Assistant connector and static file server.
- `.env.example` — configuration template. Copy it to `.env.local` and add your private details.
