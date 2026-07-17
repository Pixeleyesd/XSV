# builds the capture poc. nothing fancy, this gets replaced once there's
# more than one source file worth building.

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
LDLIBS := -lX11 -lXcomposite -lXdamage -lXfixes -lXext

BIN := xsv-capture-poc
SRC := src/main.c

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: clean
