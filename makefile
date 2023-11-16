# ----------------------------
# Makefile Options
# ----------------------------

NAME ?= AETHER
ICON ?= icon.png
DESCRIPTION ?= "Aether TI-BASIC Editor by Vital Ash"
COMPRESSED ?= NO
ARCHIVED ?= NO

CFLAGS ?= -Wall -Wextra -Wconversion -Oz
CXXFLAGS ?= -Wall -Wextra -Wconversion -Oz

# ----------------------------

ifndef CEDEV
$(error CEDEV environment path variable is not set)
endif

include $(CEDEV)/meta/makefile.mk
