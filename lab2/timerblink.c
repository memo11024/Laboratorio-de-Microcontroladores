/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2011 Damjan Marion <damjan.marion@gmail.com>
 * Copyright (C) 2011 Mark Panajotovic <marko@electrontube.org>
 * Copyright (C) 2015 Piotr Esden-Tempski <piotr@esden.net>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>

#define LGREENF GPIO13
#define LGREENF_PORT GPIOG
#define LREDF GPIO14
#define LREDF_PORT GPIOG

#define LGREENB GPIO13
#define LGREENB_PORT GPIOB
#define LREDB GPIO5
#define LREDB_PORT GPIOC

#define LCC LREDF_PORT, LREDF
#define LUP LREDB_PORT, LREDB

/*
  Timer 1 clk frequency:
  If TIMPRE == 0: (default)
    if PPRE = DIV1:
      TIM1CLK = PCLK
    else:
      TIM1CLK = 2*PCLK
  else:
    if PPRE = DIV1|DIV2|DIV4:
      TIM1CLK = HCLK
    else:
      TIM1CLK = 4*PCLK
 */


/* Set STM32 to 168 MHz. */
static void clock_setup(void)
{
    rcc_clock_setup_pll(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_168MHZ]);

    // AHB Prescaler = 1 (NODIV)
    // APB1: DIV4
    // APB2: DIV2
    // SYSCLK: 168MHz
    // AHBCLK (HCLK or FCLK): SYSCLK/AHBPre = 168MHz
    // APB1: 168/4 = 42MHz
    // APB2: 168/2 = 84MHz
    // TIM1 is on APB2
    // TIM1CLK = 2*PCLK = 2*84MHz = 168MHz

    /* Enable GPIOG clock. */
    rcc_periph_clock_enable(RCC_GPIOG);

    /* Enable GPIOB clock. */
    rcc_periph_clock_enable(RCC_GPIOB);

    /* Enable GPIOC clock. */
    rcc_periph_clock_enable(RCC_GPIOC);

    /* Enable TIM1 clock. */
    rcc_periph_clock_enable(RCC_TIM1);
}


static void gpio_setup(void)
{
    /* Set GPIO13-14 (in GPIO port G) to output push-pull. */
    gpio_mode_setup(GPIOG, GPIO_MODE_OUTPUT,
                    GPIO_PUPD_NONE, GPIO13 | GPIO14);

    /* Set GPIO5 (in GPIO port C) to output push-pull. */
    gpio_mode_setup(GPIOC, GPIO_MODE_OUTPUT,
                    GPIO_PUPD_NONE, GPIO5);

    /* Set GPIOB13 as AF1. */
    gpio_set_af(LGREENB_PORT, GPIO_AF1, LGREENB);

    /* Set GPIO13 (in GPIO port B) to alternate function. */
    gpio_mode_setup(LGREENB_PORT, GPIO_MODE_AF,
                    GPIO_PUPD_NONE, LGREENB);
}


static void tim_setup(void)
{
    /* Enable TIM1 clock. */
    rcc_periph_clock_enable(RCC_TIM1);

    /* Enable TIM1 interrupts. */
    nvic_enable_irq(NVIC_TIM1_CC_IRQ);
    nvic_enable_irq(NVIC_TIM1_UP_TIM10_IRQ);

    /* Reset TIM1 peripheral to defaults. */
    rcc_periph_reset_pulse(RST_TIM1);

    /*
     * Timer global mode:
     * - No divider
     * - Alignment edge
     * - Direction up
     */
    timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT,
                   TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);

    /*
     * TIM1 clock = 168 MHz
     *
     * Prescaler = 0x00FF = 255
     *
     * Counter frequency:
     *
     * 168 MHz / (255 + 1) = 656250 Hz
     */
    timer_set_prescaler(TIM1, 0x00FF);

    timer_disable_preload(TIM1);
    timer_continuous_mode(TIM1);

    /*
     * Period = 20 ms
     *
     * 20 ms corresponds to 13125 timer counts.
     *
     * ARR = 13125 - 1 = 13124
     */
    timer_set_period(TIM1, 13124);

    /*
     * Initial PWM value.
     *
     * 10% duty cycle:
     *
     * 13125 * 0.10 = 1312.5
     *
     * Approximately 2 ms pulse.
     */
    timer_set_oc_value(TIM1, TIM_OC1, 1312);

    /*
     * Enable complementary output OC1N on PB13.
     */
    timer_enable_oc_output(TIM1, TIM_OC1N);

    /*
     * PWM mode 1.
     */
    timer_set_oc_mode(TIM1, TIM_OC1, TIM_OCM_PWM1);

    /*
     * Enable main output.
     */
    timer_enable_break_main_output(TIM1);

    timer_disable_break(TIM1);

    /* Enable timer counter. */
    timer_enable_counter(TIM1);

    /*
     * Enable Channel 1 compare interrupt.
     */
    timer_enable_irq(TIM1, TIM_DIER_CC1IE);

    /*
     * Enable update interrupt.
     *
     * One update occurs every 20 ms.
     */
    timer_enable_irq(TIM1, TIM_DIER_UIE);
}


/*
 * Counter of TIM1 update events.
 *
 * Since the PWM period is 20 ms:
 *
 * 50 updates = 1 second
 */
volatile uint32_t timer_ticks = 0;


/*
 * Channel 1 compare interrupt.
 */
void tim1_cc_isr(void)
{
    timer_clear_flag(TIM1, TIM_SR_CC1IF);

    gpio_toggle(LCC);
}


/*
 * TIM1 update interrupt.
 *
 * This interrupt occurs every 20 ms.
 */
void tim1_up_tim10_isr(void)
{
    timer_clear_flag(TIM1, TIM_SR_UIF);

    /*
     * Count the 20 ms periods.
     */
    timer_ticks++;

    /*
     * Keep the original LED toggle.
     */
    gpio_toggle(LUP);
}


/*
 * Delay based on TIM1 update interrupts.
 *
 * 50 timer ticks = 1 second.
 */
static void delay_seconds(uint32_t seconds)
{
    uint32_t start = timer_ticks;

    while ((uint32_t)(timer_ticks - start) < (50U * seconds)) {
        __asm__("nop");
    }
}


int main(void)
{
    clock_setup();
    gpio_setup();
    tim_setup();

    /* Set green LED. */
    gpio_set(LGREENF_PORT, LGREENF);

    while (1) {

        /*
         * 0% angular position
         *
         * 5% PWM
         * 1 ms pulse
         */
        timer_set_oc_value(TIM1, TIM_OC1, 656);

        /* Wait 2 seconds. */
        delay_seconds(2);


        /*
         * 10% angular position
         *
         * 5.5% PWM
         * 1.1 ms pulse
         */
        timer_set_oc_value(TIM1, TIM_OC1, 722);

        /* Wait 1 second. */
        delay_seconds(1);


        /*
         * 100% angular position
         *
         * 10% PWM
         * 2 ms pulse
         */
        timer_set_oc_value(TIM1, TIM_OC1, 1313);

        /* Wait 3 seconds. */
        delay_seconds(3);


        /*
         * 50% angular position
         *
         * 7.5% PWM
         * 1.5 ms pulse
         */
        timer_set_oc_value(TIM1, TIM_OC1, 984);

        /* Wait 1 second. */
        delay_seconds(1);


        /*
         * 10% angular position
         *
         * 5.5% PWM
         * 1.1 ms pulse
         */
        timer_set_oc_value(TIM1, TIM_OC1, 722);

        /* Wait 5 seconds. */
        delay_seconds(5);
    }

    return 0;
}
