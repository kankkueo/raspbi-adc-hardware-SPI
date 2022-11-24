
/*  This is an ADC SPI communicator for
 *  a Raspberry pi
 *
 *  Supports 2 modes:
 *      single measurement takes a predefined amount
 *      of samples and saves them into a file
 *
 *      continuous keeps sampling and saves a 
 *      predefined amount of samples once a sample
 *      exceeds a certain threshold
 *
 *  Currently includes a tested preset for
 *  the MCP3008
 */



#include <errno.h> 
#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CHN_AMOUNT 4
#define MODE 1 // 0 = Single measurement, 1 = Continuous operation

char spidev_path[] = "/dev/spidev0.0";

const int blocks = 1; // Blocks to be read at once. default 1
const int samples = 20000; // Samples per channel
const int clockRate = 3600000; // Clock rate
const int channels[CHN_AMOUNT] = {0, 1, 2, 3}; // Channels to be used
const int threshold = 450; // Level threshold

struct spi_ioc_transfer *init_spi(unsigned char *rx, unsigned char *tx) {
    
    struct spi_ioc_transfer *spitr = (struct spi_ioc_transfer *)
        malloc(CHN_AMOUNT * blocks * sizeof(struct spi_ioc_transfer));

    if (!spitr) {
        perror("spi");
        return NULL;
    }
    else {
        memset(spitr, 0, CHN_AMOUNT * blocks * sizeof(struct spi_ioc_transfer));
    }

    /*  Here we set up the ioc transfer array
     *  Data is stored in the buffers in 4 byte increments 
    */

    for (int i = 0; i < blocks; i++) {
        for (int j = 0; j < CHN_AMOUNT; j++) {

            /* Set the correct control bits according to the datasheet
             * Different presets can be chosen by uncommenting
            */

            // mcp3008 single ended mode (len 3) (clock 3.6Mhz)
            tx[(i * CHN_AMOUNT + j) * 4] = 1;
            tx[(i * CHN_AMOUNT + j) * 4 + 1] = 1 << 7 | channels[j] << 4;

            // mcp3008 differential mode (len 3) (clock 3.6Mhz)
            //tx[(i * CHN_AMOUNT + j) * 4] = 1;
            //tx[(i * CHN_AMOUNT + j) * 4 + 1] = channels[j] << 4;

            // ad7924 without sequencer, full power, range 0 - vRef (len 2) (not tested!)
            //tx[(i * CHN_AMOUNT + j) * 4] = 1 << 7 | channels[j] << 2 | 3;
            //tx[(i * CHN_AMOUNT + j) * 4 + 1] = 1 << 1;

            // pointer to data to be sent
            spitr[i * CHN_AMOUNT + j].tx_buf = (unsigned long)&tx[(i * CHN_AMOUNT + j) * 4];
            // pointer to where received data should be stored
            spitr[i * CHN_AMOUNT + j].rx_buf = (unsigned long)&rx[(i * CHN_AMOUNT + j) * 4];
            // size of the transfer in bytes (Set to represent chosen preset!)
            spitr[i * CHN_AMOUNT + j].len = 3;
            // spi clock frequency
            spitr[i * CHN_AMOUNT + j].speed_hz = clockRate;
            // deselect device between transfers
            spitr[i * CHN_AMOUNT + j].cs_change = 1;
        }
    }
    // set the last cs_change to 0
    spitr[CHN_AMOUNT * blocks - 1].cs_change = 0;

    return spitr;
}

// Perform a single transfer and return readings by channel
int *spi_transfer(struct spi_ioc_transfer* spitr, int fd, unsigned char *rx, unsigned char *tx, int *buf) {

    if (ioctl(fd, SPI_IOC_MESSAGE(CHN_AMOUNT * blocks), spitr) < 0) {
        perror("ioctl");
        return NULL;
    }

    for (int i = 0, j = 0; i < CHN_AMOUNT * blocks; i++, j += 4) {
        // Extract the received data
        // Edit according to ADC used
        
        // mcp3008
        buf[i] = ((int) (rx[j + 1] & 3) << 8) + rx[j + 2];
    }

    return buf;
}

// Makes the buffer start from the most recent sample
// Used in continuous mode
int *fix_buffer(int *buf, int *buf_r, int offset) {
    for (int c = 0, i = offset * CHN_AMOUNT; c < samples * CHN_AMOUNT; c++, i++) {
        if (i >= samples * CHN_AMOUNT) { i = 0; }
        buf_r[c] = buf[i];
    }

    return buf_r;
}

void write_file(int *data, char *file) {
    FILE *fp = fopen(file, "w");
    int i, j;

    for (i = 0; i < samples * CHN_AMOUNT; i += CHN_AMOUNT) {
        for (j = 0; j < CHN_AMOUNT - 1; j++) {
            fprintf(fp, "%d,", data[i + j]);
        }
        fprintf(fp, "%d\n", data[i + j]);
    }

    fclose(fp);
}

// Get current time as a string
char *timestring() {
    char *t_string = malloc(128);
    time_t rawtime;
    time( &rawtime );
    struct tm *info = localtime( &rawtime );
    strftime(t_string, 128, "../tmp/%d_%m_%Y_%H_%M_%S.csv", info);
    return t_string;
}

// Calculate elapsed time with millisecond precision
float time_diff(struct timespec start, struct timespec end) {
    unsigned long start_s = start.tv_sec * 1000;
    unsigned long start_ns = start.tv_nsec / 1000000;
    unsigned long start_full = start_s + start_ns;
    unsigned long end_s = end.tv_sec * 1000;
    unsigned long end_ns = end.tv_nsec / 1000000;
    unsigned long end_full = end_s + end_ns;
    return (float) (end_full - start_full) / 1000;
}


int main(int argc, char *argv[]) {

    unsigned char *rx = (unsigned char *)malloc(CHN_AMOUNT * blocks * 4);
    unsigned char *tx = (unsigned char *)malloc(CHN_AMOUNT * blocks * 4);

    if (!rx || !tx) {
        perror("malloc");
        return 1;
    }
    else {
        memset(tx, 0, CHN_AMOUNT * blocks);
        memset(rx, 0, CHN_AMOUNT * blocks);
    }

    int fd = open(spidev_path, O_RDWR);

    struct spi_ioc_transfer *spitr = init_spi(rx, tx);

    int *data_buf = malloc(CHN_AMOUNT * blocks * sizeof(int));
    int *data = malloc(CHN_AMOUNT * sizeof(int) * samples);
    int *data_fixed = malloc(CHN_AMOUNT * sizeof(int) * samples);
    int i, j;
    int s_counter = 0;
    
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);

    // Main loop
    if (MODE) {

        for (i = 0;;i++) {
            if (i >= samples) { i = 0; }

            data_buf = spi_transfer(spitr, fd, rx, tx, data_buf);

            for (int j = 0; j < CHN_AMOUNT * blocks; j++) {
                data[i * CHN_AMOUNT * blocks + j] = data_buf[j];

                if (!s_counter && (data_buf[j] >= threshold)) {
	            printf("%d   %d\n", i, data_buf[j]);
                    s_counter = 1;
                }
            }

            if (s_counter) { s_counter++; }
            if (s_counter >= (samples*2)/3 ) {
		        printf("%d\n", s_counter);
                data_fixed = fix_buffer(data, data_fixed, i);
                char *filename = timestring();
                write_file(data_fixed, filename);
                free(filename);
                s_counter = 0;
            }
        }
    }
    else {
        for (i = 0; i < samples; i += blocks) {
            data_buf = spi_transfer(spitr, fd, rx, tx, data_buf);

            for (j = 0; j < CHN_AMOUNT * blocks; j++) {
                data[i * CHN_AMOUNT * blocks + j] = data_buf[j];
            }
        }
    }

    // Calculate and print sample rate
    clock_gettime(CLOCK_REALTIME, &end);
    float difftime = time_diff(start, end);
    printf("Time elapsed: %fs\n", difftime);
    printf("Sample rate: %fsps\n", samples/difftime);

    // Write
    char *filename = timestring();
    write_file(data, filename);

    free(filename);
    free(data_buf);
    free(data);
    free(data_fixed);
    free(rx);
    free(tx);
    free(spitr);

    return 0;
}

