#ifndef _ASIC_INNO_
#define _ASIC_INNO_


//#include "asic_inno_cmd.h"
//#include "inno_fan.h"
#include <stdint.h>

#include "elist.h"

#include "im_drv.h"
#include "im_lib.h"


#define CHIP_A12

#define ALG_SIA

//#define FPGA_DEBUG_MODE
//#define SIA_DEBUG_MODE
#define NO_FAN_CTRL

#ifdef SIA_DEBUG_MODE
#define SIA_DBG_PREVHASH        "0000000000000030e5763f4455188db331d0e0ac348118e00bf3ac6315610e6a"
#define SIA_DBG_NONCE           "da9032cfff010000"
#define SIA_DBG_NTIME           "a8f1d45900000000"
#define SIA_DBG_MERKLE          "32a8dfd9f6bddc23c04c6e7be3b8bcd7e77be86a7f042d55bdfc51468673a4dd"
#define SIA_DBG_HASH            "00000000028eef64fd8c11b0ebdc427bea4f721c32c35b236cc46499ea01a438"
#endif

#ifdef CHIP_A12
#define ASIC_CHAIN_NUM          (3)
#define ASIC_CHIP_NUM           (45)    // 45
#define ASIC_CORE_NUM           (63)    // 63
#endif

#ifdef CHIP_A6
#define JOB_LENGTH              (92)
#endif

#ifdef CHIP_A7
#define JOB_LENGTH              (98)
#endif

#ifdef CHIP_A12
#define JOB_LENGTH              (120)
#endif

#define NONCE_LEN               (4)

#ifdef CHIP_A12
#define BLOCK_HEADER_LEN        (80)
#define TARGET_LEN              (32)
#else
#define BLOCK_HEADER_LEN        (112)
#define TARGET_LEN              (4)
#endif

#define MAX_CMD_LENGTH          (JOB_LENGTH + ASIC_CHIP_NUM * 2 * 2)
#define REG_LENGTH              (12)


#define WEAK_CHIP_THRESHOLD     (1)
#define BROKEN_CHIP_THRESHOLD   (1)
#define WEAK_CHIP_SYS_CLK       (600 * 1000)
#define BROKEN_CHIP_SYS_CLK     (400 * 1000)

#ifdef CHIP_A12
#define CHIP_PLL_DEF            (30)
#define CHIP_VID_DEF            (8)
#define CHIP_VOL_MAX            (0.6)
#define CHIP_VOL_MIN            (0.45)
#else
#define CHIP_PLL_DEF            (120)
#define CHIP_VID_DEF            (8)
#define CHIP_VOL_MAX            (0.55)
#define CHIP_VOL_MIN            (0.45)
#endif

#define USE_AUTONONCE
#define USE_RT_TEMP_CTRL

#define INNO_MINER_TYPE_FILE            "/tmp/type"
#define INNO_HARDWARE_VERSION_FILE      "/tmp/hwver"

typedef enum{
    HARDWARE_VERSION_NONE = 0x00,
    HARDWARE_VERSION_G9 = 0x09,
    HARDWARE_VERSION_G19 = 0x13,
} hardware_version_e;

/*
typedef enum{
    MINER_TYPE_NONE = 0x00,
    MINER_TYPE_T0,
    MINER_TYPE_T1,
    MINER_TYPE_T2,
    MINER_TYPE_T3,
    MINER_TYPE_T4,
    MINER_TYPE_T5,
    MINER_TYPE_SUM,
}miner_type_e;
*/
    
typedef enum{
    INNO_TYPE_NONE = 0x00,
    INNO_TYPE_A4,
    INNO_TYPE_A5,
    INNO_TYPE_A6,
    INNO_TYPE_A7,
    INNO_TYPE_A8,
    INNO_TYPE_A9,
    INNO_TYPE_A12,
}inno_type_e;

typedef struct{
   float highest_vol[ASIC_CHAIN_NUM];    /* chip temp bits */;
   float lowest_vol[ASIC_CHAIN_NUM];    /* chip temp bits */;
   float avarge_vol[ASIC_CHAIN_NUM];    /* chip temp bits */; 
   int stat_val[ASIC_CHAIN_NUM][ASIC_CHIP_NUM];
   int stat_cnt[ASIC_CHAIN_NUM][ASIC_CHIP_NUM];
}inno_reg_ctrl_t;

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
};

struct A1_chain {
    int chain_id;
    struct cgpu_info *cgpu;
    struct mcp4x *trimpot;
    int num_chips;
    int num_cores;
    int num_active_chips;
    int chain_skew;
    int vid;
    uint8_t spi_tx[MAX_CMD_LENGTH];
    uint8_t spi_rx[MAX_CMD_LENGTH];
    struct spi_ctx *spi_ctx;
    struct A1_chip *chips;
    pthread_mutex_t lock;

    struct work_queue active_wq;

    /* mark chain disabled, do not try to re-enable it */
    bool disabled;
    uint8_t temp;
    int last_temp_time;
    int pre_heat;

    struct timeval tvScryptLast;
    struct timeval tvScryptCurr;
    struct timeval tvScryptDiff;
    int work_start_delay;
};

bool inno_check_voltage(struct A1_chain *a1, int chip_id, inno_reg_ctrl_t *s_reg_ctrl);
void inno_configure_tvsensor(struct A1_chain *a1, int chip_id,bool is_tsensor);
int inno_get_voltage_stats(struct A1_chain *a1, inno_reg_ctrl_t *s_reg_ctrl);

extern hardware_version_e inno_get_hwver(void);
extern inno_type_e inno_get_miner_type(void);

int get_current_ms(void);
bool is_chip_disabled(struct A1_chain *a1, uint8_t chip_id);
void disable_chip(struct A1_chain *a1, uint8_t chip_id);
void check_disabled_chips(struct A1_chain *a1);
bool check_chip(struct A1_chain *a1, int cid);
bool set_chain_pll(uint8_t chain_id, int pll_lv);
int chain_detect(struct A1_chain *a1);

bool get_nonce(struct A1_chain *a1, uint8_t *nonce, uint8_t *chip_id, uint8_t *job_id);
bool set_work(struct A1_chain *a1, uint8_t chip_id, struct work *work, uint8_t queue_states);
bool abort_work(struct A1_chain *a1);

int im_chain_power_on(int chain_id);
int im_chain_power_down(int chain_id);
int im_power_on_all_chain(void);
int im_power_down_all_chain(void);

#endif

