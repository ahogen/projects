###########################################################################
#
# 	mkEnv-avr.mk
#
# 	This file contains AVR specific defines for the build environment.
#
# 	This file should only define variables and not have any rules in it.
#
###########################################################################

MK_LIB_PREFIX	= lib
MK_LIB_EXT    	= .a
MK_OBJ_EXT    	= .o
MK_BIN_EXT    	= .elf
MK_HEX_EXT		= .hex

MK_CROSS_COMPILE		= avr-

ifeq ($(MK_AVR_MCU),)
$(error MK_AVR_MCU not specified)
endif

MK_AVR_MCU_LONG = $(patsubst m%,atmega%,$(MK_AVR_MCU))

ifeq ($(MK_AVR_FREQ),)
$(error MK_AVR_FREQ not specified)
endif

MK_OS_DIR = avr-$(MK_AVR_MCU)-$(MK_AVR_FREQ)

MK_AVR_MCU_FLAGS	= -mmcu=$(MK_AVR_MCU_LONG)

MK_COMMON_AVR_DIR	= $(MK_COMMON_DIR)/avr

CPPFLAGS += $(MK_AVR_MCU_FLAGS) -I $(MK_COMMON_AVR_DIR)
LDFLAGS  += $(MK_AVR_MCU_FLAGS)

CXXFLAGS += -fno-exceptions -fno-rtti

# On the AVR, we always optimize

CFLAGS += -Os
CXXFLAGS += -Os

MK_PRINT_BIN_SIZE	= avr-mem.sh $@ $(MK_AVR_MCU_LONG)

vpath %.c   $(MK_COMMON_AVR_DIR)
vpath %.cpp $(MK_COMMON_AVR_DIR)

include $(MK_RULES_DIR)/mkEnv-gcc.mk

BOOTHOST = $(MK_ROOT)/BootHost/exe/BootHost.exe
