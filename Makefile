PROGS = git_filter
CFLAGS = -O2 -Wall -Werror
CFLAGS += -ggdb
LDLIBS = -lgit2
UNAME = $(shell uname)
ifneq ($(UNAME), Darwin)
LDLIBS += -lrt
endif

all: $(PROGS)
 
%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

git_filter: dict.o

$(PROGS): %: %.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f *.o
	rm -f $(PROGS)
