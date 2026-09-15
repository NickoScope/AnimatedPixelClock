#pragma once
// The Waveshare ESP32-S3-RGB-Matrix's I2C bus, begun once for every module on
// it. SDA GPIO47 and SCL GPIO48 carry the SHTC3, the ES7210 microphone ADC, the
// ES8311 codec, the PCF85063 RTC and the QMI8658 IMU (Waveshare's schematic;
// its BSP, example/idf_v5.5.2/components/bsp/esp32_s3_matrix/include/bsp/
// config.h: BSP_I2C_SDA GPIO_NUM_47, BSP_I2C_SCL GPIO_NUM_48). main.cpp calls
// boardI2cBegin() early in setup(), before any module; a module may call it
// again before its first transaction, which then costs nothing.
//
// Why one owner. TwoWire::begin() on a bus that is already running logs a
// warning and returns true without touching it (arduino-esp32 2.0.17,
// libraries/Wire/src/Wire.cpp:300-303): whichever module began it first chose
// the speed for all of them, and setTimeOut() is one value for the whole bus.
//
// Speed: 100 kHz. IO47/IO48 are 1.8 V on this module and reach the 3.3 V parts
// through the NDC7002N level shifter M2, with 4.7 k pull-ups on both sides
// (schematic). Standard mode is the conservative choice through it, and ample
// for register writes and a sensor reading. A choice, not a measurement.
//
// Timeout. BOARD_I2C_TIMEOUT_MS, Wire's own default (Wire.cpp:48), does not
// bound a transaction. ESP-IDF v4.4.7, the IDF of arduino-esp32 2.0.17
// (tools/sdk/versions.txt), waits for the driver's next event for at least
// I2C_CMD_ALIVE_INTERVAL_TICK, 1000 ms (components/driver/i2c.c:68 and
// :1480-1489). A NACK is such an event and comes back at once (:1502-1508). A
// line held low sends none, so the call ends with ESP_ERR_TIMEOUT a second
// later (:1516-1522), whatever the timeout says. The controller's own timeout
// is set to its largest value, I2C_LL_MAX_TIMEOUT (esp32-hal-i2c.c:97;
// hal/esp32s3/include/hal/i2c_ll.h:91). A caller that must not stall looks at
// boardI2cLinesHigh() before a transaction and times the transaction
// (src/climate/climate_reader.h does both).
//
// Two tasks on two cores. CONFIG_DISABLE_HAL_LOCKS is not set
// (tools/sdk/esp32s3/sdkconfig:251), so Wire's mutex is compiled in:
//  - a write is atomic: beginTransmission() takes the mutex, waiting forever
//    (Wire.cpp:421), and endTransmission() gives it back after the bus write
//    (Wire.cpp:453-456);
//  - a read's transaction is atomic: requestFrom() takes the mutex (Wire.cpp:504)
//    and gives it back once the bytes are in (Wire.cpp:518). A repeated-start
//    read, endTransmission(false) then requestFrom(), holds it from the one to
//    the other within the same task (Wire.cpp:458-463, :485-488);
//  - beneath Wire, i2cWrite() and i2cRead() take the HAL's bus mutex too
//    (cores/esp32/esp32-hal-i2c.c:142, :196, :225), and the IDF its cmd_mux
//    (i2c.c:1443, :1529).
// So two tasks' transactions never interleave on the wire. Two things are not
// covered:
//  1. The bytes a read received. They stay in Wire's one rxBuffer after
//     requestFrom() has released the mutex, and read() and available() take no
//     lock (Wire.cpp:547-564). If another task's requestFrom() runs between this
//     task's requestFrom() and its read()s, it overwrites rxBuffer, rxIndex and
//     rxLength, and this task reads the other's bytes. Reads from two tasks need
//     one lock around requestFrom() and its read()s, or all reads in one task.
//  2. Waiting. The mutex is taken with portMAX_DELAY, so a transaction stuck for
//     a second (above) holds every other task's next Wire call for that second.
//     And an endTransmission(false) never followed by requestFrom() keeps the
//     mutex until the same task's next beginTransmission() (Wire.cpp:415-419).

#if defined(BOARD_WAVESHARE_RGB_MATRIX)

#include <stdint.h>

#define BOARD_I2C_SDA        47
#define BOARD_I2C_SCL        48
#define BOARD_I2C_HZ         100000UL
#define BOARD_I2C_TIMEOUT_MS 50

// Begins Wire on the board's pins at BOARD_I2C_HZ, once; later calls return at once.
void boardI2cBegin();

// Wire is running on the board's pins.
bool boardI2cReady();

// SDA and SCL both read high: nothing holds the bus. The pins are input-output
// open-drain under the driver (i2c.c:892, :905), and the IDF reads a line this
// way itself (i2c.c:596).
bool boardI2cLinesHigh();

#endif  // BOARD_WAVESHARE_RGB_MATRIX
