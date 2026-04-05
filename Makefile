NAME    = looper
BUNDLE  = $(NAME).lv2

PREFIX  ?= /usr/local
LV2DIR  ?= $(PREFIX)/lib/lv2
USERDIR  = $(HOME)/.lv2

LV2_CFLAGS  = $(shell pkg-config --cflags lv2)
UI_CFLAGS   = $(shell pkg-config --cflags cairo-xlib x11)
UI_LDFLAGS  = $(shell pkg-config --libs   cairo-xlib x11)

CXXFLAGS += -std=c++11 -fPIC -O2 -Wall -Wextra -ffast-math $(LV2_CFLAGS)
LDFLAGS  += -shared -fPIC -Wl,--no-undefined

.PHONY: all install install-user clean

all: $(BUNDLE)/$(NAME).so $(BUNDLE)/$(NAME)_ui.so

$(BUNDLE)/$(NAME).so: looper.cpp
	mkdir -p $(BUNDLE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	cp manifest.ttl looper.ttl $(BUNDLE)/

$(BUNDLE)/$(NAME)_ui.so: looper_ui.cpp
	mkdir -p $(BUNDLE)
	$(CXX) $(CXXFLAGS) $(UI_CFLAGS) -o $@ $< $(LDFLAGS) $(UI_LDFLAGS)

# Install system-wide
install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(BUNDLE)/$(NAME).so    $(DESTDIR)$(LV2DIR)/$(BUNDLE)/
	install -m755 $(BUNDLE)/$(NAME)_ui.so $(DESTDIR)$(LV2DIR)/$(BUNDLE)/
	install -m644 $(BUNDLE)/manifest.ttl  $(DESTDIR)$(LV2DIR)/$(BUNDLE)/
	install -m644 $(BUNDLE)/looper.ttl    $(DESTDIR)$(LV2DIR)/$(BUNDLE)/

# Install to user's ~/.lv2  (no sudo needed)
install-user: all
	install -d $(USERDIR)/$(BUNDLE)
	install -m755 $(BUNDLE)/$(NAME).so    $(USERDIR)/$(BUNDLE)/
	install -m755 $(BUNDLE)/$(NAME)_ui.so $(USERDIR)/$(BUNDLE)/
	install -m644 $(BUNDLE)/manifest.ttl  $(USERDIR)/$(BUNDLE)/
	install -m644 $(BUNDLE)/looper.ttl    $(USERDIR)/$(BUNDLE)/

clean:
	rm -rf $(BUNDLE)
