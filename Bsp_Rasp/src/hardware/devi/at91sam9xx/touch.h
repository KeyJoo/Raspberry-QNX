/*
 * $QNXLicenseC: 
 * Copyright 2009,   QNX Software Systems.  
 *  
 * Licensed under the Apache License, Version 2.0 (the "License"). You  
 * may not reproduce, modify or distribute this software except in  
 * compliance with the License. You may obtain a copy of the License  
 * at: http://www.apache.org/licenses/LICENSE-2.0  
 *  
 * Unless required by applicable law or agreed to in writing, software  
 * distributed under the License is distributed on an "AS IS" basis,  
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied. 
 * 
 * This file may contain contributions from others, either as  
 * contributors under the License or as licensors under other terms.   
 * Please review this entire file for other proprietary rights or license  
 * notices, as well as the QNX Development Suite License Guide at  
 * http://licensing.qnx.com/license-guide/ for other information. 
 * $
 */

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/time.h>

/*==============================================================================

  Registers.

==============================================================================*/
/*
#define BCM2835_GPIO_BASE  0x20200000
#define BCM2835_GPPUD      GPIO_BASE + 0x94
#define BCM2835_GPPUDCLK0  GPIO_BASE + 0x98


#define BCM2835_GPIO_GPFSEL0 0x00
#define BCM2835_GPIO_GPFSEL1 0x04
#define BCM2835_GPIO_GPFSEL2 0x08
#define BCM2835_GPIO_GPFSEL3 0x0C
#define BCM2835_GPIO_GPFSEL4 0x10
#define BCM2835_GPIO_GPFSEL5 0x14

#define BCM2835_GPIO_GPSET0 0x1C
#define BCM2835_GPIO_GPSET1 0x20

#define BCM2835_GPIO_GPCLR0 0x28
#define BCM2835_GPIO_GPCLR1 0x2C

#define BCM2835_GPIO_GPLEV0 0x34
#define BCM2835_GPIO_GPLEV1 0x38

#define BCM2835_GPIO_GPEDS0 0x40
#define BCM2835_GPIO_GPEDS1 0x44

#define BCM2835_GPIO_GPREN0 0x4C
#define BCM2835_GPIO_GPREN1 0x50

#define BCM2835_GPIO_GPFEN0 0x58
#define BCM2835_GPIO_GPFEN1 0x5C

#define BCM2835_GPIO_GPHEN0 0x64
#define BCM2835_GPIO_GPHEN1 0x68

#define BCM2835_GPIO_GPLEN0 0x70
#define BCM2835_GPIO_GPLEN1 0x74

#define BCM2835_GPIO_GPAREN0 0x7C
#define BCM2835_GPIO_GPAREN1 0x80

#define BCM2835_GPIO_GPAFEN0 0x88
#define BCM2835_GPIO_GPAFEN1 0x8C
*/



#define INT_MASK_REG      0x1
#define TIMER_REG         0x28
#define TSADCC_CR         0x00
#define TSADCC_MR         0x04
#define TSADCC_TRGR       0x08
#define TSADCC_TSR        0x0C
#define TSADCC_SR         0x1C
#define TSADCC_IER        0x24
#define TSADCC_IDR        0x28
#define TSADCC_IMR        0x2C
#define TSADCC_CDR0       0x30
#define TSADCC_CDR1       0x34
#define TSADCC_CDR2       0x38
#define TSADCC_CDR3       0x3C
#define BOARD_MCK         48000000
#define ADCCLK            300000 

/*==============================================================================

  Interrupts.

==============================================================================*/

#define TOUCH_INT     49    //20 BCM2835_PHYSIRQ_GPIO0


/*==============================================================================

  Touchscreen Status

==============================================================================*/
#define EOC3              (0x1<<3) 
#define PENCNT            (0x1<<20)
#define NOCNT             (1 << 21)
/*==============================================================================

  Touchscreen Modes

==============================================================================*/

#define INACTIVE_MODE     0
#define INTERRUPT_MODE    1
#define RESISTIVE_MODE    2
#define POSITION_MODE     3
#define DISABLE_TOUCHSCREEN     0
#define TSAMOD            (0x3<<0)
#define TSAMOD_TSONLYMODE (0x1<<0)
#define PENDET_ENABLE     (0x1<<6)
#define PRESCAL           (0xff<<8)
#define STARTUP           (0x7f<<16)
#define STARTUP_TIME      40	/* 40 micro seconds */
#define SHTIM             (0xf<<24)
#define SAMPLE_HOLD_TIME  2000 /* 1000 nano seconds */
#define PENDBC            (0xf<<28) 
#define DEBOUNCE_PERIOD   10000000 /* In nano seconds */
#define TRGMOD            (0x7<<0)
#define PENDET_TRGR       (0x4)
#define PERIODIC_TRGR     (0x5)
#define TRGPER            (0x1869F<<16) /* Periodic Trigger Period */
/*==============================================================================

  Configurations.

==============================================================================*/
#define SWRST             0x1
#define DISABLE_INTR      0x003f3f3f

// Maximum allowed variance in the X coordinate samples.
#define DELTA_X_COORD_VARIANCE          24

// Maximum allowed variance in the X coordinate samples.
#define DELTA_Y_COORD_VARIANCE          24

#define ABS(x)  ((x) >= 0 ? (x) : (-(x)))


/*==============================================================================

  Commands.

==============================================================================*/
#define PLL_TIMER         0x3C000


/*==============================================================================

  Flags.

==============================================================================*/

#define FLAG_INIT         0x1000
#define FLAG_RESET        0x2000


/*==============================================================================

  Defaults.

==============================================================================*/

#define RELEASE_DELAY    100// 100000000
#define INTR_DELAY        75
#define PULSE_PRIORITY    21

#define PULSE_CODE        1

#define INVALID_BASE      ((paddr_t)~0)

/*==============================================================================

  Predeclaired functions

==============================================================================*/

static int touch_init(input_module_t *module);
static int touch_devctrl(input_module_t *module, int event, void *ptr);
static int touch_reset(input_module_t *module);
static int touch_parm(input_module_t *module,int opt,char *optarg);
static int touch_shutdown(input_module_t *module, int delay);
static void *intr_thread (void *data);


/*==============================================================================

  Struct private_data_t 

==============================================================================*/

typedef struct _private_data
{
	int             irq;    /* IRQ to attach to */
	int             iid;    /* Interrupt ID */
	int             irq_pc; /* IRQ pulse code */

	int             chid;
	int             coid;
	pthread_attr_t       pattr;
	struct sched_param   param;
	struct sigevent      event;

	unsigned        physbase;
	uintptr_t       touch_base;
	uintptr_t       spi_base;
	uintptr_t       aux_base;
	uintptr_t       foo;

	struct packet_abs tp;
	
	unsigned char   verbose;
	int             flags;

	unsigned        lastx, lasty, lastbuttons;

	int             pen_toggle;
	int	            pressed;

	pthread_mutex_t mutex;

	/* Timer related stuff */
	timer_t         timerid;
	struct itimerspec itime;

	long            release_delay;
	int             intr_delay;
} private_data_t;
