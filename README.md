# esp32-freertos-dual-core-sensor-tasks — run sensor sampling and WiFi publishing as separate FreeRTOS tasks pinned to each core

IoT example by [Fluidwire](https://fluidwire.com/).

An ESP32 sketch that splits a sensor node into two FreeRTOS tasks on different cores. A sensor task on core 1 samples an analog input at a rock-steady 10 Hz with `vTaskDelayUntil()` and drops each reading into a queue. A network task on core 0 owns WiFi and MQTT, drains the queue, and publishes a 10 second summary (average, min, max) as JSON. A mutex-protected stats struct lets the Arduino `loop()` print a status line without racing either task. Pull the router's plug and watch the sampler keep its timing while the network task blocks and reconnects — the pattern most thesis prototypes need once "read sensor, then publish" starts missing samples.

## Required hardware

- 1x ESP32 dev board (dual-core: ESP32, not ESP32-C3/C6/S2 which are single-core)
- Any analog sensor, or a 10k potentiometer for testing
- WiFi and an MQTT broker (public `broker.hivemq.com` works for testing)

## Required libraries

- **PubSubClient** by Nick O'Leary (Library Manager)
- `WiFi`, FreeRTOS and the ADC driver are built into the ESP32 Arduino core — nothing else to install

## Wiring

| Sensor / pot | ESP32 |
|--------------|-------|
| Signal / wiper | GPIO34 (ADC1_CH6) |
| VCC | 3V3 |
| GND | GND |

Stay on ADC1 pins (GPIO32–39): ADC2 pins stop working once WiFi is active. GPIO34–39 are input-only, which is exactly what you want here.

## Setup

1. Copy `config.h.example` to `config.h` and fill in WiFi and broker details.
2. Flash and open Serial Monitor at **115200 baud**.
3. Subscribe from a PC:

```bash
mosquitto_sub -h broker.hivemq.com -t 'fluidwire/demo/rtos/dualcore' -v
```

4. Turn the pot (or breathe on the sensor) and watch `avg_v`/`min_v`/`max_v` move. Then disconnect WiFi for 15 seconds: `samples` keeps climbing, `dropped` stays 0 while the 200-slot queue absorbs the gap, and the next publish carries the backlog.

## How the tasks are split

| Task | Core | Priority | Job |
|------|------|----------|-----|
| `sensorTask` | 1 | 2 | Sample GPIO34 every 100 ms, push `Sample` into the queue, update jitter/drop counters |
| `networkTask` | 0 | 1 | WiFi + MQTT, drain queue, aggregate window, publish JSON |
| `loop()` (loopTask) | 1 | 1 | Print `[status]` every 5 s from the mutex-protected `SharedStats` |

- **Queue** (`xQueueCreate`): one-way hand-off from producer to consumer. `xQueueSend(..., 0)` never blocks the sampler; a full queue increments `dropped` instead.
- **Mutex** (`xSemaphoreCreateMutex`): guards `SharedStats`, which three tasks read or write. The `withStats()` helper keeps every access inside take/give.
- **`vTaskDelayUntil()`**: sleeps relative to the previous wake time, so sample timing does not drift even when the body takes longer than usual. `max_jitter_us` shows how well it holds.

## Expected serial output

```
[main] setup on core 1
[sensor] running on core 1
[net] running on core 0
[wifi] connecting to your-wifi-ssid
.....
[wifi] IP: 192.168.1.50
[mqtt] connecting... ok
[status] samples=50 dropped=0 maxJitter=41us last=1.648V queued=3 published=0 mqtt=up stack(sensor/net)=2868/5212
[mqtt] published {"avg_v":1.648,"min_v":1.612,"max_v":1.690,"samples":100,"dropped":0,"max_jitter_us":41,"seq":99,"heap":231204,"uptime_s":10}
```

## Troubleshooting

- **`xPortGetCoreID` shows both tasks on core 0:** you are on a single-core variant (C3/C6/S2). The sketch still runs, but pinning has no effect.
- **`dropped` keeps growing:** the network task is stuck longer than the queue can cover. Raise `QUEUE_LENGTH`, lower the sample rate, or persist to NVS/SD during long outages.
- **`maxJitter` in the milliseconds:** something else on core 1 is hogging the CPU with higher priority, or Serial output in `loop()` is too chatty. Keep heavy logging off the sampler's core.
- **Readings stuck at 0 or 4095:** the pin is on ADC2 (GPIO0/2/4/12–15/25–27) which WiFi disables, or the sensor exceeds 3.3 V. Move to GPIO32–39 and add a divider.
- **Guru Meditation / stack overflow:** increase the stack size in `xTaskCreatePinnedToCore()`. The `[status]` line prints the free high-water mark for both tasks so you can size them properly.
- **Nothing publishes:** check `mqtt.state()` codes in the serial log; `-2` is a network error, `5` is bad credentials.

## Security note

Never commit `config.h`; it is git-ignored. Public brokers are readable by anyone, so use a private broker with TLS and credentials for real deployments. See the Fluidwire `esp32-mqtt-tls-secure` sample for a TLS version of the MQTT client.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — happy to help.
