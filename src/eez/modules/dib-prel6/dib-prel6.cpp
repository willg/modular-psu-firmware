/*
 * EEZ DIB PREL6
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

#include <assert.h>
#include <stdlib.h>
#include <memory.h>

#if defined(EEZ_PLATFORM_STM32)
#include <spi.h>
#include <eez/platform/stm32/spi.h>
#endif

#include "eez/debug.h"
#include "eez/firmware.h"
#include "eez/system.h"
#include "eez/hmi.h"
#include "eez/gui/document.h"
#include "eez/modules/psu/psu.h"
#include "eez/modules/psu/event_queue.h"
#include "eez/modules/psu/profile.h"
#include "eez/modules/psu/gui/psu.h"
#include "eez/modules/bp3c/comm.h"

#include "scpi/scpi.h"

#include "./dib-prel6.h"

using namespace eez::psu;
using namespace eez::psu::gui;
using namespace eez::gui;

namespace eez {
namespace dib_prel6 {

static const uint16_t MODULE_REVISION_R1B2  = 0x0102;

static const uint32_t REFRESH_TIME_MS = 250;
static const uint32_t TIMEOUT_TIME_MS = 350;
static const uint32_t TIMEOUT_UNTIL_OUT_OF_SYNC_MS = 10000;

////////////////////////////////////////////////////////////////////////////////

enum Command {
	COMMAND_NONE       = 0x113B3759,
    COMMAND_GET_INFO   = 0x21EC18D4,
    COMMAND_GET_STATE  = 0x3C1D2EF4,
    COMMAND_SET_PARAMS = 0x4B723BFF
};

struct SetParams {
	uint8_t relayStates;
};

struct Request {
	uint32_t command;

    union {
    	SetParams setParams;
    };
};

struct Response {
	uint32_t command;

    union {
        struct {
            uint8_t firmwareMajorVersion;
            uint8_t firmwareMinorVersion;
            uint32_t idw0;
            uint32_t idw1;
            uint32_t idw2;
        } getInfo;

        struct {
            uint32_t tickCount;
        } getState;

        struct {
            uint8_t result; // 1 - success, 0 - failure
        } setParams;
    };
};

////////////////////////////////////////////////////////////////////////////////

#define BUFFER_SIZE 20

////////////////////////////////////////////////////////////////////////////////

struct Prel6Module : public Module {
public:
    TestResult testResult = TEST_NONE;

    bool synchronized = false;

    uint32_t input[(BUFFER_SIZE + 3) / 4 + 1];
    uint32_t output[(BUFFER_SIZE + 3) / 4];

    bool spiReady = false;
    bool spiDmaTransferCompleted = false;
    int spiDmaTransferStatus;

    uint32_t lastTransferTime = 0;
	SetParams lastTransferredParams;

	struct CommandDef {
		uint32_t command;
		void (Prel6Module::*fillRequest)(Request &request);
		void (Prel6Module::*done)(Response &response, bool isSuccess);
	};

    static const CommandDef getInfo_command;
    static const CommandDef getState_command;
    static const CommandDef setParams_command;

    enum State {
        STATE_IDLE,

        STATE_WAIT_SLAVE_READY_BEFORE_REQUEST,
        STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_REQUEST,

        STATE_WAIT_SLAVE_READY_BEFORE_RESPONSE,
        STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_RESPONSE
    };

    enum Event {
        EVENT_SLAVE_READY,
        EVENT_DMA_TRANSFER_COMPLETED,
        EVENT_DMA_TRANSFER_FAILED,
        EVENT_TIMEOUT
    };

    const CommandDef *currentCommand;
    uint32_t refreshStartTime;
    State state;
    uint32_t lastStateTransitionTime;
    uint32_t lastRefreshTime;
    int retry;

    uint8_t relayStates = 0;

    Prel6Module() {
        assert(sizeof(Request) <= BUFFER_SIZE);
        assert(sizeof(Response) <= BUFFER_SIZE);

        moduleType = MODULE_TYPE_DIB_PREL6;
        moduleName = "PREL6";
        moduleBrand = "Envox";
        latestModuleRevision = MODULE_REVISION_R1B2;
        flashMethod = FLASH_METHOD_STM32_BOOTLOADER_UART;
#if defined(EEZ_PLATFORM_STM32)        
        spiBaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
        spiCrcCalculationEnable = true;
#else
        spiBaudRatePrescaler = 0;
        spiCrcCalculationEnable = false;
#endif
        numPowerChannels = 0;
        numOtherChannels = 6;
        isResyncSupported = true;

        memset(input, 0, sizeof(input));
        memset(output, 0, sizeof(output));
    }

    Module *createModule() override {
        return new Prel6Module();
    }

    TestResult getTestResult() override {
        return testResult;
    }

    void initChannels() override {
        if (!synchronized) {
			executeCommand(&getInfo_command);

			if (!g_isBooted) {
				while (state != STATE_IDLE) {
#if defined(EEZ_PLATFORM_STM32)
					if (HAL_GPIO_ReadPin(spi::IRQ_GPIO_Port[slotIndex], spi::IRQ_Pin[slotIndex]) == GPIO_PIN_RESET) {
                        osDelay(1);
                        if (HAL_GPIO_ReadPin(spi::IRQ_GPIO_Port[slotIndex], spi::IRQ_Pin[slotIndex]) == GPIO_PIN_RESET) {
						    spiReady = true;
                        }
					}
#endif
					tick();
				}
			}

            memset(&lastTransferredParams, 0, sizeof(SetParams));
        }
    }

    ////////////////////////////////////////

    void Command_GetInfo_Done(Response &response, bool isSuccess) {
        if (isSuccess) {
            auto &data = response.getInfo;

            firmwareMajorVersion = data.firmwareMajorVersion;
            firmwareMinorVersion = data.firmwareMinorVersion;
            idw0 = data.idw0;
            idw1 = data.idw1;
            idw2 = data.idw2;

			firmwareVersionAcquired = true;

            synchronized = true;
            testResult = TEST_OK;
        } else {
            synchronized = false;
            if (firmwareInstalled) {
                event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_SYNC_ERROR + slotIndex);
            }
            testResult = TEST_FAILED;
        }
    }

    ////////////////////////////////////////

    void Command_GetState_Done(Response &response, bool isSuccess) {
        if (isSuccess) {
            lastRefreshTime = refreshStartTime;
        }
    }

    ////////////////////////////////////////

	void fillSetParams(SetParams &params) {
		memset(&params, 0, sizeof(SetParams));

		params.relayStates = relayStates;
	}

    void Command_SetParams_FillRequest(Request &request) {
        fillSetParams(request.setParams);
    }

    void Command_SetParams_Done(Response &response, bool isSuccess) {
        if (isSuccess) {
            auto &data = response.setParams;
            if (data.result) {
                memcpy(&lastTransferredParams, &((Request *)output)->setParams, sizeof(SetParams));
            }
        }
    }

    ////////////////////////////////////////

	uint32_t getRefreshTimeMs() {
		return REFRESH_TIME_MS;
	}

    void executeCommand(const CommandDef *command) {
        currentCommand = command;
        retry = 0;
        setState(STATE_WAIT_SLAVE_READY_BEFORE_REQUEST);
	}

    bool startCommand() {
		Request &request = *(Request *)output;

		request.command = currentCommand->command;

		if (currentCommand->fillRequest) {
			(this->*currentCommand->fillRequest)(request);
		}
        spiDmaTransferCompleted = false;
        auto status = bp3c::comm::transferDMA(slotIndex, (uint8_t *)output, (uint8_t *)input, BUFFER_SIZE);
        return status == bp3c::comm::TRANSFER_STATUS_OK;
    }

    bool getCommandResult() {
        Request &request = *(Request *)output;
        request.command = COMMAND_NONE;
        spiDmaTransferCompleted = false;
        auto status = bp3c::comm::transferDMA(slotIndex, (uint8_t *)output, (uint8_t *)input, BUFFER_SIZE);
        return status == bp3c::comm::TRANSFER_STATUS_OK;
    }

    bool isCommandResponse() {
        Response &response = *(Response *)input;
        return response.command == (0x8000 | currentCommand->command);
    }

    void doRetry() {
    	bp3c::comm::abortTransfer(slotIndex);
        
        static const int NUM_REQUEST_RETRIES = 100;

        if (++retry < NUM_REQUEST_RETRIES) {
            // try again
            setState(STATE_WAIT_SLAVE_READY_BEFORE_REQUEST);
        } else {
            // give up
            doCommandDone(false);
        }
    }

    void doCommandDone(bool isSuccess) {
        if (isSuccess) {
            lastTransferTime = millis();
        }

		if (currentCommand->done) {
			Response &response = *(Response *)input;
			(this->*currentCommand->done)(response, isSuccess);
		}

		currentCommand = nullptr;
        setState(STATE_IDLE);
    }

    void setState(State newState) {
        state = newState;
        lastStateTransitionTime = millis();
    }

    void stateTransition(Event event) {
    	if (event == EVENT_DMA_TRANSFER_COMPLETED) {
            numCrcErrors = 0;
            numTransferErrors = 0;
    	}
 
        if (state == STATE_WAIT_SLAVE_READY_BEFORE_REQUEST) {
            if (event == EVENT_TIMEOUT) {
				doRetry();
            } else if (event == EVENT_SLAVE_READY) {
                if (startCommand()) {
                    setState(STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_REQUEST);
                } else {
                    doRetry();
                }
            }
        } else if (state == STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_REQUEST) {
            if (event == EVENT_TIMEOUT) {
                doRetry();
            } else if (event == EVENT_DMA_TRANSFER_COMPLETED) {
                setState(STATE_WAIT_SLAVE_READY_BEFORE_RESPONSE);
            } else if (event == EVENT_DMA_TRANSFER_FAILED) {
                doRetry();
            }
        } else if (state == STATE_WAIT_SLAVE_READY_BEFORE_RESPONSE) {
            if (event == EVENT_TIMEOUT) {
				doRetry();
            } else if (event == EVENT_SLAVE_READY) {
                if (getCommandResult()) {
                    setState(STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_RESPONSE);
                } else {
                    doRetry();
                }
            }
        } else if (state == STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_RESPONSE) {
            if (event == EVENT_TIMEOUT) {
                doRetry();
            } else if (event == EVENT_DMA_TRANSFER_COMPLETED) {
                if (isCommandResponse()) {
                    doCommandDone(true);
                } else {
                    doRetry();
                }
            } else if (event == EVENT_DMA_TRANSFER_FAILED) {
                doRetry();
            }
        } 
    }

    int numCrcErrors = 0;
    int numTransferErrors = 0;

    void reportDmaTransferFailed(int status) {
        if (status == bp3c::comm::TRANSFER_STATUS_CRC_ERROR) {
            numCrcErrors++;
            if (numCrcErrors >= 100) {
                event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_CRC_CHECK_ERROR + slotIndex);
                synchronized = false;
                testResult = TEST_FAILED;
            }
            //else if (numCrcErrors > 5) {
            //    DebugTrace("Slot %d CRC error no. %d\n", slotIndex + 1, numCrcErrors);
            //}
        } else {
            numTransferErrors++;
            if (numTransferErrors >= 100) {
                event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_SYNC_ERROR + slotIndex);
                synchronized = false;
                testResult = TEST_FAILED;
            }
            //else if (numTransferErrors > 5) {
            //    DebugTrace("Slot %d SPI transfer error %d no. %d\n", slotIndex + 1, status, numTransferErrors);
            //}
        }
    }

    void tick() override {
        if (currentCommand) {
            if (!synchronized && currentCommand->command != COMMAND_GET_INFO) {
                doCommandDone(false);
            } else {
                if (
                    state == STATE_WAIT_SLAVE_READY_BEFORE_REQUEST ||
                    state == STATE_WAIT_SLAVE_READY_BEFORE_RESPONSE
                ) {
                    #if defined(EEZ_PLATFORM_STM32)
                	if (spiReady) {
                        spiReady = false;
                		stateTransition(EVENT_SLAVE_READY);
                	}
                    #endif

                    #if defined(EEZ_PLATFORM_SIMULATOR)
                    stateTransition(EVENT_SLAVE_READY);
                    #endif
                } 
                
                if (
                    state == STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_REQUEST ||
                    state == STATE_WAIT_DMA_TRANSFER_COMPLETED_FOR_RESPONSE
                ) {
                
                    #if defined(EEZ_PLATFORM_STM32)
                    if (spiDmaTransferCompleted) {
                        if (spiDmaTransferStatus == bp3c::comm::TRANSFER_STATUS_OK) {
                            stateTransition(EVENT_DMA_TRANSFER_COMPLETED);
                        } else {
                            reportDmaTransferFailed(spiDmaTransferStatus);
                            stateTransition(EVENT_DMA_TRANSFER_FAILED);
                        }
                    }
                    #endif                

                    #if defined(EEZ_PLATFORM_SIMULATOR)
                    auto response = (Response *)input;

                    response->command = 0x80 | currentCommand->command;

                    if (currentCommand->command == COMMAND_GET_INFO) {
                        response->getInfo.firmwareMajorVersion = 1;
                        response->getInfo.firmwareMinorVersion = 0;
                        response->getInfo.idw0 = 0;
                        response->getInfo.idw1 = 0;
                        response->getInfo.idw2 = 0;
                    }

                    stateTransition(EVENT_DMA_TRANSFER_COMPLETED);
                    #endif
                } 
                
                uint32_t tickCountMs = millis();
                if (tickCountMs - lastStateTransitionTime >= TIMEOUT_TIME_MS) {
                    stateTransition(EVENT_TIMEOUT);
                }
            }
        } else {
            if (synchronized) {
            	uint32_t tickCountMs = millis();
                if (tickCountMs - lastTransferTime >= TIMEOUT_UNTIL_OUT_OF_SYNC_MS) {
                    event_queue::pushEvent(event_queue::EVENT_ERROR_SLOT1_SYNC_ERROR + slotIndex);
                    synchronized = false;
                    testResult = TEST_FAILED;
                } else if (tickCountMs - lastRefreshTime >= getRefreshTimeMs()) {
                    refreshStartTime = tickCountMs;
                    executeCommand(&getState_command);
                } else {
                    SetParams params;
                    fillSetParams(params);
                    if (memcmp(&params, &lastTransferredParams, sizeof(SetParams)) != 0) {
                        executeCommand(&setParams_command);
                    }
                }
            }
        }
    }

    void onSpiIrq() {
        spiReady = true;
		if (g_isBooted) {
			stateTransition(EVENT_SLAVE_READY);
		}
    }

    void onSpiDmaTransferCompleted(int status) override {
         if (g_isBooted) {
             if (status == bp3c::comm::TRANSFER_STATUS_OK) {
                 stateTransition(EVENT_DMA_TRANSFER_COMPLETED);
             } else {
                 reportDmaTransferFailed(status);
                 stateTransition(EVENT_DMA_TRANSFER_FAILED);
             }
         } else {
             spiDmaTransferCompleted = true;
             spiDmaTransferStatus = status;
         }
    }

    void onPowerDown() override {
        synchronized = false;
    }

    void resync() override {
        if (!synchronized) {
            executeCommand(&getInfo_command);
        }
    }

    int getSlotView(SlotViewType slotViewType, int slotIndex, int cursor) override {
        if (slotViewType == SLOT_VIEW_TYPE_DEFAULT) {
            return psu::gui::isDefaultViewVertical() ? gui::PAGE_ID_DIB_PREL6_SLOT_VIEW_DEF : gui::PAGE_ID_SLOT_DEF_HORZ_EMPTY;
        }
        if (slotViewType == SLOT_VIEW_TYPE_DEFAULT_2COL) {
            return psu::gui::isDefaultViewVertical() ? gui::PAGE_ID_DIB_PREL6_SLOT_VIEW_DEF_2COL : gui::PAGE_ID_SLOT_DEF_HORZ_EMPTY;
        }
        if (slotViewType == SLOT_VIEW_TYPE_MAX) {
            return gui::PAGE_ID_DIB_PREL6_SLOT_VIEW_MAX;
        }
        if (slotViewType == SLOT_VIEW_TYPE_MIN) {
            return gui::PAGE_ID_DIB_PREL6_SLOT_VIEW_MIN;
        }
        assert(slotViewType == SLOT_VIEW_TYPE_MICRO);
        return gui::PAGE_ID_DIB_PREL6_SLOT_VIEW_MICRO;
    }

    int getSlotSettingsPageId() override {
        return PAGE_ID_DIB_PREL6_SETTINGS;
    }

    struct ProfileParameters : public Module::ProfileParameters {
        uint8_t relayStates;
    };

    void resetProfileToDefaults(uint8_t *buffer) override {
        Module::resetProfileToDefaults(buffer);
        auto parameters = (ProfileParameters *)buffer;
        parameters->relayStates = relayStates;
    }

    void getProfileParameters(uint8_t *buffer) override {
        Module::getProfileParameters(buffer);
        assert(sizeof(ProfileParameters) < MAX_CHANNEL_PARAMETERS_SIZE);
        auto parameters = (ProfileParameters *)buffer;
        parameters->relayStates = relayStates;
    }
    
    void setProfileParameters(uint8_t *buffer, bool mismatch, int recallOptions) override {
        Module::setProfileParameters(buffer, mismatch, recallOptions);
        auto parameters = (ProfileParameters *)buffer;
        relayStates = parameters->relayStates;
    }
    
    bool writeProfileProperties(psu::profile::WriteContext &ctx, const uint8_t *buffer) override {
        if (!Module::writeProfileProperties(ctx, buffer)) {
            return false;
        }
        auto parameters = (const ProfileParameters *)buffer;
        WRITE_PROPERTY("relayStates", parameters->relayStates);
        return true;
    }
    
    bool readProfileProperties(psu::profile::ReadContext &ctx, uint8_t *buffer) override {
        if (Module::readProfileProperties(ctx, buffer)) {
            return true;
        }
        auto parameters = (ProfileParameters *)buffer;
        READ_PROPERTY("relayStates", parameters->relayStates);
		return false;
    }

    void resetConfiguration() {
        Module::resetConfiguration();
        relayStates = 0;
    }

    bool isRouteOpen(int subchannelIndex, bool &isRouteOpen, int *err) override {
        if (subchannelIndex < 0 || subchannelIndex > 5) {
            if (err) {
                *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
            }
            return false;
        }

        isRouteOpen = relayStates & (1 << subchannelIndex) ? false : true;
        return true;
    }

    bool routeOpen(ChannelList channelList, int *err) override {
        for (int i = 0; i < channelList.numChannels; i++) {
			int subchannelIndex = channelList.channels[i].subchannelIndex;
            if (subchannelIndex < 0 || subchannelIndex > 5) {
                if (err) {
                    *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
                }
                return false;
            }
        }

        for (int i = 0; i < channelList.numChannels; i++) {
			int subchannelIndex = channelList.channels[i].subchannelIndex;
			relayStates &= ~(1 << subchannelIndex);
        }

        return true;
    }
    
    bool routeClose(ChannelList channelList, int *err) override {
        for (int i = 0; i < channelList.numChannels; i++) {
			int subchannelIndex = channelList.channels[i].subchannelIndex;
            if (subchannelIndex < 0 || subchannelIndex > 5) {
                if (err) {
                    *err = SCPI_ERROR_ILLEGAL_PARAMETER_VALUE;
                }
                return false;
            }
        }

        for (int i = 0; i < channelList.numChannels; i++) {
			int subchannelIndex = channelList.channels[i].subchannelIndex;
			relayStates |= (1 << subchannelIndex);
        }

        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////

const Prel6Module::CommandDef Prel6Module::getInfo_command = {
	COMMAND_GET_INFO,
	nullptr,
	&Prel6Module::Command_GetInfo_Done
};

const Prel6Module::CommandDef Prel6Module::getState_command = {
	COMMAND_GET_STATE,
	nullptr,
	&Prel6Module::Command_GetState_Done
};

const Prel6Module::CommandDef Prel6Module::setParams_command = {
	COMMAND_SET_PARAMS,
	&Prel6Module::Command_SetParams_FillRequest,
	&Prel6Module::Command_SetParams_Done
};

////////////////////////////////////////////////////////////////////////////////

static Prel6Module g_prel6Module;
Module *g_module = &g_prel6Module;

} // namespace dib_prel6

namespace gui {

using namespace dib_prel6;

void data_dib_prel6_relays(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_COUNT) {
        value = 6;
    } else if (operation == DATA_OPERATION_GET_CURSOR_VALUE) {
        value = hmi::g_selectedSlotIndex * 6 + value.getInt();
    } 
}

void data_dib_prel6_relay_is_on(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
        int slotIndex = cursor / 6;
        int subchannelIndex = cursor % 6;
        value = ((Prel6Module *)g_slots[slotIndex])->relayStates & (1 << subchannelIndex) ? 1 : 0;
    }
}

void data_dib_prel6_relay_label(DataOperationEnum operation, Cursor cursor, Value &value) {
    if (operation == DATA_OPERATION_GET) {
		static const char *OFF_LABELS[] = {
			"1: OFF",
			"2: OFF",
			"3: OFF",
			"4: OFF",
			"5: OFF",
			"6: OFF"
		};
		static const char *ON_LABELS[] = {
			"1: ON",
			"2: ON",
			"3: ON",
			"4: ON",
			"5: ON",
			"6: ON"
		};

        int slotIndex = cursor / 6;
        int subchannelIndex = cursor % 6;

        if (((Prel6Module *)g_slots[slotIndex])->relayStates & (1 << subchannelIndex)) {
            value = ON_LABELS[subchannelIndex];
        } else {
            value = OFF_LABELS[subchannelIndex];
        }
    }
}

void action_dib_prel6_toggle_relay() {
    int cursor = getFoundWidgetAtDown().cursor;
    int slotIndex = cursor / 6;
    int subchannelIndex = cursor % 6;
    ((Prel6Module *)g_slots[slotIndex])->relayStates ^= 1 << subchannelIndex;
}

} // namespace gui

} // namespace eez
