# Read after GPGX's sdl/Makefile.sdl2 by the root Makefile's Windows build: a goal for the core's
# objects, without the SDL frontend's main and its Windows icon
gpgx-objects: $(OBJDIR) $(filter-out $(OBJDIR)/main.o $(OBJDIR)/icon.o,$(OBJECTS))
