/*
 * STM32F4 board support for the bootloader.
 *
 */

#include "hw_config.h"

#include <stdlib.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/pwr.h>
# include <libopencm3/stm32/timer.h>

#include "bl.h"
#include "uart.h"
//#include "sdio.h"
#include "ff.h"
#include "SD_Card.h"

uint32_t blankFlag=0;
/* flash parameters that we should not really know */
static struct {
	uint32_t	sector_number;
	uint32_t	size;
} flash_sectors[] = {
	/* flash sector zero reserved for bootloader */
	//{0x00, 16 * 1024},            //first 16KB is for bootloader
	//{0x01, 16 * 1024},           //BL_size  11
	{0x02, 16 * 1024},
	{0x03, 16 * 1024},
	{0x04, 64 * 1024},
	{0x05, 128 * 1024},
	{0x06, 128 * 1024},
	{0x07, 128 * 1024},
	{0x08, 128 * 1024},
	{0x09, 128 * 1024},
	{0x0a, 128 * 1024},
	{0x0b, 128 * 1024},
	/* flash sectors only in 2MiB devices */ //12
	{0x10, 16 * 1024},
	{0x11, 16 * 1024},
	{0x12, 16 * 1024},
	{0x13, 16 * 1024},
	{0x14, 64 * 1024},
	{0x15, 128 * 1024},
	{0x16, 128 * 1024},
	{0x17, 128 * 1024},
	{0x18, 128 * 1024},
	{0x19, 128 * 1024},
	{0x1a, 128 * 1024},
	{0x1b, 128 * 1024},
};
#define BOOTLOADER_RESERVATION_SIZE	(32 * 1024)    //BL_size

#define OTP_BASE			0x1fff7800
#define OTP_SIZE			512
#define UDID_START		        0x1FFF7A10

// address of MCU IDCODE
#define DBGMCU_IDCODE		0xE0042000
#define STM32_UNKNOWN	0
#define STM32F40x_41x	0x413
#define STM32F42x_43x	0x419
#define STM32F42x_446xx	0x421

#define REVID_MASK	0xFFFF0000
#define DEVID_MASK	0xFFF

/* magic numbers from reference manual */

typedef enum mcu_rev_e {
	MCU_REV_STM32F4_REV_A = 0x1000,
	MCU_REV_STM32F4_REV_Z = 0x1001,
	MCU_REV_STM32F4_REV_Y = 0x1003,
	MCU_REV_STM32F4_REV_1 = 0x1007,
	MCU_REV_STM32F4_REV_3 = 0x2001
} mcu_rev_e;

typedef struct mcu_des_t {
	uint16_t mcuid;
	const char *desc;
	char  rev;
} mcu_des_t;

FATFS  Fatfs;
FIL backupfile;
FIL file;
FIL oldfile;

// The default CPU ID  of STM32_UNKNOWN is 0 and is in offset 0
// Before a rev is known it is set to ?
// There for new silicon will result in STM32F4..,?
mcu_des_t mcu_descriptions[] = {
	{ STM32_UNKNOWN,	"STM32F???",    '?'},
	{ STM32F40x_41x, 	"STM32F40x",	'?'},
	{ STM32F42x_43x, 	"STM32F42x",	'?'},
	{ STM32F42x_446xx, 	"STM32F446XX",	'?'},
};

typedef struct mcu_rev_t {
	mcu_rev_e revid;
	char  rev;
} mcu_rev_t;

/*
 * This table is used in 2 ways. One to look look up the revision 
 * of a given chip. Two to see it a revsion is in the group of "Bad" 
 * silicon.
 * 
 * Therefore when adding entries for good silicon rev, they must be inserted
 * at the beginning of the table. The value of FIRST_BAD_SILICON_OFFSET will
 * also need to be increased to that of the value of the first bad silicon offset.
 * 
 */
const mcu_rev_t silicon_revs[] = {
	{MCU_REV_STM32F4_REV_3, '3'}, /* Revision 3 */

	{MCU_REV_STM32F4_REV_A, 'A'}, /* Revision A */  // FIRST_BAD_SILICON_OFFSET (place good ones above this line and update the FIRST_BAD_SILICON_OFFSET accordingly)
	{MCU_REV_STM32F4_REV_Z, 'Z'}, /* Revision Z */
	{MCU_REV_STM32F4_REV_Y, 'Y'}, /* Revision Y */
	{MCU_REV_STM32F4_REV_1, '1'}, /* Revision 1 */
};

#define FIRST_BAD_SILICON_OFFSET 1

#define APP_SIZE_MAX			(BOARD_FLASH_SIZE - BOOTLOADER_RESERVATION_SIZE)

/* context passed to cinit */
#if INTERFACE_USART
# define BOARD_INTERFACE_CONFIG_USART	(void *)BOARD_USART
#endif
#if INTERFACE_USB
# define BOARD_INTERFACE_CONFIG_USB  	NULL
#endif

/* board definition */
struct boardinfo board_info = {
	.board_type	= BOARD_TYPE,
	.board_rev	= 0,
	.fw_size	= 0,

	.systick_mhz	= 168,
};

static void board_init(void);
static void Fatfs_init();
static void Fatfs_deinit();
static void UART7_init();
static void UART7_deinit();
static void SD_upload();
void read_chip_to_sd();

#define BOOT_RTC_SIGNATURE	0xb007b007
#define BOOT_RTC_REG		MMIO32(RTC_BASE + 0x50)

/* standard clocking for all F4 boards */
static const clock_scale_t clock_setup = {
	.pllm = OSC_FREQ,
	.plln = 336,
	.pllp = 2,
	.pllq = 7,
	.hpre = RCC_CFGR_HPRE_DIV_NONE,
	.ppre1 = RCC_CFGR_PPRE_DIV_4,
	.ppre2 = RCC_CFGR_PPRE_DIV_2,
	.power_save = 0,
	.flash_config = FLASH_ACR_ICE | FLASH_ACR_DCE | FLASH_ACR_LATENCY_5WS,
	.apb1_frequency = 42000000,
	.apb2_frequency = 84000000,
};


static uint32_t board_get_rtc_signature()
{
	/* enable the backup registers */
	PWR_CR |= PWR_CR_DBP;
	RCC_BDCR |= RCC_BDCR_RTCEN;

	uint32_t result = BOOT_RTC_REG;

	/* disable the backup registers */
	RCC_BDCR &= RCC_BDCR_RTCEN;
	PWR_CR &= ~PWR_CR_DBP;

	return result;
}

static void board_set_rtc_signature(uint32_t sig)
{
	/* enable the backup registers */
	PWR_CR |= PWR_CR_DBP;
	RCC_BDCR |= RCC_BDCR_RTCEN;

	BOOT_RTC_REG = sig;

	/* disable the backup registers */
	RCC_BDCR &= RCC_BDCR_RTCEN;
	PWR_CR &= ~PWR_CR_DBP;
}

static bool board_test_force_pin()
{
#if defined(BOARD_FORCE_BL_PIN_IN) && defined(BOARD_FORCE_BL_PIN_OUT)
	/* two pins strapped together */
	volatile unsigned samples = 0;
	volatile unsigned vote = 0;

	for (volatile unsigned cycles = 0; cycles < 10; cycles++) {
		gpio_set(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_OUT);

		for (unsigned count = 0; count < 20; count++) {
			if (gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_IN) != 0) {
				vote++;
			}

			samples++;
		}

		gpio_clear(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_OUT);

		for (unsigned count = 0; count < 20; count++) {
			if (gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_IN) == 0) {
				vote++;
			}

			samples++;
		}
	}

	/* the idea here is to reject wire-to-wire coupling, so require > 90% agreement */
	if ((vote * 100) > (samples * 90)) {
		return true;
	}

#endif
#if defined(BOARD_FORCE_BL_PIN)
	/* single pin pulled up or down */
	volatile unsigned samples = 0;
	volatile unsigned vote = 0;

	for (samples = 0; samples < 200; samples++) {
		if ((gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN) ? 1 : 0) == BOARD_FORCE_BL_STATE) {
			vote++;
		}
	}

	/* reject a little noise */
	if ((vote * 100) > (samples * 90)) {
		return true;
	}

#endif
	return false;
}

#if INTERFACE_USART
static bool
board_test_usart_receiving_break()
{
	/* (re)start the SysTick timer system */
	systick_interrupt_disable(); // Kill the interrupt if it is still active
	systick_counter_disable(); // Stop the timer
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);

	/* Set the timer period to be half the bit rate
	 *
	 * Baud rate = 115200, therefore bit period = 8.68us
	 * Half the bit rate = 4.34us
	 * Set period to 4.34 microseconds (timer_period = timer_tick / timer_reset_frequency = 168MHz / (1/4.34us) = 729.12 ~= 729)
	 */
	systick_set_reload(729);  /* 4.3us tick, magic number */
	systick_counter_enable(); // Start the timer

	uint8_t cnt_consecutive_low = 0;
	uint8_t cnt = 0;

	/* Loop for 3 transmission byte cycles and count the low and high bits. Sampled at a rate to be able to count each bit twice.
	 *
	 * One transmission byte is 10 bits (8 bytes of data + 1 start bit + 1 stop bit)
	 * We sample at every half bit time, therefore 20 samples per transmission byte,
	 * therefore 60 samples for 3 transmission bytes
	 */
	while (cnt < 60) {
		// Only read pin when SysTick timer is true
		if (systick_get_countflag() == 1) {
			if (gpio_get(BOARD_PORT_USART, BOARD_PIN_RX) == 0) {
				cnt_consecutive_low++;	// Increment the consecutive low counter

			} else {
				cnt_consecutive_low = 0; // Reset the consecutive low counter
			}

			cnt++;
		}

		// If 9 consecutive low bits were received break out of the loop
		if (cnt_consecutive_low >= 18) {
			break;
		}

	}

	systick_counter_disable(); // Stop the timer

	/*
	 * If a break is detected, return true, else false
	 *
	 * Break is detected if line was low for 9 consecutive bits.
	 */
	if (cnt_consecutive_low >= 18) {
		return true;
	}

	return false;
}
#endif


static void board_init(void)
{
	/* fix up the max firmware size, we have to read memory to get this */
	board_info.fw_size = APP_SIZE_MAX;
#if defined(TARGET_HW_PX4_FMU_V2) || defined(TARGET_HW_PX4_FMU_V4)

	if (check_silicon() && board_info.fw_size == (2 * 1024 * 1024) - BOOTLOADER_RESERVATION_SIZE) {
		board_info.fw_size = (1024 * 1024) - BOOTLOADER_RESERVATION_SIZE;
	}

#endif

#if INTERFACE_USB
	/* enable GPIO9 with a pulldown to sniff VBUS */
	rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPAEN);
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, GPIO9);
#endif

#if INTERFACE_USART
	/* configure USART pins */
	rcc_peripheral_enable_clock(&BOARD_USART_PIN_CLOCK_REGISTER, BOARD_USART_PIN_CLOCK_BIT);

	/* Setup GPIO pins for USART transmit. */
	gpio_mode_setup(BOARD_PORT_USART, GPIO_MODE_AF, GPIO_PUPD_PULLUP, BOARD_PIN_TX | BOARD_PIN_RX);
	/* Setup USART TX & RX pins as alternate function. */
	gpio_set_af(BOARD_PORT_USART, BOARD_PORT_USART_AF, BOARD_PIN_TX);
	gpio_set_af(BOARD_PORT_USART, BOARD_PORT_USART_AF, BOARD_PIN_RX);

	/* configure USART clock */
	rcc_peripheral_enable_clock(&BOARD_USART_CLOCK_REGISTER, BOARD_USART_CLOCK_BIT);

#endif

	UART7_init();
	Fatfs_init();

#if defined(BOARD_FORCE_BL_PIN_IN) && defined(BOARD_FORCE_BL_PIN_OUT)
	/* configure the force BL pins */
	rcc_peripheral_enable_clock(&BOARD_FORCE_BL_CLOCK_REGISTER, BOARD_FORCE_BL_CLOCK_BIT);
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_INPUT, BOARD_FORCE_BL_PULL, BOARD_FORCE_BL_PIN_IN);
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_FORCE_BL_PIN_OUT);
	gpio_set_output_options(BOARD_FORCE_BL_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_100MHZ, BOARD_FORCE_BL_PIN_OUT);
#endif

#if defined(BOARD_FORCE_BL_PIN)
	/* configure the force BL pins */
	rcc_peripheral_enable_clock(&BOARD_FORCE_BL_CLOCK_REGISTER, BOARD_FORCE_BL_CLOCK_BIT);
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_INPUT, BOARD_FORCE_BL_PULL, BOARD_FORCE_BL_PIN);
#endif

	/* initialise LEDs */
	rcc_peripheral_enable_clock(&RCC_AHB1ENR, BOARD_CLOCK_LEDS);
	gpio_mode_setup(
		BOARD_PORT_LEDS,
		GPIO_MODE_OUTPUT,
		GPIO_PUPD_NONE,
		BOARD_PIN_LED_BOOTLOADER | BOARD_PIN_LED_ACTIVITY);
	gpio_set_output_options(
		BOARD_PORT_LEDS,
		GPIO_OTYPE_PP,
		GPIO_OSPEED_2MHZ,
		BOARD_PIN_LED_BOOTLOADER | BOARD_PIN_LED_ACTIVITY);
	BOARD_LED_ON(
		BOARD_PORT_LEDS,
		BOARD_PIN_LED_BOOTLOADER | BOARD_PIN_LED_ACTIVITY);

	/* enable the power controller clock */
	rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_PWREN);
}

void UART7_init(void)
{
	rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPEEN);

	/* Setup GPIO pins for USART transmit. */
	gpio_mode_setup(GPIOE, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO8 | GPIO7);
	/* Setup USART TX & RX pins as alternate function. */
	gpio_set_af(GPIOE, GPIO_AF8, GPIO8);
	gpio_set_af(GPIOE, GPIO_AF8, GPIO7);

	/* configure USART clock */
	rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_UART7EN);

	// open UART7
	uart7_cinit(UART7);
}

void UART7_deinit(void)
{
	/* deinitialise GPIO pins for USART transmit. */
	gpio_mode_setup(GPIOE, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO8 | GPIO7);

	/* disable USART peripheral clock */
	rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_UART7EN);
}

void board_deinit(void)
{
#if INTERFACE_USB
	/* deinitialise GPIO9 (used to sniff VBUS) */
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO9);
#endif

#if INTERFACE_USART
	/* deinitialise GPIO pins for USART transmit. */
	gpio_mode_setup(BOARD_PORT_USART, GPIO_MODE_INPUT, GPIO_PUPD_NONE, BOARD_PIN_TX | BOARD_PIN_RX);

	/* disable USART peripheral clock */
	rcc_peripheral_disable_clock(&BOARD_USART_CLOCK_REGISTER, BOARD_USART_CLOCK_BIT);
#endif

#if defined(BOARD_FORCE_BL_PIN_IN) && defined(BOARD_FORCE_BL_PIN_OUT)
	/* deinitialise the force BL pins */
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, BOARD_FORCE_BL_PIN_OUT);
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, BOARD_FORCE_BL_PIN_IN);
#endif
//卸载文件系统，关闭串口7
	UART7_deinit();
	Fatfs_deinit();
//卸载文件系统，关闭串口7
#if defined(BOARD_FORCE_BL_PIN)
	/* deinitialise the force BL pin */
	gpio_mode_setup(BOARD_FORCE_BL_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, BOARD_FORCE_BL_PIN);
#endif

	/* deinitialise LEDs */
	gpio_mode_setup(
		BOARD_PORT_LEDS,
		GPIO_MODE_INPUT,
		GPIO_PUPD_NONE,
		BOARD_PIN_LED_BOOTLOADER | BOARD_PIN_LED_ACTIVITY);

	/* disable the power controller clock */
	rcc_peripheral_disable_clock(&RCC_APB1ENR, RCC_APB1ENR_PWREN);

	/* disable the AHB peripheral clocks */
	RCC_AHB1ENR = 0x00100000; // XXX Magic reset number from STM32F4x reference manual
}

/**
  * @brief  Initializes the RCC clock configuration.
  *
  * @param  clock_setup : The clock configuration to set
  */
static inline void clock_init(void)
{
	rcc_clock_setup_hse_3v3(&clock_setup);
}

/**
  * @brief  Resets the RCC clock configuration to the default reset state.
  * @note   The default reset state of the clock configuration is given below:
  *            - HSI ON and used as system clock source
  *            - HSE, PLL and PLLI2S OFF
  *            - AHB, APB1 and APB2 prescaler set to 1.
  *            - CSS, MCO1 and MCO2 OFF
  *            - All interrupts disabled
  * @note   This function doesn't modify the configuration of the
  *            - Peripheral clocks
  *            - LSI, LSE and RTC clocks
  */
void
clock_deinit(void)
{
	/* Enable internal high-speed oscillator. */
	rcc_osc_on(HSI);
	rcc_wait_for_osc_ready(HSI);

	/* Reset the RCC_CFGR register */
	RCC_CFGR = 0x000000;

	/* Stop the HSE, CSS, PLL, PLLI2S, PLLSAI */
	rcc_osc_off(HSE);
	rcc_osc_off(PLL);
	rcc_css_disable();

	/* Reset the RCC_PLLCFGR register */
	RCC_PLLCFGR = 0x24003010; // XXX Magic reset number from STM32F4xx reference manual

	/* Reset the HSEBYP bit */
	rcc_osc_bypass_disable(HSE);

	/* Reset the CIR register */
	RCC_CIR = 0x000000;
}

uint32_t
flash_func_sector_size(unsigned sector)
{
	if (sector < (BOARD_FLASH_SECTORS)) {
	//if (sector < (BOARD_FLASH_SECTORS-1)) {  //这样ＯＫ
		return flash_sectors[sector].size;
	}

	return 0;
}

void flash_func_read_sector(unsigned sector)
{
	UINT bwn;
	uint32_t chipData[1]={0};
	if(sector >= BOARD_FLASH_SECTORS) {
		return;    //如果要求读取的sector超过芯片固有的就返回
	}

	//得到sector的相对偏移地址
	uint32_t address =0;

	for (unsigned i = 0; i < sector; i++) {
		address +=flash_func_sector_size(i);
	}


	uint32_t size = flash_func_sector_size(sector);    //得到当前sector的大小
	//检查这个sector是否是空
	if((flash_func_read_word(address)==0xffffffff)&&(flash_func_read_word(address+4)==0xffffffff)&&(flash_func_read_word(address+16)==0xffffffff)) {
		blankFlag=1;
		return;
	}
	for(uint32_t i=0; i <size;i += sizeof(uint32_t)) {
		chipData[0]=flash_func_read_word(address+i);
		f_write (&backupfile,chipData,4,&bwn);
	}
}
void
flash_func_erase_sector(unsigned sector)
{
	if (sector >= BOARD_FLASH_SECTORS) {
		return;
	}

	/* get the base address of the sector */
	uint32_t address = 0;

	for (unsigned i = 0; i < sector; i++) {
		address += flash_func_sector_size(i);
	}

	/* blank-check the sector */
	unsigned size = flash_func_sector_size(sector);
	bool blank = true;

	for (unsigned i = 0; i < size; i += sizeof(uint32_t)) {
		if (flash_func_read_word(address + i) != 0xffffffff) {
			blank = false;
			break;
		}
	}
	/* erase the sector if it failed the blank check */
	if (!blank) {
		flash_erase_sector(flash_sectors[sector].sector_number, FLASH_CR_PROGRAM_X32);
	}
}

void
flash_func_write_word(uint32_t address, uint32_t word)
{
	flash_program_word(address + APP_LOAD_ADDRESS, word);
}

uint32_t
flash_func_read_word(uint32_t address)
{
	if (address & 3) {
		return 0;
	}

	return *(uint32_t *)(address + APP_LOAD_ADDRESS);
}


uint32_t
flash_func_read_otp(uint32_t address)
{
	if (address & 3) {
		return 0;
	}

	if (address > OTP_SIZE) {
		return 0;
	}

	return *(uint32_t *)(address + OTP_BASE);
}

uint32_t get_mcu_id(void)
{
	return *(uint32_t *)DBGMCU_IDCODE;
}

int get_mcu_desc(int max, uint8_t *revstr)
{
	uint32_t idcode = (*(uint32_t *)DBGMCU_IDCODE);
	int32_t mcuid = idcode & DEVID_MASK;
	mcu_rev_e revid = (idcode & REVID_MASK) >> 16;

	mcu_des_t des = mcu_descriptions[STM32_UNKNOWN];

	for (int i = 0; i < arraySize(mcu_descriptions); i++) {
		if (mcuid == mcu_descriptions[i].mcuid) {
			des = mcu_descriptions[i];
			break;
		}
	}

	for (int i = 0; i < arraySize(silicon_revs); i++) {
		if (silicon_revs[i].revid == revid) {
			des.rev = silicon_revs[i].rev;
		}
	}

	uint8_t *endp = &revstr[max - 1];
	uint8_t *strp = revstr;

	while (strp < endp && *des.desc) {
		*strp++ = *des.desc++;
	}

	if (strp < endp) {
		*strp++ = ',';
	}

	if (strp < endp) {
		*strp++ = des.rev;
	}

	return  strp - revstr;
}


int check_silicon(void)
{
#if defined(TARGET_HW_PX4_FMU_V2) || defined(TARGET_HW_PX4_FMU_V4)
	uint32_t idcode = (*(uint32_t *)DBGMCU_IDCODE);
	mcu_rev_e revid = (idcode & REVID_MASK) >> 16;

	for (int i = FIRST_BAD_SILICON_OFFSET; i < arraySize(silicon_revs); i++) {
		if (silicon_revs[i].revid == revid) {
			return -1;
		}
	}

#endif
	return 0;
}

uint32_t
flash_func_read_sn(uint32_t address)
{
	// read a byte out from unique chip ID area
	// it's 12 bytes, or 3 words.
	return *(uint32_t *)(address + UDID_START);
}

void
led_on(unsigned led)
{
	switch (led) {
	case LED_ACTIVITY:
		BOARD_LED_ON(BOARD_PORT_LEDS, BOARD_PIN_LED_ACTIVITY);
		break;

	case LED_BOOTLOADER:
		BOARD_LED_ON(BOARD_PORT_LEDS, BOARD_PIN_LED_BOOTLOADER);
		break;
	}
}

void
led_off(unsigned led)
{
	switch (led) {
	case LED_ACTIVITY:
		BOARD_LED_OFF(BOARD_PORT_LEDS, BOARD_PIN_LED_ACTIVITY);
		break;

	case LED_BOOTLOADER:
		BOARD_LED_OFF(BOARD_PORT_LEDS, BOARD_PIN_LED_BOOTLOADER);
		break;
	}
}

void
led_toggle(unsigned led)
{
	switch (led) {
	case LED_ACTIVITY:
		gpio_toggle(BOARD_PORT_LEDS, BOARD_PIN_LED_ACTIVITY);
		break;

	case LED_BOOTLOADER:
		gpio_toggle(BOARD_PORT_LEDS, BOARD_PIN_LED_BOOTLOADER);
		break;
	}
}

/* we should know this, but we don't */
#ifndef SCB_CPACR
# define SCB_CPACR (*((volatile uint32_t *) (((0xE000E000UL) + 0x0D00UL) + 0x088)))
#endif

//该函数主要作用是初始化SD卡，挂载FatFs文件系统
void Fatfs_init()
{

	uint8_t Res=0;
	uint8_t fail_mount[]="Fail to mount fatfs. Please try again  \r\n";
	uint8_t sd_not_found[]="Fail to find SD Card . Please insert SD Card  \r\n";
	//初始化SD卡，成功返回值0，失败进入循环处理（按需更改）
	if(SD_Init()) {
		uart7_cout(UART7, sd_not_found, sizeof(sd_not_found));
		//jump_to_app();  //如果SD卡初始化失败，一般都是没有插入SD卡
		//while(1);
	} else {
		//加载Fatfs文件系统，初始化盘符，默认为0
		Res=f_mount(&Fatfs,"",1);
		if(Res) {   //加载失败，处理函数按需更改
			uart7_cout(UART7, fail_mount, sizeof(fail_mount));
			//jump_to_app();
			//while(1);
		}
	}
}

void Fatfs_deinit()
{
	f_mount(0,"",1);                           //卸载文件系统
	SD_Deinit();                              //关闭SD卡
}

void SD_upload()
{
	uint32_t  program_addr=0x8008000;
	UINT   br;
	uint8_t fatbuf[512];
	uint8_t Res=0;
	uint8_t backupRes=0;
	uint8_t block[]={0xa1,0xf6};
	uint8_t erase_setor[]="Find the file: fw.bin ,begin to upload this file \r\nErasing     : ";
	uint8_t old_file[]="Find the file:old . to delete this file  \r\n";
	uint8_t no_file[]="Fail to find the file:fw.bin . \r\n";
	uint8_t fail_progm[]="Fail to read the file... \r\n ";
	uint8_t finish[]="\r\nAll finished  ...   \r\n";
	uint8_t  program[]="\r\nProgramming : ";
	uint8_t Init_ok[]="Check SD card  ....   \r\n";

	uint8_t backuperase[]="Find the file: backup.bin ,begin to upload this file \r\nErasing     :";
	uint8_t backupnofile[]="Fail to find the file:backup.bin.\r\n";
	uart7_cout(UART7, Init_ok, sizeof(Init_ok));
	Res=f_open(&oldfile,"old",FA_READ);//检查是否能打开“old”文件，如打开成功，则删除
	if(Res==0)
	{
		uart7_cout(UART7, old_file, sizeof(old_file));
		f_close (&oldfile);
		f_unlink("old");
	}
	backupRes=f_open(&backupfile,"backup.bin",FA_READ);//检查是否能打开“backup.bin”文件，如打开成功，更新后改名
	if(backupRes==0) {
		flash_unlock();            //关闭flash写保护
		uart7_cout(UART7, backuperase, sizeof(backuperase));  //轮循擦除扇区，每擦除一个扇区，LED变化一次，并打印相应信息
		for (unsigned i = 0; flash_func_sector_size(i) != 0; i++) {
			flash_func_erase_sector(i);
			led_toggle(LED_BOOTLOADER);
			uart7_cout(UART7, block, sizeof(block));
		}
		Res=0;
		uart7_cout(UART7, program, sizeof(program));      //开始写flash
		do {
			if(f_read(&backupfile,fatbuf ,512,&br)==0) {//读取成功
				for(unsigned i=0;i<br;i++) {
					flash_program_byte(program_addr,fatbuf [i]);//每次读512字节，此为fatfs定义数据长度最大值
					program_addr++;                //采用字节长度方式写flash。地址加1
				}
				Res++;                             //0.5kb加1
			} else {              //读取失败，按需加入处理函数
				uart7_cout(UART7, fail_progm, sizeof(fail_progm));
				break;
			}
			if((Res%100)==0) { //如果正好是100的倍数，即为50kb的倍数，串口发送一次状态数据，LED变化一次
				uart7_cout(UART7, block, sizeof(block));
				led_toggle( LED_BOOTLOADER);
			}
		} while(br==512);                          //当读取至文件末尾，退出
		flash_lock();                              //打开flash写保护
		f_close (&backupfile);                     //关闭文件
		uart7_cout(UART7, finish, sizeof(finish));
		f_unlink("backup.bin");
		jump_to_app();                             //跳转至固件
	} else {
		uart7_cout(UART7,backupnofile, sizeof(backupnofile));
	}


	Res=f_open(&file,"fw.bin",FA_READ);         //检查是否能打开“upgrade.bin”文件，打开成功后，更新后改名old
	if(Res==0) {
		flash_unlock();            //关闭flash写保护
		uart7_cout(UART7, erase_setor, sizeof(erase_setor));  //轮循擦除扇区，每擦除一个扇区，LED变化一次，并打印相应信息
		for (unsigned i = 0; flash_func_sector_size(i) != 0; i++) {
			flash_func_erase_sector(i);
			led_toggle( LED_BOOTLOADER);
			uart7_cout(UART7, block, sizeof(block));
		}
		uart7_cout(UART7, program, sizeof(program));      //开始写flash
		Res=0;
		do {
			if(f_read(&file,fatbuf ,512,&br)==0) {//读取成功
				for(unsigned i=0;i<br;i++) {
					flash_program_byte(program_addr,fatbuf[i]);//每次读512字节，此为fatfs定义数据长度最大值
					program_addr++;                //采用字节长度方式写flash。地址加1
				}
				Res++;                             //0.5kb加1
			}else {//读取失败，按需加入处理函数
				uart7_cout(UART7, fail_progm, sizeof(fail_progm));
				break;
			}
			if((Res%100)==0) {//如果正好是100的倍数，即为50kb的倍数，串口发送一次状态数据，LED变化一次
				uart7_cout(UART7, block, sizeof(block));
				led_toggle( LED_BOOTLOADER);
			}
		} while(br==512);                           //当读取至文件末尾，退出
		flash_lock();                              //打开flash写保护
		f_close (&file);                           //关闭文件
		uart7_cout(UART7, finish, sizeof(finish));
		f_rename("FW.bin","old");                  //重命名固件为backup.bin
	} else {//打开失败，则判定为不更新固件，卸载fatfs，跳转至固件
		uart7_cout(UART7, no_file, sizeof(no_file));
	}
}

//简要流程： 挂载fatfs系统（在Fatfs初始化中完成了），创建一个名为backup.bin文件，按4字节方式从APP_LOAD_ADDRESS（0x08008000）读取芯片，至0xffffffff。
void read_chip_to_sd()
{

	uint8_t block[]={0xa1,0xf6};
	uint8_t test1[]="Backup: creat the backup.bin file \r\n";
	uint8_t test2[]="Backup: finish to read the chip \r\n";
	uint8_t Res=0;
	uint8_t unlinkflag=0;
	flash_unlock();            //关闭flash写保护
	Res=f_open(&backupfile,"backup.bin",FA_WRITE|FA_CREATE_NEW);//检查是否能打开“backup.bin”文件，如打开成功，则删除
	if(Res==0) {              //backup.bin 文件创建成功
		uart7_cout(UART7, test1, sizeof(test1));
		for (unsigned i = 0; flash_func_sector_size(i) != 0; i++) {
			flash_func_read_sector(i);
			uart7_cout(UART7, block, sizeof(block));
			if(blankFlag==1) {
				break;       //读取至chip的末尾，跳出
			}
		}
	}
	flash_lock();           //开启flash写保护
	if(f_size(&backupfile)==0) unlinkflag=1;   //如果文件为空就删除
	f_close(&backupfile);
	if(unlinkflag==1) f_unlink("backup.bin");
	uart7_cout(UART7, test2, sizeof(test2));
}

int main(void)
{
	bool try_boot = true;			/* try booting before we drop to the bootloader */
	unsigned timeout = BOOTLOADER_DELAY;	/* if nonzero, drop out of the bootloader after this time */

	/* Enable the FPU before we hit any FP instructions */
	SCB_CPACR |= ((3UL << 10 * 2) | (3UL << 11 * 2)); /* set CP10 Full Access and set CP11 Full Access */


	/* configure the clock for bootloader activity */
	clock_init();   //初始化时钟
	//初始化串口7，用作SD卡更新的输出;初始化SD，挂载文件系统
	//UART7_init();
	//Fatfs_init();
	/* do board-specific initialisation */
		board_init();   //初始化串口时钟，串口IO时钟，开启复用功能
		                //初始化串口7，SD卡，挂载文件系统
		//read_chip_to_sd();
	//初始化串口7，用作SD卡更新的输出;初始化SD，挂载文件系统
	/*
	 * Check the force-bootloader register; if we find the signature there, don't
	 * try booting.
	 */

	if (board_get_rtc_signature() == BOOT_RTC_SIGNATURE) {

		/*
		 * Don't even try to boot before dropping to the bootloader.
		 */
		try_boot = false;

		/*
		 * Don't drop out of the bootloader until something has been uploaded.
		 */
		timeout = 0;

		/*
		 * Clear the signature so that if someone resets us while we're
		 * in the bootloader we'll try to boot next time.
		 */
		board_set_rtc_signature(0);
	}

#ifdef BOOT_DELAY_ADDRESS
	{
		/*
		  if a boot delay signature is present then delay the boot
		  by at least that amount of time in seconds. This allows
		  for an opportunity for a companion computer to load a
		  new firmware, while still booting fast by sending a BOOT
		  command
		 */
		uint32_t sig1 = flash_func_read_word(BOOT_DELAY_ADDRESS);
		uint32_t sig2 = flash_func_read_word(BOOT_DELAY_ADDRESS + 4);

		if (sig2 == BOOT_DELAY_SIGNATURE2 &&
		    (sig1 & 0xFFFFFF00) == (BOOT_DELAY_SIGNATURE1 & 0xFFFFFF00)) {
			unsigned boot_delay = sig1 & 0xFF;

			if (boot_delay <= BOOT_DELAY_MAX) {
				try_boot = false;

				if (timeout < boot_delay * 1000) {
					timeout = boot_delay * 1000;
				}
			}
		}
	}
#endif

	/*
	 * Check if the force-bootloader pins are strapped; if strapped,
	 * don't try booting.
	 */
	if (board_test_force_pin()) {
		try_boot = false;
	}

#if INTERFACE_USB

	/*
	 * Check for USB connection - if present, don't try to boot, but set a timeout after
	 * which we will fall out of the bootloader.
	 *
	 * If the force-bootloader pins are tied, we will stay here until they are removed and
	 * we then time out.
	 */
	if (gpio_get(GPIOA, GPIO9) != 0) {

		/* don't try booting before we set up the bootloader */
		try_boot = false;
	}

#endif

#if INTERFACE_USART

	/*
	 * Check for if the USART port RX line is receiving a break command, or is being held low. If yes,
	 * don't try to boot, but set a timeout after
	 * which we will fall out of the bootloader.
	 *
	 * If the force-bootloader pins are tied, we will stay here until they are removed and
	 * we then time out.
	 */
	if (board_test_usart_receiving_break()) {
		try_boot = false;
	}

#endif

	/* Try to boot the app if we think we should just go straight there */
	if (try_boot) {

		/* set the boot-to-bootloader flag so that if boot fails on reset we will stop here */
#ifdef BOARD_BOOT_FAIL_DETECT
		board_set_rtc_signature(BOOT_RTC_SIGNATURE);
#endif

		/* try to boot immediately */
		SD_upload();
		jump_to_app();

		// If it failed to boot, reset the boot signature and stay in bootloader
		board_set_rtc_signature(BOOT_RTC_SIGNATURE);

		/* booting failed, stay in the bootloader forever */
		timeout = 0;
	}

	/* start the interface */
#if INTERFACE_USART
	cinit(BOARD_INTERFACE_CONFIG_USART, USART);
#endif
#if INTERFACE_USB
	cinit(BOARD_INTERFACE_CONFIG_USB, USB);
#endif

#if 0
	// MCO1/02
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO8);
	gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_100MHZ, GPIO8);
	gpio_set_af(GPIOA, GPIO_AF0, GPIO8);
	gpio_mode_setup(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9);
	gpio_set_af(GPIOC, GPIO_AF0, GPIO9);
#endif


	while (1) {
		/* run the bootloader, come back after an app is uploaded or we time out */
		bootloader(timeout);

		/* if the force-bootloader pins are strapped, just loop back */
		if (board_test_force_pin()) {
			continue;
		}

#if INTERFACE_USART

		/* if the USART port RX line is still receiving a break, just loop back */
		if (board_test_usart_receiving_break()) {
			continue;
		}

#endif

		/* set the boot-to-bootloader flag so that if boot fails on reset we will stop here */
#ifdef BOARD_BOOT_FAIL_DETECT
		board_set_rtc_signature(BOOT_RTC_SIGNATURE);
#endif

		/* look to see if we can boot the app */
		SD_upload();
		jump_to_app();

		/* launching the app failed - stay in the bootloader forever */
		timeout = 0;
	}

}
