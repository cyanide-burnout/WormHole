FASTRING ?= ../../FastRing/Ring
WORMHOLE ?= ../Library

EXECUTABLE := test

DIRECTORIES := \
	$(FASTRING) \
	$(WORMHOLE) \
	$(TRACE)

LIBRARIES := \
	pthread

DEPENDENCIES := \
	liburing

OBJECTS := \
	$(FASTRING)/FastRing.o \
	$(WORMHOLE)/WormHole.o \
	$(WORMHOLE)/WormLoop.o \
	Test.o

ifneq ($(TRACE),)
	LIBRARIES += \
		stdc++ \
		unwind \
		dwarf \
		elf \
		dl \
		curses

	DEPENDENCIES += \
		libdebuginfod \
		libsystemd

	OBJECTS += \
		$(TRACE)/CXXTrace.o \
		$(TRACE)/CXXABITools.o \
		$(TRACE)/ReportTools.o \
		$(TRACE)/DebugDecoder.o \
		$(TRACE)/FaultHandler.o

	FLAGS += -DTRACE
endif

FLAGS += \
	-Wno-unused-result -Wno-format-truncation -Wno-format-overflow \
	-rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf -znoexecstack \
	$(foreach directory, $(DIRECTORIES), -I$(directory)) \
	$(shell pkg-config --cflags $(DEPENDENCIES))

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library)) \
	$(shell pkg-config --libs $(DEPENDENCIES))

CFLAGS   += $(FLAGS) -std=gnu11
CXXFLAGS += $(FLAGS) -std=gnu++2a -fnon-call-exceptions

build: $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(OBJECTS) $(patsubst %.o,%.d,$(OBJECTS)) $(EXECUTABLE) $(EXECUTABLE).d
