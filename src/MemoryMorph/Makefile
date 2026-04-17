TARGET = MemoryMorph

CPP_SOURCES = memory_morph.cpp \
              ../hothouse.cpp

LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR  = ../../DaisySP

C_INCLUDES += \
  -I.. \
  -I$(DAISYSP_DIR)/Source \
  -I$(LIBDAISY_DIR)/src \
  -I$(LIBDAISY_DIR)/src/sys \
  -I$(LIBDAISY_DIR)/src/usbd \
  -I$(LIBDAISY_DIR)/Drivers/CMSIS/Include \
  -I$(LIBDAISY_DIR)/Drivers/CMSIS/DSP/Include \
  -I$(LIBDAISY_DIR)/Drivers/STM32H7xx_HAL_Driver/Inc \
  -I$(LIBDAISY_DIR)/Drivers/BSP/Components/wm8994 \
  -I$(LIBDAISY_DIR)/core

include $(LIBDAISY_DIR)/core/Makefile
