/*
 * EEZ DIB MIO168
 * Copyright (C) 2015-present, Envox d.o.o.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <new>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#if defined(EEZ_PLATFORM_STM32)
#include <spi.h>
#include <eez/platform/stm32/spi.h>
#endif

#include <eez/debug.h>
#include <eez/firmware.h>
#include <eez/index.h>
#include <eez/hmi.h>
#include <eez/gui/gui.h>
#include <eez/modules/psu/psu.h>
#include "eez/modules/psu/profile.h"
#include "eez/modules/psu/calibration.h"
#include <eez/modules/psu/event_queue.h>
#include <eez/modules/psu/gui/psu.h>
#include "eez/modules/psu/gui/keypad.h"
#include "eez/modules/psu/gui/edit_mode.h"
#include <eez/modules/bp3c/comm.h>
#include <eez/modules/bp3c/flash_slave.h>

#include "./dib-mio168.h"

#include <scpi/scpi.h>

using namespace eez::psu;
using namespace eez::psu::gui;
using namespace eez::gui;

namespace eez {
namespace dib_mio168 {

enum Mio168HighPriorityThreadMessage {
    PSU_MESSAGE_DIN_CONFIGURE = PSU_MESSAGE_MODULE_SPECIFIC,
    PSU_MESSAGE_AIN_CONFIGURE,
    PSU_MESSAGE_AOUT_DAC7760_CONFIGURE,
};

static const uint16_t MODULE_REVISION_R1B2  = 0x0102;

static const int DIN_SUBCHANNEL_INDEX = 0;
static const int DOUT_SUBCHANNEL_INDEX = 1;
static const int AIN_1_SUBCHANNEL_INDEX = 2;
static const int AIN_2_SUBCHANNEL_INDEX = 3;
static const int AIN_3_SUBCHANNEL_INDEX = 4;
static const int AIN_4_SUBCHANNEL_INDEX = 5;
static const int AOUT_1_SUBCHANNEL_INDEX = 6;
static const int AOUT_2_SUBCHANNEL_INDEX = 7;
static const int AOUT_3_SUBCHANNEL_INDEX = 8;
static const int AOUT_4_SUBCHANNEL_INDEX = 9;
static const int PWM_1_SUBCHANNEL_INDEX = 10;
static const int PWM_2_SUBCHANNEL_INDEX = 11;

static float AIN_VOLTAGE_RESOLUTION = 0.005f;
static float AIN_CURRENT_RESOLUTION = 0.00005f;

static float AOUT_DAC7760_ENCODER_STEP_VALUES[] = { 0.01f, 0.1f, 0.2f, 0.5f };
static float AOUT_DAC7760_AMPER_ENCODER_STEP_VALUES[] = { 0.001f, 0.005f, 0.01f, 0.05f };

static float AOUT_DAC7563_MIN = -10.0f;
static float AOUT_DAC7563_MAX = 10.0f;
static float AOUT_DAC7563_RESOLUTION = 0.01f;
static float AOUT_DAC7563_ENCODER_STEP_VALUES[] = { 0.01f, 0.1f, 0.2f, 0.5f };

static float PWM_MIN_FREQUENCY = 0.1f;
static float PWM_MAX_FREQUENCY = 1000000.0f;

////////////////////////////////////////////////////////////////////////////////

#define BUFFER_SIZE 1024

struct FromMasterToSlave {
    uint8_t dinRanges;
    uint8_t dinSpeeds;

    uint8_t doutStates;

    struct {
        uint8_t mode;
        uint8_t range;
        uint8_t tempSensorBias;
    } ain[4];

    struct {
        uint8_t outputEnabled;
        uint8_t outputRange;
        float outputValue;
    } aout_dac7760[2];

    struct {
        float voltage;
    } aout_dac7563[2];

    struct {
        float freq;
        float duty;
    } pwm[2];    
};

struct FromSlaveToMaster {
    uint8_t dinStates;
};

////////////////////////////////////////////////////////////////////////////////

static const uint8_t MAX_CAL_CONF_POINTS = 4;

struct CalConf {
    static const int VERSION = 1;

    BlockHeader header;

    struct {
        unsigned calState: 1;   // is channel calibrated?
        unsigned calEnabled: 1; // is channel calibration enabled?
        unsigned ongoingCal: 1; // is channel under ongoing calibration procedure?
        unsigned numPoints: 5;
        unsigned reserved: 8;
    } state;

    struct {
        float uncalValue;
        float calValue;
    } points[MAX_CAL_CONF_POINTS];

    void clear() {
        memset(this, 0, sizeof(CalConf));
    }
};

////////////////////////////////////////////////////////////////////////////////

struct MioChannel {
};

struct DinChannel : public MioChannel {
    uint8_t m_pinStates = 0;

    // Valid for all 8 digital inputs
    // 0 - LOW, 1 - HIGH
    uint8_t m_pinRanges = 0;

    // Valid only for first two digital inputs.
    // 0 - FAST, 1 - SLOW
    uint8_t m_pinSpeeds = 0;

    struct ProfileParameters {
        uint8_t pinRanges;
        uint8_t pinSpeeds;
    };

    void getProfileParameters(ProfileParameters &parameters) {
        parameters.pinRanges = m_pinRanges;
        parameters.pinSpeeds = m_pinSpeeds;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_pinRanges = parameters.pinRanges;
        m_pinSpeeds = parameters.pinSpeeds;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, ProfileParameters &parameters) {
        WRITE_PROPERTY("din_pinRanges", parameters.pinRanges);
        WRITE_PROPERTY("din_pinSpeeds", parameters.pinSpeeds);
        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, ProfileParameters &parameters) {
        READ_PROPERTY("din_pinRanges", parameters.pinRanges);
        READ_PROPERTY("din_pinSpeeds", parameters.pinSpeeds);
        return false;
    }

    void resetConfiguration() {
        m_pinRanges = 0;
        m_pinSpeeds = 0;
    }

    uint8_t getDigitalInputData() {
        return m_pinStates;
    }

    int getPinState(int pin) {
        return m_pinStates & (1 << pin) ? 1 : 0;
    }

    int getPinRange(int pin) {
        return m_pinRanges & (1 << pin) ? 1 : 0;
    }
    
    int getPinSpeed(int pin) {
        return m_pinSpeeds & (1 << pin) ? 1 : 0;
    }
};

struct DoutChannel : public MioChannel {
    uint8_t m_pinStates = 0;

     struct ProfileParameters {
        uint8_t pinStates;
    };

    void getProfileParameters(ProfileParameters &parameters) {
        parameters.pinStates = m_pinStates;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_pinStates = parameters.pinStates;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, ProfileParameters &parameters) {
        WRITE_PROPERTY("dout_pinStates", parameters.pinStates);
        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, ProfileParameters &parameters) {
        READ_PROPERTY("dout_pinStates", parameters.pinStates);
        return false;
    }

   void resetConfiguration() {
        m_pinStates = 0;
    }

    uint8_t getDigitalOutputData() {
        return m_pinStates;
    }

    void setDigitalOutputData(uint8_t data) {
        m_pinStates = data;
    }

    int getPinState(int pin) {
        return m_pinStates & (1 << pin) ? 1 : 0;
    }

    void setPinState(int pin, int state) {
        if (state) {
            m_pinStates |= 1 << pin;
        } else {
            m_pinStates &= ~(1 << pin);
        }
    }
};

struct AinChannel : public MioChannel {
    float m_value = 0;
    uint8_t m_mode = 1;
    uint8_t m_range = 0;
    uint8_t m_tempSensorBias = 0;

    struct ProfileParameters {
        uint8_t mode;
        uint8_t range;
        uint8_t tempSensorBias;
    };

    void getProfileParameters(ProfileParameters &parameters) {
        parameters.mode = m_mode;
        parameters.range = m_range;
        parameters.tempSensorBias = m_tempSensorBias;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_mode = parameters.mode;
        m_range = parameters.range;
        m_tempSensorBias = parameters.tempSensorBias;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "ain_%d_mode", i+1);
        WRITE_PROPERTY(propName, parameters.mode);

        sprintf(propName, "ain_%d_range", i+1);
        WRITE_PROPERTY(propName, parameters.range);

        sprintf(propName, "ain_%d_tempSensorBias", i+1);
        WRITE_PROPERTY(propName, parameters.tempSensorBias);

        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "ain_%d_mode", i+1);
        READ_PROPERTY(propName, parameters.mode);

        sprintf(propName, "ain_%d_range", i+1);
        READ_PROPERTY(propName, parameters.range);

        sprintf(propName, "ain_%d_tempSensorBias", i+1);
        READ_PROPERTY(propName, parameters.tempSensorBias);

        return false;
    }

    void resetConfiguration() {
        m_mode = 1;
        m_range = 0;
        m_tempSensorBias = 0;
    }

    float convertU16Value(uint16_t value) {
        float min = 0;
        float max = 0;

        if (m_range == 0) {
            min = -10.24f;
            max = 10.24f;
        } else if (m_range == 1) {
            min = -5.12f;
            max = 5.12f;
        } else if (m_range == 2) {
            min = -2.56f;
            max = 2.56f;
        } else if (m_range == 3) {
            min = -1.28f;
            max = 1.28f;
        } else if (m_range == 11) {
            min = -0.64f;
            max = 0.64f;
        } else if (m_range == 5) {
            min = 0;
            max = 10.24f;
        } else if (m_range == 6) {
            min = 0;
            max = 5.12f;
        } else if (m_range == 7) {
            min = 0;
            max = 2.56f;
        } else if (m_range == 15) {
            min = 0;
            max = 1.28f;
        }

        float fValue = remap(value * 1.0f, 0.0f, min, 65535.0f, max);

        if (m_mode == MEASURE_MODE_CURRENT) {
            fValue /= 100.0f;
        }

        return fValue;
    }

    float getResolution() {
        return m_mode == MEASURE_MODE_VOLTAGE ? AIN_VOLTAGE_RESOLUTION : m_mode == MEASURE_MODE_CURRENT ? AIN_CURRENT_RESOLUTION : 1.0f;
    }
};

struct AoutDac7760Channel : public MioChannel {
    bool m_outputEnabled = false;
    uint8_t m_mode = SOURCE_MODE_VOLTAGE;

    // 5: 4 mA to 20 mA
    // 6: 0 mA to 20 mA
    // 7: 0 mA to 24 mA
    uint8_t m_currentRange = 5;

    // 0: 0 V to +5 V
    // 1: 0 V to +10 V
    // 2: +/- 5 V
    // 3: +/- 10 V
    uint8_t m_voltageRange = 0;

    float m_currentValue = 0;
    float m_voltageValue = 0;

    CalConf calConf;

    struct ProfileParameters {
        bool outputEnabled;
        uint8_t mode;
        uint8_t currentRange;
        uint8_t voltageRange;
        float currentValue;
        float voltageValue;
    };

    void getProfileParameters(ProfileParameters &parameters) {
        parameters.outputEnabled = m_outputEnabled;
        parameters.mode = m_mode;
        parameters.currentRange = m_currentRange;
        parameters.voltageRange = m_voltageRange;
        parameters.currentValue = m_currentValue;
        parameters.voltageValue = m_voltageValue;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_outputEnabled = parameters.outputEnabled;
        m_mode = parameters.mode;
        m_currentRange = parameters.currentRange;
        m_voltageRange = parameters.voltageRange;
        m_currentValue = parameters.currentValue;
        m_voltageValue = parameters.voltageValue;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "aout_dac7760_%d_outputEnabled", i+1);
        WRITE_PROPERTY(propName, parameters.outputEnabled);

        sprintf(propName, "aout_dac7760_%d_mode", i+1);
        WRITE_PROPERTY(propName, parameters.mode);

        sprintf(propName, "aout_dac7760_%d_currentRange", i+1);
        WRITE_PROPERTY(propName, parameters.currentRange);

        sprintf(propName, "aout_dac7760_%d_voltageRange", i+1);
        WRITE_PROPERTY(propName, parameters.voltageRange);

        sprintf(propName, "aout_dac7760_%d_currentValue", i+1);
        WRITE_PROPERTY(propName, parameters.currentValue);

        sprintf(propName, "aout_dac7760_%d_voltageValue", i+1);
        WRITE_PROPERTY(propName, parameters.voltageValue);

        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "aout_dac7760_%d_outputEnabled", i+1);
        READ_PROPERTY(propName, parameters.outputEnabled);

        sprintf(propName, "aout_dac7760_%d_mode", i+1);
        READ_PROPERTY(propName, parameters.mode);

        sprintf(propName, "aout_dac7760_%d_currentRange", i+1);
        READ_PROPERTY(propName, parameters.currentRange);

        sprintf(propName, "aout_dac7760_%d_voltageRange", i+1);
        READ_PROPERTY(propName, parameters.voltageRange);

        sprintf(propName, "aout_dac7760_%d_currentValue", i+1);
        READ_PROPERTY(propName, parameters.currentValue);

        sprintf(propName, "aout_dac7760_%d_voltageValue", i+1);
        READ_PROPERTY(propName, parameters.voltageValue);

        return false;
    }

    void resetConfiguration() {
        m_outputEnabled = false;
        m_mode = SOURCE_MODE_VOLTAGE;
        m_currentRange = 5;
        m_voltageRange = 0;
        m_currentValue = 0;
        m_voltageValue = 0;
    }

    SourceMode getMode() {
        return calConf.state.ongoingCal ? SOURCE_MODE_VOLTAGE : (SourceMode)m_mode;
    }

    void setMode(SourceMode mode) {
        m_mode = mode;
    }

    int8_t getCurrentRange() {
        return m_currentRange;
    }
    
    void setCurrentRange(int8_t range) {
        m_currentRange = range;
    }

    int8_t getVoltageRange() {
        return calConf.state.ongoingCal ? 3 : m_voltageRange;
    }
    
    void setVoltageRange(int8_t range) {
        m_voltageRange = range;
    }

    float getValue() {
        return getMode() == SOURCE_MODE_VOLTAGE ? m_voltageValue : m_currentValue;
    }

    Unit getUnit() {
        return getMode() == SOURCE_MODE_VOLTAGE ? UNIT_VOLT : UNIT_AMPER;
    }

    void setValue(float value) {
        if (getMode() == SOURCE_MODE_VOLTAGE) {
            m_voltageValue = value;
        } else {
            m_currentValue = value;
        }
    }

    float getVoltageMinValue() {
        uint8_t voltageRange = getVoltageRange();
        if (voltageRange == 0) {
            return 0;
        } 
        if (voltageRange == 1) {
            return 0;
        } 
        if (voltageRange == 2) {
            return -5.0f;
        } 
        return -10.0f;
    }

    float getCurrentMinValue() {
        if (m_currentRange == 5) {
            return 0.004f;
        }
        if (m_currentRange == 6) {
            return 0;
        }
        return 0;
    }

    float getMinValue() {
        if (getMode() == SOURCE_MODE_VOLTAGE) {
            return getVoltageMinValue();
        } else {
            return getCurrentMinValue();
        }
    }

    float getVoltageMaxValue() {
        uint8_t voltageRange = getVoltageRange();
        if (voltageRange == 0) {
            return 5.0f;
        } 
        if (voltageRange == 1) {
            return 10.0f;
        } 
        if (voltageRange == 2) {
            return 5.0f;
        } 
        return 10.0f;
    }

    float getCurrentMaxValue() {
        if (m_currentRange == 5) {
            return 0.02f;
        }
        if (m_currentRange == 6) {
            return 0.02f;
        }
        return 0.024f;
    }

    float getMaxValue() {
        if (getMode() == SOURCE_MODE_VOLTAGE) {
            return getVoltageMaxValue();
        } else {
            return getCurrentMaxValue();
        }
    }

    float getVoltageResolution() {
        return calConf.state.ongoingCal ? 0.0001f : 0.001f;
    }

    float getCurrentResolution() {
        return 0.001f;
    }

    float getResolution() {
        if (getMode() == SOURCE_MODE_VOLTAGE) {
            return getVoltageResolution();
        } else {
            return getCurrentResolution();
        }
    }

    void getStepValues(StepValues *stepValues) {
        if (getMode() == SOURCE_MODE_VOLTAGE) {
            stepValues->values = AOUT_DAC7760_ENCODER_STEP_VALUES;
            stepValues->count = sizeof(AOUT_DAC7760_ENCODER_STEP_VALUES) / sizeof(float);
            stepValues->unit = UNIT_VOLT;
        } else {
            stepValues->values = AOUT_DAC7760_AMPER_ENCODER_STEP_VALUES;
            stepValues->count = sizeof(AOUT_DAC7760_AMPER_ENCODER_STEP_VALUES) / sizeof(float);
            stepValues->unit = UNIT_AMPER;
        }
    }
};

struct AoutDac7563Channel : public MioChannel {
    float m_value = 0;

    bool calEnabled;
    CalConf calConf;

    struct ProfileParameters {
        float value;
    };

    void getProfileParameters(ProfileParameters &parameters) {
        parameters.value = m_value;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_value = parameters.value;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "aout_dac7563_%d_value", i+1);
        WRITE_PROPERTY(propName, parameters.value);

        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "aout_dac7563_%d_value", i+1);
        READ_PROPERTY(propName, parameters.value);

        return false;
    }

    void resetConfiguration() {
        m_value = 0;
    }
};

struct PwmChannel : public MioChannel {
    float m_freq = 0;
    float m_duty = 0;

    struct ProfileParameters {
        float freq;
        float duty;
    };
    
    void getProfileParameters(ProfileParameters &parameters) {
        parameters.freq = m_freq;
        parameters.duty = m_duty;
    }

    void setProfileParameters(ProfileParameters &parameters) {
        m_freq = parameters.freq;
        m_duty = parameters.duty;
    }

    void resetConfiguration() {
        m_freq = 0;
        m_duty = 0;
    }

    bool writeProfileProperties(psu::profile::WriteContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "pwm_%d_freq", i+1);
        WRITE_PROPERTY(propName, parameters.freq);

        sprintf(propName, "pwm_%d_duty", i+1);
        WRITE_PROPERTY(propName, parameters.duty);

        return true;
    }

    bool readProfileProperties(psu::profile::ReadContext &ctx, int i, ProfileParameters &parameters) {
        char propName[32];

        sprintf(propName, "pwm_%d_freq", i+1);
        READ_PROPERTY(propName, parameters.freq);

        sprintf(propName, "pwm_%d_duty", i+1);
        READ_PROPERTY(propName, parameters.duty);

        return false;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct Mio168Module : public Module {
public:
    TestResult testResult = TEST_NONE;
    bool synchronized = false;
    int numCrcErrors = 0;
    uint8_t input[BUFFER_SIZE];
    uint8_t output[BUFFER_SIZE];
    bool spiReady = false;

    DinChannel dinChannel;
    DoutChannel doutChannel;
    AinChannel ainChannels[4];
    AoutDac7760Channel aoutDac7760Channels[2];
    AoutDac7563Channel aoutDac7563Channels[2];
    PwmChannel pwmChannels[2];

    Mio168Module() {
        moduleType = MODULE_TYPE_DIB_MIO168;
        moduleName = "MIO168";
        moduleBrand = "Envox";
        latestModuleRevision = MODULE_REVISION_R1B2;
        flashMethod = FLASH_METHOD_STM32_BOOTLOADER_SPI;
        flashDuration = 10000;
#if defined(EEZ_PLATFORM_STM32)        
        spiBaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
        spiCrcCalculationEnable = true;
#else
        spiBaudRatePrescaler = 0;
        spiCrcCalculationEnable = false;
#endif
        numPowerChannels = 0;
        numOtherChannels = 12;

        resetConfiguration();

        memset(input, 0, sizeof(input));
        memset(output, 0, sizeof(output));
    }

    Module *createModule() override {
        return new Mio168Module();
    }

    TestResult getTestResult() override {
        return testResult;
    }

    void initChannels() override {
        if (!synchronized) {
            if (bp3c::comm::masterSynchroV2(slotIndex)) {
                synchronized = true;
                numCrcErrors = 0;
                testResult = TEST_OK;
            } else {
                if (g_slots[slotIndex]->firmwareInstalled) {
                    event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_SYNC_ERROR + slotIndex);
                }
                testResult = TEST_FAILED;
            }
        }
    }

    void tick() override {
        if (!synchronized) {
            return;
        }

#if defined(EEZ_PLATFORM_STM32)
        if (spiReady) {
            spiReady = false;
            transfer();
        }
#endif
    }

#if defined(EEZ_PLATFORM_STM32)
    void onSpiIrq() {
        spiReady = true;
    }
#endif

    void transfer() {
        FromMasterToSlave &data = *((FromMasterToSlave *)output);

        data.dinRanges = dinChannel.m_pinRanges;
        data.dinSpeeds = dinChannel.m_pinSpeeds;

        data.doutStates = doutChannel.m_pinStates;

        for (int i = 0; i < 4; i++) {
            auto channel = &ainChannels[i];
            data.ain[i].mode = channel->m_mode;
            data.ain[i].range = channel->m_range;
            data.ain[i].tempSensorBias = channel->m_tempSensorBias;
        }

        for (int i = 0; i < 2; i++) {
            auto channel = &aoutDac7760Channels[i];
            data.aout_dac7760[i].outputEnabled = channel->m_outputEnabled;
            data.aout_dac7760[i].outputRange = channel->getMode() == SOURCE_MODE_VOLTAGE ? channel->getVoltageRange() : channel->m_currentRange;
            data.aout_dac7760[i].outputValue = channel->getMode() == SOURCE_MODE_VOLTAGE ? channel->m_voltageValue : channel->m_currentValue;
        }

        for (int i = 0; i < 2; i++) {
            auto channel = &aoutDac7563Channels[i];
            data.aout_dac7563[i].voltage = channel->m_value;
        }

        for (int i = 0; i < 2; i++) {
            auto channel = &pwmChannels[i];
            data.pwm[i].freq = channel->m_freq;
            data.pwm[i].duty = channel->m_duty;
        }

        auto status = bp3c::comm::transferDMA(slotIndex, output, input, BUFFER_SIZE);
        if (status != bp3c::comm::TRANSFER_STATUS_OK) {
        	onSpiDmaTransferCompleted(status);
        }
    }

    void onSpiDmaTransferCompleted(int status) override {
        if (status == bp3c::comm::TRANSFER_STATUS_OK) {
            numCrcErrors = 0;

            FromSlaveToMaster &data = (FromSlaveToMaster &)*input;

            dinChannel.m_pinStates = data.dinStates;

            uint16_t *inputU16 = (uint16_t *)(input + 24);

            for (int i = 0; i < 4; i++) {
                auto &channel = ainChannels[i];
                channel.m_value = channel.convertU16Value(inputU16[i]);
            }
        } else {
            if (status == bp3c::comm::TRANSFER_STATUS_CRC_ERROR) {
                if (++numCrcErrors >= 10) {
                    event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_CRC_CHECK_ERROR + slotIndex);
                    synchronized = false;
                    testResult = TEST_FAILED;
                } else {
                    DebugTrace("Slot %d CRC %d\n", slotIndex + 1, numCrcErrors);
                }
            } else {
                DebugTrace("Slot %d SPI transfer error %d\n", slotIndex + 1, status);
            }
        }
    }

    void onPowerDown() override {
        synchronized = false;
    }

    Page *getPageFromId(int pageId) override;

    int getSlotView(SlotViewType slotViewType, int slotIndex, int cursor) override {
        if (slotViewType == SLOT_VIEW_TYPE_DEFAULT) {
            return isDefaultViewVertical() ? PAGE_ID_DIB_MIO168_SLOT_VIEW_DEF : PAGE_ID_SLOT_DEF_HORZ_EMPTY;
        }
        if (slotViewType == SLOT_VIEW_TYPE_DEFAULT_2COL) {
            return isDefaultViewVertical() ? PAGE_ID_DIB_MIO168_SLOT_VIEW_DEF_2COL : PAGE_ID_SLOT_DEF_HORZ_EMPTY_2COL;
        }
        if (slotViewType == SLOT_VIEW_TYPE_MAX) {
            return PAGE_ID_DIB_MIO168_SLOT_VIEW_MAX;
        }
        if (slotViewType == SLOT_VIEW_TYPE_MIN) {
            return PAGE_ID_DIB_MIO168_SLOT_VIEW_MIN;
        }
        assert(slotViewType == SLOT_VIEW_TYPE_MICRO);
        return PAGE_ID_DIB_MIO168_SLOT_VIEW_MICRO;
    }

    int getSlotSettingsPageId() override {
        return getTestResult() == TEST_OK ? PAGE_ID_DIB_MIO168_SETTINGS : PAGE_ID_SLOT_SETTINGS;
    }

    void onHighPriorityThreadMessage(uint8_t type, uint32_t param) override;

    struct ProfileParameters {
        DinChannel::ProfileParameters dinChannel;
        DoutChannel::ProfileParameters doutChannel;
        AinChannel::ProfileParameters ainChannels[4];
        AoutDac7760Channel::ProfileParameters aoutDac7760Channels[2];
        AoutDac7563Channel::ProfileParameters aoutDac7563Channels[2];
        PwmChannel::ProfileParameters pwmChannels[2];
    };

    void getProfileParameters(uint8_t *buffer) override {
        assert(sizeof(ProfileParameters) < MAX_CHANNEL_PARAMETERS_SIZE);

        auto parameters = (ProfileParameters *)buffer;

        dinChannel.getProfileParameters(parameters->dinChannel);
        doutChannel.getProfileParameters(parameters->doutChannel);
        for (int i = 0; i < 4; i++) {
            ainChannels[i].getProfileParameters(parameters->ainChannels[i]);
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7760Channels[i].getProfileParameters(parameters->aoutDac7760Channels[i]);
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7563Channels[i].getProfileParameters(parameters->aoutDac7563Channels[i]);
        }
        for (int i = 0; i < 2; i++) {
            pwmChannels[i].getProfileParameters(parameters->pwmChannels[i]);
        }
    }
    
    void setProfileParameters(uint8_t *buffer, bool mismatch, int recallOptions) override {
        auto parameters = (ProfileParameters *)buffer;

        dinChannel.setProfileParameters(parameters->dinChannel);
        doutChannel.setProfileParameters(parameters->doutChannel);
        for (int i = 0; i < 4; i++) {
            ainChannels[i].setProfileParameters(parameters->ainChannels[i]);
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7760Channels[i].setProfileParameters(parameters->aoutDac7760Channels[i]);
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7563Channels[i].setProfileParameters(parameters->aoutDac7563Channels[i]);
        }
        for (int i = 0; i < 2; i++) {
            pwmChannels[i].setProfileParameters(parameters->pwmChannels[i]);
        }
    }
    
    bool writeProfileProperties(psu::profile::WriteContext &ctx, const uint8_t *buffer) override {
        auto parameters = (ProfileParameters *)buffer;

        if (!dinChannel.writeProfileProperties(ctx, parameters->dinChannel)) {
            return false;
        }
        if (!doutChannel.writeProfileProperties(ctx, parameters->doutChannel)) {
            return false;
        }
        for (int i = 0; i < 4; i++) {
            if (!ainChannels[i].writeProfileProperties(ctx, i, parameters->ainChannels[i])) {
                return false;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (!aoutDac7760Channels[i].writeProfileProperties(ctx, i, parameters->aoutDac7760Channels[i])) {
                return false;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (!aoutDac7563Channels[i].writeProfileProperties(ctx, i, parameters->aoutDac7563Channels[i])) {
                return false;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (!pwmChannels[i].writeProfileProperties(ctx, i, parameters->pwmChannels[i])) {
                return false;
            }
        }

        return true;
    }
    
    bool readProfileProperties(psu::profile::ReadContext &ctx, uint8_t *buffer) override {
        auto parameters = (ProfileParameters *)buffer;

        if (dinChannel.readProfileProperties(ctx, parameters->dinChannel)) {
            return true;
        }
        if (doutChannel.readProfileProperties(ctx, parameters->doutChannel)) {
            return true;
        }
        for (int i = 0; i < 4; i++) {
            if (ainChannels[i].readProfileProperties(ctx, i, parameters->ainChannels[i])) {
                return true;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (aoutDac7760Channels[i].readProfileProperties(ctx, i, parameters->aoutDac7760Channels[i])) {
                return true;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (aoutDac7563Channels[i].readProfileProperties(ctx, i, parameters->aoutDac7563Channels[i])) {
                return true;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (pwmChannels[i].readProfileProperties(ctx, i, parameters->pwmChannels[i])) {
                return true;
            }
        }

        return false;
    }

    void resetConfiguration() {
        dinChannel.resetConfiguration();
        doutChannel.resetConfiguration();
        for (int i = 0; i < 4; i++) {
            ainChannels[i].resetConfiguration();
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7760Channels[i].resetConfiguration();
        }
        for (int i = 0; i < 2; i++) {
            aoutDac7563Channels[i].resetConfiguration();
        }
        for (int i = 0; i < 2; i++) {
            pwmChannels[i].resetConfiguration();
        }
    }

    bool getDigitalInputData(int subchannelIndex, uint8_t &data, int *err) override {
        if (subchannelIndex != DIN_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        data = dinChannel.getDigitalInputData();
        return true;
    }

    bool getDigitalInputRange(int subchannelIndex, uint8_t pin, uint8_t &range, int *err) override {
        if (subchannelIndex != DIN_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (pin < 0 || pin > 7) {
            if (*err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        range = dinChannel.m_pinRanges & (1 << pin) ? 1 : 0;

        return true;
    }
    
    bool setDigitalInputRange(int subchannelIndex, uint8_t pin, uint8_t range, int *err) override {
        if (subchannelIndex != DIN_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (pin < 0 || pin > 7) {
            if (*err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        if (range) {
            dinChannel.m_pinRanges |= 1 << pin;
        } else {
            dinChannel.m_pinRanges &= ~(1 << pin);
        }

        return true;
    }

    bool getDigitalInputSpeed(int subchannelIndex, uint8_t pin, uint8_t &speed, int *err) override {
        if (subchannelIndex != DIN_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (pin < 0 || pin > 1) {
            if (*err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        speed = dinChannel.m_pinSpeeds & (1 << pin) ? 1 : 0;

        return true;
    }
    
    bool setDigitalInputSpeed(int subchannelIndex, uint8_t pin, uint8_t speed, int *err) override {
        if (subchannelIndex != DIN_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (pin < 0 || pin > 1) {
            if (*err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        if (speed) {
            dinChannel.m_pinSpeeds |= 1 << pin;
        } else {
            dinChannel.m_pinSpeeds &= ~(1 << pin);
        }

        return true;
    }

    bool getDigitalOutputData(int subchannelIndex, uint8_t &data, int *err) override {
        if (subchannelIndex != DOUT_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        data = doutChannel.getDigitalOutputData();
        return true;
    }
    
    bool setDigitalOutputData(int subchannelIndex, uint8_t data, int *err) override {
        if (subchannelIndex != DOUT_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        doutChannel.setDigitalOutputData(data);
        return true;
    }

    bool getMode(int subchannelIndex, SourceMode &mode, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        mode = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getMode();
        return true;
    }
    
    bool setMode(int subchannelIndex, SourceMode mode, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].setMode(mode);
        return true;
    }

    bool getCurrentRange(int subchannelIndex, uint8_t &range, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        range = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getCurrentRange();
        return true;
    }
    
    bool setCurrentRange(int subchannelIndex, uint8_t range, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].setCurrentRange(range);
        return true;
    }

    bool getVoltageRange(int subchannelIndex, uint8_t &range, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        range = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getVoltageRange();
        return true;
    }
    
    bool setVoltageRange(int subchannelIndex, uint8_t range, int *err) override {
        if (subchannelIndex != AOUT_1_SUBCHANNEL_INDEX && subchannelIndex != AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].setVoltageRange(range);
        return true;
    }

    bool getMeasureMode(int subchannelIndex, MeasureMode &mode, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        mode = (MeasureMode)ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_mode;
        return true;
    }
    
    bool setMeasureMode(int subchannelIndex, MeasureMode mode, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_mode = mode;
        return true;
    }

    bool getMeasureRange(int subchannelIndex, uint8_t &range, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        range = ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_range;
        return true;
    }
    
    bool setMeasureRange(int subchannelIndex, uint8_t range, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (range != 0 && range != 1 && range != 2 && range != 3 && range != 11 && range != 5 && range != 6 && range != 7 && range != 15) {
            if (*err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_range = range;

        return true;
    }

    bool getMeasureTempSensorBias(int subchannelIndex, bool &enabled, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }
        enabled = ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_tempSensorBias;
        return true;
    }
    
    bool setMeasureTempSensorBias(int subchannelIndex, bool enabled, int *err) override {
        if (subchannelIndex != AIN_1_SUBCHANNEL_INDEX && subchannelIndex != AIN_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX].m_tempSensorBias = enabled;

        return true;
    }

    bool outputEnable(int subchannelIndex, bool enable, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_outputEnabled = enable;
        return true;
    }

    bool isOutputEnabled(int subchannelIndex, bool &enabled, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        enabled = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_outputEnabled;

        return true;
    }
    
    bool getVoltage(int subchannelIndex, float &value, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (subchannelIndex < AOUT_3_SUBCHANNEL_INDEX) {
            value = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_voltageValue;
        } else {
            value = aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].m_value;
        }

        return true;
    }

    bool setVoltage(int subchannelIndex, float value, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        if (subchannelIndex < AOUT_3_SUBCHANNEL_INDEX) {
            aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_voltageValue = value;
        } else {
            aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].m_value = value;
        }

        return true;
    }

    bool getMeasuredVoltage(int subchannelIndex, float &value, int *err) override {
        if (subchannelIndex < AIN_1_SUBCHANNEL_INDEX || subchannelIndex > AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        auto &channel = ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX];

        if (channel.m_mode != MEASURE_MODE_VOLTAGE) {
            if (*err) {
                *err = SCPI_ERROR_EXECUTION_ERROR;
            }
            return false;
        }

        value = channel.m_value;
        return true;
    }

    void getVoltageStepValues(int subchannelIndex, StepValues *stepValues, bool calibrationMode) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getStepValues(stepValues);
        } else if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            stepValues->values = AOUT_DAC7563_ENCODER_STEP_VALUES;
            stepValues->count = sizeof(AOUT_DAC7563_ENCODER_STEP_VALUES) / sizeof(float);
            stepValues->unit = UNIT_VOLT;
        }
    }
    
    float getVoltageResolution(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getVoltageResolution();
        } else {
            return AOUT_DAC7563_RESOLUTION;
        }
    }

    float getVoltageMinValue(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getVoltageMinValue();
        } else {
            return AOUT_DAC7563_MIN;
        }    
    }

    float getVoltageMaxValue(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getVoltageMaxValue();
        } else {
            return AOUT_DAC7563_MAX;
        }    
    }

    bool isConstantVoltageMode(int subchannelIndex) override {
        return true;
    }

    bool isVoltageCalibrationExists(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf.state.calState;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            return aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf.state.calState;
        }
        return false;
    }

    bool isVoltageCalibrationEnabled(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf.state.calEnabled;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            return aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf.state.calEnabled;
        }
        return false;
    }
    
    void enableVoltageCalibration(int subchannelIndex, bool enabled) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf.state.calEnabled = enabled;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf.state.calEnabled = enabled;
        } else {
            return;
        }

        saveChannelCalibration(subchannelIndex, nullptr);
    }

    bool loadChannelCalibration(int subchannelIndex, int *err) override {
        assert(sizeof(CalConf) <= 64);

        CalConf *calConf;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        } else {
            return true;
        }

        if (!persist_conf::loadChannelCalibrationConfiguration(slotIndex, subchannelIndex, &calConf->header, sizeof(CalConf), CalConf::VERSION)) {
            calConf->clear();
        }

        return true;
    }

    bool saveChannelCalibration(int subchannelIndex, int *err) override {
        CalConf *calConf = nullptr;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        }

        if (calConf) {
            return persist_conf::saveChannelCalibrationConfiguration(slotIndex, subchannelIndex, &calConf->header, sizeof(CalConf), CalConf::VERSION);
        }

        if (err) {
            *err = SCPI_ERROR_HARDWARE_MISSING;
        }
        return false;
    }

    void startChannelCalibration(int subchannelIndex) override {
        CalConf *calConf = nullptr;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        }

        if (calConf) {
            calConf->state.ongoingCal = 1;
        }
    }
    
    void stopChannelCalibration(int subchannelIndex) override {
        CalConf *calConf = nullptr;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            calConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        }

        if (calConf) {
            calConf->state.ongoingCal = 0;
        }
    }

    void getDefaultCalibrationPoints(int subchannelIndex, CalibrationValueType type, unsigned int &numPoints, float *&points) override {
        static float AOUT_POINTS[] = { -9.9f, 9.9f };
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            numPoints = 2;
            points = AOUT_POINTS;
        } else {
            numPoints = 0;
            points = nullptr;
        }
    }

    bool getCalibrationConfiguration(int subchannelIndex, CalibrationConfiguration &calConf, int *err) override {
        CalConf *mioCalConf = nullptr;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            mioCalConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            mioCalConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        }

        if (mioCalConf) {
            memset(&calConf, 0, sizeof(CalibrationConfiguration));

            calConf.u.numPoints = mioCalConf->state.numPoints;
            for (unsigned i = 0; i < mioCalConf->state.numPoints; i++) {
                calConf.u.points[i].dac = mioCalConf->points[i].uncalValue;
                calConf.u.points[i].value = mioCalConf->points[i].calValue;
            }

            return true;
        }

        if (err) {
            *err = SCPI_ERROR_HARDWARE_MISSING;
        }
        return false;
    }
    
    bool setCalibrationConfiguration(int subchannelIndex, const CalibrationConfiguration &calConf, int *err) override {
        CalConf *mioCalConf = nullptr;

        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            mioCalConf = &aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].calConf;
        } else if (subchannelIndex >= AOUT_3_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_4_SUBCHANNEL_INDEX) {
            mioCalConf = &aoutDac7563Channels[subchannelIndex - AOUT_3_SUBCHANNEL_INDEX].calConf;
        }

        if (mioCalConf) {
            memset(mioCalConf, 0, sizeof(CalConf));

            mioCalConf->state.numPoints = MIN(calConf.u.numPoints, 4);
            for (unsigned i = 0; i < mioCalConf->state.numPoints; i++) {
                mioCalConf->points[i].uncalValue = calConf.u.points[i].dac;
                mioCalConf->points[i].calValue = calConf.u.points[i].value;
            }

            mioCalConf->state.calState = mioCalConf->state.numPoints >= 2;
            mioCalConf->state.calEnabled = mioCalConf->state.calState;

            return true;
        }

        if (err) {
            *err = SCPI_ERROR_HARDWARE_MISSING;
        }
        return false;
    }

    bool getCalibrationRemark(int subchannelIndex, const char *&calibrationRemark, int *err) override {
        calibrationRemark = "";
        return true;
    }

    bool getCalibrationDate(int subchannelIndex, uint32_t &calibrationDate, int *err) override {
        calibrationDate = 0;
        return true;
    }

    bool getCurrent(int subchannelIndex, float &value, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        value = aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_currentValue;

        return true;
    }

    bool setCurrent(int subchannelIndex, float value, int *err) override {
        if (subchannelIndex < AOUT_1_SUBCHANNEL_INDEX || subchannelIndex > AOUT_2_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].m_currentValue = value;

        return true;
    }

    bool getMeasuredCurrent(int subchannelIndex, float &value, int *err) override {
        if (subchannelIndex < AIN_1_SUBCHANNEL_INDEX || subchannelIndex > AIN_4_SUBCHANNEL_INDEX) {
            if (*err) {
                *err = SCPI_ERROR_HARDWARE_MISSING;
            }
            return false;
        }

        auto &channel = ainChannels[subchannelIndex - AIN_1_SUBCHANNEL_INDEX];

        if (channel.m_mode != MEASURE_MODE_CURRENT) {
            if (*err) {
                *err = SCPI_ERROR_EXECUTION_ERROR;
            }
            return false;
        }

        value = channel.m_value;
        return true;
    }

    void getCurrentStepValues(int subchannelIndex, StepValues *stepValues, bool calibrationMode) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getStepValues(stepValues);
        }
    }
    
    float getCurrentResolution(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getCurrentResolution();
        }
        return 0.0f;
    }

    float getCurrentMinValue(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getCurrentMinValue();
        }
        return 0.0f;
    }

    float getCurrentMaxValue(int subchannelIndex) override {
        if (subchannelIndex >= AOUT_1_SUBCHANNEL_INDEX && subchannelIndex <= AOUT_2_SUBCHANNEL_INDEX) {
            return aoutDac7760Channels[subchannelIndex - AOUT_1_SUBCHANNEL_INDEX].getCurrentMaxValue();
        }
        return 0.0f;    
    }

    bool isCurrentCalibrationExists(int subchannelIndex) override {
        return false;
    }

    bool isCurrentCalibrationEnabled(int subchannelIndex) override {
        return false;
    }
    
    void enableCurrentCalibration(int subchannelIndex, bool enabled) override {
    }
};

static Mio168Module g_mio168Module;
Module *g_module = &g_mio168Module;

////////////////////////////////////////////////////////////////////////////////

class DinConfigurationPage : public SetPage {
public:
    static int g_selectedChannelIndex;

    void pageAlloc() {
        Mio168Module *module = (Mio168Module *)g_slots[hmi::g_selectedSlotIndex];

        m_pinRanges = m_pinRangesOrig = module->dinChannel.m_pinRanges;
        m_pinSpeeds = m_pinSpeedsOrig = module->dinChannel.m_pinSpeeds;
    }

    int getDirty() { 
        return m_pinRanges != m_pinRangesOrig || m_pinSpeeds != m_pinSpeedsOrig;
    }

    void set() {
        if (getDirty()) {
            sendMessageToPsu((HighPriorityThreadMessage)PSU_MESSAGE_DIN_CONFIGURE, hmi::g_selectedSlotIndex);
        }

        popPage();
    }

    int getPinRange(int pin) {
        return m_pinRanges & (1 << pin) ? 1 : 0;
    }
    
    void setPinRange(int pin, int value) {
        if (value) {
            m_pinRanges |= 1 << pin;
        } else {
            m_pinRanges &= ~(1 << pin);
        }
    }

    int getPinSpeed(int pin) {
        return m_pinSpeeds & (1 << pin) ? 1 : 0;
    }

    void setPinSpeed(int pin, int value) {
        if (value) {
            m_pinSpeeds |= 1 << pin;
        } else {
            m_pinSpeeds &= ~(1 << pin);
        }
    }

    uint8_t m_pinRanges;
    uint8_t m_pinSpeeds;

private:
    uint8_t m_pinRangesOrig;
    uint8_t m_pinSpeedsOrig;
};

static DinConfigurationPage g_dinConfigurationPage;

////////////////////////////////////////////////////////////////////////////////

class AinConfigurationPage : public SetPage {
public:
    static int g_selectedChannelIndex;

    void pageAlloc() {
        Mio168Module *module = (Mio168Module *)g_slots[hmi::g_selectedSlotIndex];
        AinChannel &channel = module->ainChannels[g_selectedChannelIndex - AIN_1_SUBCHANNEL_INDEX];

        m_mode = m_modeOrig = channel.m_mode;
        m_range = m_rangeOrig = channel.m_range;
        m_tempSensorBias = m_tempSensorBiasOrig = channel.m_tempSensorBias;
    }

    int getDirty() { 
        return m_mode != m_modeOrig || m_range != m_rangeOrig || m_tempSensorBias != m_tempSensorBiasOrig;
    }

    void set() {
        if (getDirty()) {
            sendMessageToPsu((HighPriorityThreadMessage)PSU_MESSAGE_AIN_CONFIGURE, hmi::g_selectedSlotIndex);
        }

        popPage();
    }

    uint8_t m_mode;
    uint8_t m_range;
    uint8_t m_tempSensorBias;

private:
    uint8_t m_modeOrig;
    uint8_t m_rangeOrig;
    uint8_t m_tempSensorBiasOrig;
};

int AinConfigurationPage::g_selectedChannelIndex;
static AinConfigurationPage g_ainConfigurationPage;

////////////////////////////////////////////////////////////////////////////////

class AoutDac7760ConfigurationPage : public SetPage {
public:
    static int g_selectedChannelIndex;

    void pageAlloc() {
        Mio168Module *module = (Mio168Module *)g_slots[hmi::g_selectedSlotIndex];
        AoutDac7760Channel &channel = module->aoutDac7760Channels[g_selectedChannelIndex - AOUT_1_SUBCHANNEL_INDEX];

        m_outputEnabled = m_outputEnabledOrig = channel.m_outputEnabled;
        m_mode = m_modeOrig = channel.m_mode;
        m_currentRange = m_currentRangeOrig = channel.m_currentRange;
        m_voltageRange = m_voltageRangeOrig = channel.m_voltageRange;
    }

    int getDirty() { 
        return m_outputEnabled != m_outputEnabledOrig ||
            m_mode != m_modeOrig ||
            m_currentRange != m_currentRangeOrig ||
            m_voltageRange != m_voltageRangeOrig;
    }

    void set() {
        if (getDirty()) {
            sendMessageToPsu((HighPriorityThreadMessage)PSU_MESSAGE_AOUT_DAC7760_CONFIGURE, hmi::g_selectedSlotIndex);
        }

        popPage();
    }

    bool m_outputEnabled;
    uint8_t m_mode;
    uint8_t m_currentRange;
    uint8_t m_voltageRange;

private:
    bool m_outputEnabledOrig;
    uint8_t m_modeOrig;
    uint8_t m_currentRangeOrig;
    uint8_t m_voltageRangeOrig;
};

int AoutDac7760ConfigurationPage::g_selectedChannelIndex;
static AoutDac7760ConfigurationPage g_aoutDac7760ConfigurationPage;

////////////////////////////////////////////////////////////////////////////////

class AoutDac7563ConfigurationPage : public SetPage {
public:
    static int g_selectedChannelIndex;

    void pageAlloc() {
    }

    int getDirty() { 
        return false;
    }

    void set() {
        popPage();
    }

private:
};

int AoutDac7563ConfigurationPage::g_selectedChannelIndex;
static AoutDac7563ConfigurationPage g_aoutDac7563ConfigurationPage;

////////////////////////////////////////////////////////////////////////////////

Page *Mio168Module::getPageFromId(int pageId) {
    if (pageId == PAGE_ID_DIB_MIO168_DIN_CONFIGURATION) {
        return &g_dinConfigurationPage;
    } else if (pageId == PAGE_ID_DIB_MIO168_AIN_CONFIGURATION) {
        return &g_ainConfigurationPage;
    } else if (pageId == PAGE_ID_DIB_MIO168_AOUT_DAC7760_CONFIGURATION) {
        return &g_aoutDac7760ConfigurationPage;
    } else if (pageId == PAGE_ID_DIB_MIO168_AOUT_DAC7563_CONFIGURATION) {
        return &g_aoutDac7563ConfigurationPage;
    }
    return nullptr;
}

void Mio168Module::onHighPriorityThreadMessage(uint8_t type, uint32_t param) {
    if (type == PSU_MESSAGE_DIN_CONFIGURE) {
        dinChannel.m_pinRanges = g_dinConfigurationPage.m_pinRanges;
        dinChannel.m_pinSpeeds = g_dinConfigurationPage.m_pinSpeeds;
    } else if (type == PSU_MESSAGE_AIN_CONFIGURE) {
        AinChannel &channel = ainChannels[AinConfigurationPage::g_selectedChannelIndex - AIN_1_SUBCHANNEL_INDEX];
        channel.m_mode = g_ainConfigurationPage.m_mode;
        channel.m_range = g_ainConfigurationPage.m_range;
        channel.m_tempSensorBias = g_ainConfigurationPage.m_tempSensorBias;
    } else if (type == PSU_MESSAGE_AOUT_DAC7760_CONFIGURE) {
        AoutDac7760Channel &channel = aoutDac7760Channels[AoutDac7760ConfigurationPage::g_selectedChannelIndex - AOUT_1_SUBCHANNEL_INDEX];
        channel.m_outputEnabled = g_aoutDac7760ConfigurationPage.m_outputEnabled;
        channel.m_mode = g_aoutDac7760ConfigurationPage.m_mode;
        channel.m_currentRange = g_aoutDac7760ConfigurationPage.m_currentRange;
        channel.m_voltageRange = g_aoutDac7760ConfigurationPage.m_voltageRange;

        if (channel.getValue() < channel.getMinValue()) {
            channel.setValue(channel.getMinValue());
        } else if (channel.getValue() > channel.getMaxValue()) {
            channel.setValue(channel.getMaxValue());
        }
    }
}

} // namespace dib_mio168

namespace gui {

using namespace dib_mio168;

void data_dib_mio168_din_pins(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 8;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 8 + value.getInt();
    }
}

void data_dib_mio168_din_pins_1_4(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 4;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 8 + value.getInt();
    }
}

void data_dib_mio168_din_pins_5_8(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 4;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 8 + 4 + value.getInt();
    }
}

void data_dib_mio168_din_no(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = cursor % 8 + 1;
    }
}

void data_dib_mio168_din_state(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        auto mio168Module = (Mio168Module *)g_slots[cursor / 8];
        value = mio168Module->dinChannel.getPinState(cursor % 8) ? 1 : 0;
    }
}

static int g_pin;

static EnumItem g_dinRangeEnumDefinition[] = {
    { 0, "Low" },
    { 1, "High" },
    { 0, 0 }
};

void data_dib_mio168_din_range(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = g_dinConfigurationPage.getPinRange(cursor % 8);
    }
}

void onSetPinRanges(uint16_t value) {
    popPage();
    g_dinConfigurationPage.setPinRange(g_pin, value);
}

void action_dib_mio168_din_select_range() {
    g_pin = getFoundWidgetAtDown().cursor % 8;
    pushSelectFromEnumPage(g_dinRangeEnumDefinition, g_dinConfigurationPage.getPinRange(g_pin), nullptr, onSetPinRanges);
}

void data_dib_mio168_din_has_speed(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = cursor % 8 < 2;
    }
}

static EnumItem g_dinSpeedEnumDefinition[] = {
    { 0, "Fast" },
    { 1, "Slow" },
    { 0, 0 }
};

void data_dib_mio168_din_speed(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = g_dinConfigurationPage.getPinSpeed(cursor % 8);
    }
}

void onSetPinSpeeds(uint16_t value) {
    popPage();
    g_dinConfigurationPage.setPinSpeed(g_pin, value);
}

void action_dib_mio168_din_select_speed() {
    g_pin = getFoundWidgetAtDown().cursor % 8;
    pushSelectFromEnumPage(g_dinSpeedEnumDefinition, g_dinConfigurationPage.getPinSpeed(g_pin), nullptr, onSetPinSpeeds);
}

void action_dib_mio168_din_show_configuration() {
    if (getActivePageId() == PAGE_ID_MAIN) {
        hmi::selectSlot(getFoundWidgetAtDown().cursor);
    }
    pushPage(PAGE_ID_DIB_MIO168_DIN_CONFIGURATION);
}

////////////////////////////////////////////////////////////////////////////////

void data_dib_mio168_dout_pins(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 8;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 8 + value.getInt();
    }
}

void data_dib_mio168_dout_no(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = cursor % 8 + 1;
    }
}

void data_dib_mio168_dout_state(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        auto mio168Module = (Mio168Module *)g_slots[cursor / 8];
        value = mio168Module->doutChannel.getPinState(cursor % 8);
    }
}

void action_dib_mio168_dout_toggle_state() {
    int cursor = getFoundWidgetAtDown().cursor;
    auto mio168Module = (Mio168Module *)g_slots[cursor / 8];
    int pin = cursor % 8;
    mio168Module->doutChannel.setPinState(pin, !mio168Module->doutChannel.getPinState(pin));
}

////////////////////////////////////////////////////////////////////////////////

void data_dib_mio168_ain_channels(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 4;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 4 + value.getInt();
    }
}

void data_dib_mio168_ain_label(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        static const char *labels[4] = { "AIN1", "AIN2", "AIN3", "AIN4" };
        value = labels[cursor % 4];
    }
}

void data_dib_mio168_ain_value(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        auto mio168Module = (Mio168Module *)g_slots[cursor / 4];
        auto &channel = mio168Module->ainChannels[cursor % 4];
        if (channel.m_mode == MEASURE_MODE_OPEN) {
            value = "-";
        } else {
            value = MakeValue(roundPrec(channel.m_value, channel.getResolution()), channel.m_mode == MEASURE_MODE_VOLTAGE ? UNIT_VOLT : UNIT_AMPER);
        }
    }
}

static EnumItem g_ainModeEnumDefinition[] = {
    { MEASURE_MODE_CURRENT, "Current" },
    { MEASURE_MODE_VOLTAGE, "Voltage" },
    { MEASURE_MODE_OPEN, "Open" },
    { 0, 0 }
};

void data_dib_mio168_ain_mode(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = g_ainConfigurationPage.m_mode;
    }
}

void onSetAinMode(uint16_t value) {
    popPage();
    g_ainConfigurationPage.m_mode = (uint8_t)value;
}

void action_dib_mio168_ain_select_mode() {
    pushSelectFromEnumPage(g_ainModeEnumDefinition, g_ainConfigurationPage.m_mode, nullptr, onSetAinMode);
}

static EnumItem g_ainRangeEnumDefinition[] = {
    { 0, "\xbd""10.24 V" },
    { 1, "\xbd""5.12 V" },
    { 2, "\xbd""2.56 V" },
    { 3, "\xbd""1.28 V" },
    { 11, "\xbd""0.64 V" },
    { 5, "0 V to 10.24 V" },
    { 6, "0 V to 5.12 V" },
    { 7, "0 V to 2.56 V" },
    { 15, "0 V to 1.28 V" },
    { 0, 0 }
};

void data_dib_mio168_ain_range(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = getWidgetLabel(g_ainRangeEnumDefinition, g_ainConfigurationPage.m_range);
    }
}

void onSetAinRange(uint16_t value) {
    popPage();
    g_ainConfigurationPage.m_range = (uint8_t)value;
}

void action_dib_mio168_ain_select_range() {
    pushSelectFromEnumPage(g_ainRangeEnumDefinition, g_ainConfigurationPage.m_range, nullptr, onSetAinRange);
}

void data_dib_mio168_ain_has_temp_sensor_bias_feature(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = AinConfigurationPage::g_selectedChannelIndex < AIN_3_SUBCHANNEL_INDEX ? 1 : 0;
    }
}

void data_dib_mio168_ain_temp_sensor_bias(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = g_ainConfigurationPage.m_tempSensorBias;
    }
}

void action_dib_mio168_ain_toggle_temp_sensor_bias() {
    g_ainConfigurationPage.m_tempSensorBias = !g_ainConfigurationPage.m_tempSensorBias;
}

void action_dib_mio168_ain_show_configuration() {
    int cursor = getFoundWidgetAtDown().cursor;
    
    int slotIndex = cursor / 4;
    hmi::selectSlot(slotIndex);
    
    int ainChannelIndex = cursor % 4;
    AinConfigurationPage::g_selectedChannelIndex = AIN_1_SUBCHANNEL_INDEX + ainChannelIndex;
    pushPage(PAGE_ID_DIB_MIO168_AIN_CONFIGURATION);
}

////////////////////////////////////////////////////////////////////////////////

static const char *aoutLabels[4] = { "AO1", "AO2", "AO3", "AO4" };

void data_dib_mio168_aout_channels(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 4;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 4 + value.getInt();
    }
}

void data_dib_mio168_aout_label(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {

        int aoutChannelIndex;

        AoutDac7760ConfigurationPage *page = (AoutDac7760ConfigurationPage *)getPage(PAGE_ID_DIB_MIO168_AOUT_DAC7760_CONFIGURATION);
        if (page) {
            aoutChannelIndex = AoutDac7760ConfigurationPage::g_selectedChannelIndex - AOUT_1_SUBCHANNEL_INDEX;
        } else {
            aoutChannelIndex = cursor % 4;
        }

        value = aoutLabels[aoutChannelIndex];
    }
}

void data_dib_mio168_aout_value(DataOperationEnum operation, Cursor cursor, Value &value) {
    int slotIndex = cursor / 4;
    int aoutChannelIndex = cursor % 4;

    if (operation == DATA_OPERATION_GET) {
        bool focused = g_focusCursor == cursor && g_focusDataId == DATA_ID_DIB_MIO168_AOUT_VALUE;
        if (focused && g_focusEditValue.getType() != VALUE_TYPE_NONE) {
            value = g_focusEditValue;
        } else if (focused && getActivePageId() == PAGE_ID_EDIT_MODE_KEYPAD && edit_mode_keypad::g_keypad->isEditing()) {
            data_keypad_text(operation, cursor, value);
        } else {
            if (aoutChannelIndex < 2) {
                auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
                value = MakeValue(channel.getValue(), channel.getUnit());
            } else {
                value = MakeValue(((Mio168Module *)g_slots[slotIndex])->aoutDac7563Channels[aoutChannelIndex - 2].m_value, UNIT_VOLT);
            }
        }
    } else if (operation == DATA_OPERATION_GET_MIN) {
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            value = MakeValue(channel.getMinValue(), channel.getUnit());
        } else {
            value = MakeValue(AOUT_DAC7563_MIN, UNIT_VOLT);
        }
    } else if (operation == DATA_OPERATION_GET_MAX) {
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            value = MakeValue(channel.getMaxValue(), channel.getUnit());
        } else {
            value = MakeValue(AOUT_DAC7563_MAX, UNIT_VOLT);
        }
    } else if (operation == DATA_OPERATION_GET_NAME) {
        value = aoutLabels[aoutChannelIndex];
    } else if (operation == DATA_OPERATION_GET_UNIT) {
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            value = channel.getUnit();
        } else {
            value = UNIT_VOLT;
        }
    } else if (operation == DATA_OPERATION_GET_ENCODER_PRECISION) {
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            value = Value(channel.getResolution(), channel.getUnit());
        } else {
            value = Value(AOUT_DAC7563_RESOLUTION, UNIT_VOLT);
        }
    } 
    else if (operation == DATA_OPERATION_GET_ENCODER_STEP_VALUES) {
        StepValues *stepValues = value.getStepValues();
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            channel.getStepValues(stepValues);
        } else {
            stepValues->values = AOUT_DAC7563_ENCODER_STEP_VALUES;
            stepValues->count = sizeof(AOUT_DAC7563_ENCODER_STEP_VALUES) / sizeof(float);
            stepValues->unit = UNIT_VOLT;
        }
        value = 1;
    } else if (operation == DATA_OPERATION_SET) {
        if (aoutChannelIndex < 2) {
            auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
            channel.setValue(roundPrec(value.getFloat(), channel.getResolution()));
        } else {
            ((Mio168Module *)g_slots[slotIndex])->aoutDac7563Channels[aoutChannelIndex - 2].m_value = roundPrec(value.getFloat(), AOUT_DAC7563_RESOLUTION);
        }
    }
}

void data_dib_mio168_aout_output_enabled(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        AoutDac7760ConfigurationPage *page = (AoutDac7760ConfigurationPage *)getPage(PAGE_ID_DIB_MIO168_AOUT_DAC7760_CONFIGURATION);
        if (page) {
            value = g_aoutDac7760ConfigurationPage.m_outputEnabled;
        } else {
            int slotIndex = cursor / 4;
            int aoutChannelIndex = cursor % 4;
            if (aoutChannelIndex < 2) {
                auto &channel = ((Mio168Module *)g_slots[slotIndex])->aoutDac7760Channels[aoutChannelIndex];
                value = channel.m_outputEnabled;
            } else {
                value = 0;
            }
        }
    }
}

static EnumItem g_aoutOutputModeEnumDefinition[] = {
    { SOURCE_MODE_CURRENT, "Current" },
    { SOURCE_MODE_VOLTAGE, "Voltage" },
    { 0, 0 }
};

void data_dib_mio168_aout_output_mode(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = g_aoutDac7760ConfigurationPage.m_mode;
    }
}

void onSetOutputMode(uint16_t value) {
    popPage();
    g_aoutDac7760ConfigurationPage.m_mode = (SourceMode)value;
}

void action_dib_mio168_aout_select_output_mode() {
    pushSelectFromEnumPage(g_aoutOutputModeEnumDefinition, g_aoutDac7760ConfigurationPage.m_mode, nullptr, onSetOutputMode);
}

static EnumItem g_aoutVoltageRangeEnumDefinition[] = {
    { 0, "0 V to +5 V" },
    { 1, "0 V to +10 V" },
    { 2, "\xbd""5 V" },
    { 3, "\xbd""10 V" },
    { 0, 0 }
};

void data_dib_mio168_aout_voltage_range(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = getWidgetLabel(g_aoutVoltageRangeEnumDefinition, g_aoutDac7760ConfigurationPage.m_voltageRange);
    }
}

void onSetVoltageRange(uint16_t value) {
    popPage();
    g_aoutDac7760ConfigurationPage.m_voltageRange = (uint8_t)value;
}

void action_dib_mio168_aout_select_voltage_range() {
    pushSelectFromEnumPage(g_aoutVoltageRangeEnumDefinition, g_aoutDac7760ConfigurationPage.m_voltageRange, nullptr, onSetVoltageRange);
}

static EnumItem g_aoutCurrentRangeEnumDefinition[] = {
    { 5, "4 mA to 20 mA" },
    { 6, "0 mA to 20 mA" },
    { 7, "0 mA to 24 mA" },
    { 0, 0 }
};

void data_dib_mio168_aout_current_range(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = getWidgetLabel(g_aoutCurrentRangeEnumDefinition, g_aoutDac7760ConfigurationPage.m_currentRange);
    }
}

void onSetCurrentRange(uint16_t value) {
    popPage();
    g_aoutDac7760ConfigurationPage.m_currentRange = (uint8_t)value;
}

void action_dib_mio168_aout_select_current_range() {
    pushSelectFromEnumPage(g_aoutCurrentRangeEnumDefinition, g_aoutDac7760ConfigurationPage.m_currentRange, nullptr, onSetCurrentRange);
}

void action_dib_mio168_aout_toggle_output_enabled() {
    g_aoutDac7760ConfigurationPage.m_outputEnabled = !g_aoutDac7760ConfigurationPage.m_outputEnabled;
}

void action_dib_mio168_aout_show_configuration() {
    int cursor = getFoundWidgetAtDown().cursor;
    
    int slotIndex = cursor / 4;
    hmi::selectSlot(slotIndex);
    
    int aoutChannelIndex = cursor % 4;
    if (aoutChannelIndex < 2) {
        AoutDac7760ConfigurationPage::g_selectedChannelIndex = AOUT_1_SUBCHANNEL_INDEX + aoutChannelIndex;
        pushPage(PAGE_ID_DIB_MIO168_AOUT_DAC7760_CONFIGURATION);
    } else {
        AoutDac7563ConfigurationPage::g_selectedChannelIndex = AOUT_1_SUBCHANNEL_INDEX + aoutChannelIndex;
        pushPage(PAGE_ID_DIB_MIO168_AOUT_DAC7563_CONFIGURATION);
    }
}

void data_dib_mio168_aout_channel_has_settings(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        value = cursor % 4 < 2 ? 1 : 0;
    }
}

////////////////////////////////////////////////////////////////////////////////

void data_dib_mio168_pwm_channels(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 2;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 2 + value.getInt();
    }
}

void data_dib_mio168_pwm_label(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        static const char *labels[2] = { "P1", "P2" };
        static const char *labels2Col[2] = { "PWM1", "PWM2" };
        value = g_isCol2Mode || persist_conf::isMaxView() ? labels2Col[cursor % 2] : labels[cursor % 2];
    }
}

void data_dib_mio168_pwm_freq(DataOperationEnum operation, Cursor cursor, Value &value) {
    int slotIndex = cursor / 2;
    int pwmChannelIndex = cursor % 2;

    if (operation == DATA_OPERATION_GET) {
        bool focused = g_focusCursor == cursor && g_focusDataId == DATA_ID_DIB_MIO168_PWM_FREQ;
        if (focused && g_focusEditValue.getType() != VALUE_TYPE_NONE) {
            value = g_focusEditValue;
        } else if (focused && getActivePageId() == PAGE_ID_EDIT_MODE_KEYPAD && edit_mode_keypad::g_keypad->isEditing()) {
            data_keypad_text(operation, cursor, value);
        } else {
            value = MakeValue(((Mio168Module *)g_slots[slotIndex])->pwmChannels[pwmChannelIndex].m_freq, UNIT_HERTZ);
        }
    } else if (operation == DATA_OPERATION_GET_ALLOW_ZERO) {
        value = 1;
    } else if (operation == DATA_OPERATION_GET_MIN) {
        value = MakeValue(PWM_MIN_FREQUENCY, UNIT_HERTZ);
    } else if (operation == DATA_OPERATION_GET_MAX) {
        value = MakeValue(PWM_MAX_FREQUENCY, UNIT_HERTZ);
    } else if (operation == DATA_OPERATION_GET_NAME) {
        value = "PWM frequency";
    } else if (operation == DATA_OPERATION_GET_UNIT) {
        value = UNIT_HERTZ;
    } else if (operation == DATA_OPERATION_GET_ENCODER_STEP) {
        float fvalue = ((Mio168Module *)g_slots[slotIndex])->pwmChannels[pwmChannelIndex].m_freq;
        value = Value(MAX(powf(10.0f, floorf(log10f(fabsf(fvalue))) - 1), 0.001f), UNIT_HERTZ);
    } else if (operation == DATA_OPERATION_GET_ENCODER_STEP_VALUES) {
        static float values[] = { 1.0f, 100.0f, 1000.0f, 10000.0f };
        StepValues *stepValues = value.getStepValues();
        stepValues->values = values;
        stepValues->count = sizeof(values) / sizeof(float);
        stepValues->unit = UNIT_HERTZ;
        value = 1;
    } else if (operation == DATA_OPERATION_SET) {
        ((Mio168Module *)g_slots[slotIndex])->pwmChannels[pwmChannelIndex].m_freq = value.getFloat();
    }
}

void data_dib_mio168_pwm_duty(DataOperationEnum operation, Cursor cursor, Value &value) {
    int slotIndex = cursor / 2;
    int pwmChannelIndex = cursor % 2;
    
    if (operation == DATA_OPERATION_GET) {
        bool focused = g_focusCursor == cursor && g_focusDataId == DATA_ID_DIB_MIO168_PWM_DUTY;
        if (focused && g_focusEditValue.getType() != VALUE_TYPE_NONE) {
            value = g_focusEditValue;
        } else if (focused && getActivePageId() == PAGE_ID_EDIT_MODE_KEYPAD && edit_mode_keypad::g_keypad->isEditing()) {
            data_keypad_text(operation, cursor, value);
        } else {
            value = MakeValue(((Mio168Module *)g_slots[slotIndex])->pwmChannels[pwmChannelIndex].m_duty, UNIT_PERCENT);
        }
    } else if (operation == DATA_OPERATION_GET_MIN) {
        value = MakeValue(0.0f, UNIT_PERCENT);
    } else if (operation == DATA_OPERATION_GET_MAX) {
        value = MakeValue(100.0f, UNIT_PERCENT);
    } else if (operation == DATA_OPERATION_GET_NAME) {
        value = "PWM duty cycle";
    } else if (operation == DATA_OPERATION_GET_UNIT) {
        value = UNIT_PERCENT;
    } else if (operation == DATA_OPERATION_GET_ENCODER_STEP) {
        value = Value(1.0f, UNIT_HERTZ);
    } else if (operation == DATA_OPERATION_GET_ENCODER_STEP_VALUES) {
        static float values[] = { 0.1f, 0.5f, 1.0f, 5.0f };
        StepValues *stepValues = value.getStepValues();
        stepValues->values = values;
        stepValues->count = sizeof(values) / sizeof(float);
        stepValues->unit = UNIT_PERCENT;
        value = 1;
    } else if (operation == DATA_OPERATION_SET) {
        ((Mio168Module *)g_slots[slotIndex])->pwmChannels[pwmChannelIndex].m_duty = value.getFloat();
    } 
}

////////////////////////////////////////////////////////////////////////////////

void action_dib_mio168_show_info() {
    pushPage(PAGE_ID_DIB_MIO168_INFO);
}

void action_dib_mio168_show_calibration() {
    if (getPage(PAGE_ID_DIB_MIO168_AOUT_DAC7760_CONFIGURATION)) {
        hmi::g_selectedSubchannelIndex = g_aoutDac7760ConfigurationPage.g_selectedChannelIndex;
    } else if (getPage(PAGE_ID_DIB_MIO168_AOUT_DAC7563_CONFIGURATION)) {
        hmi::g_selectedSubchannelIndex = g_aoutDac7563ConfigurationPage.g_selectedChannelIndex;
    } else {
        hmi::g_selectedSubchannelIndex = g_ainConfigurationPage.g_selectedChannelIndex;
    }

    calibration::g_viewer.start(hmi::g_selectedSlotIndex, hmi::g_selectedSubchannelIndex);
    
    pushPage(PAGE_ID_CH_SETTINGS_CALIBRATION);
}


} // namespace gui

} // namespace eez
