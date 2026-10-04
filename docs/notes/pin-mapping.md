# How the pin map was found

The pin map in the README came from three sources, in this order of trust: (1) disassembly of the
shipped firmware's drivers (PIR, battery, SD card, camera power), (2) measurements with the
firmware's own `pin` commands and a camera/microphone as witness, (3) ADC readings. This note keeps
the method and the raw observations.

## The `pin` family

The SDK's `gpio`/`adc_test` commands print through `CLI_LOGI`, which is compiled out in this build
(`CFG_LOG_LEVEL=2`), so the firmware adds its own `os_printf`-based commands on UART0:

| Command | What it does |
|---|---|
| `pin all` | one line with the level of all 56 GPIOs (`PINS 0101...`); pins read `1` until input-enabled |
| `pin mux` | per-pin config byte from the AON GPIO registers (bit 2 in-en, bit 3 out-en, bit 4/5 pull, bit 6 peripheral function) |
| `pin dump` | raw register dump |
| `pin get N` | read one pin (+ its config byte) |
| `pin in N [up\|down\|none]` | reclaim N from any peripheral function, make it a true input (output stage off) with that pull, print the level |
| `pin hiz N` | reclaim N and leave it high-impedance (no input, no output, no pull) |
| `pin out N 0\|1` | reclaim N and drive it |
| `pin raw N HEX` | write the config byte directly |
| `pin blink N [count]` | reclaim N, toggle at 300 ms (LED hunting), leave it low |
| `pin watch SECONDS` | poll all pins every 5 ms on the device and print each edge with a timestamp |
| `adc CHANNEL` | one calibrated SARADC reading (raw + mV) |

`pin in/out/blink` refuse GPIO 10/11 (the console). Since v0.24.2 `pin get/in/out/hiz/raw` and `pin dump N` also
return their line over TCP (`tools/dbcmd.py`), and `pin cfg N` decodes one pin: the full AON config word plus
the system-controller function-select nibble (`0x44010000 + 0x30*4 + N/8*4`, 4 bits per pin = which column of
the pin's `gpio_map.h` row is routed, e.g. GPIO 2 `sel 1` = `SDIO_HOST_CLK`). Note that bit 3 of the config word
is an output *disable* (active low) on the BK7258: `pin cfg` prints `out_en` already inverted. Important: at boot the SDK's default pin map
leaves almost every pin owned by a peripheral function (`pin mux` shows bit 6 set), and reads/pulls
on such pins are meaningless. Firmware v0.4.0+ reclaims a pin with `gpio_dev_unmap()` +
`bk_gpio_set_config(..., GPIO_SECOND_FUNC_DISABLE)`; results from before that are noise, except the
ADC readings.

Host-side helpers in `tools/`: `pinmap_session.py` (mux / pull classification / scan / watch / ADC
sweep, writes a session log), `led_hunt.py` (interactive: blinks one candidate pin at a time and
asks what you saw), `led_by_camera.py` (uses a webcam as the eyes), `led_by_light.py` (tried the
board's own ADCs as the eyes: nothing on this board reacts), `pinscan.py` (older, one pin per
command).

## Observations

| GPIO | Observation | Conclusion |
|---|---|---|
| 7 (ADC9) | 3.07 V on the ADC, stable across sessions; later bimodal 2994/3069 mV under load | regulated rail / supply sense, reported as `mv` |
| 8 (ADC10) | 0.30 V on the ADC, stable; boots as plain GPIO | unknown (light sensor or PIR analog guess did not hold: the light sensor is ADC14) |
| 9 | active-high enable: high = light sensor alive (303 mV), low/hi-Z = dead (24 mV) | sensor / LED rail enable (held low through deep sleep) |
| 5 | boots as plain GPIO like 9, no effect on the sensor rail; cut with the camera rails on close | camera-related rail, exact function unverified |
| 24 | idle low; 8 isolated high pulses of 2-4 s in 8 min with nobody at the device, then confirmed by waving | PIR output, active high, 2-4 s hold |
| 53 | active-low, about 0.3 s presses; 46 edges / 13 `ring` posts in a 45 s press test. The same line keys the on-board 433 MHz chime transmitter (the chimes pair to the button, no firmware involved); the stock firmware's boot pin map has it as plain input + pull-up, the SDK's as an LCD_B2 output, see below | doorbell button (confirmed) + chime TX key line |
| 51 | stock firmware: a second key (long-press handler), configured input + pull-up like 53; nothing on this board reacts to it | second key per stock, unmapped here; parked input + pull-up since 0.26.2 |
| 44 | high = white LED on (big flare on a camera), low/hi-Z = off; pure white, no IR | white fill LED, active high |
| 45 (ADC11) | high = the four blue LEDs around the button light up. Not infrared: the IR LED sits above the camera lens and is still unmapped; an earlier "kills the white LED" reading was a coincidence | blue button ring, active high |
| 46 | GPIO sweep with a microphone listening: 1 kHz tone scored 108 with 46 high vs < 1 for the other pins; floating it leaks a faint output | speaker amplifier enable, active high |
| 12 (ADC14) | about 700 mV lit room, about 2100 mV covered, powered from GPIO 28 | light sensor, higher = darker |
| 13 (ADC15) | x2 divider, 3.9 V range; the shipped firmware's SDK glue also drives it high as the SD "LDO enable" (`bk_gpio_ctrl_external_ldo(GPIO_CTRL_LDO_MODULE_SDIO, GPIO_13, HIGH)`, the SDK's default `SDCARD_LDO_CTRL_GPIO`) before every card init | battery voltage (shipped firmware). **Not** the SD power: with the camera supply (GPIO 28) up the card answers whether 13 is released, high or low; with 28 down it never answers (2026-10-04, sdcard.md) |
| 0, 1 | I2C1 SCL/SDA (SDK default pins; the shipped firmware uses them too) | camera I2C |
| 28 | camera power enable, active high (shipped firmware `.data`: pin 28, level 1); also the microSD slot's supply (card silent with 28 low, answers with 28 high, 2026-10-04) and the LDR divider | camera power, SD card power |
| 52 | shipped firmware pulses it low, 100 ms high, low before probing | camera PWDN/RESET |
| 54, 55 | shipped firmware pulses 55 high 300 ms for night, 54 high 300 ms for day (plus sensor B/W registers) | IR-cut filter coil, unused (the white night light replaces it) |
| 50, 25, 26 | shipped firmware PIR driver | PIR enable, sensitivity bit 0/1 |
| 15, 19 | shipped firmware battery driver; 15 is externally low on the bench | charging / USB present, polarity unconfirmed |
| 6 | shipped firmware SD driver: configured as INPUT with pull-up and read, 0 = card inserted (2026-10-04 disassembly; reads 0 with a card in, 1 without). Earlier notes called it "SD power / camera LDO enable": wrong, it was only ever read | SD card detect (input, active low) |
| 2, 3, 4 | SDK default SDIO group 1 | SD CLK/CMD/DAT0 |
| 10, 11 | UART0 | console, never touch |

Still unmapped: the IR illuminator above the lens, the charger IC's other status lines, and
whether there is a power-hold line at all (the board stays up on battery without one being driven).

## The boot-time pin map and the ringing chimes (fw 0.26.2, SDK patch 0010)

Symptom (2026-10-04, device on the wall): all three 433 MHz chimes paired to the doorbell rang once
at every reset of the SoC, i.e. every deep-sleep wake (RTC, button, PIR) and every warm `reboot`.
Toggling GPIO 9/28/5/50/46/44/45 from the running firmware never rang them, so it was not a rail.

Cause: the SDK's `GPIO_DEFAULT_DEV_CONFIG` (`middleware/soc/bk7258/soc/gpio_map.h`) is written to
the AON GPIO registers by `bk_gpio_driver_init()` on CPU0 at every reset, before `user_app_main()`.
It is the map of Beken's LCD dev kit and makes **GPIO 53 = LCD_B2**, a peripheral output: with the
LCD block idle the pin sits low, which is a button press on the TX encoder's key input, until
`inputs_init()` reclaims the pin as input + pull-up a few hundred ms later. The stock firmware's own
table (disassembled the same day) has 53 as `FUNC_DISABLE / INVALID / INPUT_ENABLE / PULL_UP_EN /
LOW_POWER_KEEP_INPUT_STATUS / CAPACITY_3`; its other differences from the SDK are 6/7 (hi-Z, not
I2S1), 27-39 (JPEG/I2S functions off until the camera driver maps them, 38 INVALID) and 25/26/50
(low-power keep). Rows 0-5, 8-24, 40-49, 51, 52, 54, 55 are identical. The stock cp1/cp2 apply no
table and the stock bootloader only touches the UART pins, which matches our build (only CPU0 has
`CONFIG_GPIO_DEFAULT_SET_SUPPORT=y`). Patch 0010 makes the SDK table the stock one at those rows
(details: [sdk-patches.md](sdk-patches.md)); `pin cfg 53` after a boot must now show input +
pull-up with no function, and `pin cfg 27`/`28` no function until the camera opens.

If the chimes still ring after 0.26.2, the remaining suspects in order: the bootloader's UART pin
writes (the only pins it touches), GPIO 6/7/28 behaving differently as hi-Z than as idle peripheral
outputs (a pull on 28 would show in `pin cfg 28` at boot), and the `pwr hold9` experiment
(`docs/notes/power.md`), which keeps the sensor rail up through the sleep.

