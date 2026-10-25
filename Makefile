.RECIPEPREFIX = >

# ---- Identidade do pacote ----
TITLE      := PurpleFlix by Davitxtz
VERSION    := 01.12
TITLE_ID   := PFLX00002
CONTENT_ID := IV0000-PFLX00002_00-PURPLEFLIX000000

# ---- Ferramentas (o workflow passa os caminhos reais) ----
FSELF   ?= create-fself
GP4     ?= create-gp4
PKGTOOL ?= PkgTool.Core

# ---- Toolchain ----
TOOLCHAIN := $(OO_PS4_TOOLCHAIN)

CORE_LIBS  := -lc -lkernel -lc++ -lSceLibcInternal
ALL_STUBS  := $(sort $(patsubst $(TOOLCHAIN)/lib/lib%.so,-l%,$(wildcard $(TOOLCHAIN)/lib/lib*.so)) \
                     $(patsubst $(TOOLCHAIN)/lib/lib%.a,-l%,$(wildcard $(TOOLCHAIN)/lib/lib*.a)))
EXTRA_LIBS := $(filter-out $(CORE_LIBS),$(ALL_STUBS))

SRCDIR   := src
INTDIR   := build
PROJ     := purpleflix
OUT_ELF  := $(INTDIR)/$(PROJ).elf
OUT_OELF := $(INTDIR)/$(PROJ).oelf

CC  := clang
CXX := clang++
LD  := ld.lld

CPPFILES := $(wildcard $(SRCDIR)/*.cpp)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(INTDIR)/%.o,$(CPPFILES))

ifeq ($(strip $(OBJS)),)
$(error Nenhum .cpp encontrado em $(SRCDIR)/)
endif

LIBMODULES := $(wildcard sce_module/*)

CFLAGS   := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c \
            -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1 -isystem $(TOOLCHAIN)/include/SDL2

LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
           -L$(TOOLCHAIN)/lib $(CORE_LIBS) --as-needed $(EXTRA_LIBS) --no-as-needed \
           $(TOOLCHAIN)/lib/crt1.o

all: $(CONTENT_ID).pkg

$(INTDIR):
> mkdir -p $(INTDIR)

$(INTDIR)/%.o: $(SRCDIR)/%.cpp | $(INTDIR)
> $(CXX) $(CXXFLAGS) -o $@ $<

eboot.bin: $(OBJS) | $(INTDIR)
> $(LD) $(OBJS) -o $(OUT_ELF) $(LDFLAGS)
> $(FSELF) -in=$(OUT_ELF) -out=$(OUT_OELF) --eboot "eboot.bin" --paid 0x3800000000000011

sce_sys/param.sfo: Makefile
> $(PKGTOOL) sfo_new $@
> $(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
> $(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
> $(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
> $(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
> $(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
> $(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
> $(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

pkg.gp4: eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx $(LIBMODULES)
> $(GP4) -out $@ --content-id=$(CONTENT_ID) --files "$^"

$(CONTENT_ID).pkg: pkg.gp4
> $(PKGTOOL) pkg_build $< .

clean:
> rm -rf $(INTDIR) eboot.bin pkg.gp4 sce_sys/param.sfo *.pkg

.PHONY: all clean
