## I2cBus: the device I2C bus

`I2cBus` (one instance, `i2cBus`) is the firmware's only way onto the I2C
bus. It sits directly on the IDF I2C master driver (`driver/i2c_master.h`)
and offers what the chip drivers need: begin the bus on two pins, end it,
probe an address, write registers, read registers. `main.cpp` begins it on
`DeviceConfig::i2cSdaPin/i2cSclPin`; the charger (`src/power/Bq25628`), the
IMU (`src/fleet/Imu.h`), the codec (`src/sound/Es8311Driver`) and the
expander below all go through it.

The bus runs at 100 kHz with the pins' internal pull-ups on and a 50 ms
timeout per transaction. Each call is one transaction under the bus's lock,
so the main loop and the sound player task share the bus.

## TCA9555: register access through I2cBus

`Tca9555Expander` drives single pins of a TCA9555 16-bit I2C GPIO expander
through its registers (TI datasheet SCPS200E): input port 0x00/0x01, output
port 0x02/0x03, configuration 0x06/0x07.

It changes one pin at a time: `beginOutput()`/`beginInput()`/`write()` read
the pin's register, change that pin's bit and write the register back. Boards
multiplex several unrelated peripherals across the same expander (e.g. LCD
reset, buttons, camera power, alongside a speaker-amp enable line), and every
other pin keeps its direction and level.

This directory holds generic hardware-adjacent building blocks (the I2C bus
and the expander) that aren't specific to any one feature (e.g. `src/sound/`)
so they can be reused elsewhere without pulling in unrelated code.
