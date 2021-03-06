
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

static uint32_t update_temp[ASIC_CHAIN_NUM];
static uint32_t check_disbale_flag[ASIC_CHAIN_NUM];


#define STD_V          0.84

int spi_plug_status[ASIC_CHAIN_NUM] = {0};

hardware_version_e g_hwver;
//inno_type_e g_type;
int g_reset_delay = 0xffff;
int g_miner_state = 0;
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
//static void coinflex_print_hw_error(char *drv_name, int device_id, struct work *work, uint32_t nonce);
//static bool coinflex_set_algorithm(struct cgpu_info *coinflex);

/********** work queue */
static bool wq_enqueue(struct work_queue *wq, struct work *work)
{
    if (work == NULL)
        return false;

    struct work_ent *we = malloc(sizeof(*we));
    //assert(we != NULL);
    if(we == NULL){
        applog(LOG_ERR,"malloc failed in wq_enqueue function!\n");
    }

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
	if(a1 == NULL){
        applog(LOG_ERR,"a1 malloc failed at chain_detect_thread function!\n");
    }
	memset(a1, 0, sizeof(struct A1_chain));

	cid = g_chain_id[chain_id];
	a1->chain_id = cid;
	a1->num_chips = mcompat_chain_preinit(cid);
	if (a1->num_chips == 0) {
		goto failure;
	}

	if (!mcompat_chain_set_pll_vid(cid, opt_A1Pll1, opt_voltage[cid])) {
		goto failure;
	}

    //bistmask
	if (!mcompat_chain_init(cid, SPI_SPEED_RUN, false)) {
		goto failure;
	}

	a1->num_active_chips = a1->num_chips;
	a1->chips = calloc(a1->num_active_chips, sizeof(struct A1_chip));
    if (a1->chips == NULL){
        applog(LOG_ERR,"a1->chips malloc failed at chain_detect_thread function!\n");
    }

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
        if (cgpu == NULL){
            applog(LOG_ERR,"cgpu malloc failed at A5_chain_detect function!\n");
        }
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
    #if 0
    fan_cfg.fan_speed_target = 100;//fan speed 100%
    mcompat_fanctrl_set_bypass(true);
    #else
    fan_cfg.fan_speed_target = 50;
    #endif
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

    A5_chain_detect();

    mcompat_get_miner_status();
    
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

    if(!mcompat_cmd_resetjob(a1->chain_id, ADDR_BROADCAST, buffer))
    {
        applog(LOG_WARNING, "chip %d clear work failed", i); 
    }

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
        //assert(work != NULL);
        if (work == NULL){
            applog(LOG_ERR,"work malloc failed at coinflex_flush_work function!\n");
        }
        work_completed(coinflex, work);
    }
    mutex_unlock(&a1->lock);
}

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
    static int tries[8] = {0};
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
        check_disbale_flag[cid]++;

        cgpu->chip_num = a1->num_active_chips;
        cgpu->core_num = a1->num_cores;
        a1->last_temp_time = get_current_ms();
    }
    
    cgtime(&now);

    /* poll queued results */
    while (true){
        if (!get_nonce(a1, (uint8_t*)&nonce, &chip_id, &job_id)){
            break;
        }

        work_updated = true;
        if (chip_id < 1 || chip_id > a1->num_active_chips) {
            applog(LOG_ERR, "%d: wrong chip_id %d", cid, chip_id);
            continue;
        }

        if (job_id < 1 || job_id > 4){
            applog(LOG_ERR, "%d: chip %d: result has wrong ""job_id %d", cid, chip_id, job_id);
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
            nonce_ranges_processed--;
            continue;
        }

        //applog(LOG_INFO, "Got nonce for chain %d / chip %d / job_id %d", a1->chain_id, chip_id, job_id);
        chip->nonces_found++;
        hashes += work->device_diff;
		a1->lastshare = now.tv_sec;
        tries[cid] = 0;
    }

    if ((now.tv_sec - a1->lastshare) > CHAIN_DEAD_TIME)
    {
        a1->lastshare = now.tv_sec;
        tries[cid]++;
		applog(LOG_ERR, "A5/A5+ chain %d not producing nounce for more than %d mins",
		       cid, (tries[cid]*CHAIN_DEAD_TIME / 60));
        if(!mcompat_cmd_resetjob(cid, CMD_ADDR_BROADCAST, reg))
        {
            applog(LOG_ERR, "(not share)chain:%d spi hub reset failed",cid);
        }
        if (tries[cid] == 4)
        {
           tries[cid] == 0;
           mcompat_chain_power_down_all();
           sleep(5);
		   quit(1, "A5/A5+ chain %d not producing nounce for more than %d mins,all chains power down",
                 cid, (tries[cid]*CHAIN_DEAD_TIME / 60));
        }
	}


    /* check for completed works */
    if(a1->work_start_delay > 0)
    {
        applog(LOG_INFO, "wait for pll stable");
        a1->work_start_delay--;
    }
    else
    {
		for (i = a1->num_active_chips; i > 0; i--)
		{
			if(mcompat_cmd_read_register(a1->chain_id, i, reg, REG_LENGTH_SG))
			{
				struct A1_chip *chip = NULL;
				struct work *work = NULL;
				// hexdump_error("Read reg",reg,REG_LENGTH);
				uint8_t qstate = reg[9] & 0x03;

				if (qstate != 0x03)
				{
					work_updated = true;
					if(qstate == 0x0){
						chip = &a1->chips[i - 1];
						work = wq_dequeue(&a1->active_wq);

						if (work == NULL){
							printf("Sorry work is NULL\n");
							continue;
						}

						if (set_work(a1, i, work, 0))
						{
							nonce_ranges_processed++;
							chip->nonce_ranges_done++;
						}
					}

					chip = &a1->chips[i - 1];
					work = wq_dequeue(&a1->active_wq);

					if (work == NULL){
						printf("Sorry work is NULL\n");
						continue;
					}

					if (set_work(a1, i, work, 0))
					{
						nonce_ranges_processed++;
						chip->nonce_ranges_done++;
					}
				}
			}
			hub_spi_clean_chain(a1->chain_id);
			//mcompat_cmd_clean_spi_hub(a1->chain_id);
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
    .max_diff               = 65536
};
