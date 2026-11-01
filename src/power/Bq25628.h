#pragma once
#include <Arduino.h>
#include <Wire.h>

// Register-level driver for the TI BQ25628 I2C 1-cell buck charger with NVDC
// power path, boost OTG on PMID and a 12-bit ADC.
//
// Source for every register, field and value below: TI datasheet SLUSEG4C
// ("BQ25628 / BQ25629 ...", Rev. C, Dec 2022 - Feb 2025), section 8.6.2
// "BQ25628 Registers" (Table 8-7 and the per-register field tables cited at
// each constant) plus the functional description in section 8.3/8.4.
//
// 16-bit registers are little endian and must be written in one 16-bit
// transaction starting at the low byte (§8.6.1); multi-byte reads and writes
// may cross register boundaries (§8.5.1.7).
//
// No interrupts (the INT pin is not used): the caller polls. Assumes the I2C
// bus (Wire) is already begun.
class Bq25628 {
   public:
    // §8.6 "I2C Device Address: 0x6A".
    static constexpr uint8_t I2C_ADDRESS = 0x6A;

    // ── Register addresses (Table 8-7) ──────────────────────────────────────
    static constexpr uint8_t REG_ICHG = 0x02;            // 16-bit, Table 8-9
    static constexpr uint8_t REG_VREG = 0x04;            // 16-bit, Table 8-10
    static constexpr uint8_t REG_IINDPM = 0x06;          // 16-bit, Table 8-11
    static constexpr uint8_t REG_VOTG = 0x0C;            // 16-bit, Table 8-13
    static constexpr uint8_t REG_CHG_TMR_CTRL = 0x15;    // Table 8-18
    static constexpr uint8_t REG_CHG_CTRL_0 = 0x16;      // Table 8-19
    static constexpr uint8_t REG_CHG_CTRL_1 = 0x17;      // Table 8-20
    static constexpr uint8_t REG_CHG_CTRL_2 = 0x18;      // Table 8-21
    static constexpr uint8_t REG_CHG_CTRL_3 = 0x19;      // Table 8-22
    static constexpr uint8_t REG_NTC_CTRL_0 = 0x1A;      // Table 8-23
    static constexpr uint8_t REG_CHG_STAT_0 = 0x1D;      // Table 8-26
    static constexpr uint8_t REG_CHG_STAT_1 = 0x1E;      // Table 8-27
    static constexpr uint8_t REG_FAULT_STAT_0 = 0x1F;    // Table 8-28
    static constexpr uint8_t REG_ADC_CTRL = 0x26;        // Table 8-35
    static constexpr uint8_t REG_ADC_FUNC_DIS_0 = 0x27;  // Table 8-36
    static constexpr uint8_t REG_IBUS_ADC = 0x28;        // 16-bit, Table 8-37
    static constexpr uint8_t REG_IBAT_ADC = 0x2A;        // 16-bit, Table 8-38
    static constexpr uint8_t REG_VBUS_ADC = 0x2C;        // 16-bit, Table 8-39
    static constexpr uint8_t REG_VPMID_ADC = 0x2E;       // 16-bit, Table 8-40
    static constexpr uint8_t REG_VBAT_ADC = 0x30;        // 16-bit, Table 8-41
    static constexpr uint8_t REG_VSYS_ADC = 0x32;        // 16-bit, Table 8-42
    static constexpr uint8_t REG_TS_ADC = 0x34;          // 16-bit, Table 8-43
    static constexpr uint8_t REG_TDIE_ADC = 0x36;        // 16-bit, Table 8-44
    static constexpr uint8_t REG_PART_INFO = 0x38;       // Table 8-45

    // ── REG0x02 ICHG, bits 10:5, 40 mA/LSB, 40-2000 mA, POR 320 mA. Watchdog
    // expiry halves it (Table 8-9).
    static constexpr uint16_t ICHG_SHIFT = 5;
    static constexpr uint16_t ICHG_MASK = 0x3F << ICHG_SHIFT;
    static constexpr uint16_t ICHG_STEP_MA = 40;
    static constexpr uint16_t ICHG_MIN_MA = 40;
    static constexpr uint16_t ICHG_MAX_MA = 2000;

    // ── REG0x04 VREG, bits 11:3, 10 mV/LSB, 3500-4800 mV, POR 4200 mV
    // (Table 8-10).
    static constexpr uint16_t VREG_SHIFT = 3;
    static constexpr uint16_t VREG_MASK = 0x1FF << VREG_SHIFT;
    static constexpr uint16_t VREG_STEP_MV = 10;

    // ── REG0x0C VOTG, bits 12:6, 80 mV/LSB, 3840-5200 mV, POR 5040 mV
    // (Table 8-13).
    static constexpr uint16_t VOTG_SHIFT = 6;
    static constexpr uint16_t VOTG_MASK = 0x7F << VOTG_SHIFT;
    static constexpr uint16_t VOTG_STEP_MV = 80;

    // ── REG0x15 Charge Timer Control (Table 8-18, POR 5Ch).
    static constexpr uint8_t TMR_EN_SAFETY_TMRS = 1 << 2;  // reset by WATCHDOG to 1

    // ── REG0x16 Charger Control 0 (Table 8-19, POR A1h).
    static constexpr uint8_t CTRL0_EN_CHG = 1 << 5;  // reset by WATCHDOG to 1
    static constexpr uint8_t CTRL0_EN_HIZ = 1 << 4;  // reset by WATCHDOG to 0
    static constexpr uint8_t CTRL0_WD_RST = 1 << 2;  // self-clearing
    static constexpr uint8_t CTRL0_WATCHDOG_MASK = 0x03;
    static constexpr uint8_t CTRL0_WATCHDOG_200S = 0x03;  // 00 off, 01 50 s, 10 100 s, 11 200 s

    // ── REG0x18 Charger Control 2 (Table 8-21, POR 04h).
    static constexpr uint8_t CTRL2_EN_BYPASS_OTG = 1 << 7;  // reset by WATCHDOG to 0
    static constexpr uint8_t CTRL2_EN_OTG = 1 << 6;         // reset by WATCHDOG to 0
    static constexpr uint8_t CTRL2_BATFET_CTRL_WVBUS = 1 << 3;
    static constexpr uint8_t CTRL2_BATFET_DLY = 1 << 2;  // 0 = 25 ms, 1 = 12.5 s (POR)
    static constexpr uint8_t CTRL2_BATFET_CTRL_MASK = 0x03;
    static constexpr uint8_t CTRL2_BATFET_CTRL_SHIP = 0x02;  // 00 normal, 01 shutdown,
                                                             // 10 ship, 11 system reset

    // ── REG0x19 Charger Control 3 (Table 8-22, POR C4h).
    static constexpr uint8_t CTRL3_EN_EXTILIM = 1 << 2;  // reset by WATCHDOG to 1

    // ── REG0x1A NTC Control 0 (Table 8-23, POR 3Dh).
    static constexpr uint8_t NTC0_TS_IGNORE = 1 << 7;  // reset by WATCHDOG to 0

    // ── REG0x1D Charger Status 0 (Table 8-26).
    static constexpr uint8_t STAT0_ADC_DONE = 1 << 6;  // one-shot mode only
    static constexpr uint8_t STAT0_TREG = 1 << 5;
    static constexpr uint8_t STAT0_VSYS = 1 << 4;
    static constexpr uint8_t STAT0_IINDPM = 1 << 3;
    static constexpr uint8_t STAT0_VINDPM = 1 << 2;
    static constexpr uint8_t STAT0_SAFETY_TMR = 1 << 1;
    static constexpr uint8_t STAT0_WD = 1 << 0;  // 1 = watchdog expired / default mode

    // ── REG0x1E Charger Status 1 (Table 8-27). VBUS_STAT on the BQ25628:
    // 000 not powered from VBUS, 100 unknown adapter (default IINDPM),
    // 111 boost OTG (also set in bypass OTG, §8.3.6.2).
    static constexpr uint8_t STAT1_CHG_SHIFT = 3;
    static constexpr uint8_t STAT1_CHG_MASK = 0x03 << STAT1_CHG_SHIFT;
    static constexpr uint8_t STAT1_VBUS_MASK = 0x07;
    static constexpr uint8_t VBUS_STAT_NONE = 0x0;
    static constexpr uint8_t VBUS_STAT_ADAPTER = 0x4;
    static constexpr uint8_t VBUS_STAT_OTG = 0x7;

    // ── REG0x1F FAULT Status 0 (Table 8-28).
    static constexpr uint8_t FAULT_VBUS = 1 << 7;
    static constexpr uint8_t FAULT_BAT = 1 << 6;
    static constexpr uint8_t FAULT_SYS = 1 << 5;
    static constexpr uint8_t FAULT_OTG = 1 << 4;
    static constexpr uint8_t FAULT_TSHUT = 1 << 3;
    static constexpr uint8_t FAULT_TS_MASK = 0x07;

    // ── REG0x26 ADC Control (Table 8-35, POR 30h).
    static constexpr uint8_t ADC_EN = 1 << 7;               // reset by WATCHDOG to 0
    static constexpr uint8_t ADC_RATE = 1 << 6;             // 0 continuous, 1 one-shot
    static constexpr uint8_t ADC_SAMPLE_12BIT = 0x00 << 4;  // 30 ms per channel (tADC_CONV)

    // ── REG0x27 ADC Function Disable 0 (Table 8-36, POR 00h = all on).
    static constexpr uint8_t ADC_DIS_TS = 1 << 2;

    // ── REG0x38 Part Information (Table 8-45): PN bits 5:3, 2h = BQ25628.
    static constexpr uint8_t PART_PN_SHIFT = 3;
    static constexpr uint8_t PART_PN_MASK = 0x07 << PART_PN_SHIFT;
    static constexpr uint8_t PART_PN_BQ25628 = 0x2;

    enum class ChargeState : uint8_t {
        NotCharging = 0,  // not charging or charge terminated
        Fast = 1,         // trickle, pre-charge or fast charge (CC)
        Taper = 2,        // taper charge (CV)
        TopOff = 3,       // top-off timer active
    };

    struct Status {
        uint8_t ctrl0 = 0;  // REG0x16
        uint8_t ctrl2 = 0;  // REG0x18
        uint8_t ctrl3 = 0;  // REG0x19
        uint8_t stat0 = 0;  // REG0x1D
        uint8_t stat1 = 0;  // REG0x1E
        uint8_t fault = 0;  // REG0x1F

        uint8_t vbusStat() const { return stat1 & STAT1_VBUS_MASK; }
        // Power good: the charger runs from a qualified USB adapter.
        bool powerGood() const { return vbusStat() == VBUS_STAT_ADAPTER; }
        bool otgActive() const { return vbusStat() == VBUS_STAT_OTG; }
        bool otgEnabled() const { return ctrl2 & CTRL2_EN_OTG; }
        bool watchdogExpired() const { return stat0 & STAT0_WD; }
        ChargeState chargeState() const {
            return (ChargeState)((stat1 & STAT1_CHG_MASK) >> STAT1_CHG_SHIFT);
        }
        uint8_t tsStat() const { return fault & FAULT_TS_MASK; }
    };

    // Converted ADC channels (REG0x28-0x37, Tables 8-37..8-44).
    struct Adc {
        int16_t ibusMa = 0;      // + = VBUS -> PMID
        int16_t ibatMa = 0;      // + = charging, - = discharging
        bool ibatValid = false;  // false if the conversion was aborted (code 0x8000)
        uint16_t vbusMv = 0;
        uint16_t vpmidMv = 0;
        uint16_t vbatMv = 0;
        uint16_t vsysMv = 0;
        uint16_t tsPermille = 0;  // TS pin voltage in 0.1 % of the TS bias reference
        int16_t tdieDeciC = 0;    // die temperature in 0.1 °C
    };

    // What configure() writes.
    struct Settings {
        uint16_t chargeCurrentMa;  // ICHG
        uint16_t chargeVoltageMv;  // VREG
        uint16_t otgVoltageMv;     // VOTG
    };

    explicit Bq25628(TwoWire& wire = Wire) : _wire(wire) {}

    // Reads the part number. True if a BQ25628 answers at I2C_ADDRESS.
    bool probe();

    bool readStatus(Status& out);

    // Moves the charger to host mode and writes every setting below, none
    // left at its default. Call at start-up and whenever
    // Status::watchdogExpired() reports a fall-back to default mode
    // (§8.4.1), which resets several fields.
    //   - watchdog 200 s (reset WD_RST every kick)
    //   - ICHG, VREG and VOTG from s
    //   - charging enabled, HIZ off
    //   - safety timers on (EN_SAFETY_TMRS = 1)
    //   - TS feedback used (TS_IGNORE = 0)
    //   - ILIM pin input-current limit on (EN_EXTILIM = 1)
    //   - boost (not bypass) OTG
    // Returns false on an I2C error or if VREG does not read back as written.
    bool configure(const Settings& s);

    // Resets the I2C watchdog (WD_RST, §8.4.1).
    bool kickWatchdog();

    // Sets or clears EN_OTG. Boost mode only (EN_BYPASS_OTG stays 0).
    bool setOtg(bool enable);

    // Includes the TS channel in ADC conversions or leaves it out
    // (TS_ADC_DIS). On battery only, with the TS channel on, the ADC only
    // runs above VBAT 3.2 V (§8.3.7).
    bool setTsAdc(bool enable);

    // Starts one ADC conversion of every channel (one-shot, 12 bit). Returns
    // false if the charger refused it: ADC_EN stays 0 while neither VBUS nor
    // VBAT is above its ADC threshold (§8.3.7).
    bool startAdcOneShot();

    // True once the one-shot conversion started by startAdcOneShot() is done
    // (ADC_EN back to 0, §8.3.7).
    bool adcDone(bool& done);

    bool readAdc(Adc& out);

    // BATFET_DLY = 25 ms, BATFET_CTRL = ship mode (§8.3.9.2, Table 8-21).
    // Call on battery only: with an adapter present (BATFET_CTRL_WVBUS = 0)
    // the request waits for the adapter's removal (Table 8-6).
    bool requestShipMode();

    // BATFET_CTRL = normal: withdraws a ship-mode request that has not taken
    // effect yet (Table 8-6).
    bool cancelShipMode();

   private:
    TwoWire& _wire;

    bool _read(uint8_t reg, uint8_t* buf, size_t len);
    bool _write(uint8_t reg, const uint8_t* buf, size_t len);
    bool _read8(uint8_t reg, uint8_t& v) { return _read(reg, &v, 1); }
    bool _write8(uint8_t reg, uint8_t v) { return _write(reg, &v, 1); }
    bool _write16(uint8_t reg, uint16_t v);
    bool _update8(uint8_t reg, uint8_t mask, uint8_t value);
};
