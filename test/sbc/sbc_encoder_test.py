#!/usr/bin/env python
import numpy as np
import wave
import struct
import sys
from sbc import *
from sbc_encoder import *
from sbc_decoder import *

error = 0.99
max_error = -1

def sbc_compare_audio_frames(frame_count, actual_frame, expected_frame):
    global error, max_error

    M = mse(actual_frame.audio_sample, expected_frame.audio_sample)
    if M > max_error:
        max_error = M
       
    if M > error:
        print "audio_sample error (%d, %d ) " % (frame_count, M)
        return -1
    return 0 


def sbc_compare_headers(frame_count, actual_frame, expected_frame):
    if actual_frame.syncword != expected_frame.syncword:
        print "syncword wrong ", actual_frame.syncword
        return -1

    if actual_frame.sampling_frequency != expected_frame.sampling_frequency:
        print "sampling_frequency wrong ", actual_frame.sampling_frequency
        return -1

    if actual_frame.nr_blocks != expected_frame.nr_blocks:
        print "nr_blocks wrong ", actual_frame.nr_blocks
        return -1

    if actual_frame.channel_mode != expected_frame.channel_mode:
        print "channel_mode wrong ", actual_frame.channel_mode
        return -1

    if actual_frame.nr_channels != expected_frame.nr_channels:
        print "nr_channels wrong ", actual_frame.nr_channels
        return -1

    if actual_frame.allocation_method != expected_frame.allocation_method:
        print "allocation_method wrong ", actual_frame.allocation_method
        return -1

    if actual_frame.nr_subbands != expected_frame.nr_subbands:
        print "nr_subbands wrong ", actual_frame.nr_subbands
        return -1

    if actual_frame.bitpool != expected_frame.bitpool:
        print "bitpool wrong (E: %d, D: %d)" % (actual_frame.bitpool, expected_frame.bitpool)
        return -1
    
    if  mse(actual_frame.join, expected_frame.join) > 0:
        print "join error \nE:\n %s \nD:\n %s" % (actual_frame.join, expected_frame.join)
        return -1
    
    if  mse(actual_frame.scale_factor, expected_frame.scale_factor) > 0:
        print "scale_factor error \nE:\n %s \nD:\n %s" % (actual_frame.scale_factor, expected_frame.scale_factor)
        return -1

    if  mse(actual_frame.scalefactor, expected_frame.scalefactor) > 0:
        print "scalefactor error \nE:\n %s \nD:\n %s" % (actual_frame.scalefactor, expected_frame.scalefactor)
        return -1
    
    if  mse(actual_frame.bits, expected_frame.bits) > 0:
        print "bits error \nE:\n %s \nD:\n %s" % (actual_frame.bits, expected_frame.bits)
        return -1

    if actual_frame.crc_check != expected_frame.crc_check:
        print "crc_check wrong (E: %d, D: %d)" % (actual_frame.crc_check, expected_frame.crc_check)
        return -1

    return 0


def get_actual_frame(fin, nr_blocks, nr_subbands, nr_channels, sampling_frequency, bitpool):
    actual_frame = SBCFrame(nr_blocks, nr_subbands, nr_channels, sampling_frequency, bitpool)
    fetch_samples_for_next_sbc_frame(fin, actual_frame)
    sbc_encode(actual_frame)
    return actual_frame

def get_expected_frame(fin_expected):
    expected_frame = SBCFrame()
    sbc_unpack_frame(fin_expected, expected_frame)
    return expected_frame

usage = '''
Usage:      ./sbc_encoder_test.py encoder_input.wav blocks subbands bitpool encoder_expected_output.sbc
Example:    ./sbc_encoder_test.py fanfare.wav 16 4 31 fanfare-4sb.sbc
'''

if (len(sys.argv) < 6):
    print(usage)
    sys.exit(1)
try:
    encoder_input_wav = sys.argv[1]
    nr_blocks = int(sys.argv[2])
    nr_subbands = int(sys.argv[3])
    bitpool = int(sys.argv[4])
    encoder_expected_sbc = sys.argv[5]
    sampling_frequency = 44100

    if not encoder_input_wav.endswith('.wav'):
        print(usage)
        sys.exit(1)

    if not encoder_expected_sbc.endswith('.sbc'):
        print(usage)
        sys.exit(1)

    fin = wave.open(encoder_input_wav, 'rb')
    nr_channels = fin.getnchannels()
    sampling_frequency = fin.getframerate()
    nr_audio_frames = fin.getnframes()

    fin_expected = open(encoder_expected_sbc, 'rb')
    subband_frame_count = 0
    audio_frame_count = 0
    nr_samples = nr_blocks * nr_subbands

    while audio_frame_count < nr_audio_frames:
        if subband_frame_count % 200 == 0:
            print("== Frame %d ==" % (subband_frame_count))

        actual_frame = get_actual_frame(fin, nr_blocks, nr_subbands, nr_channels, bitpool, sampling_frequency)
        expected_frame = get_expected_frame(fin_expected)

        err = sbc_compare_headers(subband_frame_count, actual_frame, expected_frame)
        if err < 0:
            exit(1)

        err = sbc_compare_audio_frames(subband_frame_count, actual_frame, expected_frame)
        if err < 0:
            exit(1)
        audio_frame_count += nr_samples
        subband_frame_count += 1

    print "DONE, max MSE audio sample error %d", max_error
    fin.close()
    fin_expected.close()

except IOError:
    print(usage)
    sys.exit(1)




