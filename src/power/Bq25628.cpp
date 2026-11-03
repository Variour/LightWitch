#include "Bq25628.h"

#include "../logging/Logger.h"

bool Bq25628::_read(uint8_t reg, uint8_t* buf, size_t len) {
    _wire.beginTransmission(I2C_ADDRESS);
    _wire.write(reg);
    if (_wire.endTransmission(false) != 0) return false;
    if (_wire.requestFrom((uint16_t)I2C_ADDRESS, len, true) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)_wire.read();
    return true;
}

bool Bq25628::_write(uint8_t reg, const uint8_t* buf, size_t len) {
    _wire.beginTransmission(I2C_ADDRESS);
    _wire.write(reg);
    _wire.write(buf, len);
    return _wire.endTransmission(true) == 0;
}

// Little endian, low byte first, both bytes in one transaction (§8.6.1).
bool Bq25628::_write16(uint8_t reg, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
    return _write(reg, b, 2);
}

bool Bq25628::_update8(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t v;
    if (!_read8(reg, v)) return false;
    v = (uint8_t)((v & ~mask) | (value & mask));
    return _write8(reg, v);
}

bool Bq25628::probe() {
    uint8_t part;
    if (!_read8(REG_PART_INFO, part)) return false;
    uint8_t pn = (part & PART_PN_MASK) >> PART_PN_SHIFT;
    if (pn != PART_PN_BQ25628) {
        Logger::w("[pwr] I2C 0x%02X: part info 0x%02X, PN %u is not a BQ25628 (2)", I2C_ADDRESS,
                  part, pn);
        return false;
    }
    Logger::i("[pwr] BQ25628 found at 0x%02X, revision %u", I2C_ADDRESS, part & 0x07);
    return true;
}

bool Bq25628::readStatus(Status& out) {
    // REG0x16..0x20 in one burst: control 0-3, NTC control 0-2, status 0-1,
    // fault status 0, charger flag 0. Of these only the flag register is
    // cleared by reading (§8.3.8.1).
    uint8_t b[11];
    if (!_read(REG_CHG_CTRL_0, b, sizeof(b))) return false;
    out.ctrl0 = b[REG_CHG_CTRL_0 - REG_CHG_CTRL_0];
    out.ctrl2 = b[REG_CHG_CTRL_2 - REG_CHG_CTRL_0];
    out.ctrl3 = b[REG_CHG_CTRL_3 - REG_CHG_CTRL_0];
    out.stat0 = b[REG_CHG_STAT_0 - REG_CHG_CTRL_0];
    out.stat1 = b[REG_CHG_STAT_1 - REG_CHG_CTRL_0];
    out.fault = b[REG_FAULT_STAT_0 - REG_CHG_CTRL_0];
    out.flag0 = b[REG_CHG_FLAG_0 - REG_CHG_CTRL_0];
    return true;
}

bool Bq25628::configure(const Settings& s) {
    // The first write moves the charger from default mode to host mode and
    // starts the watchdog (§8.4.1), so the watchdog setting and its reset go
    // first.
    bool ok =
        _update8(REG_CHG_CTRL_0, CTRL0_WATCHDOG_MASK | CTRL0_WD_RST | CTRL0_EN_CHG | CTRL0_EN_HIZ,
                 CTRL0_WATCHDOG_200S | CTRL0_WD_RST | CTRL0_EN_CHG);

    uint16_t ichg = constrain(s.chargeCurrentMa, ICHG_MIN_MA, ICHG_MAX_MA) / ICHG_STEP_MA;
    ok &= _write16(REG_ICHG, (uint16_t)(ichg << ICHG_SHIFT) & ICHG_MASK);

    uint16_t vreg = s.chargeVoltageMv / VREG_STEP_MV;
    ok &= _write16(REG_VREG, (uint16_t)(vreg << VREG_SHIFT) & VREG_MASK);

    uint16_t votg = s.otgVoltageMv / VOTG_STEP_MV;
    ok &= _write16(REG_VOTG, (uint16_t)(votg << VOTG_SHIFT) & VOTG_MASK);

    uint16_t iterm = s.terminationCurrentMa / ITERM_STEP_MA;
    ok &= _write16(REG_ITERM, (uint16_t)(iterm << ITERM_SHIFT) & ITERM_MASK);

    // Charge safety timers on, TS (battery NTC) feedback used.
    ok &= _update8(REG_CHG_TMR_CTRL, TMR_EN_SAFETY_TMRS, TMR_EN_SAFETY_TMRS);
    ok &= _update8(REG_NTC_CTRL_0, NTC0_TS_IGNORE, 0);

    // Boost OTG (VOTG on PMID), not bypass (PMID = VBAT).
    ok &= _update8(REG_CHG_CTRL_2, CTRL2_EN_BYPASS_OTG, 0);

    // Input current from the ILIM pin; the lower of the ILIM pin and IINDPM
    // limits apply (§8.3.3.4). IINDPM is left to the D+/D- input source
    // detection, which sets it to 500 mA or more (Table 8-11).
    ok &= _update8(REG_CHG_CTRL_3, CTRL3_EN_EXTILIM, CTRL3_EN_EXTILIM);

    ok &= _update8(REG_CHG_CTRL_3, CTRL3_IBAT_PK_MASK, s.batteryPeakCurrent & CTRL3_IBAT_PK_MASK);

    // Read back what the charger actually holds (ICHG, VREG and VOTG are
    // clamped to their ranges, Tables 8-9 / 8-10 / 8-13). VREG must match.
    uint8_t b[2];
    if (!_read(REG_VREG, b, 2)) return false;
    uint16_t vregMv = (((uint16_t)(b[0] | (b[1] << 8)) & VREG_MASK) >> VREG_SHIFT) * VREG_STEP_MV;
    if (vregMv != s.chargeVoltageMv) {
        Logger::e("[pwr] VREG reads %u mV, not %u mV", vregMv, s.chargeVoltageMv);
        return false;
    }
    Logger::i("[pwr] VREG %u mV", vregMv);
    if (_read(REG_ICHG, b, 2)) {
        uint16_t v = (uint16_t)(b[0] | (b[1] << 8));
        Logger::i("[pwr] ICHG %u mA", ((v & ICHG_MASK) >> ICHG_SHIFT) * ICHG_STEP_MA);
    }
    if (_read(REG_VOTG, b, 2)) {
        uint16_t v = (uint16_t)(b[0] | (b[1] << 8));
        Logger::i("[pwr] VOTG %u mV", ((v & VOTG_MASK) >> VOTG_SHIFT) * VOTG_STEP_MV);
    }
    return ok;
}

bool Bq25628::kickWatchdog() { return _update8(REG_CHG_CTRL_0, CTRL0_WD_RST, CTRL0_WD_RST); }

bool Bq25628::setOtg(bool enable) {
    return _update8(REG_CHG_CTRL_2, CTRL2_EN_OTG | CTRL2_EN_BYPASS_OTG, enable ? CTRL2_EN_OTG : 0);
}

bool Bq25628::setTsAdc(bool enable) {
    return _update8(REG_ADC_FUNC_DIS_0, ADC_DIS_TS, enable ? 0 : ADC_DIS_TS);
}

Bq25628::AdcStart Bq25628::startAdcOneShot() {
    if (!_write8(REG_ADC_CTRL, ADC_EN | ADC_RATE | ADC_SAMPLE_12BIT)) return AdcStart::Error;
    // Read back right away: a conversion of all channels takes far longer
    // than this I2C read, so ADC_EN = 0 here means the charger refused it.
    // UNVERIFIED: that a refused ADC_EN reads back as 0 at once (§8.3.7 says
    // it "stays 0", without timing).
    uint8_t v;
    if (!_read8(REG_ADC_CTRL, v)) return AdcStart::Error;
    return (v & ADC_EN) ? AdcStart::Started : AdcStart::Refused;
}

bool Bq25628::adcDone(bool& done) {
    uint8_t v;
    if (!_read8(REG_ADC_CTRL, v)) return false;
    done = (v & ADC_EN) == 0;
    return true;
}

bool Bq25628::readAdc(Adc& out) {
    // REG0x28..0x37 in one burst, each channel little endian.
    uint8_t b[16];
    if (!_read(REG_IBUS_ADC, b, sizeof(b))) return false;
    auto u16 = [&](uint8_t reg) {
        size_t i = reg - REG_IBUS_ADC;
        return (uint16_t)(b[i] | (b[i + 1] << 8));
    };

    // IBUS: bits 15:1, two's complement, 2 mA/LSB (Table 8-37).
    out.ibusMa = (int16_t)(((int16_t)u16(REG_IBUS_ADC) >> 1) * 2);

    // IBAT: bits 15:2, two's complement, 4 mA/LSB; code 0x8000 = conversion
    // aborted because the current changed polarity (Table 8-38).
    uint16_t ibat = u16(REG_IBAT_ADC);
    out.ibatValid = ibat != 0x8000;
    out.ibatMa = out.ibatValid ? (int16_t)(((int16_t)ibat >> 2) * 4) : 0;

    // VBUS / VPMID: bits 14:2, 3.97 mV/LSB (Tables 8-39 / 8-40).
    out.vbusMv = (uint16_t)(((uint32_t)((u16(REG_VBUS_ADC) >> 2) & 0x1FFF) * 397) / 100);
    out.vpmidMv = (uint16_t)(((uint32_t)((u16(REG_VPMID_ADC) >> 2) & 0x1FFF) * 397) / 100);

    // VBAT / VSYS: bits 12:1, 1.99 mV/LSB (Tables 8-41 / 8-42).
    out.vbatMv = (uint16_t)(((uint32_t)((u16(REG_VBAT_ADC) >> 1) & 0x0FFF) * 199) / 100);
    out.vsysMv = (uint16_t)(((uint32_t)((u16(REG_VSYS_ADC) >> 1) & 0x0FFF) * 199) / 100);

    // TS: bits 11:0, 0.0961 %/LSB of the TS bias reference (Table 8-43).
    out.tsPermille = (uint16_t)(((uint32_t)(u16(REG_TS_ADC) & 0x0FFF) * 961) / 1000);

    // TDIE: bits 11:0, two's complement, 0.5 °C/LSB (Table 8-44).
    int16_t tdie = (int16_t)(u16(REG_TDIE_ADC) & 0x0FFF);
    if (tdie & 0x0800) tdie -= 0x1000;
    out.tdieDeciC = (int16_t)(tdie * 5);
    return true;
}

bool Bq25628::requestShipMode() {
    // Delay first (25 ms instead of the POR 12.5 s) with BATFET_CTRL_WVBUS =
    // 0, so an adapter blocks ship mode; then the ship-mode request itself.
    if (!_update8(REG_CHG_CTRL_2, CTRL2_BATFET_CTRL_WVBUS | CTRL2_BATFET_DLY, 0)) return false;
    return _update8(REG_CHG_CTRL_2, CTRL2_BATFET_CTRL_MASK, CTRL2_BATFET_CTRL_SHIP);
}

bool Bq25628::cancelShipMode() { return _update8(REG_CHG_CTRL_2, CTRL2_BATFET_CTRL_MASK, 0); }
