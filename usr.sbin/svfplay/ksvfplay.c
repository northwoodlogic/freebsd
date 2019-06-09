/*-
 * Copyright (c) 2014,2015,2016 David Rush <northwoodlogic@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/gpio.h>

#include "libxsvf/libxsvf.h"
#include <dev/ksvf/ksvf.h>

struct ksvfplay_args {
    int dev_fd;

    /* The SVF file is memory mapped into this process */
    int   svf_fd;
    char *svf_data;
    int   svf_data_len;
    int   svf_data_ptr;

    uint32_t idcode;
    uint32_t simulate;
    uint64_t tck_total;
    uint64_t tck_count;

    /* This is just for the progress indicator */
    uint64_t tck_step;
    uint64_t tck_compare;

    struct ksvf_req kreq;
};

static void usage(void);
int main_analyze(struct ksvfplay_args *args);
int main_idcode(struct ksvfplay_args *args);
int main_play(struct ksvfplay_args *args);

static void
usage()
{
    static const char* help = 
    "\nAbout: ksvfplay - A SVF player for FreeBSD on the Raspberry Pi\n"
    "\n"
    "Usage:\n"
    "  Play SVF file onto target device\n"
    "    ksvfplay -p -f /path/to/my.svf -d /dev/ksvf0\n\n"
    "  Read JTAG IDCODE from target device\n"
    "    ksvfplay -i -d /dev/ksvf0\n\n"
    "Options:\n"
    "\n"
    " -f <svf file>       Path to SVF file\n"
    " -d <jtag device>    Path to JTAG device, usually /dev/ksvf0\n"
    " -p                  Play SVF file\n"
    " -a                  Analyze only, test play SVF file\n"
#if 0
    " -s                  Analyze then simulate, drive GPIO pins and\n"
    "                     simulate success. The device should not be connected\n"
    " -q                  Be quiet, don't print progress indicator\n"
#endif
    " -i                  Read device ID code\n"
    " -h                  Show this message\n"
    "\n"
    "Returns: non-zero on failure\n"
    "\n"
    " R-PI Model B GPIO, J1 Pinout:\n"
    "\n"
    " P1-01 is in the left side of the board, closest to the SD card socket. GPIO\n"
    " pins utilized for JTAG signals are identified by the JTAG signal name in \n"
    " parenthesis. Remember, the RPI-B I/O pins utilize 3.3V signaling. They are\n"
    " not 5 volt tolerant!\n"
    "\n"
    " For the most current documentation on the RPI-B device see the following\n"
    " URL: http://elinux.org/RPi_Low-level_peripherals\n"
    "\n"
    "The following diagram shows the suggested pin numbers to use for the JTAG\n"
    "signals. This is in fact what the ksvf_gpio.ko driver uses by default. If\n"
    "needed, the JTAG signals may be moved to alternate pins via the driver's\n"
    "sysctl interface.\n"
    "\n"
    "                          -----                             \n"
    "           3.3V - [P1-01] |*|*| [P1-02] - 5V                \n"
    "        I2C SDA - [P1-03] |*|*| [P1-04] - 5V                \n"
    "        I2C SCL - [P1-05] |*|*| [P1-06] - Ground            \n"
    "         GPIO 4 - [P1-07] |*|*| [P1-08] - UART TX           \n"
    "         Ground - [P1-09] |*|*| [P1-10] - UART RX           \n"
    "(TDO) - GPIO 17 - [P1-11] |*|*| [P1-12] - GPIO 18           \n"
    "      - GPIO 27 - [P1-13] |*|*| [P1-14] - Ground            \n"
    "(TDI) - GPIO 22 - [P1-15] |*|*| [P1-16] - GPIO 23 - (TMS)   \n"
    "           3.3V - [P1-17] |*|*| [P1-18] - GPIO 24 - (TCK)   \n"
    "       SPI MOSI - [P1-19] |*|*| [P1-20] - Ground            \n"
    "       SPI MISO - [P1-21] |*|*| [P1-22] - GPIO 25           \n"
    "       SPI SCLK - [P1-23] |*|*| [P1-24] - SPI CE0           \n"
    "         Ground - [P1-25] |*|*| [P1-26] - SPI CE1           \n"
    "                          -----                             \n"
    "\n";
    printf("%s", help);
    exit(0);
}

static void
show_progress(struct ksvfplay_args *args)
{
//    double progress = 0.0;
    if(args->tck_count < args->tck_compare) {
        return;
    }
    args->tck_compare += args->tck_step;
//    progress = ((double)args->tck_total / (double)args->tck_count) * 100.0;
    fprintf(stdout, "Progress --> %lld --> %lld\n",
            args->tck_total,
            args->tck_count);
}


/* LibXSVF callback functions */

static void*
cb_realloc(struct libxsvf_host *h __unused,
    void *ptr, int size, enum libxsvf_mem which __unused)
{
    return realloc(ptr, size);
}

static int
cb_set_frequency(struct libxsvf_host *h __unused, int freq __unused)
{
    /* Do nothing */
    return 0;
}

static void
cb_report_error(
    struct libxsvf_host* h __unused,
    const char* file, int line, const char* message)
{
    fprintf(stderr,
        "Error: file = %s, line = %d, (%s)\n", file, line, message);
}

static void
cb_report_device(struct libxsvf_host *h, unsigned long idcode)
{
    struct ksvfplay_args *args = h->user_data;
    args->idcode = (uint32_t)idcode;
}

static int
cb_get_byte(struct libxsvf_host *h)
{
    struct ksvfplay_args *args = h->user_data;
    if(args->svf_data_ptr >= args->svf_data_len) {
        return -1;
    }
    return args->svf_data[args->svf_data_ptr++];
}

/* analyze mode counts total TCK count during test mode run operations too */
static void
cb_analyze_udelay(struct libxsvf_host *h,
    long usecs __unused, int tms __unused, long num_tck)
{
    struct ksvfplay_args *args = h->user_data;
    args->tck_total += num_tck;
}

/* analyze mode counts the total TCK count and always returns success */
static int
cb_analyze_pulse_tck(struct libxsvf_host *h,
    int tms __unused, int tdi __unused, int tdo,
    int rmask __unused, int sync __unused)
{
    struct ksvfplay_args *args = h->user_data;
    args->tck_total++;
    return tdo < 0 ? 0 : tdo;
}

/* analyze setup only needs to make sure the svf file is mmapped in and
 * then zero the svf_data_ptr. main() should have already mmapped it in. */
static int
cb_analyze_setup(struct libxsvf_host *h)
{
    struct ksvfplay_args *args = h->user_data;
    args->svf_data_ptr = 0;
    return (args->svf_data_len <= 0) || (args->svf_data == NULL) ? -1 : 0;
}

static int
cb_analyze_shutdown(struct libxsvf_host *h __unused)
{
    return 0;
}

static void
cb_play_udelay(struct libxsvf_host *h, long usecs, int tms, long num_tck)
{
    long remaining = num_tck;
    struct ksvfplay_args *args = h->user_data;

    if(usecs > 0) {
        usleep(usecs);
    }

    while(remaining > 0) {
        args->kreq.tck_cnt = (remaining > 100000) ? 100000 : remaining;
        args->kreq.tms_val = tms;
        if(ioctl(args->dev_fd, KSVF_UDELAY, &args->kreq)) {
            // TODO: How to report error from the udelay function?????
            perror("KSVF ERROR");
        }
        remaining -= args->kreq.tck_cnt;
        args->tck_count += args->kreq.tck_cnt;
        show_progress(args);
    }
}

static int
cb_play_pulse_tck(struct libxsvf_host *h,
    int tms, int tdi, int tdo, int rmask __unused, int sync __unused)
{
    int line_tdo = -1;
    struct ksvfplay_args *args = h->user_data;
    
    args->kreq.tdi_val = tdi;
    args->kreq.tdo_val = tdo;
    args->kreq.tms_val = tms;
    if(ioctl(args->dev_fd, KSVF_PULSE, &args->kreq)) {
        perror("KSVF pulse failed");
    }
    line_tdo = args->kreq.tdo_val;
    args->tck_count++;
    return (tdo < 0) || (line_tdo == tdo) ? line_tdo : -1;
}

static int
cb_verbose_play_pulse_tck(struct libxsvf_host *h,
    int tms, int tdi, int tdo, int rmask, int sync)
{
    struct ksvfplay_args *args = h->user_data;
    int rc = cb_play_pulse_tck(h, tms, tdi, tdo, rmask, sync);
    show_progress(args);
    return rc;
}


static int
cb_play_setup(struct libxsvf_host *h)
{
    struct ksvfplay_args *args = h->user_data;
    int rc = ioctl(args->dev_fd, KSVF_INIT, &args->kreq);
    args->tck_count = 0;
    args->tck_compare = 0;
    /* The analyze pass before program will have computed tck_total */
    args->tck_step = args->tck_total / 100;

    args->svf_data_ptr = 0;
    return rc;
}

static int
cb_play_shutdown(struct libxsvf_host *h)
{
    struct ksvfplay_args *args = h->user_data;
    int rc = ioctl(args->dev_fd, KSVF_FINI, &args->kreq);
    // TODO: Close the device file descriptor and unmap memory.
    return rc;
}

/* analyze requires an svf file, does not require hardware access */
int
main_analyze(struct ksvfplay_args *args)
{
    int rc;
    struct libxsvf_host jtag_host = {
        /* Specific to analyze mode */
        .setup         = cb_analyze_setup,
        .shutdown      = cb_analyze_shutdown,
        .udelay        = cb_analyze_udelay,
        .pulse_tck     = cb_analyze_pulse_tck,

        /* Common to all player modes */
        .getbyte       = cb_get_byte,
        .realloc       = cb_realloc,
        .report_error  = cb_report_error,
        .set_frequency = cb_set_frequency,
        .report_device = cb_report_device,
        .user_data     = args
    };
    rc = libxsvf_play(&jtag_host, LIBXSVF_MODE_SVF);
    if(rc) {
        fprintf(stderr, "Analyze play failed\n");
    }
    return rc;
}

/* idcode does not require an svf file, does require hardware access */
int
main_idcode(struct ksvfplay_args *args)
{
    int rc;
    struct libxsvf_host jtag_host = {
        /* Specific to play mode */
        .setup         = cb_play_setup,
        .shutdown      = cb_play_shutdown,
        .udelay        = cb_play_udelay,
        .pulse_tck     = cb_play_pulse_tck,

        /* Common to all player modes */
        .getbyte       = cb_get_byte,
        .realloc       = cb_realloc,
        .report_error  = cb_report_error,
        .set_frequency = cb_set_frequency,
        .report_device = cb_report_device,
        .user_data     = args
    };

    rc = libxsvf_play(&jtag_host, LIBXSVF_MODE_SCAN);
    if(rc) {
        fprintf(stderr, "IDCODE play failed\n");
    }
    return rc;
}

/* play requires both an svf file and hardware access */
int
main_play(struct ksvfplay_args *args)
{
    int rc;
    struct libxsvf_host jtag_host = {
        /* Specific to play mode */
        .setup         = cb_play_setup,
        .shutdown      = cb_play_shutdown,
        .udelay        = cb_play_udelay,
        .pulse_tck     = cb_verbose_play_pulse_tck,

        /* Common to all player modes */
        .getbyte       = cb_get_byte,
        .realloc       = cb_realloc,
        .report_error  = cb_report_error,
        .set_frequency = cb_set_frequency,
        .report_device = cb_report_device,
        .user_data     = args
    };

    /* Analyze first to validate the SVF file and count the total
     * TCK count so that a progress indicator can be printed */
    rc = main_analyze(args);
    if(rc) {
        return rc;
    }

    rc = libxsvf_play(&jtag_host, LIBXSVF_MODE_SVF);
    if(rc) {
        fprintf(stderr, "Program play failed\n");
    }
    return rc;
}

int
main(int argc, char *argv[])
{
    int rc;
    int nothing = 1;
    int help = 0;
    int play = 0;
    int idcode = 0;
    int analyze = 0;
    int simulate = 0;
    const char *svf_file_name = NULL;
    //const char *ksvf_dev_name = "/dev/ksvf0";
    const char *ksvf_dev_name = NULL;
    struct ksvfplay_args svf_args;
    memset(&svf_args, 0, sizeof(svf_args));

    int ch;
    while((ch = getopt(argc, argv, "hHaf:d:ip")) != -1) {
        switch(ch) {
            case 'h':
            case 'H':
                help++;
                break;
            case 'a':
                nothing = 0;
                analyze++;
                break;
            case 'i':
                nothing = 0;
                idcode++;
                break;
            case 'p':
                nothing = 0;
                play++;
                break;
            case 'f':
                svf_file_name = optarg;
                break;
            case 'd':
                ksvf_dev_name = optarg;
                break;
            default:
                fprintf(stderr, "Error: Unrecognized option\n\n");
                usage();
                break;
        }
    }

    if(help || nothing) {
        usage();
    }

    /* Open the SVF file and map it into our process memory */
    if(svf_file_name != NULL) {
        struct stat sb;
        void *map_addr = NULL;
	memset(&sb, 0, sizeof(sb));
        svf_args.svf_fd = open(svf_file_name, O_RDONLY);
        if(svf_args.svf_fd == -1) {
            fprintf(stderr, "\nError: could not open SVF file: %s\n\n", svf_file_name);
            exit(1);
        }

        rc = fstat(svf_args.svf_fd, &sb);
        if(rc == -1) {
            fprintf(stderr, "\nError: could not stat SVF file: %s\n\n", svf_file_name);
            exit(1);
        }

        if(!S_ISREG(sb.st_mode)) {
            fprintf(stderr, "\nError: SVF file is not a regular file: %s\n\n", svf_file_name);
            exit(1);
        }

        svf_args.svf_data_ptr = 0;
        svf_args.svf_data_len = sb.st_size;

        map_addr = mmap(NULL, svf_args.svf_data_len, PROT_READ, MAP_PRIVATE, svf_args.svf_fd, 0);
        if(map_addr == MAP_FAILED) {
            fprintf(stderr, "\nError: Unable to mmap SVF file: %s\n\n", svf_file_name);
            exit(1);
        }
        svf_args.svf_data = (char *)map_addr;
    }

    /* Open the KSVF kernel driver node */
    if(ksvf_dev_name != NULL) {
        struct stat sb;
	memset(&sb, 0, sizeof(sb));
        svf_args.dev_fd = open(ksvf_dev_name, O_RDONLY);
        if(svf_args.dev_fd == -1) {
            fprintf(stderr, "\nError: could not open KSVF device: %s\n\n", ksvf_dev_name);
            exit(1);
        }

        rc = fstat(svf_args.svf_fd, &sb);
        if(rc == -1) {
            fprintf(stderr, "\nError: could not stat KSVF device: %s\n\n", ksvf_dev_name);
            exit(1);
        }
/* Don't know why this test fails???
        if(!S_ISCHR(sb.st_mode)) {
            fprintf(stderr, "\nError: KSVF dev is not a character device: %s\n\n", ksvf_dev_name);
            exit(1);
        }
*/
    }

    if(analyze) {

        if(idcode || simulate) {
            fprintf(stderr, "\nError: '-a' is mutually exclusive with '-i'|'-s' options\n\n");
            exit(1);
        }

        if(!svf_file_name) {
            fprintf(stderr, "\nError: '-a' requires '-f' option\n\n");
            exit(1);
        }

        rc = main_analyze(&svf_args);
        if(rc == 0) {
            fprintf(stdout,
                "TCK Count: %llu\n", (unsigned long long)svf_args.tck_total);
        } else {
            fprintf(stderr,
                "Error: Analyze (%s) failed\n", svf_file_name);
        }
        return rc;
    }

    if(idcode) {
        if(analyze || simulate) {
            fprintf(stderr, "\nError: '-i' is mutually exclusive with '-a'|'-s' options\n\n");
            exit(1);
        }

        if(!ksvf_dev_name) {
            fprintf(stderr, "\nError: '-i' requires '-d' option\n\n");
            exit(1);
        }

        rc = main_idcode(&svf_args);
        if(rc == 0) {
            fprintf(stdout,
                "IDCODE: 0x%08X\n", svf_args.idcode);
        } else {
            fprintf(stderr,
                "Error: Read ID code (%s) failed\n", ksvf_dev_name);
        }
        return rc;
    }

    if(play) {
        if(!svf_file_name) {
            fprintf(stderr, "\nError: SVF file name unspecified\n\n");
            exit(1);
        }

        if(!ksvf_dev_name) {
            fprintf(stderr, "\nError: Device name unspecified\n\n");
            exit(1);
        }
        rc = main_play(&svf_args);
        if(rc == 0) {
            fprintf(stderr, "\nOperation Complete\n");
        }
        return rc;
    }

    fprintf(stderr, "Specify one of [-a|-i|-d] player actions...\n");
    return 1;
}

