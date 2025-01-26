COMPILER = gcc
INCLUDES = -Isrc -Isrc/*
OUTPUT = mocker
FLAGS = -g -Wall
LINKS = -lcurl -lmnl
SRC = src/*.c src/*/*.c
DEBUG = # -DENABLE_LOGGING

all:
	$(COMPILER) $(FLAGS) $(DEBUG) -o $(OUTPUT) -I$(INCLUDES) $(SRC) $(LINKS)

clean:
	rm -f mocker

run:
	echo "Running ./mocker run ubuntu:latest /bin/sh"
	sudo ./mocker run ubuntu:latest /bin/sh
	echo "Goodbye"
