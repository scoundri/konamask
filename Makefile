# --- toolchain -----------------------------------------------------
CXX       := g++
PKG_CONFIG:= pkg-config

# --- flags ---------------------------------------------------------
CXXFLAGS  := $(shell $(PKG_CONFIG) --cflags espeak-ng portaudio-2.0 sdl2 libpulse-simple gtk+-3.0 appindicator3-0.1 rtaudio)
CXXFLAGS += -Iinclude -I/usr/include/glib-2.0 -I/usr/include/gtk-3.0

LDFLAGS   := $(shell $(PKG_CONFIG) --libs espeak-ng portaudio-2.0 sdl2 libpulse-simple gtk+-3.0 appindicator3-0.1 rtaudio)
LDFLAGS  += -lvosk -pthread -limgui -lvulkan -lcurl

# --- directories ---------------------------------------------------
SRCDIR     := src
OBJDIR     := obj
BINDIR     := output

# --- sources -------------------------------------------------------
SRC_FILES     := $(wildcard $(SRCDIR)/*.cpp)
SOURCES       := $(SRC_FILES)

OBJECTS       := $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(SOURCES)))
TARGET        := $(BINDIR)/main

# --- default rule --------------------------------------------------
.PHONY: all
all: $(TARGET)

# --- linking -------------------------------------------------------
$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $@"
	$(CXX) $(addprefix $(OBJDIR)/,$(notdir $(OBJECTS))) -o $@ $(LDFLAGS)

# --- compilation ---------------------------------------------------
# from src/
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# from third_party/imgui_backends/
$(OBJDIR)/%.o: $(BACKENDDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling backend $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# --- dirs ----------------------------------------------------------
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# --- utils ---------------------------------------------------------
.PHONY: run
run: all
	@echo "Running $(TARGET)"
	$(TARGET)

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(BINDIR)
