/****************************************************************************
*
*   Copyright (c) 2007 Jon Hylands	 <jon@huv.com>
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License version 2 as
*   published by the Free Software Foundation.
*
*   Alternatively, this software may be distributed under the terms of BSD
*   license.
*
*   See README and COPYING for more details.
*
****************************************************************************/

/* ---- Include Files ---------------------------------------------------- */

#include <avr/io.h>
#include <stdio.h>
#include <string.h>
#include <avr/eeprom.h>

#include "Delay.h"
#include "adc.h"
#include "Log.h"
#include "Timer.h"
#include "Timer-UART.h"
#include "Motor.h"

/* ---- Public Variables ------------------------------------------------- */

/* ---- Private Constants and Types -------------------------------------- */

//	Our LED is connected to PD3

#define LED_DDR DDRD
#define LED_PORT PORTD
#define LED_MASK (1 << 3)

/* ---- Private Variables ------------------------------------------------ */

/* ---- Functions -------------------------------------------------------- */


//***************************************************************************

static void ledOn (void)
{
	LED_PORT &= ~LED_MASK;
}

//***************************************************************************

static void ledOff (void)
{
	LED_PORT |= LED_MASK;
}


//***************************************************************************
/**
*   Bioloid Gripper Test
*/

int main (void)
{
	speed_t index;

	// initialize the hardware stuff we use
	InitTimer ();
	InitTimerUART ();
	ADC_Init (ADC_PRESCALAR_AUTO);
	InitMotors ();

	// set the LED port for output
	LED_DDR |= LED_MASK;

	// Flash the LED for 1/2 a second...
	ledOn ();
	ms_spin (500);
	ledOff ();

	Log ("*****\n");
	Log ("***** Bioloid Gripper Test\n");
	Log ("***** Copyright 2007 HUVrobotics\n");
	Log ("*****\n");

	SetMotorSpeed (SPEED_OFF, 0);
	ms_spin (1000);
	ledOn ();
	SetMotorSpeed (255, 0);
	ms_spin (1000);
	ledOff ();
	SetMotorSpeed (SPEED_OFF, 0);
	ms_spin (1000);
	ledOn ();
	SetMotorSpeed (0, 0);
	ms_spin (1000);
	ledOff ();
	SetMotorSpeed (SPEED_OFF, 0);

	return 0;
}

