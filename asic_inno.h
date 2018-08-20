#ifndef _ASIC_INNO_
#define _ASIC_INNO_

#include <stdint.h>
#include "elist.h"
#include "mcompat_drv.h"
#include "mcompat_lib.h"
#include "asic_inno_cmd.h"


#define WEAK_CHIP_THRESHOLD     1
#define BROKEN_CHIP_THRESHOLD   1
#define WEAK_CHIP_SYS_CLK       (600 * 1000)
#define BROKEN_CHIP_SYS_CLK     (400 * 1000)


#define ASIC_CHAIN_NUM          6
#define ASIC_CHIP_NUM           33
#define ASIC_CORE_NUM           (8)
#define MAX_CHIP_NUM            (ASIC_CHIP_NUM)
#define MAX_CORES               (MAX_CHIP_NUM * ASIC_CORE_NUM)



#define CHIP57_VID_DEF          (3)
#define CHIP33_VID_DEF          (8)
#define CHIP_VID_RUN            (10)
#define SPI_SPEED_RUN			(SPI_SPEED_6250K)
#define PLL_LV_NUM				(119)
#define CHAIN_DEAD_TIME			(600)



#define INNO_MINER_TYPE_FILE            "/tmp/type"
#define INNO_HARDWARE_VERSION_FILE      "/tmp/hwver"

typedef enum{
    HARDWARE_VERSION_NONE = 0x00,
    HARDWARE_VERSION_G9   = 0x09,
    HARDWARE_VERSION_G19  = 0x13,
}hardware_version_e;

typedef enum{
    INNO_TYPE_NONE = 0x00,
    INNO_TYPE_A4,
    INNO_TYPE_A5,
    INNO_TYPE_A6,
    INNO_TYPE_A7,
    INNO_TYPE_A8,
    INNO_TYPE_A9,
	INNO_TYPE_A11,
    INNO_TYPE_A12,
}inno_type_e;



typedef enum{
    MINER_DEVICE_33 = 0x00,
    MINER_DEVICE_57,
}Miner_Device_e;

Miner_Device_e eMinerDevice;

//add 0922
typedef struct{
   double highest_vol[ASIC_CHAIN_NUM];    /* chip temp bits */;
   double lowest_vol[ASIC_CHAIN_NUM];    /* chip temp bits */;
   double avarge_vol[ASIC_CHAIN_NUM];    /* chip temp bits */; 
   int stat_val[ASIC_CHAIN_NUM][ASIC_CHIP_NUM];
   int stat_cnt[ASIC_CHAIN_NUM][ASIC_CHIP_NUM];
}inno_reg_ctrl_t;


bool inno_check_voltage(struct A1_chain *a1, int chip_id, inno_reg_ctrl_t *s_reg_ctrl);
void inno_configure_tvsensor(struct A1_chain *a1, int chip_id,bool is_tsensor);
int inno_get_voltage_stats(struct A1_chain *a1, inno_reg_ctrl_t *s_reg_ctrl);


//extern int power_down_all_chain(void);
extern bool zynq_spi_exit(void);
extern hardware_version_e inno_get_hwver(void);
extern inno_type_e inno_get_miner_type(void);

int get_current_ms(void);
bool is_chip_disabled(struct A1_chain *a1, uint8_t chip_id);
void disable_chip(struct A1_chain *a1, uint8_t chip_id);
void check_disabled_chips(struct A1_chain *a1);
bool check_chip(struct A1_chain *a1, int cid);
int chain_detect(struct A1_chain *a1);

bool get_nonce(struct A1_chain *a1, uint8_t *nonce, uint8_t *chip_id, uint8_t *job_id);
bool set_work(struct A1_chain *a1, uint8_t chip_id, struct work *work, uint8_t queue_states);
bool abort_work(struct A1_chain *a1);

const int g_pll_list[PLL_LV_NUM];
const uint8_t g_pll_regs[PLL_LV_NUM][REG_LENGTH_SG];


#endif

