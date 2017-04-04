/*
 * Copyright (C) 2016 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "stdin_support.h"
#include "avdtp_source.h"

#include "btstack_sbc.h"
#include "avdtp_util.h"

typedef struct {
    // bitmaps
    uint8_t sampling_frequency_bitmap;
    uint8_t channel_mode_bitmap;
    uint8_t block_length_bitmap;
    uint8_t subbands_bitmap;
    uint8_t allocation_method_bitmap;
    uint8_t min_bitpool_value;
    uint8_t max_bitpool_value;
} adtvp_media_codec_information_sbc_t;

typedef struct {
    int reconfigure;
    int num_channels;
    int sampling_frequency;
    int channel_mode;
    int block_length;
    int subbands;
    int allocation_method;
    int min_bitpool_value;
    int max_bitpool_value;
    int frames_per_buffer;
} avdtp_media_codec_configuration_sbc_t;

static char * device_name = "A2DP Source BTstack";

// mac 2011: static bd_addr_t remote = {0x04, 0x0C, 0xCE, 0xE4, 0x85, 0xD3};
// pts: static bd_addr_t remote = {0x00, 0x1B, 0xDC, 0x08, 0x0A, 0xA5};
// mac 2013: static bd_addr_t remote = {0x84, 0x38, 0x35, 0x65, 0xd1, 0x15};
// phone 2013: static bd_addr_t remote = {0xD8, 0xBB, 0x2C, 0xDF, 0xF0, 0xF2};
// minijambox: 
static bd_addr_t remote = {0x00, 0x21, 0x3c, 0xac, 0xf7, 0x38};
// head phones: static bd_addr_t remote = {0x00, 0x18, 0x09, 0x28, 0x50, 0x18};
// bt dongle: -u 02-04-01 
// static bd_addr_t remote = {0x00, 0x15, 0x83, 0x5F, 0x9D, 0x46};

static uint16_t con_handle = 0;
static uint16_t media_con_handle = 0;
static uint8_t sdp_avdtp_source_service_buffer[150];

static adtvp_media_codec_information_sbc_t sbc_capability;
static avdtp_media_codec_configuration_sbc_t sbc_configuration;
static avdtp_stream_endpoint_t * local_stream_endpoint;

static uint16_t remote_configuration_bitmap;
static avdtp_capabilities_t remote_configuration;

static avdtp_sep_t remote_seps[MAX_NUM_SEPS];
static uint16_t num_remote_seps = 0;
static int next_remote_sep_index_to_query = -1;
static avdtp_sep_t * active_remote_sep = NULL;

static uint8_t media_sbc_codec_configuration[4];
                            
static const char * avdtp_si_name[] = {
    "ERROR",
    "AVDTP_SI_DISCOVER",            
    "AVDTP_SI_GET_CAPABILITIES",
    "AVDTP_SI_SET_CONFIGURATION",
    "AVDTP_SI_GET_CONFIGURATION",
    "AVDTP_SI_RECONFIGURE",
    "AVDTP_SI_OPEN", 
    "AVDTP_SI_START", 
    "AVDTP_SI_CLOSE",
    "AVDTP_SI_SUSPEND",
    "AVDTP_SI_ABORT", 
    "AVDTP_SI_SECURITY_CONTROL",
    "AVDTP_SI_GET_ALL_CAPABILITIES", 
    "AVDTP_SI_DELAY_REPORT" 
};

typedef enum {
    AVDTP_APPLICATION_IDLE,
    AVDTP_APPLICATION_CONNECTED,
    AVDTP_APPLICATION_W2_DISCOVER_SEPS,
    AVDTP_APPLICATION_W2_GET_CAPABILITIES,
    AVDTP_APPLICATION_W2_GET_ALL_CAPABILITIES,
    AVDTP_APPLICATION_W2_SET_CONFIGURATION,
    AVDTP_APPLICATION_W4_SET_CONFIGURATION,
    AVDTP_APPLICATION_W2_SUSPEND_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W2_RECONFIGURE_WITH_SEID,
    AVDTP_APPLICATION_W2_OPEN_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W4_OPEN_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W2_START_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W2_ABORT_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W2_STOP_STREAM_WITH_SEID,
    AVDTP_APPLICATION_W2_GET_CONFIGURATION, 
    AVDTP_APPLICATION_STREAMING_OPENED
} avdtp_application_state_t;

avdtp_application_state_t app_state = AVDTP_APPLICATION_IDLE;

static btstack_packet_callback_registration_t hci_event_callback_registration;

static const char * avdtp_si2str(uint16_t index){
    if (index <= 0 || index > sizeof(avdtp_si_name)) return avdtp_si_name[0];
    return avdtp_si_name[index];
}

static void dump_sbc_capability(adtvp_media_codec_information_sbc_t media_codec_sbc){
    printf(" --- avdtp source --- Received media codec capability:\n");
    printf("  - sampling_frequency: 0x%02x\n", media_codec_sbc.sampling_frequency_bitmap);
    printf("  - channel_mode: 0x%02x\n", media_codec_sbc.channel_mode_bitmap);
    printf("  - block_length: 0x%02x\n", media_codec_sbc.block_length_bitmap);
    printf("  - subbands: 0x%02x\n", media_codec_sbc.subbands_bitmap);
    printf("  - allocation_method: 0x%02x\n", media_codec_sbc.allocation_method_bitmap);
    printf("  - bitpool_value [%d, %d] \n", media_codec_sbc.min_bitpool_value, media_codec_sbc.max_bitpool_value);
    printf("\n");
}

static void dump_sbc_configuration(avdtp_media_codec_configuration_sbc_t configuration){
    printf(" --- avdtp source --- Received media codec configuration:\n");
    printf("  - num_channels: %d\n", configuration.num_channels);
    printf("  - sampling_frequency: %d\n", configuration.sampling_frequency);
    printf("  - channel_mode: %d\n", configuration.channel_mode);
    printf("  - block_length: %d\n", configuration.block_length);
    printf("  - subbands: %d\n", configuration.subbands);
    printf("  - allocation_method: %d\n", configuration.allocation_method);
    printf("  - bitpool_value [%d, %d] \n", configuration.min_bitpool_value, configuration.max_bitpool_value);
    printf("\n");
}

static void sbc_choose_configuration_from_media_codec_information(adtvp_media_codec_information_sbc_t * media_codec_information, uint8_t * configuration){
    uint8_t sampling_frequency = AVDTP_SBC_44100;
    uint8_t channel_mode = AVDTP_SBC_STEREO;
    uint8_t block_length = AVDTP_SBC_BLOCK_LENGTH_16;
    uint8_t subbands = AVDTP_SBC_SUBBANDS_8;
    uint8_t allocation_method = AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS;

    if (media_codec_information->channel_mode_bitmap & AVDTP_SBC_JOINT_STEREO){
        channel_mode = AVDTP_SBC_JOINT_STEREO;
    } else if (media_codec_information->channel_mode_bitmap & AVDTP_SBC_STEREO){
        channel_mode = AVDTP_SBC_STEREO;
    } else if (media_codec_information->channel_mode_bitmap & AVDTP_SBC_DUAL_CHANNEL){
        channel_mode = AVDTP_SBC_DUAL_CHANNEL;
    } else if (media_codec_information->channel_mode_bitmap & AVDTP_SBC_MONO){
        channel_mode = AVDTP_SBC_MONO;
    } 

    if (media_codec_information->block_length_bitmap & AVDTP_SBC_BLOCK_LENGTH_16){
        block_length = AVDTP_SBC_BLOCK_LENGTH_16;
    } else if (media_codec_information->block_length_bitmap & AVDTP_SBC_BLOCK_LENGTH_12){
        block_length = AVDTP_SBC_BLOCK_LENGTH_12;
    } else if (media_codec_information->block_length_bitmap & AVDTP_SBC_BLOCK_LENGTH_8){
        block_length = AVDTP_SBC_BLOCK_LENGTH_8;
    } else if (media_codec_information->block_length_bitmap & AVDTP_SBC_BLOCK_LENGTH_4){
        block_length = AVDTP_SBC_BLOCK_LENGTH_4;
    } 

    if (media_codec_information->subbands_bitmap & AVDTP_SBC_SUBBANDS_8){
        subbands = AVDTP_SBC_SUBBANDS_8;
    } else if (media_codec_information->subbands_bitmap & AVDTP_SBC_SUBBANDS_4){
        subbands = AVDTP_SBC_SUBBANDS_4;
    }

    if (media_codec_information->allocation_method_bitmap & AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS){
        allocation_method = AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS;
    } else if (media_codec_information->allocation_method_bitmap & AVDTP_SBC_ALLOCATION_METHOD_SNR){
        allocation_method = AVDTP_SBC_ALLOCATION_METHOD_SNR;
    }

    configuration[0] = (sampling_frequency << 4) | channel_mode;
    configuration[1] = (block_length << 4) | (subbands << 2) | allocation_method;
    configuration[2] = media_codec_information->min_bitpool_value;
    configuration[3] = media_codec_information->max_bitpool_value;
    printf(" --- avdtp source --- SBC settings: frequency %d, channel_mode %d, block_length %d, subbands %d, allocation_method %d, bitpool [%d,%d] \n", sampling_frequency, channel_mode, block_length, 
        subbands, allocation_method, media_codec_information->min_bitpool_value, media_codec_information->max_bitpool_value);
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    bd_addr_t event_addr;
    uint8_t signal_identifier;
    uint8_t status;
    avdtp_sep_t sep;

    switch (packet_type) {
 
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case HCI_EVENT_PIN_CODE_REQUEST:
                    // inform about pin code request
                    printf("Pin code request - using '0000'\n");
                    hci_event_pin_code_request_get_bd_addr(packet, event_addr);
                    hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    // connection closed -> quit test app
                    printf("\n --- avdtp source --- HCI_EVENT_DISCONNECTION_COMPLETE ---\n");
                    break;
                case HCI_EVENT_AVDTP_META:
                    switch (packet[2]){
                        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
                            con_handle = avdtp_subevent_signaling_connection_established_get_con_handle(packet);
                            status = avdtp_subevent_signaling_connection_established_get_status(packet);
                            if (status != 0){
                                printf(" --- avdtp source --- AVDTP_SUBEVENT_SIGNALING_CONNECTION could not be established, status %d ---\n", status);
                                break;
                            }
                            active_remote_sep = NULL;
                            num_remote_seps = 0;
                            next_remote_sep_index_to_query = -1;
                            app_state = AVDTP_APPLICATION_CONNECTED;
                            printf(" --- avdtp source --- AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED, con handle 0x%02x ---\n", con_handle);
                            break;
                        
                        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED:
                            media_con_handle = avdtp_subevent_streaming_connection_established_get_con_handle(packet);
                            status = avdtp_subevent_streaming_connection_established_get_status(packet);
                            if (status != 0){
                                printf(" --- avdtp source --- AVDTP_SUBEVENT_STREAMING_CONNECTION could not be established, status %d ---\n", status);
                                break;
                            }
                            app_state = AVDTP_APPLICATION_STREAMING_OPENED;
                            printf(" --- avdtp source --- AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED, con handle 0x%02x ---\n", media_con_handle);
                            break;

                        case AVDTP_SUBEVENT_SIGNALING_SEP_FOUND:
                            if (app_state != AVDTP_APPLICATION_W2_DISCOVER_SEPS) return;
                            sep.seid = avdtp_subevent_signaling_sep_found_get_seid(packet);
                            sep.in_use = avdtp_subevent_signaling_sep_found_get_in_use(packet);
                            sep.media_type = avdtp_subevent_signaling_sep_found_get_media_type(packet);
                            sep.type = avdtp_subevent_signaling_sep_found_get_sep_type(packet);
                            remote_seps[num_remote_seps++] = sep;
                            printf(" --- avdtp source --- Found sep [%d]: seid %u, in_use %d, media type %d, sep type %d (1-SNK)\n", num_remote_seps, sep.seid, sep.in_use, sep.media_type, sep.type);
                            break;

                        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:{
                            sbc_capability.sampling_frequency_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet);
                            sbc_capability.channel_mode_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet);
                            sbc_capability.block_length_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet);
                            sbc_capability.subbands_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet);
                            sbc_capability.allocation_method_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet);
                            sbc_capability.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet);
                            sbc_capability.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet);
                            
                            dump_sbc_capability(sbc_capability);
                            
                            if (!sbc_capability.sampling_frequency_bitmap) break;
                            if (!sbc_capability.channel_mode_bitmap) break;
                            if (!sbc_capability.block_length_bitmap) break;
                            if (!sbc_capability.allocation_method_bitmap) break;
                            if (!sbc_capability.subbands_bitmap) break;
                            
                            if (!(sbc_capability.sampling_frequency_bitmap & AVDTP_SBC_44100)) break;

                            sbc_choose_configuration_from_media_codec_information(&sbc_capability, media_sbc_codec_configuration);
                            app_state = AVDTP_APPLICATION_W2_SET_CONFIGURATION;
                            
                            active_remote_sep = &remote_seps[next_remote_sep_index_to_query];
                            remote_configuration_bitmap = store_bit16(remote_configuration_bitmap, AVDTP_MEDIA_CODEC, 1);
                            remote_configuration.media_codec.media_type = AVDTP_AUDIO;
                            remote_configuration.media_codec.media_codec_type = AVDTP_CODEC_SBC;
                            remote_configuration.media_codec.media_codec_information_len = sizeof(media_sbc_codec_configuration);
                            remote_configuration.media_codec.media_codec_information = media_sbc_codec_configuration;
                            app_state = AVDTP_APPLICATION_W2_SET_CONFIGURATION;
                            break;
                        }
                        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
                            printf(" --- avdtp source ---  received non SBC codec. not implemented\n");
                            break;
                        
                        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
                            app_state = AVDTP_APPLICATION_IDLE;
                            sbc_configuration.reconfigure = avdtp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
                            sbc_configuration.num_channels = avdtp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
                            sbc_configuration.sampling_frequency = avdtp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
                            sbc_configuration.channel_mode = avdtp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
                            sbc_configuration.block_length = avdtp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
                            sbc_configuration.subbands = avdtp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
                            sbc_configuration.allocation_method = avdtp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
                            sbc_configuration.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
                            sbc_configuration.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
                            sbc_configuration.frames_per_buffer = sbc_configuration.subbands * sbc_configuration.block_length;
                            dump_sbc_configuration(sbc_configuration);
                            // if (sbc_configuration.reconfigure){}
                            break;
                        }  
                        
                        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
                            signal_identifier = avdtp_subevent_signaling_accept_get_signal_identifier(packet);
                            status = avdtp_subevent_signaling_accept_get_status(packet);
                            printf(" --- avdtp source ---  Accepted %d %s\n", signal_identifier, avdtp_si2str(signal_identifier));
                            switch (app_state){
                                case AVDTP_APPLICATION_W2_DISCOVER_SEPS:
                                    app_state = AVDTP_APPLICATION_W2_GET_ALL_CAPABILITIES;
                                    next_remote_sep_index_to_query = 0;
                                    printf(" --- avdtp source ---  Query get caps for seid %d\n", remote_seps[next_remote_sep_index_to_query].seid);
                                    avdtp_source_get_capabilities(con_handle, remote_seps[next_remote_sep_index_to_query].seid);
                                    break;
                                case AVDTP_APPLICATION_W2_GET_CAPABILITIES:
                                case AVDTP_APPLICATION_W2_GET_ALL_CAPABILITIES:
                                    if (next_remote_sep_index_to_query < num_remote_seps - 1){
                                        next_remote_sep_index_to_query++;
                                        printf(" --- avdtp source ---  Query get caps for seid %d\n", remote_seps[next_remote_sep_index_to_query].seid);
                                        avdtp_source_get_capabilities(con_handle, remote_seps[next_remote_sep_index_to_query].seid);
                                    } else {
                                        printf(" --- avdtp source ---  Cannot query get caps, index %d\n", next_remote_sep_index_to_query);
                                        app_state = AVDTP_APPLICATION_IDLE;
                                    }
                                    break;
                                case AVDTP_APPLICATION_W2_SET_CONFIGURATION:
                                    app_state = AVDTP_APPLICATION_W4_SET_CONFIGURATION;
                                    avdtp_source_set_configuration(con_handle, local_stream_endpoint->sep.seid, active_remote_sep->seid, remote_configuration_bitmap, remote_configuration);
                                    break;
                                case AVDTP_APPLICATION_W4_SET_CONFIGURATION:
                                    app_state = AVDTP_APPLICATION_W4_OPEN_STREAM_WITH_SEID;
                                    avdtp_source_open_stream(con_handle, active_remote_sep->seid);
                                    break;
                                case AVDTP_APPLICATION_STREAMING_OPENED:
                                    switch (signal_identifier){
                                        case AVDTP_SI_START:
                                            break;
                                        case AVDTP_SI_CLOSE:
                                            break;
                                        case AVDTP_SI_SUSPEND:
                                            break;
                                        case AVDTP_SI_ABORT:
                                            break;
                                        default:
                                            break;
                                    }
                                    break;
                                default:
                                    app_state = AVDTP_APPLICATION_IDLE;
                                    break;
                            }
                            
                            break;
                        case AVDTP_SUBEVENT_SIGNALING_REJECT:
                            app_state = AVDTP_APPLICATION_IDLE;
                            signal_identifier = avdtp_subevent_signaling_reject_get_signal_identifier(packet);
                            printf(" --- avdtp source ---  Rejected %s\n", avdtp_si2str(signal_identifier));
                            break;
                        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT:
                            app_state = AVDTP_APPLICATION_IDLE;
                            signal_identifier = avdtp_subevent_signaling_general_reject_get_signal_identifier(packet);
                            printf(" --- avdtp source ---  Rejected %s\n", avdtp_si2str(signal_identifier));
                            break;
                        default:
                            app_state = AVDTP_APPLICATION_IDLE;
                            printf(" --- avdtp source ---  not implemented\n");
                            break; 
                    }
                    break;   
                default:
                    break;
            }
            break;
        default:
            // other packet type
            break;
    }
}

static void show_usage(void){
    bd_addr_t      iut_address;
    gap_local_bd_addr(iut_address);
    printf("\n--- Bluetooth AVDTP SOURCE Test Console %s ---\n", bd_addr_to_str(iut_address));
    printf("c      - create connection to addr %s\n", bd_addr_to_str(remote));
    printf("C      - disconnect\n");
    printf("d      - discover stream endpoints\n");
    
    if (!active_remote_sep){
        printf("Ctrl-c - exit\n");
        printf("---\n");
        return;
    }

    printf("m      - start stream with %d\n", active_remote_sep->seid);
    printf("x      - start data stream\n");
    printf("X      - stop  data stream\n");
    printf("A      - abort stream with %d\n", active_remote_sep->seid);
    printf("S      - stop stream with %d\n", active_remote_sep->seid);
    printf("P      - suspend stream with %d\n", active_remote_sep->seid);
    printf("Ctrl-c - exit\n");
    printf("---\n");
}

static  uint8_t media_sbc_codec_capabilities[] = {
    0xFF,//(AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF,//(AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
}; 

// static const uint8_t media_sbc_codec_reconfiguration[] = {
//     (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
//     (AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_SNR,
//     2, 53
// }; 

#define NUM_CHANNELS        2
#define SAMPLE_RATE         44100
#define BYTES_PER_AUDIO_SAMPLE   (2*NUM_CHANNELS)
#define LATENCY             300 // ms

#ifndef M_PI
#define M_PI  3.14159265
#endif
#define TABLE_SIZE_441HZ   100

typedef struct {
    int16_t source[TABLE_SIZE_441HZ];
    int left_phase;
    int right_phase;
} paTestData;

static uint32_t fill_audio_ring_buffer_timeout_ms = 50;

static uint8_t audio_samples_storage[44100*4]; // 1s buffer
static btstack_ring_buffer_t audio_ring_buffer;

static uint8_t sbc_samples_storage[44100*4];
// static btstack_ring_buffer_t sbc_ring_buffer;
static paTestData sin_data;

static void fill_sbc_ring_buffer(uint8_t * sbc_frame, int sbc_frame_size, avdtp_stream_endpoint_t * stream_endpoint){
    if (btstack_ring_buffer_bytes_free(&stream_endpoint->sbc_ring_buffer) >= sbc_frame_size ){
        // printf("    fill_sbc_ring_buffer\n");
        uint8_t size_buffer = sbc_frame_size;
        btstack_ring_buffer_write(&stream_endpoint->sbc_ring_buffer, &size_buffer, 1);
        btstack_ring_buffer_write(&stream_endpoint->sbc_ring_buffer, sbc_frame, sbc_frame_size);
    } else {
        printf("No space in sbc buffer\n");
    }
}

static void avdtp_source_stream_endpoint_run(avdtp_stream_endpoint_t * stream_endpoint){
    // performe sbc encoding
    int total_num_bytes_read = 0;
    int num_audio_samples_to_read = btstack_sbc_encoder_num_audio_frames();
    int audio_bytes_to_read = num_audio_samples_to_read * BYTES_PER_AUDIO_SAMPLE; 

    // printf("run: audio_bytes_to_read:        %d\n", audio_bytes_to_read);
    // printf("     audio buf, bytes available: %d\n", btstack_ring_buffer_bytes_available(&audio_ring_buffer));
    // printf("     sbc buf,   bytes free:      %d\n", btstack_ring_buffer_bytes_free(&stream_endpoint->sbc_ring_buffer));

    while (btstack_ring_buffer_bytes_available(&audio_ring_buffer) >= audio_bytes_to_read
        && btstack_ring_buffer_bytes_free(&stream_endpoint->sbc_ring_buffer) >= 120){ // TODO use real value
        
        uint32_t number_of_bytes_read = 0;
        uint8_t pcm_frame[256*BYTES_PER_AUDIO_SAMPLE];
        btstack_ring_buffer_read(&audio_ring_buffer, pcm_frame, audio_bytes_to_read, &number_of_bytes_read); 
        // printf("     num audio bytes read %d\n", number_of_bytes_read);
        btstack_sbc_encoder_process_data((int16_t *) pcm_frame);
        
        uint16_t sbc_frame_bytes = 119; //btstack_sbc_encoder_sbc_buffer_length();

        total_num_bytes_read += number_of_bytes_read;
        fill_sbc_ring_buffer(btstack_sbc_encoder_sbc_buffer(), sbc_frame_bytes, stream_endpoint);
    }
    // schedule sending
    if (total_num_bytes_read != 0){
        stream_endpoint->state = AVDTP_STREAM_ENDPOINT_STREAMING_W2_SEND;
        avdtp_request_can_send_now_self(stream_endpoint->connection, stream_endpoint->l2cap_media_cid);
    }
}

static void fill_audio_ring_buffer(void *userData, int num_samples_to_write){
    paTestData *data = (paTestData*)userData;
    int count = 0;
    while (btstack_ring_buffer_bytes_free(&audio_ring_buffer) >= BYTES_PER_AUDIO_SAMPLE && count < num_samples_to_write){
        uint8_t write_data[BYTES_PER_AUDIO_SAMPLE];
        *(int16_t*)&write_data[0] = data->source[data->left_phase];
        *(int16_t*)&write_data[2] = data->source[data->right_phase];
        
        btstack_ring_buffer_write(&audio_ring_buffer, write_data, BYTES_PER_AUDIO_SAMPLE);
        count++;

        data->left_phase += 1;
        if (data->left_phase >= TABLE_SIZE_441HZ){
            data->left_phase -= TABLE_SIZE_441HZ;
        }
        data->right_phase += 1; 
        if (data->right_phase >= TABLE_SIZE_441HZ){
            data->right_phase -= TABLE_SIZE_441HZ;
        } 
    }
}

static void avdtp_fill_audio_ring_buffer_timeout_handler(btstack_timer_source_t * timer){
    avdtp_stream_endpoint_t * stream_endpoint = btstack_run_loop_get_timer_context(timer);
    btstack_run_loop_set_timer(&stream_endpoint->fill_audio_ring_buffer_timer, fill_audio_ring_buffer_timeout_ms); // 2 seconds timeout
    btstack_run_loop_add_timer(&stream_endpoint->fill_audio_ring_buffer_timer);
    uint32_t now = btstack_run_loop_get_time_ms();

    uint32_t update_period_ms = fill_audio_ring_buffer_timeout_ms;
    if (stream_endpoint->time_audio_data_sent > 0){
        update_period_ms = now - stream_endpoint->time_audio_data_sent;
    } 
    uint32_t num_samples = (update_period_ms * 44100) / 1000;
    stream_endpoint->acc_num_missed_samples += (update_period_ms * 44100) % 1000;

    if (stream_endpoint->acc_num_missed_samples >= 1000){
        num_samples++;
        stream_endpoint->acc_num_missed_samples -= 1000;
    }

    fill_audio_ring_buffer(&sin_data, num_samples);
    stream_endpoint->time_audio_data_sent = now;

    avdtp_source_stream_endpoint_run(stream_endpoint);
    // 
}

static void avdtp_fill_audio_ring_buffer_timer_start(avdtp_stream_endpoint_t * stream_endpoint){
    btstack_run_loop_remove_timer(&stream_endpoint->fill_audio_ring_buffer_timer);
    btstack_run_loop_set_timer_handler(&stream_endpoint->fill_audio_ring_buffer_timer, avdtp_fill_audio_ring_buffer_timeout_handler);
    btstack_run_loop_set_timer_context(&stream_endpoint->fill_audio_ring_buffer_timer, stream_endpoint);
    btstack_run_loop_set_timer(&stream_endpoint->fill_audio_ring_buffer_timer, fill_audio_ring_buffer_timeout_ms); // 50 ms timeout
    btstack_run_loop_add_timer(&stream_endpoint->fill_audio_ring_buffer_timer);
}

static void avdtp_fill_audio_ring_buffer_timer_stop(avdtp_stream_endpoint_t * stream_endpoint){
    btstack_run_loop_remove_timer(&stream_endpoint->fill_audio_ring_buffer_timer);
} 

static void avdtp_source_stream_data_start(avdtp_stream_endpoint_t * stream_endpoint){
    if (!stream_endpoint) {
        printf("no stream_endpoint found for 0x%02x", con_handle);
        return;
    }
    if (stream_endpoint->state != AVDTP_STREAM_ENDPOINT_STREAMING){
        printf("stream_endpoint in wrong state %d\n", stream_endpoint->state);
        return;
    } 
    avdtp_fill_audio_ring_buffer_timer_start(stream_endpoint);
}

static void avdtp_source_stream_data_stop(avdtp_stream_endpoint_t * stream_endpoint){
    if (!stream_endpoint) {
        log_error("no stream_endpoint found");
        return;
    }
    switch (stream_endpoint->state){
        case AVDTP_STREAM_ENDPOINT_STREAMING_W2_SEND:
        case AVDTP_STREAM_ENDPOINT_STREAMING:
            // TODO: initialize randomly sequence number
            stream_endpoint->sequence_number = 0;
            stream_endpoint->state = AVDTP_STREAM_ENDPOINT_STREAMING;
            avdtp_fill_audio_ring_buffer_timer_stop(stream_endpoint);
            break;
        default:
            printf("stream_endpoint in wrong state %d\n", stream_endpoint->state);
            return;
    } 

}

static void stdin_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type){
    UNUSED(ds);
    UNUSED(callback_type);

    int cmd = btstack_stdin_read();

    switch (cmd){
        case 'c':
            printf("Creating L2CAP Connection to %s, PSM_AVDTP\n", bd_addr_to_str(remote));
            avdtp_source_connect(remote);
            break;
        case 'C':
            printf("Disconnect not implemented\n");
            avdtp_source_disconnect(con_handle);
            break;
        case 'd':
            app_state = AVDTP_APPLICATION_W2_DISCOVER_SEPS;
            avdtp_source_discover_stream_endpoints(con_handle);
            break;
        default:
            if (!active_remote_sep){
                show_usage();
                return;
            }
            break;
    }
    if (!active_remote_sep) return;
    switch (cmd){
        case 'f':
            app_state = AVDTP_APPLICATION_W2_GET_CONFIGURATION;
            avdtp_source_get_configuration(con_handle, active_remote_sep->seid);
            break;
        case 'm': 
            app_state = AVDTP_APPLICATION_W2_START_STREAM_WITH_SEID;
            avdtp_source_start_stream(con_handle, active_remote_sep->seid);
            break;
        case 'A':
            app_state = AVDTP_APPLICATION_W2_ABORT_STREAM_WITH_SEID;
            avdtp_source_abort_stream(con_handle, active_remote_sep->seid);
            break;
        case 'S':
            app_state = AVDTP_APPLICATION_W2_STOP_STREAM_WITH_SEID;
            avdtp_source_stop_stream(con_handle, active_remote_sep->seid);
            break;
        case 'P':
            app_state = AVDTP_APPLICATION_W2_SUSPEND_STREAM_WITH_SEID;
            avdtp_source_suspend(con_handle, active_remote_sep->seid);
            break;
        case 'x':
            printf("start streaming sine\n");
            avdtp_source_stream_data_start(local_stream_endpoint);
            break;
        case 'X':
            printf("stop streaming sine\n");
            avdtp_source_stream_data_stop(local_stream_endpoint);
            break;
            
        case '\n':
        case '\r':
            break;
        default:
            show_usage();
            break;

    }
}


int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    UNUSED(argc);
    (void)argv;

    /* Register for HCI events */
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    l2cap_init();
    // Initialize AVDTP Sink
    avdtp_source_init();
    avdtp_source_register_packet_handler(&packet_handler);

//#ifndef SMG_BI
    local_stream_endpoint = avdtp_source_create_stream_endpoint(AVDTP_SOURCE, AVDTP_AUDIO);
    local_stream_endpoint->sep.seid = 2;
    avdtp_source_register_media_transport_category(local_stream_endpoint->sep.seid);
    avdtp_source_register_media_codec_category(local_stream_endpoint->sep.seid, AVDTP_AUDIO, AVDTP_CODEC_SBC, media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities));
//#endif
    // uint8_t cp_type_lsb,  uint8_t cp_type_msb, const uint8_t * cp_type_value, uint8_t cp_type_value_len
    // avdtp_source_register_content_protection_category(seid, 2, 2, NULL, 0);

    // Initialize SDP 
    sdp_init();
    memset(sdp_avdtp_source_service_buffer, 0, sizeof(sdp_avdtp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_avdtp_source_service_buffer, 0x10002, 1, NULL, NULL);
    sdp_register_service(sdp_avdtp_source_service_buffer);
    
    gap_set_local_name(device_name);
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408);
    
    avdtp_set_fill_audio_ring_buffer_timeout_ms(local_stream_endpoint, 50);
    memset(audio_samples_storage, 0, sizeof(audio_samples_storage));
    memset(sbc_samples_storage, 0, sizeof(sbc_samples_storage));
    
    btstack_ring_buffer_init(&audio_ring_buffer, audio_samples_storage, sizeof(audio_samples_storage));
    avdtp_init_sbc_buffer(local_stream_endpoint, sbc_samples_storage, sizeof(sbc_samples_storage));
        
    /* initialise sinusoidal wavetable */
    int i;
    for (i=0; i<TABLE_SIZE_441HZ; i++){
        sin_data.source[i] = sin(((double)i/(double)TABLE_SIZE_441HZ) * M_PI * 2.)*32767;
    }
    sin_data.left_phase = sin_data.right_phase = 0;

    // turn on!
    hci_power_control(HCI_POWER_ON);

    btstack_stdin_setup(stdin_process);
    return 0;
}