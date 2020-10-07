
O = o
C_SRC = typebridge.c
DEPS_ALL = $(C_SRC:%.c=$(O)/%.d) 
APP = typebridge
LDLIBS += -lmxml


# some sane build-defaults
DATE_FMT = +%Y-%m-%d_%H:%M
ifdef SOURCE_DATE_EPOCH
    BUILD_DATE ?= $(shell date -u -d "@$(SOURCE_DATE_EPOCH)" "$(DATE_FMT)" 2>/dev/null || date -u -r "$(SOURCE_DATE_EPOCH)" "$(DATE_FMT)" 2>/dev/null || date -u "$(DATE_FMT)")
else
    BUILD_DATE ?= $(shell date "$(DATE_FMT)")
endif

CPPFLAGS += -DPOSIX -DBUILD_DATE=\"$(BUILD_DATE)\"
CPPFLAGS += -Os -g
LDFLAGS  += -g

ifeq ($(DEBUG),TRUE)
	CFLAGS += -DDEBUG
endif

CPPFLAGS += -Wall -Werror
CFLAGS += -c -fstrength-reduce -fmessage-length=0 -funsigned-bitfields

.PHONY: all distclean clean typebridge

all: $(O)/$(APP)

$(O):
	@mkdir -p "$@"

%.o : %.c
$(O)/%.o : %.c $(O)/%.d | $(O)
	@echo "  CC      $@"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"

$(APP): $(O)/$(APP)

$(O)/$(APP): $(C_SRC:%.c=$(O)/%.o)
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

distclean: clean
	$(RM) -r $(O)

clean:
	@echo "  RM      *.o *.d"
	@$(RM)  $(DEPS_ALL:%.d=%.o) $(DEPS_ALL)

$(DEPS_ALL) :  | $(O)

#Include the dependency files that exist.
-include $(DEPS_ALL)

