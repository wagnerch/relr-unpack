CPPFLAGS=-Wall -Wextra -pedantic
LDFLAGS=-lelf
OBJ=main.o packer.o elf_file.o debug.o sleb128.o delta_encoder.o
EXE=relocation_packer

all: $(EXE)

$(EXE): $(OBJ)
	g++ -o $(EXE) $(OBJ) $(LDFLAGS)

clean:
	rm -f *.o $(EXE)
