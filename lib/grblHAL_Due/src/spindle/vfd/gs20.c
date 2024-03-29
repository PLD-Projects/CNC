/*
  gs20.c - GS20 VFD spindle support

  Part of grblHAL

  Copyright (c) 2022 Andrew Marles
  Copyright (c) 2022 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "../shared.h"

#if VFD_ENABLE == SPINDLE_ALL || VFD_ENABLE == SPINDLE_GS20

#include <math.h>
#include <string.h>

#include "spindle.h"

static spindle_id_t vfd_spindle_id;
static float rpm_programmed = -1.0f;
static spindle_state_t vfd_state = {0};
static spindle_data_t spindle_data = {0};
static uint32_t rpm_max = 0;
static uint16_t retry_counter = 0;
static on_report_options_ptr on_report_options;
static on_spindle_select_ptr on_spindle_select;
static driver_reset_ptr driver_reset;

static void gs20_rx_packet (modbus_message_t *msg);
static void gs20_rx_exception (uint8_t code, void *context);

static const modbus_callbacks_t callbacks = {
    .on_rx_packet = gs20_rx_packet,
    .on_rx_exception = gs20_rx_exception
};

// To-do, this should be a mechanism to read max RPM from the VFD in order to configure RPM/Hz instead of above define.
bool gs20_spindle_config (void)
{
    modbus_set_silence(NULL);

    return 1;
}

static void spindleSetRPM (float rpm, bool block)
{
    uint16_t data = ((uint32_t)(rpm) * 50) / vfd_config.vfd_rpm_hz;

    modbus_message_t rpm_cmd = {
        .context = (void *)VFD_SetRPM,
        .crc_check = false,
        .adu[0] = vfd_config.modbus_address,
        .adu[1] = ModBus_WriteRegister,
        .adu[2] = 0x20,
        .adu[3] = 0x01,
        .adu[4] = data >> 8,
        .adu[5] = data & 0xFF,
        .tx_length = 8,
        .rx_length = 8
    };

    vfd_state.at_speed = false;

    modbus_send(&rpm_cmd, &callbacks, block);

    if(settings.spindle.at_speed_tolerance > 0.0f) {
        spindle_data.rpm_low_limit = rpm / (1.0f + settings.spindle.at_speed_tolerance);
        spindle_data.rpm_high_limit = rpm * (1.0f + settings.spindle.at_speed_tolerance);
    }
    rpm_programmed = rpm;
}

void gs20_spindleUpdateRPM (float rpm)
{
    spindleSetRPM(rpm, false);
}

// Start or stop spindle
void gs20_spindleSetState (spindle_state_t state, float rpm)
{
    uint8_t runstop = 0;
    uint8_t direction = 0;

    if(!state.on || rpm == 0.0f)
        runstop = 0x1;
    else
        runstop = 0x2;

    if(state.ccw)
        direction = 0x20;
    else
        direction = 0x10;

    modbus_message_t mode_cmd = {
        .context = (void *)VFD_SetStatus,
        .crc_check = false,
        .adu[0] = vfd_config.modbus_address,
        .adu[1] = ModBus_WriteRegister,
        .adu[2] = 0x20,
        .adu[3] = 0x00,
        .adu[4] = 0x00,
        .adu[5] = direction|runstop,
        .tx_length = 8,
        .rx_length = 8
    };

    if(vfd_state.ccw != state.ccw)
        rpm_programmed = 0.0f;

    vfd_state.on = state.on;
    vfd_state.ccw = state.ccw;

    if(modbus_send(&mode_cmd, &callbacks, true))
        spindleSetRPM(rpm, true);
}

static spindle_data_t *gs20_spindleGetData (spindle_data_request_t request)
{
    return &spindle_data;
}

// Returns spindle state in a spindle_state_t variable
spindle_state_t gs20_spindleGetState (void)
{
    static uint32_t last_ms;
    uint32_t ms = hal.get_elapsed_ticks();

    modbus_message_t mode_cmd = {
        .context = (void *)VFD_GetRPM,
        .crc_check = false,
        .adu[0] = vfd_config.modbus_address,
        .adu[1] = ModBus_ReadHoldingRegisters,
        .adu[2] = 0x21,
        .adu[3] = 0x03,
        .adu[4] = 0x00,
        .adu[5] = 0x01,
        .tx_length = 8,
        .rx_length = 7
    };

    if(ms > (last_ms + VFD_RETRY_DELAY)){ //don't spam the port
        modbus_send(&mode_cmd, &callbacks, false); // TODO: add flag for not raising alarm?
        last_ms = ms;
    }

    // Get the actual RPM from spindle encoder input when available.
    if(hal.spindle.get_data && hal.spindle.get_data != gs20_spindleGetData) {
        float rpm = hal.spindle.get_data(SpindleData_RPM)->rpm;
        vfd_state.at_speed = settings.spindle.at_speed_tolerance <= 0.0f || (rpm >= spindle_data.rpm_low_limit && rpm <= spindle_data.rpm_high_limit);
    }

    return vfd_state; // return previous state as we do not want to wait for the response
}

static void gs20_rx_packet (modbus_message_t *msg)
{
    if(!(msg->adu[0] & 0x80)) {

        switch((vfd_response_t)msg->context) {

            case VFD_GetRPM:
                spindle_data.rpm = (float)((msg->adu[3] << 8) | msg->adu[4]) * vfd_config.vfd_rpm_hz / 100;
                vfd_state.at_speed = settings.spindle.at_speed_tolerance <= 0.0f || (spindle_data.rpm >= spindle_data.rpm_low_limit && spindle_data.rpm <= spindle_data.rpm_high_limit);
                retry_counter = 0;
                break;

            case VFD_GetMaxRPM:
                rpm_max = (msg->adu[4] << 8) | msg->adu[5];
                retry_counter = 0;
                break;

            case VFD_SetStatus:
                retry_counter = 0;
                break;

            case VFD_SetRPM:
                retry_counter = 0;
                break;

            default:
                retry_counter = 0;
                break;
        }
    }
}

static void raise_alarm (uint_fast16_t state)
{
    system_raise_alarm(Alarm_Spindle);
}

static void gs20_rx_exception (uint8_t code, void *context)
{
    // Alarm needs to be raised directly to correctly handle an error during reset (the rt command queue is
    // emptied on a warm reset). Exception is during cold start, where alarms need to be queued.
    if(sys.cold_start) {
        protocol_enqueue_rt_command(raise_alarm);
    }
    //if RX exceptions during one of the VFD messages, need to retry.
    else if ((vfd_response_t)context > 0 ) {
        retry_counter++;
        if (retry_counter >= VFD_RETRIES) {
            system_raise_alarm(Alarm_Spindle);
            retry_counter = 0;
            return;
        }

        switch((vfd_response_t)context) {

            case VFD_SetStatus:
            case VFD_SetRPM:
//                modbus_reset();
                hal.spindle.set_state(hal.spindle.get_state(), sys.spindle_rpm);
                break;

            case VFD_GetRPM:
//                modbus_reset();
                hal.spindle.get_state();
                break;

            default:
                break;
        }//close switch statement
    } else {
        retry_counter = 0;
        system_raise_alarm(Alarm_Spindle);
    }
}

void gs20_onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt) {
        hal.stream.write("[PLUGIN:Durapulse VFD GS20 v0.03]" ASCII_EOL);
    }
}

void gs20_reset (void)
{
    driver_reset();
}

bool gs20_spindle_select (spindle_id_t spindle_id)
{
    if(spindle_id == vfd_spindle_id) {

        if(settings.spindle.ppr == 0)
            hal.spindle.get_data = gs20_spindleGetData;

    } else if(hal.spindle.get_data == gs20_spindleGetData)
        hal.spindle.get_data = NULL;

    if(on_spindle_select)
        on_spindle_select(spindle_id);

    return true;
}

void vfd_gs20_init (void)
{
    static const vfd_spindle_ptrs_t spindle = {
        .spindle.cap.variable = On,
        .spindle.cap.at_speed = On,
        .spindle.cap.direction = On,
        .spindle.config = gs20_spindle_config,
        .spindle.set_state = gs20_spindleSetState,
        .spindle.get_state = gs20_spindleGetState,
        .spindle.update_rpm = gs20_spindleUpdateRPM
    };

    if((vfd_spindle_id = vfd_register(&spindle, "Durapulse GS20")) != -1) {

        on_spindle_select = grbl.on_spindle_select;
        grbl.on_spindle_select = gs20_spindle_select;

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = gs20_onReportOptions;

        //driver_reset = hal.driver_reset;
        //hal.driver_reset = gs20_reset;
    }
}

#endif
