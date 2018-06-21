#ifndef _ASIC_INNO_CMD_
#define _ASIC_INNO_CMD_

#include <pthread.h>
#include <sys/time.h>

//#include "asic_inno.h"

#define CMD_TYPE_A7         0xb0
#define CMD_TYPE_A8         0x30
#define CMD_TYPE_A11        0x70
#define CMD_TYPE_A12        0xc0


#define ADDR_BROADCAST      0x00

#define LEN_BIST_START      4
#define LEN_BIST_COLLECT    4
#define LEN_BIST_FIX        4
#define LEN_RESET           6
#define LEN_WRITE_JOB       92
#define LEN_READ_RESULT     10
#define LEN_WRITE_REG       18
#define LEN_READ_REG        12

#define SPI_REC_DATA_LOOP   10
#define SPI_REC_DATA_DELAY  1

#define ASIC_RESULT_LEN     6
#define READ_RESULT_LEN     (ASIC_RESULT_LEN + 2)

#define REG_LENGTH_SG      12
#define JOB_LENGTH      98


#define MAX_CHAIN_LENGTH    33
#define MAX_CMD_LENGTH      (JOB_LENGTH + MAX_CHAIN_LENGTH * 2 * 2)

#define WORK_BUSY 0
#define WORK_FREE 1


struct work_ent {
    struct work *work;
    struct list_head head;
};

struct work_queue {
    int num_elems;
    struct list_head head;
};

struct A1_chip {
    uint8_t reg[12];
    int num_cores;
    int last_queued_id;
    struct work *work[4];
    /* stats */
    int hw_errors;
    int stales;
    int nonces_found;
    int nonce_ranges_done;

    /* systime in ms when chip was disabled */
    int cooldown_begin;
    /* number of consecutive failures to access the chip */
    int fail_count;
    int fail_reset;
    /* mark chip disabled, do not try to re-enable it */
    bool disabled;

    /* temp */
    int temp;

    int nVol;
    int tunedir; // Tune direction, +/- 1

	int pll;
	int cycles;
	double product; // Hashrate product of cycles / time
	bool pllOptimal; // We've stopped tuning frequency
};

struct A1_chain {
    int chain_id;
    struct cgpu_info *cgpu;
    struct mcp4x *trimpot;
    int num_chips;
    int num_cores;
    int num_active_chips;
    int chain_skew;

    uint8_t spi_tx[MAX_CMD_LENGTH];
    uint8_t spi_rx[MAX_CMD_LENGTH];
    struct spi_ctx *spi_ctx;
    struct A1_chip *chips;
    pthread_mutex_t lock;

    struct work_queue active_wq;
    bool throttle; /* Needs throttling */
    int cycles; /* Cycles used for iVid tuning */
	int tunedir; // Tune direction, -1..+1
	int pll; /* Current chain speed */
	int base_pll; /* Initial chain speed */

    int vid; /* Current actual iVid */
    double product; // Hashrate product of cycles / time
	bool VidOptimal; // We've stopped tuning voltage
	bool pllOptimal; // We've stopped tuning frequency
	bool voltagebalanced; // We've balanced voltage b/w chips

    /* mark chain disabled, do not try to re-enable it */
    bool disabled;
    uint8_t temp;
    int last_temp_time;
    int pre_heat;

    time_t lastshare;

    struct timeval tvScryptLast;
    struct timeval tvScryptCurr;
    struct timeval tvScryptDiff;
    int work_start_delay;
};

uint16_t CRC16_2(unsigned char* pchMsg, unsigned short wDataLen);
void hexdump_error(char *prefix, uint8_t *buff, int len);
void hexdump(char *prefix, uint8_t *buff, int len);


#endif
