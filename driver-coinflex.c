
/*
 * Copyright 2012-2013 Andrew Smith
 * Copyright 2012 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

//=====================================================================//
//***   driver-coinflex.c is for X11 algorithm mining by using Han-Lab's Pantheon-XXX series miner      ***//
//=====================================================================//

//=====================================================================//
//  DRIVER_COINFLEX DEFINITION FOR X11 ALGORITHM
//  Support Product:
//      1) Pantheon-A   : Altera Stratix V E9 FPGA Chip
//                      : 1 base b'd, 10 miner b'd, 1 miner b'd includes 4EA FPGA Chip
//      2) Pantheon-AFS4    : Altera Stratix IV 530 FPGA Chip
//                      : 2 miner b'd(operating independently), 1 miner b'd includes 10EA FPGA Chip
//      3) Pantheon-CMF1 : Altera Stratix V E9 FPGA Chip
//                      :  1 base b'd, 1 core b'd, 1 core b'd includes 1EA FPGA Chip
//=====================================================================//


#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <error.h>

#include "logging.h"
#include "miner.h"
//#include "usbutils.h"
#include "util.h"
#include "driver-coinflex.h"
#include "compat.h"


#include "spi-context.h"
#include "logging.h"
#include "miner.h"
#include "util.h"

#include "A1-board-selector.h"
#include "A1-trimpot-mcp4x.h"

#include "asic_inno.h"
#include "asic_inno_clock.h"
#include "asic_inno_cmd.h"
#include "asic_inno_gpio.h"

#include "mcompat_chain.h"
#include "mcompat_tempctrl.h"
#include "mcompat_fanctrl.h"



#define WORK_SIZE               (80)
#define DEVICE_TARGET_SIZE      (32)
#define TARGET_POS              (80)
#define TARGET_SIZE             (4)
#define MINER_ID_POS            (84)
#define MINER_ID_SIZE           (1)
#define WORK_ID_POS             (85)
#define WORK_ID_SIZE            (1)
#define FIND_NONCE_SIZE         (6)             // For receive value from miner: 4-Bytes nonce, 1-Byte miner_id, 1-Byte work_id

#define REPLY_SIZE              (2)
#define BUF_SIZE                (128)
#define TEMP_UPDATE_INT_MS  10000
#define CHECK_DISABLE_TIME  0

#define DIFF_DEF		(1)
#define DIFF_1HR		(4)
#define DIFF_4HR		(32)
#define DIFF_RUN		(64)


static int ret_pll[ASIC_CHAIN_NUM] = {0};
extern Miner_Device_e eMinerDevice;
static int nCoresNum = 6;

struct spi_config cfg[ASIC_CHAIN_NUM];
struct spi_ctx *spi[ASIC_CHAIN_NUM];
struct A1_chain *chain[ASIC_CHAIN_NUM];

uint8_t A1Pll1=A5_PLL_CLOCK_400MHz;
static uint8_t A1Pll2=A5_PLL_CLOCK_400MHz;
static uint8_t A1Pll3=A5_PLL_CLOCK_400MHz;
static uint8_t A1Pll4=A5_PLL_CLOCK_400MHz;
static uint8_t A1Pll5=A5_PLL_CLOCK_400MHz;
static uint8_t A1Pll6=A5_PLL_CLOCK_400MHz;

static uint32_t show_log[ASIC_CHAIN_NUM];
static uint32_t update_temp[ASIC_CHAIN_NUM];
static uint32_t check_disbale_flag[ASIC_CHAIN_NUM];
static int nChipsNum = 57;


#define STD_V          0.84

int spi_plug_status[ASIC_CHAIN_NUM] = {0};

char szShowLog[ASIC_CHAIN_NUM][ASIC_CHIP_NUM][256] = {0};
char volShowLog[ASIC_CHAIN_NUM][256] = {0};

hardware_version_e g_hwver;
//inno_type_e g_type;
int g_reset_delay = 0xffff;
int g_miner_state = 0;
int chain_flag[ASIC_CHAIN_NUM] = {0};
inno_reg_ctrl_t s_reg_ctrl;

#define MAX_CMD_FAILS		(0)
#define MAX_CMD_RESETS		(50)

static int g_cmd_fails[ASIC_CHAIN_NUM];
static int g_cmd_resets[ASIC_CHAIN_NUM];

struct A1_config_options A1_config_options = {
    .ref_clk_khz = 16000, .sys_clk_khz = 800000, .spi_clk_khz = 2000,
};

/* override values with --bitmine-a1-options ref:sys:spi: - use 0 for default */
static struct A1_config_options *parsed_config_options;
void inno_log_record(int cid, void* log, int len);

//static void coinflex_print_hw_error(char *drv_name, int device_id, struct work *work, uint32_t nonce);
//static bool coinflex_set_algorithm(struct cgpu_info *coinflex);

/********** work queue */
static bool wq_enqueue(struct work_queue *wq, struct work *work)
{
    if (work == NULL)
        return false;

    struct work_ent *we = malloc(sizeof(*we));
    assert(we != NULL);

    we->work = work;
    INIT_LIST_HEAD(&we->head);
    list_add_tail(&we->head, &wq->head);
    wq->num_elems++;
    return true;
}

static struct work *wq_dequeue(struct work_queue *wq)
{
    if (wq == NULL)
        return NULL;
    if (wq->num_elems == 0)
        return NULL;
    struct work_ent *we;
    we = list_entry(wq->head.next, struct work_ent, head);
    struct work *work = we->work;

    list_del(&we->head);
    free(we);
    wq->num_elems--;
    return work;
}


/* queue two work items per chip in chain */
static bool coinflex_queue_full(struct cgpu_info *cgpu)
{
    struct A1_chain *a1 = cgpu->device_data;
    int queue_full = false;

    mutex_lock(&a1->lock);
    //  applog(LOG_NOTICE, "%d, A1 running queue_full: %d/%d",
    //     a1->chain_id, a1->active_wq.num_elems, a1->num_active_chips);

    if (a1->active_wq.num_elems >= a1->num_active_chips * 2)
        queue_full = true;
    else
        wq_enqueue(&a1->active_wq, get_queued(cgpu));

    mutex_unlock(&a1->lock);

    return queue_full;
}

int chain_encore_flag[ASIC_CHAIN_NUM] = {0};

void *chain_detect_thread(void *argv)
{
	int i, cid;
	int chain_id = *(int*)argv;
	uint8_t buffer[REG_LENGTH];

	if (chain_id >= g_chain_num) {
		applog(LOG_ERR, "invalid chain id %d", chain_id);
		return NULL;
	}

	struct A1_chain *a1 = malloc(sizeof(*a1));
	assert(a1 != NULL);
	memset(a1, 0, sizeof(struct A1_chain));

	cid = g_chain_id[chain_id];
	a1->chain_id = cid;
	a1->num_chips = mcompat_chain_preinit(cid);
	if (a1->num_chips == 0) {
		goto failure;
	}

	if (!mcompat_chain_set_pll(cid, opt_A1Pll1, opt_voltage[cid])) {
		goto failure;
	}

	if (!mcompat_chain_init(cid, SPI_SPEED_RUN, false)) {
		goto failure;
	}

	a1->num_active_chips = a1->num_chips;
	a1->chips = calloc(a1->num_active_chips, sizeof(struct A1_chip));
    assert (a1->chips != NULL);

	mcompat_configure_tvsensor(cid, CMD_ADDR_BROADCAST, 0);
	usleep(1000);

	for (i = 0; i < a1->num_active_chips; ++i) {
		check_chip(a1, i);
		s_reg_ctrl.stat_val[cid][i] = a1->chips[i].nVol;
    }

	/* Config to T-sensor */
	mcompat_configure_tvsensor(cid, CMD_ADDR_BROADCAST, 1);
	usleep(1000);

	/* Chip voltage stat. */
	inno_get_voltage_stats(a1, &s_reg_ctrl);

    mutex_init(&a1->lock);
    INIT_LIST_HEAD(&a1->active_wq.head);

	chain[chain_id] = a1;

	return NULL;

failure:
	if (a1->chips) {
		free(a1->chips);
		a1->chips = NULL;
	}
	free(a1);

	g_chain_alive[cid] = 0;

	return NULL;
}


static bool A5_chain_detect()
{
    int i, cid;

    /* Determine working PLL & VID */
    //performance_cfg();

    /* Register PLL map config */
    mcompat_chain_set_pllcfg(g_pll_list, g_pll_regs, PLL_LV_NUM);

    applog(LOG_NOTICE, "Total chains: %d", g_chain_num);
    for (i = 0; i < g_chain_num; ++i) 
    {
        cid = g_chain_id[i];

        /* FIXME: should be thread */
        chain_detect_thread(&i);

        if (!g_chain_alive[cid])
            continue;

        struct cgpu_info *cgpu = malloc(sizeof(*cgpu));
        assert(cgpu != NULL);
        memset(cgpu, 0, sizeof(*cgpu));

        cgpu->drv = &coinflex_drv;
        cgpu->name = "A5.SingleChain";
        cgpu->threads = 1;
        cgpu->chainNum = cid;
        cgpu->device_data = chain[i];

        if ((chain[i]->num_chips <= MAX_CHIP_NUM) && (chain[i]->num_cores <= MAX_CORES))
            cgpu->mhs_av = (double)(opt_A1Pll1 *  (chain[i]->num_cores) / 2);
        else
            cgpu->mhs_av = 0;

        cgtime(&cgpu->dev_start_tv);
        chain[i]->lastshare = cgpu->dev_start_tv.tv_sec;
        chain[i]->cgpu = cgpu;
        add_cgpu(cgpu);

        applog(LOG_NOTICE, "chain%d: detected %d chips / %d cores",
            cid, chain[i]->num_active_chips, chain[i]->num_cores);
    }
}


static void coinflex_detect(bool __maybe_unused hotplug)
{
    struct timeval test_tv;
    int j = 0;
    
    applog(LOG_DEBUG, "A5 Detect Init......\n");
    g_hwver = inno_get_hwver();
    memset(&s_reg_ctrl,0,sizeof(s_reg_ctrl));

    c_temp_cfg tmp_cfg;
    mcompat_tempctrl_get_defcfg(&tmp_cfg);
    tmp_cfg.tmp_min      = -40;     // min value of temperature
    tmp_cfg.tmp_max      = 125;     // max value of temperature
    tmp_cfg.tmp_target   = 70;      // target temperature
    tmp_cfg.tmp_thr_lo   = 30;      // low temperature threshold
    tmp_cfg.tmp_thr_hi   = 95;      // high temperature threshold
    tmp_cfg.tmp_thr_warn = 100;     // warning threshold
    tmp_cfg.tmp_thr_pd   = 105;     // power down threshold
    tmp_cfg.tmp_exp_time = 2000;   // temperature expiring time (ms)
    mcompat_tempctrl_init(&tmp_cfg);
    
    // start fan ctrl thread
    c_fan_cfg fan_cfg;
    mcompat_fanctrl_get_defcfg(&fan_cfg);
    fan_cfg.preheat = false;        // disable preheat
    fan_cfg.fan_mode = g_auto_fan;
    fan_cfg.fan_speed = g_fan_speed;
    fan_cfg.fan_speed_target = 50;
    mcompat_fanctrl_init(&fan_cfg);
    pthread_t tid;
    pthread_create(&tid, NULL, mcompat_fanctrl_thread, NULL);

    // update time
    for(j = 0; j < 64; j++)
    {
        cgtime(&test_tv);
        if(test_tv.tv_sec > 1000000000)
        {
            break;
        }
        usleep(500000);
    }

    //if(detect_A1_chain()){
        //return ;
    //}

    A5_chain_detect();
    
    applog(LOG_WARNING, "A5 Chain dectect finish!\n");
}


static void coinflex_get_statline_before(char *buf, size_t bufsiz, struct cgpu_info *coinflex)
{
    struct A1_chain *a1 = coinflex->device_data;
    char temp[10];
    if (a1->temp != 0)
        snprintf(temp, 9, "%2dC", a1->temp);
    tailsprintf(buf, bufsiz, " %2d:%2d/%3d %s",
            a1->chain_id, a1->num_active_chips, a1->num_cores,
            a1->temp == 0 ? "   " : temp);
}



static void coinflex_flush_work(struct cgpu_info *coinflex)
{
    struct A1_chain *a1 = coinflex->device_data;
    int cid = a1->chain_id;
    //board_selector->select(cid);
    int i;
    uint8_t buffer[4] = {0};

    mutex_lock(&a1->lock);
    /* stop chips hashing current work */
  //  if (!abort_work(a1)) 
   // {
  //      applog(LOG_ERR, "%d: failed to abort work in chip chain!", cid);
  //  }
    /* flush the work chips were currently hashing */
    for (i = 0; i < a1->num_active_chips; i++) 
    {
        int j;
        struct A1_chip *chip = &a1->chips[i];
        for (j = 0; j < 4; j++) 
        {
            struct work *work = chip->work[j];
            if (work == NULL)
                continue;
            //applog(LOG_DEBUG, "%d: flushing chip %d, work %d: 0x%p",
            //       cid, i, j + 1, work);
            work_completed(coinflex, work);
            chip->work[j] = NULL;
        }

        chip->last_queued_id = 0;

       // if(!im_cmd_resetjob(a1->chain_id, i+1, buffer))
       // {
      //      applog(LOG_WARNING, "chip %d clear work failed", i);
      //      continue;
      //  }

        //applog(LOG_INFO, "chip :%d flushing queued work success", i);
    }
    /* flush queued work */
    //applog(LOG_DEBUG, "%d: flushing queued work...", cid);
    while (a1->active_wq.num_elems > 0) 
    {
        struct work *work = wq_dequeue(&a1->active_wq);
        assert(work != NULL);
        work_completed(coinflex, work);
    }
    mutex_unlock(&a1->lock);
}


#define VOLTAGE_UPDATE_INT  6000
#define  LOG_FILE_PREFIX "/tmp/log/analys"
#define  LOG_VOL_PREFIX "/tmp/log/volAnalys"


const char cLevelError1[3] = "!";
const char cLevelError2[3] = "#";
const char cLevelError3[3] = "$";
const char cLevelError4[3] = "%";
const char cLevelError5[3] = "*";
const char cLevelNormal[3] = "+";

void Inno_Log_Save(struct A1_chip *chip,int nChip,int nChain)
{
    char szInNormal[8] = {0};
    memset(szInNormal,0, sizeof(szInNormal));
    if(chip->hw_errors > 0){
        strcat(szInNormal,cLevelError1);
    }
    if(chip->stales > 0){
        strcat(szInNormal,cLevelError2);
    }
    if((chip->temp > 564) || (chip->temp < 445)){
        strcat(szInNormal,cLevelError3);
    }
    if(chip->num_cores < nCoresNum){
        strcat(szInNormal,cLevelError4);
    }
    if((chip->nVol > 580) || (chip->nVol < 450)){
        strcat(szInNormal,cLevelError5);
    }

    if((chip->hw_errors == 0) && (chip->stales == 0) && ((chip->temp < 564) && (chip->temp > 445)) &&((chip->nVol < 550) && (chip->nVol > 450)) && (chip->num_cores == 8)){
        strcat(szInNormal,cLevelNormal);
    }

    sprintf(szShowLog[nChain][nChip], "\n%-8s|%32d|%8d|%8d|%8d|%8d|%8d|%8d|%8d",szInNormal,chip->nonces_found,
            chip->hw_errors, chip->stales,chip->temp,chip->nVol,chip->num_cores,nChip,nChain);
}

void inno_log_print(int cid, void* log, int len)
{
    FILE* fd;
    char fileName[128] = {0};

    sprintf(fileName, "%s%d.log", LOG_FILE_PREFIX, cid);
    
    fd = fopen(fileName, "w+"); 
    
    if(fd == NULL){
        applog(LOG_ERR, "Open log File%d Failed!%s", cid, strerror(errno));
        return; 
    }

    fwrite(log, len, 1, fd);
    fflush(fd);
    fclose(fd);
}

void inno_log_record(int cid, void* log, int len)
{
    FILE* fd;
    char fileName[128] = {0};

    sprintf(fileName, "%s%d.log", LOG_VOL_PREFIX, cid);
    fd = fopen(fileName, "w+"); 
    if(fd == NULL){             
        applog(LOG_ERR, "Open log File%d Failed!%s", cid, strerror(errno));
        return; 
    }

    fwrite(log, len, 1, fd);
    fflush(fd);
    fclose(fd);
}

volatile int g_nonce_read_err = 0;

#define VAL_TO_TEMP(x)  ((double)((594 - x)* 5) / 7.5)
#define INC_PLL_TEMP	95	
#define DEC_PLL_TEMP	105
#define HIGH_PLL		1200
#define LOW_PLL			1100


static void overheated_blinking(int cid)
{
	// block thread and blink led
	while (42) {
		mcompat_set_led(cid, LED_OFF);
		cgsleep_ms(500);
		mcompat_set_led(cid, LED_ON);
		cgsleep_ms(500);
	}
}

static int64_t coinflex_scanwork(struct thr_info *thr)
{
    int i;
    uint8_t reg[128] = {0};
    struct cgpu_info *cgpu = thr->cgpu;
    struct A1_chain *a1 = cgpu->device_data;
    int32_t nonce_ranges_processed = 0;
    int64_t hashes = 0;

    uint32_t nonce;
    uint8_t chip_id;
    uint8_t job_id;
    bool work_updated = false;
    struct timeval now;

    if (a1->num_cores == 0) 
    {
        cgpu->deven = DEV_DISABLED;
        return 0;
    }

    mutex_lock(&a1->lock);
    int cid = a1->chain_id;

    if (a1->last_temp_time + TEMP_UPDATE_INT_MS < get_current_ms())
    {
        show_log[cid]++;
        check_disbale_flag[cid]++;

        cgpu->chip_num = a1->num_active_chips;
        cgpu->core_num = a1->num_cores;

       inno_log_print(cid, szShowLog[cid], sizeof(szShowLog[0]));

        a1->last_temp_time = get_current_ms();
    }
    
    cgtime(&now);
    if (cgpu->drv->max_diff < DIFF_RUN) {
		int hours;

		hours = tdiff(&now, &cgpu->dev_start_tv) / 3600;
		if (hours > 3)
			cgpu->drv->max_diff = DIFF_RUN;
		else if (hours > 2 && cgpu->drv->max_diff < DIFF_4HR)
			cgpu->drv->max_diff = DIFF_4HR;
		else if (hours > 1 && cgpu->drv->max_diff < DIFF_1HR)
			cgpu->drv->max_diff = DIFF_1HR;
	}

    /* poll queued results */
    while (true){
        if (!get_nonce(a1, (uint8_t*)&nonce, &chip_id, &job_id)){
            break;
        }

        work_updated = true;
        if (chip_id < 1 || chip_id > a1->num_active_chips) {
            applog(LOG_WARNING, "%d: wrong chip_id %d", cid, chip_id);
            continue;
        }

        if (job_id < 1 || job_id > 4){
            applog(LOG_WARNING, "%d: chip %d: result has wrong ""job_id %d", cid, chip_id, job_id);
            continue;
        }

        struct A1_chip *chip = &a1->chips[chip_id - 1];
        struct work *work = chip->work[job_id - 1];
        if (work == NULL){
            /* already been flushed => stale */
            applog(LOG_WARNING, "%d: chip %d: stale nonce 0x%08x", cid, chip_id, nonce);
            chip->stales++;
            continue;
        }
        if (!submit_nonce(thr, work, nonce)){
            applog(LOG_WARNING, "%d: chip %d: invalid nonce 0x%08x", cid, chip_id, nonce);
            chip->hw_errors++;
            /* add a penalty of a full nonce range on HW errors */
            nonce_ranges_processed--;
            continue;
        }

        applog(LOG_INFO, "Got nonce for chain %d / chip %d / job_id %d", a1->chain_id, chip_id, job_id);

        chip->nonces_found++;
        hashes += work->device_diff;
		a1->lastshare = now.tv_sec;
    }

    if (unlikely(now.tv_sec - a1->lastshare > CHAIN_DEAD_TIME)) {
		applog(LOG_EMERG, "chain %d not producing shares for more than %d mins, shutting down.",
		       cid, CHAIN_DEAD_TIME / 60);
		// TODO: should restart current chain only
		/* Exit cgminer, allowing systemd watchdog to restart */
		for (i = 0; i < ASIC_CHAIN_NUM; ++i)
			mcompat_chain_power_down(cid);
		exit(1);
	}

    /* check for completed works */
    if(a1->work_start_delay > 0)
    {
        applog(LOG_INFO, "wait for pll stable");
        a1->work_start_delay--;
    }
    else
    {
        hub_spi_clean_chain(cid);
        if( mcompat_cmd_read_register(a1->chain_id, 10, reg,REG_LENGTH_SG) ||  mcompat_cmd_read_register(a1->chain_id, 11, reg,REG_LENGTH_SG) ||  mcompat_cmd_read_register(a1, 12, reg,REG_LENGTH_SG))
        {
            uint8_t qstate = reg[9] & 0x01;

            if (qstate != 0x01)
            {
                work_updated = true;
                for (i = a1->num_active_chips; i > 0; i--) 
                {
                    uint8_t c=i;
                    struct A1_chip *chip = &a1->chips[i - 1];
                    struct work *work = wq_dequeue(&a1->active_wq);
                    if(work == NULL)
                    {
                        applog(LOG_ERR, "Wait work queue...");
                        usleep(500);
                        continue;
                    }
                    //assert(work != NULL);

                    if (set_work(a1, c, work, 0))
                    {
                        nonce_ranges_processed++;
                        chip->nonce_ranges_done++;
                    }                 
                }
            } 
        }
        else
        {
				g_cmd_fails[cid]++;
				if (g_cmd_fails[cid] > MAX_CMD_FAILS) {
					applog(LOG_ERR, "Chain %d reset spihub", cid);
					// TODO: replaced with mcompat_spi_reset()
					hub_spi_clean_chain(cid);
					g_cmd_resets[cid]++;
					if (g_cmd_resets[cid] > MAX_CMD_RESETS) {
						applog(LOG_ERR, "Chain %d is not working due to multiple resets. shutdown.",
						       cid);
						/* Exit cgminer, allowing systemd watchdog to
						 * restart */
						for (i = 0; i < ASIC_CHAIN_NUM; ++i)
							mcompat_chain_power_down(cid);
						exit(1);
					}
				}
			}
    }

	/* Temperature control */
	int chain_temp_status = mcompat_tempctrl_update_chain_temp(cid);

	cgpu->temp_min = (double)g_chain_tmp[cid].tmp_lo;
	cgpu->temp_max = (double)g_chain_tmp[cid].tmp_hi;
	cgpu->temp	   = (double)g_chain_tmp[cid].tmp_avg;

	if (chain_temp_status == TEMP_SHUTDOWN) {
		// shut down chain
		applog(LOG_ERR, "DANGEROUS TEMPERATURE(%.0f): power down chain %d",
			cgpu->temp_max, cid);
		mcompat_chain_power_down(cid);
		cgpu->status = LIFE_DEAD;
		cgtime(&thr->sick);

		/* Function doesn't currently return */
		overheated_blinking(cid);
	}

    mutex_unlock(&a1->lock);

    if (nonce_ranges_processed < 0){
        applog(LOG_INFO, "nonce_ranges_processed less than 0");
        nonce_ranges_processed = 0;
    }else{
        applog(LOG_DEBUG, "%d, nonces processed %d", cid, nonce_ranges_processed);
    }

    cgtime(&a1->tvScryptCurr);
    timersub(&a1->tvScryptCurr, &a1->tvScryptLast, &a1->tvScryptDiff);
    cgtime(&a1->tvScryptLast);

    /* in case of no progress, prevent busy looping */
    if (!work_updated){ // after work updated, also delay 10ms
        cgsleep_ms(5);
    }

    //return ((((double)opt_A1Pll1*a1->tvScryptDiff.tv_usec /2) * (a1->num_cores))/13);
    return hashes * 0x100000000ull;
}

static struct api_data *coinflex_api_stats(struct cgpu_info *cgpu)
{
    struct A1_chain *t1 = cgpu->device_data;
    int fan_speed = g_fan_cfg.fan_speed;
    unsigned long long int chipmap = 0;
    struct api_data *root = NULL;
    char s[32];
    int i;
	
    ROOT_ADD_API(int, "Chain ID", t1->chain_id, false);
    ROOT_ADD_API(int, "Num chips", t1->num_chips, false);
    ROOT_ADD_API(int, "Num cores", t1->num_cores, false);
    ROOT_ADD_API(int, "Num active chips", t1->num_active_chips, false);
    ROOT_ADD_API(int, "Chain skew", t1->chain_skew, false);
    ROOT_ADD_API(double, "Temp max", cgpu->temp_max, false);
    ROOT_ADD_API(double, "Temp min", cgpu->temp_min, false);
   
    ROOT_ADD_API(int, "Fan duty", fan_speed, true);
//	ROOT_ADD_API(bool, "FanOptimal", g_fan_ctrl.optimal, false);
	ROOT_ADD_API(int, "iVid", t1->vid, false);
    ROOT_ADD_API(int, "PLL", t1->pll, false);
	ROOT_ADD_API(double, "Voltage Max", s_reg_ctrl.highest_vol[t1->chain_id], false);
	ROOT_ADD_API(double, "Voltage Min", s_reg_ctrl.lowest_vol[t1->chain_id], false);
	ROOT_ADD_API(double, "Voltage Avg", s_reg_ctrl.avarge_vol[t1->chain_id], false);
//	ROOT_ADD_API(bool, "VidOptimal", t1->VidOptimal, false);
//	ROOT_ADD_API(bool, "pllOptimal", t1->pllOptimal, false);
	ROOT_ADD_API(bool, "VoltageBalanced", t1->voltagebalanced, false);
	ROOT_ADD_API(int, "Chain num", cgpu->chainNum, false);
	ROOT_ADD_API(double, "MHS av", cgpu->mhs_av, false);
	ROOT_ADD_API(bool, "Disabled", t1->disabled, false);
	for (i = 0; i < t1->num_chips; i++) {
		if (!t1->chips[i].disabled)
			chipmap |= 1 << i;
	}
	sprintf(s, "%Lx", chipmap);
	ROOT_ADD_API(string, "Enabled chips", s[0], true);
	ROOT_ADD_API(double, "Temp", cgpu->temp, false);

    int nTemp = 0;
    double fVolValue = 0;
	for (i = 0; i < t1->num_chips; i++) {
		sprintf(s, "%02d HW errors", i);
		ROOT_ADD_API(int, s, t1->chips[i].hw_errors, true);
		sprintf(s, "%02d Stales", i);
		ROOT_ADD_API(int, s, t1->chips[i].stales, true);
		sprintf(s, "%02d Nonces found", i);
		ROOT_ADD_API(int, s, t1->chips[i].nonces_found, true);
		sprintf(s, "%02d Nonce ranges", i);
		ROOT_ADD_API(int, s, t1->chips[i].nonce_ranges_done, true);
		sprintf(s, "%02d Cooldown", i);
		ROOT_ADD_API(int, s, t1->chips[i].cooldown_begin, true);
		sprintf(s, "%02d Fail count", i);
		ROOT_ADD_API(int, s, t1->chips[i].fail_count, true);
		sprintf(s, "%02d Fail reset", i);
		ROOT_ADD_API(int, s, t1->chips[i].fail_reset, true);
		sprintf(s, "%02d Temp", i);
        //nTemp = (int)((594 - t1->chips[i].temp)* 5) / 7.5;
		ROOT_ADD_API(int, s, t1->chips[i].temp , true);
		sprintf(s, "%02d nVol", i);
		ROOT_ADD_API(int, s, t1->chips[i].nVol, true);
		sprintf(s, "%02d PLL", i);
		ROOT_ADD_API(int, s, t1->chips[i].pll, true);
		sprintf(s, "%02d pllOptimal", i);
		ROOT_ADD_API(bool, s, t1->chips[i].pllOptimal, true);
	}
	return root;
}

static struct api_data *coinflex_api_debug(struct cgpu_info *cgpu)
{
    //return;
    applog(LOG_ERR,"\n\n\nGet chip status\n\n\n");
    char buffer[12]={0};
    struct A1_chain *a1 = cgpu->device_data;
    
    int i=0;
    //configure for vsensor

    mutex_lock(&a1->lock);

  
    int chip_temp[MCOMPAT_CONFIG_MAX_CHIP_NUM];
    int chip_volt[MCOMPAT_CONFIG_MAX_CHIP_NUM] = {0};

    
	mcompat_configure_tvsensor(a1->chain_id, CMD_ADDR_BROADCAST, 0);
	usleep(1000);
    mcompat_get_chip_volt(a1->chain_id, chip_volt);
    
    mcompat_configure_tvsensor(a1->chain_id, CMD_ADDR_BROADCAST, 1);
    usleep(1000);
    mcompat_get_chip_temp(a1->chain_id, chip_temp);

    for (i = 0; i < a1->num_active_chips; i++){
        a1->chips[i].temp = chip_temp[i];
        a1->chips[i].nVol = chip_volt[i];
    }

    mutex_unlock(&a1->lock);

    applog(LOG_NOTICE, "A5+ api command dbgstats recieved");
    return coinflex_api_stats(cgpu);

}



struct device_drv coinflex_drv = 
{
    .drv_id                 = DRIVER_coinflex,
    .dname                  = "HLT_Coinflex",
    .name                   = "HLT",
    .drv_ver                = COINFLEX_DRIVER_VER,
    .drv_date               = COINFLEX_DRIVER_DATE,
    .drv_detect             = coinflex_detect,
    .get_statline_before    = coinflex_get_statline_before,
    .queue_full             = coinflex_queue_full,
    .get_api_stats          = coinflex_api_stats,
    .get_api_debug          = coinflex_api_debug,
    .identify_device        = NULL,
    .set_device             = NULL,
    .thread_prepare         = NULL,
    .thread_shutdown        = NULL,
    .hw_reset               = NULL,
    .hash_work              = hash_queued_work,
    .update_work            = NULL,
    .flush_work             = coinflex_flush_work,          // new block detected or work restart 
    .scanwork               = coinflex_scanwork,                // scan hash
    .max_diff                   = 1//65536
};
