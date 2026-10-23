# Rf-Trail-Cam
Capturing RF signals in the Wild

RF Trail Cam is firmware for the LilyGO T-SIM7000G, an ESP32 board with a SIM7000G cellular and GPS modem, plus a Flask web server that collects what the board finds. The board scans for Wi-Fi networks, tags each new network with its GPS position, and uploads the networks to the server over the cellular network.

![alt text](trail_cam.jpg)

After you log in, the web interface shows:

- The latest heartbeat from each device, with its MAC address, battery voltage, temperature and humidity.
- A map of where the networks were seen.
- A table of every network, with the fields WiGLE uses.

From there you can delete the stored networks or upload the networks that have not been uploaded yet to WiGLE. Visitors who are not logged in see only the login button.

## How the device works

- It scans for Wi-Fi networks without pause and keeps up to 100 new networks in memory.
- Every 5 minutes, or sooner if those 100 slots fill up, it turns on the cellular radio, uploads the stored networks in one request, sends a heartbeat, and turns the cellular radio off again. If no new networks were found, it skips that upload.
- It also sends a heartbeat when it boots. The heartbeat carries the battery voltage and the temperature and humidity from a DHT22 sensor on GPIO 33. If the sensor read fails, temperature and humidity are sent as `-1`.
- It reboots every 6 hours.

## Server setup

The server reads its credentials from environment variables.

| Variable | What it does |
|---|---|
| `ADMIN_USERNAME` | Username for the dashboard login. Login is refused if this or `ADMIN_PASSWORD` is not set. |
| `ADMIN_PASSWORD` | Password for the dashboard login. |
| `DEVICE_TOKEN` | A secret you choose that each device sends with its uploads. It must match `deviceToken` in the firmware. The server rejects all device uploads if it is not set. |
| `WIGLE_API_KEY` | Your WiGLE API name and API token joined by a colon, as `name:token`. WiGLE shows the API token on your account page. The **Upload to WiGLE** button uses it. |
| `FLASK_DEBUG` | Set it to `0` to turn off Flask debug mode when you run `python app.py`. If it is not set, `python app.py` runs in debug mode. The Docker image does not use it. |
| `TUNNEL_TOKEN` | Your Cloudflare Tunnel token. Only the `tunnel` service in Docker Compose uses it. |

Use a long random value for `ADMIN_PASSWORD` and `DEVICE_TOKEN`. One way to make one is `python3 -c "import secrets; print(secrets.token_urlsafe(32))"`.

### Run with Docker Compose

1. Create a file named `.env` next to `docker-compose.yml` and set the variables above in it, one `NAME=value` per line. Git ignores `.env`.
2. Run `docker compose up -d --build`.

The container runs the app with gunicorn as an unprivileged user. The database is kept in the `db-data` Docker volume. Port 5000 is open only to the other containers in the project. The `tunnel` service publishes the site through Cloudflare Tunnel.

If your `db-data` volume was created by an earlier image that ran as root, the app cannot write to it. Run `docker compose run --rm --user root web chown -R app:app /app/instance` once to fix the ownership.

### Run directly

1. Run `pip install -r requirements.txt`.
2. Set `ADMIN_USERNAME`, `ADMIN_PASSWORD`, `DEVICE_TOKEN` and `WIGLE_API_KEY` in your shell.
3. Run `python app.py`.

The server creates its SQLite database in the `instance` folder next to `app.py` and listens on port 5000 on all network interfaces.

## Firmware setup

1. Install these Arduino libraries: TinyGSM, ArduinoHttpClient, DHT22 (by dvarrel) and Adafruit Unified Sensor.
2. In `sim7000G.ino`, set `apn` to your cellular provider's APN (the access point name your SIM needs to get online). Set `server` and `port` to your server's address and port, and set `deviceToken` to the same value as `DEVICE_TOKEN` on the server.
3. Flash `sim7000G.ino` to the LilyGO T-SIM7000G.

The sketch compiles with version 3.3.10 of the esp32 Arduino core.
