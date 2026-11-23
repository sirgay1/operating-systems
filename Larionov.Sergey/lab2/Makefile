transports := $(basename $(notdir $(wildcard src/conn_*.cpp src/conn_*.c)))
types := $(patsubst conn_%,%,$(transports))

.PHONY: all clean

all:
	+bash ./build.sh

clean:
	$(RM) $(addprefix host_,$(types)) $(addprefix client_,$(types))
