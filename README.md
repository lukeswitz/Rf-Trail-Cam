# Rf-Trail-Cam
Capturing RF signals in the Wild

RF Trail Cam is firmware for the LilyGO T-SIM7000G, an ESP32 board with a SIM7000G cellular and GPS modem, plus a Flask web server that collects what the board finds. The board scans for Wi-Fi networks, tags each new network with its GPS position, and uploads the networks to the server over the cellular network.

![alt text](trail_cam.jpg)

The web interface shows:

- The latest heartbeat from each device, with its MAC address, battery voltage, temperature and humidity.
- A map of where the networks were seen.
- A table of every network, with the fields WiGLE uses.

After you log in, you can delete the stored networks or upload the networks that have not been uploaded yet to WiGLE.

## How the device works

- It scans for Wi-Fi networks without pause and keeps up to 100 new networks in memory.
- Every 5 minutes, or sooner if those 100 slots fill up, it turns on the cellular radio, uploads the stored networks in one request, sends a heartbeat, and turns the cellular radio off again. If no new networks were found, it skips that upload.
- It also sends a heartbeat when it boots. The heartbeat carries the battery voltage and the temperature and humidity from a DHT22 sensor on GPIO 33. If the sensor read fails, temperature and humidity are sent as `-1`.
- It reboots every 6 hours.

## Server setup

The server reads its login and WiGLE credentials from environment variables.

| Variable | What it does |
|---|---|
| `ADMIN_USERNAME` | Username for the dashboard login. Login is refused if this or `ADMIN_PASSWORD` is not set. |
| `ADMIN_PASSWORD` | Password for the dashboard login. |
| `WIGLE_API_KEY` | Your WiGLE API name and API token joined by a colon, as `name:token`. WiGLE shows the API token on your account page. The **Upload to WiGLE** button uses it. |
| `FLASK_DEBUG` | Set it to `0` to turn off Flask debug mode. If it is not set, `python app.py` runs in debug mode. The Docker image sets it to `0`. |
| `TUNNEL_TOKEN` | Your Cloudflare Tunnel token. Only the `tunnel` service in Docker Compose uses it. |

### Run with Docker Compose

1. Create a file named `.env` next to `docker-compose.yml` and set the variables above in it, one `NAME=value` per line. Git ignores `.env`.
2. Run `docker compose up -d --build`.

The database is kept in the `db-data` Docker volume. Port 5000 is open only to the other containers in the project. The `tunnel` service publishes the site through Cloudflare Tunnel.

### Run directly

1. Run `pip install -r requirements.txt`.
2. Set `ADMIN_USERNAME`, `ADMIN_PASSWORD` and `WIGLE_API_KEY` in your shell.
3. Run `python app.py`.

The server creates its SQLite database in the `instance` folder next to `app.py` and listens on port 5000 on all network interfaces.

## Firmware setup

1. Install these Arduino libraries: TinyGSM, ArduinoHttpClient, DHT22 (by dvarrel) and Adafruit Unified Sensor.
2. In `sim7000G.ino`, set `apn` to your cellular provider's APN (the access point name your SIM needs to get online). Set `server` and `port` to your server's address and port.
3. Flash `sim7000G.ino` to the LilyGO T-SIM7000G.

The sketch compiles with version 3.3.10 of the esp32 Arduino core.
