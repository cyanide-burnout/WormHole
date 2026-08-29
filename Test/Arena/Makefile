WORMHOLE ?= ../../Library
TOOLKIT  ?= ../../../TetraPack/Product/Toolkit

EXECUTABLE := arena

DIRECTORIES := \
	$(WORMHOLE) \
	$(TOOLKIT)/Common/

LIBRARIES := \
	pthread

OBJECTS := \
	$(WORMHOLE)/WormArena.o \
	$(TOOLKIT)/Common/CRC32C.o \
	$(TOOLKIT)/Common/HashMap.o \
	Arena.o

FLAGS += \
	-Wno-unused-result -rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf -znoexecstack \
	$(foreach directory, $(DIRECTORIES), -I$(directory))

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library))

CFLAGS += $(FLAGS) -std=gnu11

build: $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(OBJECTS) $(patsubst %.o,%.d,$(OBJECTS)) $(EXECUTABLE)
