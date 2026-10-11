WORMHOLE ?= ../../Library

EXECUTABLE := stress

DIRECTORIES := \
	$(WORMHOLE)

LIBRARIES := \
	pthread

OBJECTS := \
	$(WORMHOLE)/WormHole.o \
	Stress.o

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
