CPPFLAGS=-Wall -Wextra -pedantic
LDFLAGS=-lelf
OBJ=main.o packer.o elf_file.o debug.o
EXE=unpack

all: $(EXE)

$(EXE): $(OBJ)
	g++ -o $(EXE) $(OBJ) $(LDFLAGS)

clean:
	rm -f *.o $(EXE)
