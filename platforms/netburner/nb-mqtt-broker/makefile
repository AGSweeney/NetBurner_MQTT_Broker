NAME = nb-mqtt-broker

PLATFORM ?= NANO54415

MQTT_BROKER_ROOT = ../../../libs/mqtt_broker

CPP_SRC = \
	src/main.cpp \
	src/broker_server.cpp \
	src/broker_config.cpp \
	src/broker_web.cpp \
	src/ssluser.cpp \
	src/certificatekey.cpp \
	src/nvsettings.cpp \
	src/webui_config.cpp \
	src/ssl_service.cpp \
	src/webfunctions.cpp \
	src/mqtt_auth_store.cpp \
	$(MQTT_BROKER_ROOT)/src/varint.cpp \
	$(MQTT_BROKER_ROOT)/src/wire.cpp \
	$(MQTT_BROKER_ROOT)/src/message_pool.cpp \
	$(MQTT_BROKER_ROOT)/src/protocol_error.cpp \
	$(MQTT_BROKER_ROOT)/src/debug_log.cpp \
	$(MQTT_BROKER_ROOT)/src/subscription_options.cpp \
	$(MQTT_BROKER_ROOT)/src/topic_trie.cpp \
	$(MQTT_BROKER_ROOT)/src/topic_intern.cpp \
	$(MQTT_BROKER_ROOT)/src/property_pool.cpp \
	$(MQTT_BROKER_ROOT)/src/property_rules.cpp \
	$(MQTT_BROKER_ROOT)/src/parser.cpp \
	$(MQTT_BROKER_ROOT)/src/encode.cpp \
	$(MQTT_BROKER_ROOT)/src/tx_queue.cpp \
	$(MQTT_BROKER_ROOT)/src/retained_store.cpp \
	$(MQTT_BROKER_ROOT)/src/offline_queue.cpp \
	$(MQTT_BROKER_ROOT)/src/inflight.cpp \
	$(MQTT_BROKER_ROOT)/src/broker.cpp \
	$(MQTT_BROKER_ROOT)/src/broker_policy.cpp

CREATEDTARGS += src/htmldata.cpp
CPP_SRC += src/htmldata.cpp

src/htmldata.cpp : $(wildcard html/*.*)
	comphtml html -osrc/htmldata.cpp

EXTRACPPFLAGS += -I$(MQTT_BROKER_ROOT)/include -DPLATFORM_$(PLATFORM)

# EFFS-STD: RT platforms (SOMRT1061/MODRT1171) mount flash in the platform library.
# ColdFire and SAME70 targets need libStdFFile, COMPCODEFLAGS, and fs_main() in UserMain.
ifeq ($(PLATFORM),MODM7AE70)
	EXTRAPACKARGS += -cflag C:3
endif
ifeq ($(PLATFORM),NANO54415)
	COMPCODEFLAGS = 0x04000 0x700000
endif
ifeq ($(PLATFORM),MOD5441X)
	COMPCODEFLAGS = 0xC0040000 0xC1F00000
endif
ifeq ($(PLATFORM),SB800EX)
	COMPCODEFLAGS = 0x04000 0x700000
endif

ifeq ($(filter $(PLATFORM),SOMRT1061 MODRT1171),)
	XTRALIB = $(NNDK_ROOT)/platform/$(PLATFORM)/original/lib/libStdFFile.a
	DBXTRALIB = $(NNDK_ROOT)/platform/$(PLATFORM)/original/lib/libStdFFile.a
endif

include $(NNDK_ROOT)/examples/_common/EFFS/STD/common.mak

include $(NNDK_ROOT)/make/boilerplate.mk
