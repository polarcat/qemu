#!/bin/sh

#target=arc-softmmu,aarch64-softmmu
target=arc-softmmu

print_help()
{
	echo
	printf "\033[1m> Usage: $(basename $0) <options>\033[0m\n"
	printf "\033[2m| Options:\033[0m\n"
	printf "\033[2m|   make     make qemu binary\033[0m\n"
	printf "\033[2m|   config   run configure tool for $target\033[0m\n"
	printf "\033[2m|   static   run configure tool for arc-softmmu static\033[0m\n"
	echo
}

case $1 in
config)
	set -x
	CFLAGS=-rdynamic ./configure --enable-debug --disable-strip --target-list=$target
	;;
static)
	set -x
	CFLAGS=-rdynamic ./configure\
	 --audio-drv-list=alsa\
	 --disable-gtk --disable-sdl --disable-opengl\
	 --static --target-list=arc-softmmu
	;;
make)
	set -x
	make -j12
	;;
*)
	print_help
	;;
esac
