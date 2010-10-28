/*
 * Copyright (c) 2007, 2008, 2009, 2010 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010 Virginia Polytechnic
 *                                      Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <complex>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <liquid/liquid.h>

#include "usrp_io.h"

#include "config.h"
#if HAVE_LIBLIQUIDRM
#include <liquid/liquidrm.h>
#endif
 
#define USRP_CHANNEL        (0)
 
static bool verbose;
static unsigned int num_packets_received;
static unsigned int num_valid_headers_received;
static unsigned int num_valid_packets_received;
static unsigned int num_bytes_received;

static float SNRdB_av;

typedef struct {
    unsigned char * header;
    unsigned char * payload;
    packetizer p;
} framedata;

static int callback(unsigned char * _rx_header,
                    int _rx_header_valid,
                    unsigned char * _rx_payload,
                    unsigned int _rx_payload_len,
                    framesyncstats_s _stats,
                    void * _userdata)
{
    num_packets_received++;
    if (verbose) {
        printf("********* callback invoked, ");
        printf("SNR=%5.1fdB, ", _stats.SNR);
        printf("rssi=%5.1fdB, ", _stats.rssi);
    }

    // make better estimate of SNR
    float noise_floor = -38.0f; // noise floor estimate
    float SNRdB = _stats.rssi - noise_floor;
    SNRdB_av += SNRdB;

    if ( !_rx_header_valid ) {
        if (verbose) printf("header crc : FAIL\n");
        return 0;
    }
    num_valid_headers_received++;
    unsigned int packet_id = (_rx_header[0] << 8 | _rx_header[1]);
    if (verbose) printf("packet id: %6u\n", packet_id);
    unsigned int payload_len = (_rx_header[2] << 8 | _rx_header[3]);
    fec_scheme fec0 = (fec_scheme)(_rx_header[4]);
    fec_scheme fec1 = (fec_scheme)(_rx_header[5]);

    /*
    // TODO: validate fec0,fec1 before indexing fec_scheme_str
    printf("    payload len : %u\n", payload_len);
    printf("    fec0        : %s\n", fec_scheme_str[fec0]);
    printf("    fec1        : %s\n", fec_scheme_str[fec1]);
    */

    framedata * fd = (framedata*) _userdata;
    fd->p = packetizer_recreate(fd->p, payload_len, fec0, fec1);

    // decode packet
    unsigned char msg_dec[payload_len];
    bool crc_pass = packetizer_decode(fd->p, _rx_payload, msg_dec);
    if (crc_pass) {
        num_valid_packets_received++;
        num_bytes_received += payload_len;
    } else {
        if (verbose) printf("  <<< payload crc fail >>>\n");
    }

    /*
    packetizer_print(fd->p);
    printf("payload len: %u\n", _rx_payload_len);
    unsigned int i;
    for (i=0; i<_rx_payload_len; i++)
        printf("%.2x ", _rx_payload[i]);
    printf("\n");

    for (i=0; i<payload_len; i++)
        printf("%.2x ", msg_dec[i]);
    printf("\n");
    */

    return 0;
}

double calculate_execution_time(struct rusage _start, struct rusage _finish)
{
    double utime = _finish.ru_utime.tv_sec - _start.ru_utime.tv_sec
        + 1e-6*(_finish.ru_utime.tv_usec - _start.ru_utime.tv_usec);
    double stime = _finish.ru_stime.tv_sec - _start.ru_stime.tv_sec
        + 1e-6*(_finish.ru_stime.tv_usec - _start.ru_stime.tv_usec);
    //printf("utime : %12.8f, stime : %12.8f\n", utime, stime);
    return utime + stime;
}

void usage() {
    printf("packet_tx:\n");
    printf("  f     :   center frequency [Hz]\n");
    printf("  b     :   bandwidth [Hz]\n");
    printf("  t     :   run time [seconds]\n");
    printf("  q     :   quiet\n");
    printf("  v     :   verbose\n");
    printf("  u,h   :   usage/help\n");
}

int main (int argc, char **argv)
{
    // command-line options
    verbose = true;

    float min_bandwidth = (32e6 / 512.0);
    float max_bandwidth = (32e6 /   4.0);

    float frequency = 462.0e6;
    float bandwidth = min_bandwidth;
    float num_seconds = 5.0f;

    //
    int d;
    while ((d = getopt(argc,argv,"f:b:t:qvuh")) != EOF) {
        switch (d) {
        case 'f':   frequency = atof(optarg);       break;
        case 'b':   bandwidth = atof(optarg);       break;
        case 't':   num_seconds = atof(optarg);     break;
        case 'q':   verbose = false;                break;
        case 'v':   verbose = true;                 break;
        case 'u':
        case 'h':
        default:
            usage();
            return 0;
        }
    }

    if (bandwidth > max_bandwidth) {
        printf("error: maximum bandwidth exceeded (%8.4f MHz)\n", max_bandwidth*1e-6);
        return 0;
    } else if (bandwidth < min_bandwidth) {
        printf("error: minimum bandwidth exceeded (%8.4f kHz)\n", min_bandwidth*1e-3);
        return 0;
    }

    printf("frequency   :   %12.8f [MHz]\n", frequency*1e-6f);
    printf("bandwidth   :   %12.8f [kHz]\n", bandwidth*1e-3f);
    printf("verbosity   :   %s\n", (verbose?"enabled":"disabled"));

    unsigned int num_blocks = (unsigned int)((2.0f*bandwidth*num_seconds)/(512));

    // create usrp_io object and set properties
    usrp_io * uio = new usrp_io();
    uio->set_rx_freq(USRP_CHANNEL, frequency);
    uio->set_rx_samplerate(2.0f*bandwidth);
    uio->enable_auto_tx(USRP_CHANNEL);

    // retrieve rx port
    gport port_rx = uio->get_rx_port(USRP_CHANNEL);

    num_packets_received = 0;
    num_valid_packets_received = 0;
    num_valid_headers_received = 0;
    num_bytes_received = 0;
    SNRdB_av = 0.0f;

    // framing
    packetizer p = packetizer_create(0,FEC_NONE,FEC_NONE);
    framedata fd;
    fd.header = NULL;
    fd.payload = NULL;
    fd.p = p;
    // set properties to default
    framesyncprops_s props;
    framesyncprops_init_default(&props);
    props.squelch_threshold = -37.0f;
    props.squelch_enabled = 1;
#if 0
    props.agc_bw0 = 1e-3f;
    props.agc_bw1 = 1e-5f;
    props.agc_gmin = 1e-3f;
    props.agc_gmax = 1e4f;
    props.pll_bw0 = 1e-3f;
    props.pll_bw1 = 3e-5f;
#endif
    flexframesync fs = flexframesync_create(&props,callback,(void*)&fd);

    std::complex<float> data_rx[512];

#if HAVE_LIBLIQUIDRM
    // test r/m daemon
    rmdaemon rmd = rmdaemon_create();
    rmdaemon_start(rmd);
#endif
 
    // start usrp data transfer
    uio->start_rx(USRP_CHANNEL);
    printf("usrp data transfer started\n");
 
    unsigned int i;
    struct rusage start;
    struct rusage finish;
    getrusage(RUSAGE_SELF, &start);
    // read samples from buffer, run through frame synchronizer
    for (i=0; i<num_blocks; i++) {
        // grab data from port
        gport_consume(port_rx, (void*)data_rx, 512);

        // run through frame synchronizer
        flexframesync_execute(fs, data_rx, 512);

#if HAVE_LIBLIQUIDRM
        // compute cpu load
        double runtime = rmdaemon_gettime(rmd);
        if ( runtime > 0.7 ) {
            double cpuload = rmdaemon_getcpuload(rmd);
            rmdaemon_resettimer(rmd);
            printf("  cpuload : %f\n", cpuload);
        }
#endif
    }
    getrusage(RUSAGE_SELF, &finish);

#if HAVE_LIBLIQUIDRM
    rmdaemon_stop(rmd);
    rmdaemon_destroy(rmd);
#endif

    double extime = calculate_execution_time(start,finish);

    // stop usrp data transfer
    uio->stop_rx(USRP_CHANNEL);
    printf("usrp data transfer complete\n");

    // print results
    float data_rate = 8.0f * (float)(num_bytes_received) / num_seconds;
    float percent_headers_valid = (num_packets_received == 0) ?
                          0.0f :
                          100.0f * (float)num_valid_headers_received / (float)num_packets_received;
    float percent_packets_valid = (num_packets_received == 0) ?
                          0.0f :
                          100.0f * (float)num_valid_packets_received / (float)num_packets_received;
    if (num_packets_received > 0)
        SNRdB_av /= num_packets_received;
    float PER = 1.0f - 0.01f*percent_packets_valid;
    float spectral_efficiency = data_rate / bandwidth;
    float cpu_speed = 2.4e9;    // cpu clock frequency (estimate)
    printf("    packets received    : %6u\n", num_packets_received);
    printf("    valid headers       : %6u (%6.2f%%)\n", num_valid_headers_received,percent_headers_valid);
    printf("    valid packets       : %6u (%6.2f%%)\n", num_valid_packets_received,percent_packets_valid);
    printf("    packet error rate   : %16.8e\n", PER);
    printf("    average SNR [dB]    : %8.4f\n", SNRdB_av);
    printf("    bytes_received      : %6u\n", num_bytes_received);
    printf("    data rate           : %12.8f kbps\n", data_rate*1e-3f);
    printf("    spectral efficiency : %12.8f b/s/Hz\n", spectral_efficiency);
    printf("    execution time      : %12.8f s\n", extime);
    printf("    %% cpu               : %12.8f\n", 100.0f*extime / num_seconds);
    printf("    clock cycles / bit  : ");
    if (num_bytes_received == 0)
        printf("-\n");
    else
        printf("%12.4e\n", cpu_speed * extime / (8.0f*num_bytes_received));

    printf("%12.8f %11.4e\n", spectral_efficiency, cpu_speed * extime / (8.0f*num_bytes_received));

#if 0
    printf("    # rx   # ok      %% ok        # data     %% data       PER          SNR      sp. eff.\n");
    printf("    %-6u %-6u  %12.4e  %-6u   %12.4e %12.4e  %8.4f  %6.4f\n",
            num_packets_received,
            num_valid_headers_received,
            percent_headers_valid * 0.01f,
            num_valid_packets_received,
            percent_packets_valid * 0.01f,
            PER,
            SNRdB_av,
            spectral_efficiency);
#endif

    // clean it up
    flexframesync_destroy(fs);
    packetizer_destroy(p);

    delete uio;
    return 0;
}

