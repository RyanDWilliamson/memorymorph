TARGET = MemoryMorph

CPP_SOURCES = memory_morph.cpp ../hothouse.cpp

C_INCLUDES = -I.. -I../../DaisySP/DaisySP-LGPL/Source

LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR  = ../../DaisySP

# Enable LGPL modules (ReverbSc lives here)
USE_DAISYSP_LGPL = 1
C_DEFS += -DUSE_DAISYSP_LGPL

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
